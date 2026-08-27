#include "shardrecover/binary_file.hpp"
#include "shardrecover/fragment_generator.hpp"
#include "shardrecover/fragment_graph.hpp"
#include "shardrecover/fragment_loader.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

enum class StrategySelection {
    exhaustive,
    indexed,
    both,
};

enum class IoSelection {
    buffered,
    mapped,
    both,
};

struct Options {
    std::size_t fragment_count = 100;
    std::size_t fragment_size = 4096;
    std::size_t overlap = 512;
    std::size_t iterations = 5;
    std::uint64_t seed = 42;
    StrategySelection strategy = StrategySelection::both;
    std::size_t threads = 1;
    IoSelection io = IoSelection::buffered;
};

struct Result {
    shardrecover::GraphBuildStrategy strategy;
    shardrecover::FileLoadStrategy io;
    shardrecover::GraphBuildStats statistics;
    std::vector<double> load_milliseconds;
    std::vector<double> graph_milliseconds;
    std::vector<double> total_milliseconds;
    std::size_t nodes = 0;
    std::size_t edges = 0;
    std::size_t total_bytes = 0;
};

std::uint64_t parse_integer(std::string_view value, std::string_view name)
{
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error("Invalid " + std::string(name) + ": '" + std::string(value) + "'");
    }
    return parsed;
}

std::size_t parse_count(std::string_view value, std::string_view name)
{
    const auto parsed = parse_integer(value, name);
    if (parsed == 0 || parsed > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error(std::string(name) + " must be a positive size_t value");
    }
    return static_cast<std::size_t>(parsed);
}

Options parse_options(int argc, char* argv[])
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option{argv[index]};
        if (++index >= argc) {
            throw std::runtime_error(std::string(option) + " requires a value");
        }
        const std::string_view value{argv[index]};
        if (option == "--fragments") {
            options.fragment_count = parse_count(value, "fragment count");
        } else if (option == "--fragment-size") {
            options.fragment_size = parse_count(value, "fragment size");
        } else if (option == "--overlap") {
            const auto parsed = parse_integer(value, "overlap");
            if (parsed > std::numeric_limits<std::size_t>::max()) {
                throw std::runtime_error("Overlap exceeds size_t");
            }
            options.overlap = static_cast<std::size_t>(parsed);
        } else if (option == "--iterations") {
            options.iterations = parse_count(value, "iteration count");
        } else if (option == "--seed") {
            options.seed = parse_integer(value, "seed");
        } else if (option == "--strategy") {
            if (value == "exhaustive") {
                options.strategy = StrategySelection::exhaustive;
            } else if (value == "indexed") {
                options.strategy = StrategySelection::indexed;
            } else if (value == "both") {
                options.strategy = StrategySelection::both;
            } else {
                throw std::runtime_error("Strategy must be exhaustive, indexed, or both");
            }
        } else if (option == "--threads") {
            options.threads = parse_count(value, "thread count");
        } else if (option == "--io") {
            if (value == "buffered") {
                options.io = IoSelection::buffered;
            } else if (value == "mmap") {
                options.io = IoSelection::mapped;
            } else if (value == "both") {
                options.io = IoSelection::both;
            } else {
                throw std::runtime_error("I/O strategy must be buffered, mmap, or both");
            }
        } else {
            throw std::runtime_error("Unexpected argument: '" + std::string(option) + "'");
        }
    }
    if (options.overlap >= options.fragment_size) {
        throw std::runtime_error("Overlap must be smaller than fragment size");
    }
    return options;
}

std::string_view strategy_name(shardrecover::GraphBuildStrategy strategy)
{
    return strategy == shardrecover::GraphBuildStrategy::indexed ? "indexed" : "exhaustive";
}

Result run_benchmark(const std::filesystem::path& directory,
                     const Options& options,
                     shardrecover::GraphBuildStrategy strategy,
                     shardrecover::FileLoadStrategy io)
{
    Result result{};
    result.strategy = strategy;
    result.io = io;
    result.load_milliseconds.reserve(options.iterations);
    result.graph_milliseconds.reserve(options.iterations);
    result.total_milliseconds.reserve(options.iterations);
    for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
        const auto total_start = std::chrono::steady_clock::now();
        const auto load_start = total_start;
        const auto files = shardrecover::load_fragment_directory(directory, io);
        const auto load_elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - load_start);
        shardrecover::GraphBuildStats statistics;
        const auto graph_start = std::chrono::steady_clock::now();
        const auto graph = shardrecover::FragmentGraph::build(
            files,
            shardrecover::GraphBuildConfig{options.overlap, 0, strategy, options.threads},
            &statistics);
        const auto graph_elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - graph_start);
        const auto total_elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - total_start);
        result.load_milliseconds.push_back(load_elapsed.count());
        result.graph_milliseconds.push_back(graph_elapsed.count());
        result.total_milliseconds.push_back(total_elapsed.count());
        result.statistics = statistics;
        result.nodes = graph.node_count();
        result.edges = graph.edge_count();
        result.total_bytes = std::accumulate(
            files.begin(), files.end(), std::size_t{0},
            [](std::size_t total, const auto& file) { return total + file.size(); });
    }
    return result;
}

