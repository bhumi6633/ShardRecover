#include "shardrecover/binary_file.hpp"

#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace shardrecover {
namespace {

std::string quoted_path(const std::filesystem::path& path)
{
    return "'" + path.string() + "'";
}

}  // namespace

BinaryFile BinaryFile::load(const std::filesystem::path& path)
{
    std::error_code error;
    const auto status = std::filesystem::status(path, error);

    if (error) {
        throw std::runtime_error("Failed to inspect " + quoted_path(path) + ": " + error.message());
    }
    if (!std::filesystem::exists(status)) {
        throw std::runtime_error("File does not exist: " + quoted_path(path));
    }
    if (!std::filesystem::is_regular_file(status)) {
        throw std::runtime_error("Path is not a regular file: " + quoted_path(path));
    }

    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("Failed to open file: " + quoted_path(path));
    }

    const std::streampos end = input.tellg();
    if (end < 0) {
        throw std::runtime_error("Failed to determine file size: " + quoted_path(path));
    }

    const auto length = static_cast<std::uintmax_t>(end);
    constexpr auto max_size = static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max());
    constexpr auto max_read = static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max());
    if (length > max_size || length > max_read) {
        throw std::runtime_error("File is too large to load: " + quoted_path(path));
    }

    std::vector<std::byte> data(static_cast<std::size_t>(length));
    input.seekg(0, std::ios::beg);
    if (!input) {
        throw std::runtime_error("Failed to seek file: " + quoted_path(path));
    }

    if (!data.empty()) {
        const auto expected = static_cast<std::streamsize>(data.size());
        input.read(reinterpret_cast<char*>(data.data()), expected);
        if (input.gcount() != expected || !input) {
            throw std::runtime_error("Failed to read complete file: " + quoted_path(path));
        }
    }

    return BinaryFile(path, std::move(data));
}

BinaryFile BinaryFile::map(const std::filesystem::path& path)
{
    return BinaryFile(path, MappedFile(path));
}

BinaryFile::BinaryFile(std::filesystem::path path, std::vector<std::byte> data)
    : path_(std::move(path)), storage_(std::move(data))
{
}

BinaryFile::BinaryFile(std::filesystem::path path, MappedFile mapping)
    : path_(std::move(path)), storage_(std::move(mapping))
{
}

const std::filesystem::path& BinaryFile::path() const noexcept
{
    return path_;
}

std::span<const std::byte> BinaryFile::bytes() const noexcept
{
    if (const auto* data = std::get_if<std::vector<std::byte>>(&storage_)) {
        return *data;
    }
    return std::get<MappedFile>(storage_).bytes();
}

std::size_t BinaryFile::size() const noexcept
{
    return bytes().size();
}

bool BinaryFile::empty() const noexcept
{
    return bytes().empty();
}

bool BinaryFile::mapped() const noexcept
{
    return std::holds_alternative<MappedFile>(storage_);
}

}  // namespace shardrecover
