#include "shardrecover/overlap.hpp"

#include <algorithm>
#include <stdexcept>

namespace shardrecover {

OverlapResult find_suffix_prefix_overlap(std::span<const std::byte> left,
                                         std::span<const std::byte> right) noexcept
{
    const auto maximum = std::min(left.size(), right.size());
    for (std::size_t length = maximum; length > 0; --length) {
        if (std::equal(left.last(length).begin(),
                       left.last(length).end(),
                       right.first(length).begin())) {
            return OverlapResult{length, length, 0, true, {}};
        }
    }

    return {};
}

OverlapResult find_tolerant_suffix_prefix_overlap(std::span<const std::byte> left,
                                                  std::span<const std::byte> right,
                                                  std::size_t minimum_overlap,
                                                  std::size_t max_mismatches)
{
    if (minimum_overlap == 0) {
        throw std::invalid_argument("Minimum overlap must be greater than zero");
    }

    const auto exact = find_suffix_prefix_overlap(left, right);
    if (exact.length >= minimum_overlap || max_mismatches == 0) {
        return exact.length >= minimum_overlap ? exact : OverlapResult{};
    }

    const auto maximum = std::min(left.size(), right.size());
    if (maximum < minimum_overlap) {
        return {};
    }
    for (std::size_t length = maximum;; --length) {
        OverlapResult candidate;
        candidate.length = length;
        candidate.exact = true;
        candidate.mismatch_details.reserve(std::min(length, max_mismatches));
        const auto suffix = left.last(length);
        const auto prefix = right.first(length);
        for (std::size_t offset = 0; offset < length; ++offset) {
            if (suffix[offset] == prefix[offset]) {
                ++candidate.matches;
                continue;
            }
            ++candidate.mismatches;
            candidate.exact = false;
            if (candidate.mismatches > max_mismatches) {
                break;
            }
            candidate.mismatch_details.push_back(
                OverlapMismatch{offset, suffix[offset], prefix[offset]});
        }
        if (candidate.mismatches <= max_mismatches) {
            return candidate;
        }
        if (length == minimum_overlap) {
            break;
        }
    }
    return {};
}

}  // namespace shardrecover
