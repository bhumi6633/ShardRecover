#include "shardrecover/binary_file.hpp"
#include "shardrecover/damage.hpp"
#include "shardrecover/file_load_strategy.hpp"
#include "shardrecover/fragment_emitter.hpp"
#include "shardrecover/fragment_generator.hpp"
#include "shardrecover/fragment_graph.hpp"
#include "shardrecover/fragment_loader.hpp"
#include "shardrecover/png/reconstruction_evaluator.hpp"
#include "shardrecover/png/repair.hpp"
#include "shardrecover/repair.hpp"
#include "shardrecover/reconstruction.hpp"

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
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

enum class SearchStrategy { greedy, beam };
enum class Format { none, png };
enum class RepairStrategy { none, consensus, png };

struct Options {
    std::optional<std::filesystem::path> input;
    std::optional<std::size_t> input_size;
    std::size_t fragment_size = 256;
    std::size_t overlap = 128;
    std::uint64_t seed = 42;
    std::size_t duplicate_count = 0;
    std::size_t noise_count = 0;
    std::size_t drop_count = 0;
    std::size_t corrupt_byte_count = 0;
    SearchStrategy search = SearchStrategy::beam;
    std::size_t beam_width = 8;
    shardrecover::GraphBuildStrategy graph_build = shardrecover::GraphBuildStrategy::indexed;
    std::size_t threads = 1;
    shardrecover::FileLoadStrategy io = shardrecover::FileLoadStrategy::buffered;
    std::size_t max_mismatches = 0;
    RepairStrategy repair = RepairStrategy::none;
    Format format = Format::none;
    std::size_t iterations = 1;
    bool csv = false;
};

struct Timings {
    double generation_ms = 0;
    double damage_ms = 0;
    double load_ms = 0;
    double graph_ms = 0;
    double search_ms = 0;
    double repair_ms = 0;
    double pipeline_ms = 0;
};

