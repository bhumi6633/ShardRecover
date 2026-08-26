#include "reconstruct_command.hpp"

#include "fragment_directory.hpp"
#include "shardrecover/fragment_graph.hpp"
#include "shardrecover/reconstruction.hpp"

#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

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

void write_reconstruction(const std::filesystem::path& path,
                          std::span<const std::byte> bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to create reconstruction output: '" + path.string() + "'");
    }
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    output.close();
    if (!output) {
        throw std::runtime_error("Failed to write reconstruction output: '" + path.string() + "'");
    }
}

}  // namespace

int run_reconstruct_command(int argc, char* argv[])
{
    if (argc == 0) {
        throw std::runtime_error("reconstruct requires a fragment directory");
    }

    const std::filesystem::path directory{argv[0]};
    std::filesystem::path output_path;
    std::size_t minimum_overlap = 1;
    bool has_minimum = false;
    bool has_output = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view option{argv[index]};
        if (option == "--min-overlap") {
            if (has_minimum) {
                throw std::runtime_error("--min-overlap may only be specified once");
            }
            if (++index >= argc || std::string_view(argv[index]).starts_with("--")) {
                throw std::runtime_error("--min-overlap requires a value");
            }
            minimum_overlap = parse_minimum_overlap(argv[index]);
            has_minimum = true;
        } else if (option == "--output") {
            if (has_output) {
                throw std::runtime_error("--output may only be specified once");
            }
            if (++index >= argc || std::string_view(argv[index]).starts_with("--")) {
                throw std::runtime_error("--output requires a file path");
            }
            output_path = argv[index];
            has_output = true;
        } else {
            throw std::runtime_error("Unexpected argument: '" + std::string(option) + "'");
        }
    }

    if (!has_output) {
        throw std::runtime_error("Missing required option: --output <file>");
    }

    const auto fragments = load_fragment_directory(directory);
    if (fragments.empty()) {
        throw std::runtime_error("Fragment directory contains no regular files: '"
                                 + directory.string() + "'");
    }

    const auto fragment_span = std::span<const BinaryFile>{fragments};
    const auto graph = FragmentGraph::build(fragment_span, minimum_overlap);
    const auto result = GreedyReconstructor::reconstruct(graph, fragment_span);
    write_reconstruction(output_path, result.bytes);

    std::cout << "Reconstruction algorithm: greedy\n"
              << "Fragments loaded: " << fragments.size() << '\n'
              << "Graph edges: " << graph.edge_count() << '\n'
              << "Minimum overlap: " << minimum_overlap << " bytes\n"
              << "\nStarting fragment:\n"
              << graph.nodes()[result.steps.front().node_id].path.filename().string() << '\n'
              << "\nReconstruction path:\n"
              << graph.nodes()[result.steps.front().node_id].path.filename().string() << '\n';

    for (std::size_t index = 1; index < result.steps.size(); ++index) {
        const auto& step = result.steps[index];
        std::cout << "  -> " << graph.nodes()[step.node_id].path.filename().string()
                  << "    overlap=" << step.overlap_from_previous << '\n';
    }

    const auto unresolved = fragments.size() - result.steps.size();
    std::cout << "\nFragments used: " << result.steps.size() << " / " << fragments.size() << '\n'
              << "Recovered bytes: " << result.bytes.size() << '\n'
              << "Overlap bytes removed: " << result.total_overlap_bytes << '\n'
              << "Status: " << (result.complete ? "complete" : "incomplete") << '\n';
    if (!result.complete) {
        std::cout << "Unresolved fragments: " << unresolved << '\n'
                  << "Exit status: 2 (partial output written)\n";
    }
    std::cout << "Output: " << output_path.string() << '\n';
    return result.complete ? 0 : 2;
}

}  // namespace shardrecover::cli
