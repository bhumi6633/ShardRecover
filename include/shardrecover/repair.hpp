#pragma once

#include "shardrecover/binary_file.hpp"
#include "shardrecover/reconstruction.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace shardrecover {

struct ByteVote {
    std::byte value;
    std::size_t votes;
};

struct ByteRepair {
    std::size_t position;
    std::byte baseline_value;
    std::byte repaired_value;
    std::size_t supporting_votes;
    std::size_t total_observations;
};

struct AmbiguousByte {
    std::size_t position;
    std::byte preserved_value;
    std::size_t total_observations;
    std::vector<ByteVote> votes;
};

struct RepairResult {
    std::vector<std::byte> bytes;
    std::vector<ByteRepair> repairs;
    std::vector<AmbiguousByte> ambiguities;
    std::size_t redundant_positions = 0;
    std::size_t conflicting_positions = 0;
    std::size_t corroborated_positions = 0;
};

class ConsensusRepairer {
public:
    // Votes treat selected fragment instances independently; exact duplicates can bias consensus.
    static RepairResult repair(const ReconstructionResult& baseline,
                               std::span<const BinaryFile> fragments);
};

}  // namespace shardrecover
