#include "shardrecover/mapped_file.hpp"

#if !defined(__unix__) && !defined(__APPLE__)
#error "MappedFile requires a POSIX mmap implementation"
#endif

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace shardrecover {
namespace {

std::runtime_error system_error(std::string_view operation,
                                const std::filesystem::path& path,
                                int error_number)
{
    return std::runtime_error(std::string(operation) + " '" + path.string()
                              + "': " + std::strerror(error_number));
}

}  // namespace

MappedFile::MappedFile(const std::filesystem::path& path) : path_(path)
{
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor == -1) {
        throw system_error("Failed to open mapped file", path_, errno);
    }

    struct stat status {};
    if (::fstat(descriptor, &status) == -1) {
        const int error_number = errno;
        ::close(descriptor);
        throw system_error("Failed to inspect mapped file", path_, error_number);
    }
    if (!S_ISREG(status.st_mode)) {
        ::close(descriptor);
        throw std::runtime_error("Mapped file path is not a regular file: '"
                                 + path_.string() + "'");
    }
    if (status.st_size < 0
        || static_cast<std::uintmax_t>(status.st_size)
               > std::numeric_limits<std::size_t>::max()) {
        ::close(descriptor);
        throw std::runtime_error("Mapped file size cannot be represented: '"
                                 + path_.string() + "'");
    }

    size_ = static_cast<std::size_t>(status.st_size);
    if (size_ != 0) {
        mapping_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, descriptor, 0);
        if (mapping_ == MAP_FAILED) {
            const int error_number = errno;
            mapping_ = nullptr;
            size_ = 0;
            ::close(descriptor);
            throw system_error("Failed to map file", path_, error_number);
        }
    }
    if (::close(descriptor) == -1) {
        const int error_number = errno;
        release();
        throw system_error("Failed to close mapped file descriptor", path_, error_number);
    }
}

MappedFile::~MappedFile()
{
    release();
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : path_(std::move(other.path_)), mapping_(other.mapping_), size_(other.size_)
{
    other.mapping_ = nullptr;
    other.size_ = 0;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept
{
    if (this != &other) {
        release();
        path_ = std::move(other.path_);
        mapping_ = other.mapping_;
        size_ = other.size_;
        other.mapping_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

const std::filesystem::path& MappedFile::path() const noexcept
{
    return path_;
}

std::span<const std::byte> MappedFile::bytes() const noexcept
{
    return {static_cast<const std::byte*>(mapping_), size_};
}

std::size_t MappedFile::size() const noexcept
{
    return size_;
}

bool MappedFile::empty() const noexcept
{
    return size_ == 0;
}

void MappedFile::release() noexcept
{
    if (mapping_ != nullptr) {
        ::munmap(mapping_, size_);
        mapping_ = nullptr;
        size_ = 0;
    }
}

}  // namespace shardrecover