double average_time(const std::vector<double>& times)
{
    return std::accumulate(times.begin(), times.end(), 0.0)
           / static_cast<double>(times.size());
}

void print_result(const Result& result)
{
    const auto [minimum, maximum] = std::minmax_element(result.graph_milliseconds.begin(),
                                                        result.graph_milliseconds.end());
    std::cout << "\nStrategy: " << strategy_name(result.strategy) << '\n'
              << "I/O strategy: "
              << (result.io == shardrecover::FileLoadStrategy::mapped ? "mmap" : "buffered")
              << '\n'
              << "Threads: " << result.statistics.threads_used << '\n'
              << "Graph nodes: " << result.nodes << '\n'
              << "Graph edges: " << result.edges << '\n'
              << "Directed pairs possible: " << result.statistics.theoretical_pairs << '\n'
              << "Candidate pairs: " << result.statistics.candidate_pairs << '\n'
              << "Full overlap checks: " << result.statistics.full_overlap_checks << '\n'
              << "Total input bytes: " << result.total_bytes << '\n'
              << std::fixed << std::setprecision(3)
              << "Average load time: " << average_time(result.load_milliseconds) << " ms\n"
              << "Average graph build time: " << average_time(result.graph_milliseconds) << " ms\n"
              << "Average total time: " << average_time(result.total_milliseconds) << " ms\n"
              << "Minimum graph build time: " << *minimum << " ms\n"
              << "Maximum graph build time: " << *maximum << " ms\n";
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto base = std::filesystem::temp_directory_path()
                          / ("shardrecover-graph-bench-" + std::to_string(stamp));
        for (int suffix = 0; suffix < 100; ++suffix) {
            auto candidate = base;
            candidate += "-" + std::to_string(suffix);
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = std::move(candidate);
                return;
            }
            if (error) {
                throw std::runtime_error("Failed to create benchmark directory: " + error.message());
            }
        }
        throw std::runtime_error("Failed to find benchmark directory name");
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void make_dataset(const Options& options, const std::filesystem::path& directory)
{
    const auto stride = options.fragment_size - options.overlap;
    if (options.fragment_count - 1
        > (std::numeric_limits<std::size_t>::max() - options.fragment_size) / stride) {
        throw std::runtime_error("Requested benchmark source size overflows size_t");
    }
    const auto source_size = options.fragment_size + (options.fragment_count - 1) * stride;
    std::mt19937_64 engine(options.seed);
    std::vector<std::byte> source(source_size);
    for (auto& value : source) {
        value = static_cast<std::byte>(engine() & 0xffU);
    }
    const auto fragments = shardrecover::FragmentGenerator::generate(
        source, options.fragment_size, options.overlap);
    if (fragments.size() != options.fragment_count) {
        throw std::logic_error("Fragment generator did not produce requested benchmark count");
    }

    for (const auto& fragment : fragments) {
        const auto path = directory / ("fragment-" + std::to_string(fragment.index) + ".bin");
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Failed to create benchmark fragment");
        }
        output.write(reinterpret_cast<const char*>(fragment.data.data()),
                     static_cast<std::streamsize>(fragment.data.size()));
        output.close();
        if (!output) {
            throw std::runtime_error("Failed to write benchmark fragment");
        }
    }
}

}  // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        const TemporaryDirectory directory;
        make_dataset(options, directory.path());
        std::cout << "Fragments: " << options.fragment_count << '\n'
                  << "Fragment size: " << options.fragment_size << '\n'
                  << "Minimum overlap: " << options.overlap << '\n'
                  << "Seed: " << options.seed << '\n'
                  << "Iterations: " << options.iterations << '\n';

        std::vector<shardrecover::GraphBuildStrategy> graph_strategies;
        if (options.strategy != StrategySelection::indexed) {
            graph_strategies.push_back(shardrecover::GraphBuildStrategy::exhaustive);
        }
        if (options.strategy != StrategySelection::exhaustive) {
            graph_strategies.push_back(shardrecover::GraphBuildStrategy::indexed);
        }
        std::vector<shardrecover::FileLoadStrategy> io_strategies;
        if (options.io != IoSelection::mapped) {
            io_strategies.push_back(shardrecover::FileLoadStrategy::buffered);
        }
        if (options.io != IoSelection::buffered) {
            io_strategies.push_back(shardrecover::FileLoadStrategy::mapped);
        }
        for (const auto io : io_strategies) {
            for (const auto strategy : graph_strategies) {
                print_result(run_benchmark(directory.path(), options, strategy, io));
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n'
                  << "Usage: shardrecover_graph_bench --fragments <count> "
                     "--fragment-size <bytes> --overlap <bytes> "
                     "--iterations <count> --seed <integer> "
                     "--strategy <exhaustive|indexed|both> --threads <count> "
                     "--io <buffered|mmap|both>\n";
        return 1;
    }
}
