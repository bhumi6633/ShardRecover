#pragma once

#include <cstddef>
#include <filesystem>
#include <span>

namespace shardrecover {

// Views returned by bytes() remain valid only while this object owns the mapping.
class MappedFile {
public:
    explicit MappedFile(const std::filesystem::path& path);
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    const std::filesystem::path& path() const noexcept;
    std::span<const std::byte> bytes() const noexcept;
    std::size_t size() const noexcept;
    bool empty() const noexcept;

private:
    void release() noexcept;

    std::filesystem::path path_;
    void* mapping_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace shardrecover
