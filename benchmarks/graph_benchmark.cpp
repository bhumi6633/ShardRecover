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

struct Options {
    std::size_t fragment_count = 100;
    std::size_t fragment_size = 4096;
    std::size_t overlap = 512;
    std::size_t iterations = 5;
    std::uint64_t seed = 42;
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
        } else {
            throw std::runtime_error("Unexpected argument: '" + std::string(option) + "'");
        }
    }
    if (options.overlap >= options.fragment_size) {
        throw std::runtime_error("Overlap must be smaller than fragment size");
    }
    return options;
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
        std::vector<double> milliseconds;
        milliseconds.reserve(options.iterations);
        std::size_t nodes = 0;
        std::size_t edges = 0;
        for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
            const auto start = std::chrono::steady_clock::now();
            const auto graph = shardrecover::FragmentGraph::build(files, options.overlap);
            const auto elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start);
            milliseconds.push_back(elapsed.count());
            nodes = graph.node_count();
            edges = graph.edge_count();
        }
        const auto total = std::accumulate(milliseconds.begin(), milliseconds.end(), 0.0);
        const auto [minimum, maximum] = std::minmax_element(milliseconds.begin(), milliseconds.end());
        const auto pairs = options.fragment_count * (options.fragment_count - 1);
        std::cout << "Strategy: exhaustive\n"
                  << "Fragments: " << options.fragment_count << '\n'
                  << "Fragment size: " << options.fragment_size << '\n'
                  << "Minimum overlap: " << options.overlap << '\n'
                  << "Seed: " << options.seed << '\n'
                  << "Iterations: " << options.iterations << '\n'
                  << "Graph nodes: " << nodes << '\n'
                  << "Graph edges: " << edges << '\n'
                  << "Directed pairs considered: " << pairs << '\n'
                  << std::fixed << std::setprecision(3)
                  << "Average graph build time: " << total / options.iterations << " ms\n"
                  << "Minimum graph build time: " << *minimum << " ms\n"
                  << "Maximum graph build time: " << *maximum << " ms\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n'
                  << "Usage: shardrecover_graph_bench --fragments <count> "
                     "--fragment-size <bytes> --overlap <bytes> "
                     "--iterations <count> --seed <integer>\n";
        return 1;
    }
}
