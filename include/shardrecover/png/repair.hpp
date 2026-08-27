#pragma once

#include "shardrecover/repair.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace shardrecover::png {

struct ValidationSummary {
    bool signature_valid = false;
    bool structurally_valid = false;
    bool semantically_valid = false;
    std::size_t parsed_chunks = 0;
    std::size_t valid_crc_count = 0;
    std::size_t invalid_crc_count = 0;
    bool all_crcs_valid = false;
};

struct CrcGuidedRepair {
    std::size_t position;
    std::byte previous_value;
    std::byte crc_consistent_value;
    std::uint32_t stored_crc;
    std::uint32_t computed_crc;
    std::size_t candidates_tested;
    bool changed;
};

struct CrcGuidedUnresolved {
    std::size_t position;
    std::string reason;
};

struct CrcRepairResult {
    std::vector<std::byte> bytes;
    std::vector<CrcGuidedRepair> repairs;
    std::vector<CrcGuidedUnresolved> unresolved;
    std::size_t eligible_ambiguous_bytes = 0;
    std::size_t chunks_evaluated = 0;
    ValidationSummary before;
    ValidationSummary after;
};

class CrcGuidedRepairer {
public:
    // Only one ambiguous type/data byte per chunk is evaluated in this version.
    static CrcRepairResult repair(const shardrecover::RepairResult& consensus);
};

}  // namespace shardrecover::png
