#pragma once

#include "shardrecover/binary_file.hpp"
#include "shardrecover/file_load_strategy.hpp"

#include <filesystem>
#include <vector>

namespace shardrecover::cli {

std::vector<BinaryFile> load_fragment_directory(
    const std::filesystem::path& directory,
    FileLoadStrategy strategy = FileLoadStrategy::buffered);

}  // namespace shardrecover::cli
