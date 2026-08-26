#include "analyze_command.hpp"

#include "shardrecover/binary_file.hpp"
#include "shardrecover/fragment_graph.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <span>
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

std::size_t parse_top_count(std::string_view value)
{
    std::size_t count = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error("Invalid top count: '" + std::string(value) + "'");
    }
    if (count == 0) {
        throw std::runtime_error("Top count must be greater than zero");
    }
    return count;
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

}  // namespace

int run_analyze_command(int argc, char* argv[])
{
    if (argc == 0) {
        throw std::runtime_error("analyze requires a fragment directory");
    }

    const std::filesystem::path directory{argv[0]};
    std::size_t minimum_overlap = 1;
    std::size_t top_count = 20;
    bool has_minimum = false;
    bool has_top = false;

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
        } else if (option == "--top") {
            if (has_top) {
                throw std::runtime_error("--top may only be specified once");
            }
            if (++index >= argc) {
                throw std::runtime_error("--top requires a value");
            }
            top_count = parse_top_count(argv[index]);
            has_top = true;
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

    const auto start = std::chrono::steady_clock::now();
    const auto graph = FragmentGraph::build(std::span<const BinaryFile>{fragments}, minimum_overlap);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);
    const auto pair_count = fragments.empty() ? 0 : fragments.size() * (fragments.size() - 1);

    std::cout << "Fragments analyzed: " << fragments.size() << '\n'
              << "Graph nodes: " << graph.node_count() << '\n'
              << "Directed pairs tested: " << pair_count << '\n'
              << "Graph edges: " << graph.edge_count() << '\n'
              << "Minimum overlap: " << minimum_overlap << " bytes\n"
              << "Analysis time: " << elapsed.count() << " us\n"
              << "\nTop relationships:\n";

    const auto edges = graph.edges();
    const auto displayed = std::min(top_count, edges.size());
    for (std::size_t index = 0; index < displayed; ++index) {
        const auto& edge = edges[index];
        std::cout << graph.nodes()[edge.from].path.filename().string() << " -> "
                  << graph.nodes()[edge.to].path.filename().string()
                  << "    overlap=" << edge.overlap << '\n';
    }
    return 0;
}

}  // namespace shardrecover::cli
