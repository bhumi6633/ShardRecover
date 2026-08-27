#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace shardrecover {

struct OverlapMismatch {
    std::size_t overlap_offset;
    std::byte left_byte;
    std::byte right_byte;
};

struct OverlapResult {
    std::size_t length = 0;
    std::size_t matches = 0;
    std::size_t mismatches = 0;
    bool exact = false;
    std::vector<OverlapMismatch> mismatch_details;
};

OverlapResult find_suffix_prefix_overlap(std::span<const std::byte> left,
                                         std::span<const std::byte> right) noexcept;

OverlapResult find_tolerant_suffix_prefix_overlap(std::span<const std::byte> left,
                                                  std::span<const std::byte> right,
                                                  std::size_t minimum_overlap,
                                                  std::size_t max_mismatches);

}  // namespace shardrecover
