#include "shardrecover/fragment_loader.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <system_error>

namespace shardrecover {

std::vector<BinaryFile> load_fragment_directory(const std::filesystem::path& directory,
                                                FileLoadStrategy strategy)
{
    std::error_code error;
    const auto status = std::filesystem::status(directory, error);
    if (error) {
        throw std::runtime_error("Failed to inspect fragment directory '" + directory.string()
                                 + "': " + error.message());
    }
    if (!std::filesystem::exists(status)) {
        throw std::runtime_error("Fragment directory does not exist: '" + directory.string() + "'");
    }
    if (!std::filesystem::is_directory(status)) {
        throw std::runtime_error("Fragment path is not a directory: '" + directory.string() + "'");
    }

    std::vector<std::filesystem::path> paths;
    std::filesystem::directory_iterator iterator(directory, error);
    const std::filesystem::directory_iterator end;
    if (error) {
        throw std::runtime_error("Failed to read fragment directory '" + directory.string()
                                 + "': " + error.message());
    }
    while (iterator != end) {
        if (iterator->is_regular_file(error)) {
            paths.push_back(iterator->path());
        } else if (error) {
            throw std::runtime_error("Failed to inspect directory entry '"
                                     + iterator->path().string() + "': " + error.message());
        }
        iterator.increment(error);
        if (error) {
            throw std::runtime_error("Failed while reading fragment directory '"
                                     + directory.string() + "': " + error.message());
        }
    }

    std::sort(paths.begin(), paths.end(), [](const auto& left, const auto& right) {
        return left.filename().string() < right.filename().string();
    });

    std::vector<BinaryFile> fragments;
    fragments.reserve(paths.size());
    for (const auto& path : paths) {
        fragments.push_back(strategy == FileLoadStrategy::mapped
                                ? BinaryFile::map(path)
                                : BinaryFile::load(path));
    }
    return fragments;
}

}  // namespace shardrecover
