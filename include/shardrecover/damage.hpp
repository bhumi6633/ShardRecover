#pragma once

#include "shardrecover/fragment_generator.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace shardrecover {

struct DamageConfig {
    std::size_t duplicate_count = 0;
    std::size_t noise_count = 0;
    std::size_t drop_count = 0;
    std::size_t corrupt_byte_count = 0;
    std::uint64_t seed = 0;
};

struct DuplicateRecord {
    std::size_t dataset_id;
    std::size_t source_fragment_id;
};

struct CorruptionRecord {
    std::size_t dataset_id;
    std::size_t byte_offset;
    std::byte original_value;
    std::byte corrupted_value;
};

struct DamageResult {
    std::vector<Fragment> fragments;
    std::size_t original_fragment_count = 0;
    std::vector<std::size_t> surviving_original_ids;
    std::vector<std::size_t> dropped_original_ids;
    std::vector<DuplicateRecord> duplicates;
    std::vector<std::size_t> noise_fragment_ids;
    std::vector<CorruptionRecord> corruptions;
};

class DamageSimulator {
public:
    static DamageResult apply(const std::vector<Fragment>& originals,
                              const DamageConfig& config);
};

}  // namespace shardrecover
