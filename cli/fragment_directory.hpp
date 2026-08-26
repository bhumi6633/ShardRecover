#pragma once

#include "shardrecover/binary_file.hpp"

#include <filesystem>
#include <vector>

namespace shardrecover::cli {

std::vector<BinaryFile> load_fragment_directory(const std::filesystem::path& directory);

}  // namespace shardrecover::cli