struct RunResult {
    shardrecover::DamageResult damage;
    shardrecover::GraphBuildStats graph_stats;
    std::size_t graph_nodes = 0;
    std::size_t graph_edges = 0;
    std::size_t exact_edges = 0;
    std::size_t selected_fragments = 0;
    bool engine_complete = false;
    std::vector<std::byte> output;
    std::vector<std::byte> original;
    std::size_t repair_events = 0;
    Timings timings;
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

std::size_t parse_size(std::string_view value, std::string_view name, bool allow_zero = true)
{
    const auto parsed = parse_integer(value, name);
    if (parsed > std::numeric_limits<std::size_t>::max() || (!allow_zero && parsed == 0)) {
        throw std::runtime_error(std::string(name) + " is outside the supported range");
    }
    return static_cast<std::size_t>(parsed);
}

Options parse_options(int argc, char* argv[])
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option{argv[index]};
        if (option == "--csv") {
            options.csv = true;
            continue;
        }
        if (++index >= argc) {
            throw std::runtime_error(std::string(option) + " requires a value");
        }
        const std::string_view value{argv[index]};
        if (option == "--input") {
            options.input = std::filesystem::path(value);
        } else if (option == "--input-size") {
            options.input_size = parse_size(value, "input size");
        } else if (option == "--fragment-size") {
            options.fragment_size = parse_size(value, "fragment size", false);
        } else if (option == "--overlap") {
            options.overlap = parse_size(value, "overlap");
        } else if (option == "--seed") {
            options.seed = parse_integer(value, "seed");
        } else if (option == "--duplicates") {
            options.duplicate_count = parse_size(value, "duplicate count");
        } else if (option == "--noise") {
            options.noise_count = parse_size(value, "noise count");
        } else if (option == "--drop") {
            options.drop_count = parse_size(value, "drop count");
        } else if (option == "--corrupt-bytes") {
            options.corrupt_byte_count = parse_size(value, "corrupt byte count");
        } else if (option == "--strategy") {
            if (value == "greedy") options.search = SearchStrategy::greedy;
            else if (value == "beam") options.search = SearchStrategy::beam;
            else throw std::runtime_error("Strategy must be greedy or beam");
        } else if (option == "--beam-width") {
            options.beam_width = parse_size(value, "beam width", false);
        } else if (option == "--graph-build") {
            if (value == "exhaustive") options.graph_build = shardrecover::GraphBuildStrategy::exhaustive;
            else if (value == "indexed") options.graph_build = shardrecover::GraphBuildStrategy::indexed;
            else throw std::runtime_error("Graph build must be exhaustive or indexed");
        } else if (option == "--threads") {
            options.threads = parse_size(value, "thread count", false);
        } else if (option == "--io") {
            if (value == "buffered") options.io = shardrecover::FileLoadStrategy::buffered;
            else if (value == "mmap") options.io = shardrecover::FileLoadStrategy::mapped;
            else throw std::runtime_error("I/O strategy must be buffered or mmap");
        } else if (option == "--max-mismatches") {
            options.max_mismatches = parse_size(value, "maximum mismatches");
        } else if (option == "--repair") {
            if (value == "none") options.repair = RepairStrategy::none;
            else if (value == "consensus") options.repair = RepairStrategy::consensus;
            else if (value == "png") options.repair = RepairStrategy::png;
            else throw std::runtime_error("Repair must be none, consensus, or png");
        } else if (option == "--format") {
            if (value == "none") options.format = Format::none;
            else if (value == "png") options.format = Format::png;
            else throw std::runtime_error("Format must be none or png");
        } else if (option == "--iterations") {
            options.iterations = parse_size(value, "iteration count", false);
        } else {
            throw std::runtime_error("Unexpected argument: '" + std::string(option) + "'");
        }
    }
    if (options.input.has_value() == options.input_size.has_value()) {
        throw std::runtime_error("Specify exactly one of --input or --input-size");
    }
    if (options.overlap >= options.fragment_size) {
        throw std::runtime_error("Overlap must be smaller than fragment size");
    }
    if (options.repair == RepairStrategy::png && options.format != Format::png) {
        throw std::runtime_error("PNG repair requires --format png");
    }
    return options;
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
                / ("shardrecover-recovery-bench-" + std::to_string(stamp));
        if (!std::filesystem::create_directory(path_)) {
            throw std::runtime_error("Failed to create recovery benchmark directory");
        }
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

