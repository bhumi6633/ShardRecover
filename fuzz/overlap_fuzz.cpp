#include "shardrecover/overlap.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

void require(bool condition)
{
    if (!condition) {
        __builtin_trap();
    }
}

void validate(const shardrecover::OverlapResult& result,
              std::span<const std::byte> left,
              std::span<const std::byte> right)
{
    require(result.length <= left.size());
    require(result.length <= right.size());
    require(result.matches <= result.length);
    require(result.mismatches <= result.length);
    require(result.matches + result.mismatches == result.length);
    require(result.mismatch_details.size() == result.mismatches);
    if (result.exact) {
        require(result.mismatches == 0);
    }
    for (const auto& mismatch : result.mismatch_details) {
        require(mismatch.overlap_offset < result.length);
        require(mismatch.left_byte != mismatch.right_byte);
        require(mismatch.left_byte
                == left[left.size() - result.length + mismatch.overlap_offset]);
        require(mismatch.right_byte == right[mismatch.overlap_offset]);
    }
}

void evaluate_direction(std::span<const std::byte> left,
                        std::span<const std::byte> right,
                        std::size_t minimum_overlap,
                        std::size_t max_mismatches)
{
    const auto exact = shardrecover::find_suffix_prefix_overlap(left, right);
    const auto tolerant = shardrecover::find_tolerant_suffix_prefix_overlap(
        left, right, minimum_overlap, max_mismatches);
    validate(exact, left, right);
    validate(tolerant, left, right);
    if (max_mismatches == 0) {
        if (exact.length >= minimum_overlap) {
            require(tolerant.length == exact.length && tolerant.exact);
        } else {
            require(tolerant.length == 0);
        }
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    constexpr std::size_t maximum_input_size = 2U * 1024U * 1024U;
    if (size < 3 || size > maximum_input_size) {
        return 0;
    }

    const auto payload_size = size - 3;
    const auto split = static_cast<std::size_t>(data[0]) * (payload_size + 1) / 256;
    const auto payload = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(data + 3), payload_size};
    const auto left = payload.first(split);
    const auto right = payload.subspan(split);
    const auto maximum_overlap = std::min(left.size(), right.size());
    const auto minimum_overlap = maximum_overlap == 0
                                     ? std::size_t{1}
                                     : std::size_t{1} + data[1] % (maximum_overlap + 1);
    const auto max_mismatches = maximum_overlap == 0
                                    ? std::size_t{0}
                                    : static_cast<std::size_t>(data[2]) % (maximum_overlap + 1);

    evaluate_direction(left, right, minimum_overlap, max_mismatches);
    evaluate_direction(right, left, minimum_overlap, max_mismatches);
    return 0;
}
