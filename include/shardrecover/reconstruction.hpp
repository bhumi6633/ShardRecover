#pragma once

#include "shardrecover/binary_file.hpp"
#include "shardrecover/fragment_graph.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace shardrecover {

struct ReconstructionStep {
    std::size_t node_id;
    std::size_t overlap_from_previous;
};

struct ReconstructionResult {
    std::vector<ReconstructionStep> steps;
    std::vector<std::byte> bytes;
    std::size_t total_overlap_bytes = 0;
    bool complete = false;
};

struct ReconstructionCandidateResult {
    std::vector<ReconstructionStep> steps;
    std::size_t total_overlap_bytes = 0;
    std::size_t recovered_size = 0;
    bool complete = false;
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
                                                std::size_t beam_width);
};

}  // namespace shardrecover