template <typename Function>
double measure(Function&& function)
{
    const auto start = std::chrono::steady_clock::now();
    function();
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

std::vector<std::byte> make_source(const Options& options)
{
    if (options.input.has_value()) {
        const auto file = shardrecover::BinaryFile::load(*options.input);
        return {file.bytes().begin(), file.bytes().end()};
    }
    std::mt19937_64 engine(options.seed);
    std::vector<std::byte> source(*options.input_size);
    for (auto& value : source) {
        value = static_cast<std::byte>(engine() & 0xffU);
    }
    return source;
}

void write_dataset(const std::filesystem::path& directory,
                   std::vector<shardrecover::Fragment>& fragments,
                   std::uint64_t seed)
{
    shardrecover::FragmentEmitter::shuffle(fragments, seed);
    const auto filenames = shardrecover::FragmentEmitter::opaque_filenames(fragments.size(), seed);
    for (std::size_t index = 0; index < fragments.size(); ++index) {
        std::ofstream output(directory / filenames[index], std::ios::binary | std::ios::trunc);
        const auto& bytes = fragments[index].data;
        if (!bytes.empty()) {
            output.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
        }
        output.close();
        if (!output) throw std::runtime_error("Failed to emit benchmark fragment");
    }
}

RunResult run_once(const Options& options)
{
    RunResult result;
    result.original = make_source(options);
    std::vector<shardrecover::Fragment> originals;
    result.timings.generation_ms = measure([&] {
        originals = shardrecover::FragmentGenerator::generate(
            result.original, options.fragment_size, options.overlap);
    });
    result.timings.damage_ms = measure([&] {
        result.damage = shardrecover::DamageSimulator::apply(
            originals, {options.duplicate_count, options.noise_count, options.drop_count,
                        options.corrupt_byte_count, options.seed});
    });

    const TemporaryDirectory directory;
    write_dataset(directory.path(), result.damage.fragments, options.seed);
    std::vector<shardrecover::BinaryFile> files;
    result.timings.load_ms = measure([&] {
        files = shardrecover::load_fragment_directory(directory.path(), options.io);
    });
    shardrecover::FragmentGraph graph;
    result.timings.graph_ms = measure([&] {
        graph = shardrecover::FragmentGraph::build(
            files,
            {options.overlap, options.max_mismatches, options.graph_build, options.threads},
            &result.graph_stats);
    });
    result.graph_nodes = graph.node_count();
    result.graph_edges = graph.edge_count();
    result.exact_edges = static_cast<std::size_t>(std::count_if(
        graph.edges().begin(), graph.edges().end(), [](const auto& edge) { return edge.exact; }));

    shardrecover::ReconstructionResult reconstruction;
    result.timings.search_ms = measure([&] {
        if (options.search == SearchStrategy::beam) {
            shardrecover::png::ReconstructionEvaluator evaluator;
            reconstruction = shardrecover::BeamReconstructor::reconstruct(
                graph, files, options.beam_width,
                options.format == Format::png ? &evaluator : nullptr).selected;
        } else {
            reconstruction = shardrecover::GreedyReconstructor::reconstruct(graph, files);
        }
    });
    result.selected_fragments = reconstruction.steps.size();
    result.engine_complete = reconstruction.complete;
    result.output = reconstruction.bytes;
    result.timings.repair_ms = measure([&] {
        if (options.repair != RepairStrategy::none) {
            const auto consensus = shardrecover::ConsensusRepairer::repair(reconstruction, files);
            result.repair_events = consensus.repairs.size();
            if (options.repair == RepairStrategy::png) {
                const auto png = shardrecover::png::CrcGuidedRepairer::repair(consensus);
                result.repair_events += png.repairs.size();
                result.output = png.bytes;
            } else {
                result.output = consensus.bytes;
            }
        }
    });
    result.timings.pipeline_ms = result.timings.load_ms + result.timings.graph_ms
                                 + result.timings.search_ms + result.timings.repair_ms;
    return result;
}

std::int64_t size_delta(std::size_t output, std::size_t original)
{
    const auto limit = static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max());
    if (output > limit || original > limit) throw std::overflow_error("Size delta exceeds int64_t");
    return static_cast<std::int64_t>(output) - static_cast<std::int64_t>(original);
}

