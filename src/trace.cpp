#include "shardrecover/trace.hpp"

#include <array>
#include <stdexcept>

namespace shardrecover::trace {
namespace {
std::uint8_t byte_value(std::byte value) { return std::to_integer<std::uint8_t>(value); }
std::string chunk_type(const std::array<char, 4>& type) { return {type.data(), type.size()}; }
}  // namespace

ReconstructionTrace Builder::build(std::span<const BinaryFile> fragments,
                                   const FragmentGraph& graph,
                                   const ReconstructionResult& result,
                                   const Configuration& configuration,
                                   const GraphBuildStats& graph_stats,
                                   const RepairResult* consensus,
                                   const png::CrcRepairResult* png_repairs,
                                   const png::AnalysisResult* png_analysis)
{
    if (fragments.size() != graph.node_count()) {
        throw std::invalid_argument("Trace fragments and graph nodes do not match");
    }
    ReconstructionTrace trace;
    trace.configuration = configuration;
    trace.graph_stats = graph_stats;
    trace.reconstruction = {configuration.reconstruction, result.complete, result.steps.size(),
                            fragments.size(), result.bytes.size(), result.approximate_joins,
                            result.overlap_mismatches};
    for (const auto& node : graph.nodes()) {
        trace.fragments.push_back({node.id, node.path.filename().string(), node.size});
    }
    for (const auto& edge : graph.edges()) {
        Edge item{edge.from, edge.to, edge.overlap, edge.matches, edge.mismatches, edge.exact, {}};
        for (const auto& mismatch : edge.mismatch_details) {
            item.mismatch_details.push_back({mismatch.overlap_offset,
                                             byte_value(mismatch.left_byte),
                                             byte_value(mismatch.right_byte)});
        }
        trace.edges.push_back(std::move(item));
    }
    for (std::size_t index = 0; index < result.steps.size(); ++index) {
        const auto& step = result.steps[index];
        trace.selected_path.push_back(step.node_id);
        if (index == 0) continue;
        const auto from = result.steps[index - 1].node_id;
        for (const auto& mismatch : step.mismatch_details) {
            trace.mismatches.push_back({from, step.node_id, mismatch.overlap_offset,
                                        byte_value(mismatch.left_byte),
                                        byte_value(mismatch.right_byte)});
        }
    }
    if (consensus != nullptr) {
        trace.unresolved_ambiguities = consensus->ambiguities.size();
        for (const auto& repair : consensus->repairs) {
            trace.repairs.push_back({repair.position, "consensus",
                                     byte_value(repair.baseline_value),
                                     byte_value(repair.repaired_value), repair.supporting_votes,
                                     repair.total_observations, 0, false});
        }
    }
    if (png_repairs != nullptr) {
        trace.unresolved_ambiguities = png_repairs->unresolved.size();
        for (const auto& repair : png_repairs->repairs) {
            trace.repairs.push_back({repair.position, "png_crc",
                                     byte_value(repair.previous_value),
                                     byte_value(repair.crc_consistent_value), 0, 0,
                                     repair.candidates_tested, true});
        }
    }
    if (png_analysis != nullptr) {
        trace.format.type = "png";
        trace.format.signature_valid = png_analysis->signature_valid;
        trace.format.structurally_valid = png_analysis->structurally_valid;
        trace.format.valid_crc_count = png_analysis->valid_crc_count;
        trace.format.invalid_crc_count = png_analysis->invalid_crc_count;
        trace.format.all_crc_valid = png_analysis->all_crcs_valid;
        if (png_analysis->ihdr) {
            trace.format.width = png_analysis->ihdr->width;
            trace.format.height = png_analysis->ihdr->height;
        }
        for (std::size_t index = 0; index < png_analysis->chunks.size(); ++index) {
            const auto& chunk = png_analysis->chunks[index];
            trace.format.chunks.push_back(
                {index, chunk.offset, chunk_type(chunk.type), chunk.length, chunk.crc_valid});
        }
    }
    return trace;
}

}  // namespace shardrecover::trace
