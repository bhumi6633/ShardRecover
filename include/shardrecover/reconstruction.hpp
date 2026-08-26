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

class GreedyReconstructor {
public:
    static ReconstructionResult reconstruct(const FragmentGraph& graph,
                                            std::span<const BinaryFile> fragments);
};

}  // namespace shardrecover
