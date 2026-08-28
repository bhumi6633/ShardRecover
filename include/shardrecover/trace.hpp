#pragma once

#include "shardrecover/binary_file.hpp"
#include "shardrecover/fragment_graph.hpp"
#include "shardrecover/png/repair.hpp"
#include "shardrecover/png/types.hpp"
#include "shardrecover/repair.hpp"
#include "shardrecover/reconstruction.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace shardrecover::trace {

inline constexpr std::uint32_t schema_version = 1;

struct Configuration {
    std::size_t minimum_overlap = 1;
    std::size_t maximum_mismatches = 0;
    std::string graph_build = "exhaustive";
    std::size_t threads = 1;
    std::string reconstruction = "greedy";
    std::size_t beam_width = 0;
    std::string io = "buffered";
    std::string format = "none";
    std::string repair = "none";
};

struct Fragment { std::size_t id; std::string display_name; std::size_t size; };
struct Mismatch { std::size_t overlap_offset; std::uint8_t left_byte; std::uint8_t right_byte; };
struct Edge {
    std::size_t from;
    std::size_t to;
    std::size_t overlap;
    std::size_t matches;
    std::size_t mismatches;
    bool exact;
    std::vector<Mismatch> mismatch_details;
};
struct SelectedMismatch {
    std::size_t from;
    std::size_t to;
    std::size_t overlap_offset;
    std::uint8_t left_byte;
    std::uint8_t right_byte;
};
struct Repair {
    std::size_t position;
    std::string stage;
    std::uint8_t before;
    std::uint8_t after;
    std::size_t support = 0;
    std::size_t observations = 0;
    std::size_t candidates_tested = 0;
    bool crc_consistent = false;
};
struct FormatChunk {
    std::size_t index;
    std::size_t offset;
    std::string type;
    std::uint32_t length;
    bool crc_valid;
};
struct FormatEvidence {
    std::string type = "none";
    bool signature_valid = false;
    bool structurally_valid = false;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::size_t valid_crc_count = 0;
    std::size_t invalid_crc_count = 0;
    bool all_crc_valid = false;
    std::vector<FormatChunk> chunks;
};
struct ReconstructionSummary {
    std::string strategy;
    bool complete;
    std::size_t fragments_used;
    std::size_t fragments_total;
    std::size_t output_bytes;
    std::size_t approximate_joins;
    std::size_t overlap_mismatches;
};

struct ReconstructionTrace {
    std::uint32_t version = schema_version;
    std::string engine_name = "ShardRecover";
    std::string engine_version = "0.1.0";
    Configuration configuration;
    std::vector<Fragment> fragments;
    std::vector<Edge> edges;
    std::vector<std::size_t> selected_path;
    ReconstructionSummary reconstruction;
    std::vector<SelectedMismatch> mismatches;
    std::vector<Repair> repairs;
    std::size_t unresolved_ambiguities = 0;
    FormatEvidence format;
    GraphBuildStats graph_stats;
};

class Builder {
public:
    static ReconstructionTrace build(std::span<const BinaryFile> fragments,
                                     const FragmentGraph& graph,
                                     const ReconstructionResult& result,
                                     const Configuration& configuration,
                                     const GraphBuildStats& graph_stats,
                                     const RepairResult* consensus = nullptr,
                                     const png::CrcRepairResult* png_repairs = nullptr,
                                     const png::AnalysisResult* png_analysis = nullptr);
};

void write_json(const ReconstructionTrace& trace, std::ostream& output);
void write_json_file(const ReconstructionTrace& trace, const std::filesystem::path& path);

}  // namespace shardrecover::trace
