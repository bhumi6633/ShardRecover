#include "analyze_command.hpp"
#include "fragment_directory.hpp"

#include "shardrecover/fragment_graph.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <iomanip>
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

std::size_t parse_mismatch_count(std::string_view value)
{
    std::size_t count = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error("Invalid maximum mismatch count: '" + std::string(value) + "'");
    }
    return count;
}

std::size_t parse_thread_count(std::string_view value)
{
    std::size_t count = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size() || count == 0) {
        throw std::runtime_error("Thread count must be a positive integer: '"
                                 + std::string(value) + "'");
    }
    return count;
}

GraphBuildStrategy parse_graph_strategy(std::string_view value)
{
    if (value == "exhaustive") {
        return GraphBuildStrategy::exhaustive;
    }
    if (value == "indexed") {
        return GraphBuildStrategy::indexed;
    }
    throw std::runtime_error("Invalid graph build strategy: '" + std::string(value) + "'");
}

std::string_view strategy_name(GraphBuildStrategy strategy)
{
    return strategy == GraphBuildStrategy::indexed ? "indexed" : "exhaustive";
}

FileLoadStrategy parse_io_strategy(std::string_view value)
{
    if (value == "buffered") {
        return FileLoadStrategy::buffered;
    }
    if (value == "mmap") {
        return FileLoadStrategy::mapped;
    }
    throw std::runtime_error("Invalid I/O strategy: '" + std::string(value) + "'");
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
    std::size_t max_mismatches = 0;
    GraphBuildStrategy graph_strategy = GraphBuildStrategy::exhaustive;
    std::size_t threads = 1;
    FileLoadStrategy io_strategy = FileLoadStrategy::buffered;
    bool has_minimum = false;
    bool has_top = false;
    bool has_max_mismatches = false;
    bool has_graph_strategy = false;
    bool has_threads = false;
    bool has_io_strategy = false;

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
        } else if (option == "--max-mismatches") {
            if (has_max_mismatches) {
                throw std::runtime_error("--max-mismatches may only be specified once");
            }
            if (++index >= argc || std::string_view(argv[index]).starts_with("--")) {
                throw std::runtime_error("--max-mismatches requires a value");
            }
            max_mismatches = parse_mismatch_count(argv[index]);
            has_max_mismatches = true;
        } else if (option == "--graph-build") {
            if (has_graph_strategy) {
                throw std::runtime_error("--graph-build may only be specified once");
            }
            if (++index >= argc || std::string_view(argv[index]).starts_with("--")) {
                throw std::runtime_error("--graph-build requires exhaustive or indexed");
            }
            graph_strategy = parse_graph_strategy(argv[index]);
            has_graph_strategy = true;
        } else if (option == "--threads") {
            if (has_threads) {
                throw std::runtime_error("--threads may only be specified once");
            }
            if (++index >= argc || std::string_view(argv[index]).starts_with("--")) {
                throw std::runtime_error("--threads requires a positive integer");
            }
            threads = parse_thread_count(argv[index]);
            has_threads = true;
        } else if (option == "--io") {
            if (has_io_strategy) {
                throw std::runtime_error("--io may only be specified once");
            }
            if (++index >= argc || std::string_view(argv[index]).starts_with("--")) {
                throw std::runtime_error("--io requires buffered or mmap");
            }
            io_strategy = parse_io_strategy(argv[index]);
            has_io_strategy = true;
        } else {
            throw std::runtime_error("Unexpected argument: '" + std::string(option) + "'");
        }
    }

    const auto fragments = load_fragment_directory(directory, io_strategy);

    const auto start = std::chrono::steady_clock::now();
    GraphBuildStats graph_stats;
    const auto graph = FragmentGraph::build(
        std::span<const BinaryFile>{fragments},
        GraphBuildConfig{minimum_overlap, max_mismatches, graph_strategy, threads},
        &graph_stats);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);
    const auto exact_edges = static_cast<std::size_t>(std::count_if(
        graph.edges().begin(), graph.edges().end(), [](const auto& edge) { return edge.exact; }));

    std::cout << "Fragments analyzed: " << fragments.size() << '\n'
              << "I/O strategy: "
              << (io_strategy == FileLoadStrategy::mapped ? "mmap" : "buffered") << '\n'
              << "Graph nodes: " << graph.node_count() << '\n'
              << "Graph build requested: " << strategy_name(graph_stats.requested_strategy) << '\n'
              << "Graph build effective: " << strategy_name(graph_stats.effective_strategy) << '\n'
              << "Graph build fallback: " << (graph_stats.approximate_fallback ? "yes" : "no") << '\n'
              << "Directed pairs possible: " << graph_stats.theoretical_pairs << '\n'
              << "Candidate pairs: " << graph_stats.candidate_pairs << '\n'
              << "Full overlap checks: " << graph_stats.full_overlap_checks << '\n'
              << "Threads used: " << graph_stats.threads_used << '\n'
              << "Graph edges: " << graph.edge_count() << '\n'
              << "Exact edges: " << exact_edges << '\n'
              << "Approximate edges: " << graph.edge_count() - exact_edges << '\n'
              << "Minimum overlap: " << minimum_overlap << " bytes\n"
              << "Maximum mismatches: " << max_mismatches << '\n'
              << "Analysis time: " << elapsed.count() << " us\n"
              << "\nTop relationships:\n";

    const auto edges = graph.edges();
    const auto displayed = std::min(top_count, edges.size());
    for (std::size_t index = 0; index < displayed; ++index) {
        const auto& edge = edges[index];
        std::cout << graph.nodes()[edge.from].path.filename().string() << " -> "
                  << graph.nodes()[edge.to].path.filename().string()
                  << "    overlap=" << edge.overlap
                  << " matches=" << edge.matches
                  << " mismatches=" << edge.mismatches
                  << " similarity=" << std::fixed << std::setprecision(2)
                  << (edge.overlap == 0 ? 0.0
                                        : 100.0 * static_cast<double>(edge.matches)
                                              / static_cast<double>(edge.overlap))
                  << "% exact=" << (edge.exact ? "yes" : "no") << '\n';
    }
    return 0;
}

}  // namespace shardrecover::cli