void print_result(const Options& options, const RunResult& result, const Timings& average)
{
    const bool exact = result.output == result.original;
    if (options.csv) {
        std::cout << "seed,input_bytes,fragment_size,overlap,original_fragments,emitted_fragments,"
                     "duplicates,noise,dropped,corrupted_bytes,strategy,beam_width,graph_build,threads,"
                     "io,max_mismatches,repair,graph_nodes,graph_edges,candidate_pairs,overlap_checks,"
                     "selected_fragments,exact_recovery,output_bytes,size_delta,graph_ms,search_ms,"
                     "repair_ms,total_ms\n";
        std::cout << options.seed << ',' << result.original.size() << ',' << options.fragment_size << ','
                  << options.overlap << ',' << result.damage.original_fragment_count << ','
                  << result.damage.fragments.size() << ',' << result.damage.duplicates.size() << ','
                  << result.damage.noise_fragment_ids.size() << ','
                  << result.damage.dropped_original_ids.size() << ',' << result.damage.corruptions.size()
                  << ',' << (options.search == SearchStrategy::beam ? "beam" : "greedy") << ','
                  << options.beam_width << ','
                  << (options.graph_build == shardrecover::GraphBuildStrategy::indexed ? "indexed" : "exhaustive")
                  << ',' << result.graph_stats.threads_used << ','
                  << (options.io == shardrecover::FileLoadStrategy::mapped ? "mmap" : "buffered")
                  << ',' << options.max_mismatches << ','
                  << (options.repair == RepairStrategy::none ? "none" : options.repair == RepairStrategy::png ? "png" : "consensus")
                  << ',' << result.graph_nodes << ',' << result.graph_edges << ','
                  << result.graph_stats.candidate_pairs << ',' << result.graph_stats.full_overlap_checks
                  << ',' << result.selected_fragments << ',' << (exact ? 1 : 0) << ','
                  << result.output.size() << ',' << size_delta(result.output.size(), result.original.size())
                  << ',' << average.graph_ms << ',' << average.search_ms << ',' << average.repair_ms
                  << ',' << average.pipeline_ms << '\n';
        return;
    }
    std::cout << "Dataset\n"
              << "Original bytes: " << result.original.size() << '\n'
              << "Original fragments: " << result.damage.original_fragment_count << '\n'
              << "Dropped originals: " << result.damage.dropped_original_ids.size() << '\n'
              << "Duplicates added: " << result.damage.duplicates.size() << '\n'
              << "Noise added: " << result.damage.noise_fragment_ids.size() << '\n'
              << "Injected corruptions: " << result.damage.corruptions.size() << '\n'
              << "Fragments emitted: " << result.damage.fragments.size() << "\n\n"
              << "Reconstruction\n"
              << "Selected fragments: " << result.selected_fragments << '\n'
              << "Engine complete: " << (result.engine_complete ? "yes" : "no") << '\n'
              << "Output bytes: " << result.output.size() << '\n'
              << "Size delta: " << size_delta(result.output.size(), result.original.size()) << '\n'
              << "Exact recovery: " << (exact ? "yes" : "no") << "\n\n"
              << "Graph\n"
              << "Nodes: " << result.graph_nodes << '\n'
              << "Edges: " << result.graph_edges << '\n'
              << "Exact edges: " << result.exact_edges << '\n'
              << "Approximate edges: " << result.graph_edges - result.exact_edges << '\n'
              << "Theoretical pairs: " << result.graph_stats.theoretical_pairs << '\n'
              << "Candidate pairs: " << result.graph_stats.candidate_pairs << '\n'
              << "Full overlap checks: " << result.graph_stats.full_overlap_checks << '\n'
              << "Threads used: " << result.graph_stats.threads_used << "\n\n"
              << std::fixed << std::setprecision(3)
              << "Performance (average of " << options.iterations << ")\n"
              << "Fragment generation: " << average.generation_ms << " ms\n"
              << "Damage simulation: " << average.damage_ms << " ms\n"
              << "Dataset loading: " << average.load_ms << " ms\n"
              << "Graph: " << average.graph_ms << " ms\n"
              << "Search: " << average.search_ms << " ms\n"
              << "Repair: " << average.repair_ms << " ms\n"
              << "Total reconstruction pipeline: " << average.pipeline_ms << " ms\n"
              << "Repair events: " << result.repair_events << '\n';
}

}  // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        Timings total;
        RunResult result;
        for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
            result = run_once(options);
            total.generation_ms += result.timings.generation_ms;
            total.damage_ms += result.timings.damage_ms;
            total.load_ms += result.timings.load_ms;
            total.graph_ms += result.timings.graph_ms;
            total.search_ms += result.timings.search_ms;
            total.repair_ms += result.timings.repair_ms;
            total.pipeline_ms += result.timings.pipeline_ms;
        }
        const auto divisor = static_cast<double>(options.iterations);
        total.generation_ms /= divisor;
        total.damage_ms /= divisor;
        total.load_ms /= divisor;
        total.graph_ms /= divisor;
        total.search_ms /= divisor;
        total.repair_ms /= divisor;
        total.pipeline_ms /= divisor;
        print_result(options, result, total);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n'
                  << "Usage: shardrecover_recovery_bench (--input <file>|--input-size <bytes>) "
                     "[--fragment-size N] [--overlap N] [--seed N] [--duplicates N] "
                     "[--noise N] [--drop N] [--corrupt-bytes N] [--strategy greedy|beam] "
                     "[--beam-width N] [--graph-build exhaustive|indexed] [--threads N] "
                     "[--io buffered|mmap] [--max-mismatches N] [--repair none|consensus|png] "
                     "[--format none|png] [--iterations N] [--csv]\n";
        return 1;
    }
}
