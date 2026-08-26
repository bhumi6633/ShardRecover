#include "shardrecover/overlap.hpp"

#include <algorithm>

namespace shardrecover {

OverlapResult find_suffix_prefix_overlap(std::span<const std::byte> left,
                                         std::span<const std::byte> right) noexcept
{
    const auto maximum = std::min(left.size(), right.size());
    for (std::size_t length = maximum; length > 0; --length) {
        if (std::equal(left.last(length).begin(),
                       left.last(length).end(),
                       right.first(length).begin())) {
            return OverlapResult{length};
        }
    }

    return OverlapResult{0};
}

}  // namespace shardrecover
