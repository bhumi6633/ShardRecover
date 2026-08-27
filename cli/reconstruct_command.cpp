#include "reconstruct_command.hpp"

#include "fragment_directory.hpp"
#include "shardrecover/fragment_graph.hpp"
#include "shardrecover/png/reconstruction_evaluator.hpp"
#include "shardrecover/png/repair.hpp"
#include "shardrecover/repair.hpp"
#include "shardrecover/reconstruction.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace shardrecover::cli {
namespace {

enum class ReconstructionStrategy {
    greedy,
    beam,
};

enum class ReconstructionFormat {
    none,
    png,
};

enum class RepairStrategy {
    none,
    consensus,
    png,
};

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

std::size_t parse_positive_count(std::string_view value, std::string_view name)
{
    std::size_t count = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error("Invalid " + std::string(name) + ": '" + std::string(value) + "'");
    }
    if (count == 0) {
        throw std::runtime_error(std::string(name) + " must be greater than zero");
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

std::string_view graph_strategy_name(GraphBuildStrategy strategy)
{
    return strategy == GraphBuildStrategy::indexed ? "indexed" : "exhaustive";
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

std::string hex_byte(std::byte value)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(2)
           << std::to_integer<unsigned int>(value);
    return output.str();
}

void write_repair_report(const std::filesystem::path& path,
                         const RepairResult& consensus,
                         const png::CrcRepairResult* png_result)
{
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to create repair report: '" + path.string() + "'");
    }
    output << "status\tposition\tstage\tbaseline\tresult\tevidence\n";
    for (const auto& repair : consensus.repairs) {
        output << "repaired\t" << repair.position << "\tconsensus\t"
               << hex_byte(repair.baseline_value) << '\t' << hex_byte(repair.repaired_value)
               << '\t' << repair.supporting_votes << '/' << repair.total_observations
               << " votes\n";
    }
    if (png_result != nullptr) {
        for (const auto& repair : png_result->repairs) {
            output << (repair.changed ? "repaired" : "corroborated") << '\t'
                   << repair.position << "\tpng_crc\t" << hex_byte(repair.previous_value)
                   << '\t' << hex_byte(repair.crc_consistent_value)
                   << "\tCRC-consistent observed candidate; stored=" << std::hex
                   << std::setfill('0') << std::setw(8) << repair.stored_crc << std::dec << '\n';
        }
        for (const auto& unresolved : png_result->unresolved) {
            output << "unresolved\t" << unresolved.position
                   << "\tpng_crc\t-\t-\t" << unresolved.reason << '\n';
        }
    } else {
        for (const auto& ambiguity : consensus.ambiguities) {
            output << "ambiguous\t" << ambiguity.position << "\tconsensus\t"
                   << hex_byte(ambiguity.preserved_value) << "\t-\t";
            for (std::size_t index = 0; index < ambiguity.votes.size(); ++index) {
                if (index != 0) {
                    output << ',';
                }
                output << hex_byte(ambiguity.votes[index].value) << ':'
                       << ambiguity.votes[index].votes;
            }
            output << '\n';
        }
    }
    output.close();
    if (!output) {
        throw std::runtime_error("Failed to write repair report: '" + path.string() + "'");
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
    std::size_t beam_width = 8;
    std::size_t candidate_count = 3;
    std::size_t max_mismatches = 0;
    ReconstructionStrategy strategy = ReconstructionStrategy::greedy;
    ReconstructionFormat format = ReconstructionFormat::none;
    RepairStrategy repair_strategy = RepairStrategy::none;
    GraphBuildStrategy graph_strategy = GraphBuildStrategy::exhaustive;
    std::filesystem::path repair_report_path;
    bool has_minimum = false;
    bool has_output = false;
    bool has_strategy = false;
    bool has_beam_width = false;
    bool has_candidate_count = false;
    bool has_format = false;
    bool has_max_mismatches = false;
    bool has_repair = false;
    bool has_repair_report = false;
    bool has_graph_strategy = false;

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
        } else if (option == "--strategy") {
            if (has_strategy) {
                throw std::runtime_error("--strategy may only be specified once");
            }
            if (++index >= argc || std::string_view(argv[index]).starts_with("--")) {
                throw std::runtime_error("--strategy requires greedy or beam");
            }
            const std::string_view value{argv[index]};
            if (value == "greedy") {
                strategy = ReconstructionStrategy::greedy;
            } else if (value == "beam") {
                strategy = ReconstructionStrategy::beam;
            } else {
                throw std::runtime_error("Invalid reconstruction strategy: '"
                                         + std::string(value) + "'");
            }
            has_strategy = true;
        } else if (option == "--beam-width") {
            if (has_beam_width) {
                throw std::runtime_error("--beam-width may only be specified once");
            }
            if (++index >= argc || std::string_view(argv[index]).starts_with("--")) {
                throw std::runtime_error("--beam-width requires a value");
            }
            beam_width = parse_positive_count(argv[index], "Beam width");
            has_beam_width = true;
        } else if (option == "--candidates") {
            if (has_candidate_count) {
                throw std::runtime_error("--candidates may only be specified once");
            }
            if (++index >= argc || std::string_view(argv[index]).starts_with("--")) {
                throw std::runtime_error("--candidates requires a value");
            }
            candidate_count = parse_positive_count(argv[index], "Candidate count");
            has_candidate_count = true;
        } else if (option == "--format") {
            if (has_format) {
                throw std::runtime_error("--format may only be specified once");
            }
            if (++index >= argc || std::string_view(argv[index]).starts_with("--")) {
                throw std::runtime_error("--format requires none or png");
            }
            const std::string_view value{argv[index]};
            if (value == "none") {
                format = ReconstructionFormat::none;
            } else if (value == "png") {
                format = ReconstructionFormat::png;
            } else {
                throw std::runtime_error("Invalid reconstruction format: '"
                                         + std::string(value) + "'");
            }
            has_format = true;
        } else if (option == "--repair") {
            if (has_repair) {
                throw std::runtime_error("--repair may only be specified once");
            }
            if (++index >= argc || std::string_view(argv[index]).starts_with("--")) {
                throw std::runtime_error("--repair requires none, consensus, or png");
            }
            const std::string_view value{argv[index]};
            if (value == "none") {
                repair_strategy = RepairStrategy::none;
            } else if (value == "consensus") {
                repair_strategy = RepairStrategy::consensus;
            } else if (value == "png") {
                repair_strategy = RepairStrategy::png;
            } else {
                throw std::runtime_error("Invalid repair strategy: '" + std::string(value) + "'");
            }
            has_repair = true;
        } else if (option == "--repair-report") {
            if (has_repair_report) {
                throw std::runtime_error("--repair-report may only be specified once");
            }
            if (++index >= argc || std::string_view(argv[index]).starts_with("--")) {
                throw std::runtime_error("--repair-report requires a file path");
            }
            repair_report_path = argv[index];
            has_repair_report = true;
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
    if (strategy == ReconstructionStrategy::greedy && (has_beam_width || has_candidate_count)) {
        throw std::runtime_error("--beam-width and --candidates require --strategy beam");
    }
    if (strategy == ReconstructionStrategy::greedy && format != ReconstructionFormat::none) {
        throw std::runtime_error("--format png requires --strategy beam");
    }
    if (repair_strategy == RepairStrategy::png && format != ReconstructionFormat::png) {
        throw std::runtime_error("--repair png requires --format png");
    }
    if (has_repair_report && repair_strategy == RepairStrategy::none) {
        throw std::runtime_error("--repair-report requires consensus or PNG repair");
    }

    const auto fragments = load_fragment_directory(directory);
    if (fragments.empty()) {
        throw std::runtime_error("Fragment directory contains no regular files: '"
                                 + directory.string() + "'");
    }

    const auto fragment_span = std::span<const BinaryFile>{fragments};
    GraphBuildStats graph_stats;
    const auto graph = FragmentGraph::build(
        fragment_span,
        GraphBuildConfig{minimum_overlap, max_mismatches, graph_strategy},
        &graph_stats);
    const auto search_start = std::chrono::steady_clock::now();
    ReconstructionResult result;
    std::optional<BeamReconstructionResult> beam_result;
    png::ReconstructionEvaluator png_evaluator;
    if (strategy == ReconstructionStrategy::beam) {
        const CandidateEvaluator* evaluator = format == ReconstructionFormat::png
                                                  ? &png_evaluator
                                                  : nullptr;
        beam_result = BeamReconstructor::reconstruct(graph, fragment_span, beam_width, evaluator);
        result = beam_result->selected;
    } else {
        result = GreedyReconstructor::reconstruct(graph, fragment_span);
    }
    const auto search_time = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - search_start);
    std::optional<RepairResult> repair_result;
    std::optional<png::CrcRepairResult> png_repair_result;
    if (repair_strategy != RepairStrategy::none) {
        repair_result = ConsensusRepairer::repair(result, fragment_span);
        if (repair_strategy == RepairStrategy::png) {
            png_repair_result = png::CrcGuidedRepairer::repair(*repair_result);
            write_reconstruction(output_path, png_repair_result->bytes);
        } else {
            write_reconstruction(output_path, repair_result->bytes);
        }
        if (has_repair_report) {
            write_repair_report(repair_report_path,
                                *repair_result,
                                png_repair_result.has_value() ? &*png_repair_result : nullptr);
        }
    } else {
        write_reconstruction(output_path, result.bytes);
    }

    std::cout << "Reconstruction strategy: "
              << (strategy == ReconstructionStrategy::beam ? "beam" : "greedy") << '\n';
    std::cout << "Graph build requested: " << graph_strategy_name(graph_stats.requested_strategy) << '\n'
              << "Graph build effective: " << graph_strategy_name(graph_stats.effective_strategy) << '\n'
              << "Graph build fallback: " << (graph_stats.approximate_fallback ? "yes" : "no") << '\n'
              << "Candidate pairs: " << graph_stats.candidate_pairs << '\n'
              << "Full overlap checks: " << graph_stats.full_overlap_checks << '\n';
    std::cout << "Repair strategy: ";
    if (repair_strategy == RepairStrategy::png) {
        std::cout << "consensus + PNG CRC\n";
    } else {
        std::cout << (repair_strategy == RepairStrategy::consensus ? "consensus" : "none") << '\n';
    }
    if (strategy == ReconstructionStrategy::beam) {
        std::cout << "Beam width: " << beam_width << '\n';
    }
    std::cout << "Format evidence: "
              << (format == ReconstructionFormat::png ? "png" : "none") << '\n';
    std::cout << "Fragments loaded: " << fragments.size() << '\n'
              << "Graph edges: " << graph.edge_count() << '\n'
              << "Minimum overlap: " << minimum_overlap << " bytes\n"
              << "Maximum mismatches: " << max_mismatches << '\n';

    if (beam_result.has_value()) {
        const auto displayed = std::min(candidate_count, beam_result->candidates.size());
        for (std::size_t index = 0; index < displayed; ++index) {
            const auto& candidate = beam_result->candidates[index];
            std::cout << "\nCandidate #" << index + 1 << '\n'
                      << "Fragments used: " << candidate.steps.size() << " / "
                      << fragments.size() << '\n'
                      << "Total overlap: " << candidate.total_overlap_bytes << " bytes\n"
                      << "Approximate joins: " << candidate.approximate_joins << '\n'
                      << "Overlap mismatches: " << candidate.overlap_mismatches << '\n'
                      << "Recovered bytes: " << candidate.recovered_size << '\n'
                      << "Status: " << (candidate.complete ? "complete" : "incomplete") << '\n';
            if (candidate.evidence.format.has_value()) {
                for (const auto& fact : candidate.evidence.format->facts) {
                    std::cout << fact.name << ": " << fact.value << '\n';
                }
            }
        }
        std::cout << "\nSelected candidate: #1\n"
                  << "States expanded: " << beam_result->statistics.states_expanded << '\n'
                  << "States generated: " << beam_result->statistics.states_generated << '\n'
                  << "Maximum beam size: " << beam_result->statistics.maximum_beam_size << '\n'
                  << "Complete candidates found: "
                  << beam_result->statistics.complete_candidates_found << '\n';
    }

    std::cout << "Search time: " << search_time.count() << " us\n"
              << "\nStarting fragment:\n"
              << graph.nodes()[result.steps.front().node_id].path.filename().string() << '\n'
              << "\nReconstruction path:\n"
              << graph.nodes()[result.steps.front().node_id].path.filename().string() << '\n';

    for (std::size_t index = 1; index < result.steps.size(); ++index) {
        const auto& step = result.steps[index];
        std::cout << "  -> " << graph.nodes()[step.node_id].path.filename().string()
                  << "    overlap=" << step.overlap_from_previous
                  << " mismatches=" << step.mismatches
                  << " exact=" << (step.exact ? "yes" : "no") << '\n';
    }

    const auto unresolved = fragments.size() - result.steps.size();
    std::cout << "\nFragments used: " << result.steps.size() << " / " << fragments.size() << '\n'
              << "Recovered bytes: " << result.bytes.size() << '\n'
              << "Overlap bytes removed: " << result.total_overlap_bytes << '\n'
              << "Approximate joins used: " << result.approximate_joins << '\n'
              << "Overlap mismatches observed: " << result.overlap_mismatches << '\n'
              << "Status: " << (result.complete ? "complete" : "incomplete") << '\n';
    if (!result.complete) {
        std::cout << "Unresolved fragments: " << unresolved << '\n'
                  << "Exit status: 2 (partial output written)\n";
    }
    if (repair_result.has_value()) {
        std::cout << "\nConsensus stage:\n"
                  << "Positions with redundant coverage: "
                  << repair_result->redundant_positions << '\n'
                  << "Conflicting positions: " << repair_result->conflicting_positions << '\n'
                  << "Corroborated positions: " << repair_result->corroborated_positions << '\n'
                  << "Bytes repaired: " << repair_result->repairs.size() << '\n'
                  << "Ambiguous conflicts: " << repair_result->ambiguities.size() << '\n';
        if (png_repair_result.has_value()) {
            const auto changed = static_cast<std::size_t>(std::count_if(
                png_repair_result->repairs.begin(), png_repair_result->repairs.end(),
                [](const auto& repair) { return repair.changed; }));
            std::cout << "\nPNG CRC stage:\n"
                      << "Eligible ambiguous bytes: "
                      << png_repair_result->eligible_ambiguous_bytes << '\n'
                      << "Chunks evaluated: " << png_repair_result->chunks_evaluated << '\n'
                      << "CRC-consistent resolutions: "
                      << png_repair_result->repairs.size() << '\n'
                      << "Bytes changed: " << changed << '\n'
                      << "Still unresolved: " << png_repair_result->unresolved.size() << '\n'
                      << "PNG validation before repair: CRC valid "
                      << png_repair_result->before.valid_crc_count << " / "
                      << png_repair_result->before.parsed_chunks << '\n'
                      << "PNG validation after repair: CRC valid "
                      << png_repair_result->after.valid_crc_count << " / "
                      << png_repair_result->after.parsed_chunks << '\n'
                      << "PNG structure after repair: "
                      << (png_repair_result->after.structurally_valid ? "valid" : "invalid")
                      << '\n';
        }
        if (has_repair_report) {
            std::cout << "Repair report: " << repair_report_path.string() << '\n';
        }
    }
    std::cout << "Output: " << output_path.string() << '\n';
    return result.complete ? 0 : 2;
}

}  // namespace shardrecover::cli
