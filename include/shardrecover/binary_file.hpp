#pragma once

#include "shardrecover/mapped_file.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <variant>
#include <vector>

namespace shardrecover {

class BinaryFile {
public:
    static BinaryFile load(const std::filesystem::path& path);
    static BinaryFile map(const std::filesystem::path& path);

    const std::filesystem::path& path() const noexcept;
    std::span<const std::byte> bytes() const noexcept;
    std::size_t size() const noexcept;
    bool empty() const noexcept;
    bool mapped() const noexcept;

private:
    BinaryFile(std::filesystem::path path, std::vector<std::byte> data);
    BinaryFile(std::filesystem::path path, MappedFile mapping);

    std::filesystem::path path_;
    std::variant<std::vector<std::byte>, MappedFile> storage_;
};

}  // namespace shardrecover
