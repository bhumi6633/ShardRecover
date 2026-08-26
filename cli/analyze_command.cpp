#include "analyze_command.hpp"

#include "shardrecover/binary_file.hpp"
#include "shardrecover/overlap.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace shardrecover::cli {
namespace {

std::size_t parse_minimum_overlap(std::string_view value)
{
    std::size_t minimum = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), minimum);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error("Invalid minimum overlap: '" + std::string(value) + "'");
    }
    if (minimum == 0) {
        throw std::runtime_error("Minimum overlap must be greater than zero");
    }
    return minimum;
}

std::vector<std::filesystem::path> discover_fragments(const std::filesystem::path& directory)
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
    return paths;
}

struct AnalysisSummary {
    std::size_t pairs_tested = 0;
    std::size_t matches = 0;
    std::chrono::steady_clock::duration elapsed{};
};

AnalysisSummary analyze_pairs(const std::vector<BinaryFile>& fragments,
                              std::size_t minimum_overlap)
{
    AnalysisSummary summary;
    const auto start = std::chrono::steady_clock::now();

    for (std::size_t left = 0; left < fragments.size(); ++left) {
        for (std::size_t right = 0; right < fragments.size(); ++right) {
            if (left == right) {
                continue;
            }

            ++summary.pairs_tested;
            const auto result = find_suffix_prefix_overlap(fragments[left].bytes(),
                                                           fragments[right].bytes());
            if (result.length < minimum_overlap) {
                continue;
            }

            ++summary.matches;
            std::cout << fragments[left].path().filename().string() << " -> "
                      << fragments[right].path().filename().string()
                      << "    overlap=" << result.length << '\n';
        }
    }

    summary.elapsed = std::chrono::steady_clock::now() - start;
    return summary;
}

}  // namespace

int run_analyze_command(int argc, char* argv[])
{
    if (argc == 0) {
        throw std::runtime_error("analyze requires a fragment directory");
    }

    const std::filesystem::path directory{argv[0]};
    std::size_t minimum_overlap = 1;
    bool has_minimum = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view option{argv[index]};
        if (option == "--min-overlap") {
            if (has_minimum) {
                throw std::runtime_error("--min-overlap may only be specified once");
            }
            if (++index >= argc) {
                throw std::runtime_error("--min-overlap requires a value");
            }
            minimum_overlap = parse_minimum_overlap(argv[index]);
            has_minimum = true;
        } else {
            throw std::runtime_error("Unexpected argument: '" + std::string(option) + "'");
        }
    }

    const auto paths = discover_fragments(directory);
    std::vector<BinaryFile> fragments;
    fragments.reserve(paths.size());
    for (const auto& path : paths) {
        fragments.push_back(BinaryFile::load(path));
    }

    const auto summary = analyze_pairs(fragments, minimum_overlap);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(summary.elapsed);

    std::cout << "Fragments analyzed: " << fragments.size() << '\n'
              << "Directed pairs tested: " << summary.pairs_tested << '\n'
              << "Matches above threshold: " << summary.matches << '\n'
              << "Minimum overlap: " << minimum_overlap << " bytes\n"
              << "Analysis time: " << elapsed.count() << " us\n";
    return 0;
}

}  // namespace shardrecover::cli
