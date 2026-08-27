#include "shardrecover/binary_file.hpp"
#include "shardrecover/fragment_generator.hpp"
#include "shardrecover/fragment_graph.hpp"

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

struct Options {
    std::size_t fragment_count = 100;
    std::size_t fragment_size = 4096;
    std::size_t overlap = 512;
    std::size_t iterations = 5;
    std::uint64_t seed = 42;
    StrategySelection strategy = StrategySelection::both;
    std::size_t threads = 1;
};

struct Result {
    shardrecover::GraphBuildStrategy strategy;
    shardrecover::GraphBuildStats statistics;
    std::vector<double> milliseconds;
    std::size_t nodes = 0;
    std::size_t edges = 0;
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

Result run_benchmark(std::span<const shardrecover::BinaryFile> files,
                     const Options& options,
                     shardrecover::GraphBuildStrategy strategy)
{
    Result result{};
    result.strategy = strategy;
    result.milliseconds.reserve(options.iterations);
    for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
        shardrecover::GraphBuildStats statistics;
        const auto start = std::chrono::steady_clock::now();
        const auto graph = shardrecover::FragmentGraph::build(
            files,
            shardrecover::GraphBuildConfig{options.overlap, 0, strategy, options.threads},
            &statistics);
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start);
        result.milliseconds.push_back(elapsed.count());
        result.statistics = statistics;
        result.nodes = graph.node_count();
        result.edges = graph.edge_count();
    }
    return result;
}

double average_time(const Result& result)
{
    return std::accumulate(result.milliseconds.begin(), result.milliseconds.end(), 0.0)
           / static_cast<double>(result.milliseconds.size());
}

void print_result(const Result& result)
{
    const auto [minimum, maximum] = std::minmax_element(result.milliseconds.begin(),
                                                        result.milliseconds.end());
    std::cout << "\nStrategy: " << strategy_name(result.strategy) << '\n'
              << "Threads: " << result.statistics.threads_used << '\n'
              << "Graph nodes: " << result.nodes << '\n'
              << "Graph edges: " << result.edges << '\n'
              << "Directed pairs possible: " << result.statistics.theoretical_pairs << '\n'
              << "Candidate pairs: " << result.statistics.candidate_pairs << '\n'
              << "Full overlap checks: " << result.statistics.full_overlap_checks << '\n'
              << std::fixed << std::setprecision(3)
              << "Average graph build time: " << average_time(result) << " ms\n"
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

std::vector<shardrecover::BinaryFile> make_dataset(const Options& options,
                                                   const std::filesystem::path& directory)
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

    std::vector<shardrecover::BinaryFile> files;
    files.reserve(fragments.size());
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
        files.push_back(shardrecover::BinaryFile::load(path));
    }
    return files;
}

}  // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        const TemporaryDirectory directory;
        const auto files = make_dataset(options, directory.path());
        std::cout << "Fragments: " << options.fragment_count << '\n'
                  << "Fragment size: " << options.fragment_size << '\n'
                  << "Minimum overlap: " << options.overlap << '\n'
                  << "Seed: " << options.seed << '\n'
                  << "Iterations: " << options.iterations << '\n';

        if (options.strategy == StrategySelection::exhaustive) {
            print_result(run_benchmark(files, options,
                                       shardrecover::GraphBuildStrategy::exhaustive));
        } else if (options.strategy == StrategySelection::indexed) {
            print_result(run_benchmark(files, options,
                                       shardrecover::GraphBuildStrategy::indexed));
        } else {
            const auto exhaustive = run_benchmark(
                files, options, shardrecover::GraphBuildStrategy::exhaustive);
            const auto indexed = run_benchmark(
                files, options, shardrecover::GraphBuildStrategy::indexed);
            print_result(exhaustive);
            print_result(indexed);
            if (exhaustive.nodes != indexed.nodes || exhaustive.edges != indexed.edges) {
                throw std::runtime_error("Graph strategies produced different graph sizes");
            }
            std::cout << std::fixed << std::setprecision(2)
                      << "\nIndexed speedup: "
                      << average_time(exhaustive) / average_time(indexed) << "x\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n'
                  << "Usage: shardrecover_graph_bench --fragments <count> "
                     "--fragment-size <bytes> --overlap <bytes> "
                     "--iterations <count> --seed <integer> "
                     "--strategy <exhaustive|indexed|both> --threads <count>\n";
        return 1;
    }
}
