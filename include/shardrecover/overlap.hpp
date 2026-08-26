#pragma once

#include <cstddef>
#include <span>

namespace shardrecover {

struct OverlapResult {
    std::size_t length;
};

OverlapResult find_suffix_prefix_overlap(std::span<const std::byte> left,
                                         std::span<const std::byte> right) noexcept;

}  // namespace shardrecover
