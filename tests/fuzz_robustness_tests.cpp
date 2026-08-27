#include "shardrecover/overlap.hpp"
#include "shardrecover/png/analyzer.hpp"
#include "shardrecover/png/types.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void test_malformed_png_boundaries()
{
    const std::vector<std::byte> huge_length{
        std::byte{0x89}, std::byte{0x50}, std::byte{0x4e}, std::byte{0x47},
        std::byte{0x0d}, std::byte{0x0a}, std::byte{0x1a}, std::byte{0x0a},
        std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
        std::byte{'I'}, std::byte{'D'}, std::byte{'A'}, std::byte{'T'},
    };
    const auto result = shardrecover::png::Analyzer::analyze(huge_length);
    check(!result.parsing_completed && result.chunks.empty(),
          "maximum PNG length did not stop safely before a chunk view was created");

    auto truncated_crc = huge_length;
    truncated_crc[8] = std::byte{0};
    truncated_crc[9] = std::byte{0};
    truncated_crc[10] = std::byte{0};
    truncated_crc[11] = std::byte{1};
    truncated_crc.push_back(std::byte{0x42});
    const auto truncated = shardrecover::png::Analyzer::analyze(truncated_crc);
    check(!truncated.parsing_completed && truncated.chunks.empty(),
          "truncated PNG CRC produced a completed chunk");
}

void test_overlap_extremes()
{
    const std::vector<std::byte> one{std::byte{0xff}};
    const std::vector<std::byte> empty;
    check(shardrecover::find_suffix_prefix_overlap(empty, one).length == 0,
          "empty exact overlap was nonzero");
    check(shardrecover::find_tolerant_suffix_prefix_overlap(
              one, one, std::numeric_limits<std::size_t>::max(),
              std::numeric_limits<std::size_t>::max()).length == 0,
          "impossible extreme minimum overlap was accepted");
    const auto tolerant = shardrecover::find_tolerant_suffix_prefix_overlap(
        one, one, 1, std::numeric_limits<std::size_t>::max());
    check(tolerant.length == 1 && tolerant.matches == 1 && tolerant.mismatches == 0,
          "large mismatch budget changed a one-byte exact overlap");
}

}  // namespace

int main()
{
    try {
        test_malformed_png_boundaries();
        test_overlap_extremes();
        std::cout << "All fuzz robustness tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Fuzz robustness test failure: " << error.what() << '\n';
        return 1;
    }
}
