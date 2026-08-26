#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace shardrecover {

class BinaryFile {
public:
    static BinaryFile load(const std::filesystem::path& path);

    const std::filesystem::path& path() const noexcept;
    std::span<const std::byte> bytes() const noexcept;
    std::size_t size() const noexcept;
    bool empty() const noexcept;

private:
    BinaryFile(std::filesystem::path path, std::vector<std::byte> data);

    std::filesystem::path path_;
    std::vector<std::byte> data_;
};

}  // namespace shardrecover
