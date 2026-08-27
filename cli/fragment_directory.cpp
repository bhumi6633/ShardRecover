#include "fragment_directory.hpp"

#include "shardrecover/fragment_loader.hpp"

namespace shardrecover::cli {

std::vector<BinaryFile> load_fragment_directory(const std::filesystem::path& directory,
                                                FileLoadStrategy strategy)
{
    return shardrecover::load_fragment_directory(directory, strategy);
}

}  // namespace shardrecover::cli
