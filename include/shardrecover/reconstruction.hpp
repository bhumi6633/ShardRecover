#pragma once

#include "shardrecover/binary_file.hpp"
#include "shardrecover/fragment_graph.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace shardrecover {

struct ReconstructionStep {
    std::size_t node_id;
    std::size_t overlap_from_previous;
    std::size_t matches = 0;
    std::size_t mismatches = 0;
    bool exact = true;
    std::vector<OverlapMismatch> mismatch_details;
};

struct ReconstructionResult {
    std::vector<ReconstructionStep> steps;
    std::vector<std::byte> bytes;
    std::size_t total_overlap_bytes = 0;
    std::size_t approximate_joins = 0;
    std::size_t overlap_mismatches = 0;
    bool complete = false;
};

struct EvidenceFact {
    std::string name;
    std::string value;
};

struct FormatEvidence {
    std::string format;
    std::vector<std::int64_t> ranking_keys;
    std::vector<EvidenceFact> facts;
};

class CandidateEvaluator {
public:
    virtual ~CandidateEvaluator() = default;
    // Beam search evaluates terminal candidates only; partial-state pruning remains generic.
    virtual FormatEvidence evaluate(std::span<const std::byte> candidate) const = 0;
};

struct ReconstructionEvidence {
    bool complete = false;
    std::size_t fragments_used = 0;
    std::size_t total_overlap = 0;
    std::optional<FormatEvidence> format;
};

struct ReconstructionCandidateResult {
    std::vector<ReconstructionStep> steps;
    std::size_t total_overlap_bytes = 0;
    std::size_t approximate_joins = 0;
    std::size_t overlap_mismatches = 0;
    std::size_t recovered_size = 0;
    bool complete = false;
    ReconstructionEvidence evidence;
};

struct BeamSearchStatistics {
    std::size_t states_expanded = 0;
    std::size_t states_generated = 0;
    std::size_t maximum_beam_size = 0;
    std::size_t complete_candidates_found = 0;
};

struct BeamReconstructionResult {
    ReconstructionResult selected;
    std::vector<ReconstructionCandidateResult> candidates;
    BeamSearchStatistics statistics;
};

class GreedyReconstructor {
public:
    static ReconstructionResult reconstruct(const FragmentGraph& graph,
                                            std::span<const BinaryFile> fragments);
};

class BeamReconstructor {
public:
    static BeamReconstructionResult reconstruct(const FragmentGraph& graph,
                                                std::span<const BinaryFile> fragments,
                                                std::size_t beam_width,
                                                const CandidateEvaluator* evaluator = nullptr);
};

}  // namespace shardrecover
