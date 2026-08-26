#include "shardrecover/fragment_emitter.hpp"
#include "shardrecover/fragment_generator.hpp"
#include "shardrecover/overlap.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
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

std::vector<std::byte> bytes(std::string_view text)
{
    std::vector<std::byte> result;
    result.reserve(text.size());
    for (const auto character : text) {
        result.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return result;
}

std::size_t overlap(std::span<const std::byte> left, std::span<const std::byte> right)
{
    return shardrecover::find_suffix_prefix_overlap(left, right).length;
}

void test_basic_overlap()
{
    check(overlap(bytes("ABCDEF"), bytes("DEFGHI")) == 3, "basic overlap was not 3");
}

void test_no_overlap()
{
    check(overlap(bytes("ABC"), bytes("XYZ")) == 0, "unrelated fragments overlapped");
}

void test_complete_equality()
{
    check(overlap(bytes("ABCDEF"), bytes("ABCDEF")) == 6,
          "equal fragments did not overlap completely");
}

void test_right_is_suffix_of_left()
{
    check(overlap(bytes("ABCDEF"), bytes("EF")) == 2,
          "right suffix did not overlap completely");
}

void test_left_smaller_than_right()
{
    check(overlap(bytes("ABC"), bytes("ABCDEF")) == 3,
          "overlap was not bounded by the smaller left fragment");
}

void test_one_byte_overlap()
{
    check(overlap(bytes("ABC"), bytes("CDE")) == 1, "one-byte overlap was not detected");
}

void test_direction_matters()
{
    const auto left = bytes("ABCDEF");
    const auto right = bytes("DEFGHI");
    check(overlap(left, right) == 3, "forward overlap was incorrect");
    check(overlap(right, left) == 0, "reverse overlap should have been zero");
}

void test_empty_inputs()
{
    const std::vector<std::byte> empty;
    const auto data = bytes("ABC");
    check(overlap(empty, data) == 0, "empty left fragment was unsafe");
    check(overlap(data, empty) == 0, "empty right fragment was unsafe");
    check(overlap(empty, empty) == 0, "two empty fragments were unsafe");
}

void test_binary_zero_bytes()
{
    const std::vector<std::byte> left{
        std::byte{0xaa}, std::byte{0x00}, std::byte{0xff}, std::byte{0x13}, std::byte{0x7a},
    };
    const std::vector<std::byte> right{
        std::byte{0x00}, std::byte{0xff}, std::byte{0x13}, std::byte{0x7a}, std::byte{0x55},
    };
    check(overlap(left, right) == 4, "binary overlap containing zero bytes was incorrect");
}

void test_long_exact_overlap()
{
    constexpr std::size_t overlap_size = 4096;
    std::vector<std::byte> shared;
    shared.reserve(overlap_size);
    for (std::size_t index = 0; index < overlap_size; ++index) {
        shared.push_back(static_cast<std::byte>(index % 251U + 1U));
    }

    std::vector<std::byte> left(128, std::byte{0x00});
    left.insert(left.end(), shared.begin(), shared.end());
    auto right = shared;
    right.insert(right.end(), {std::byte{0xfe}, std::byte{0xfd}});

    check(overlap(left, right) == overlap_size, "long exact overlap was incorrect");
}

void test_false_partial_match()
{
    check(overlap(bytes("XXABCYABC"), bytes("ABCZ")) == 3,
          "detector accepted a non-exact larger partial match");
}

void test_generated_fragment_property()
{
    std::vector<std::byte> input;
    for (std::size_t value = 0; value < 20; ++value) {
        input.push_back(static_cast<std::byte>(value));
    }

    const auto fragments = shardrecover::FragmentGenerator::generate(input, 8, 4);
    check(fragments.size() == 4, "generated fixture had an unexpected fragment count");
    for (std::size_t index = 1; index < fragments.size(); ++index) {
        check(overlap(fragments[index - 1].data, fragments[index].data) == 4,
              "detector did not rediscover generated adjacent overlap");
    }
}

void test_shuffled_opaque_compatibility()
{
    std::vector<std::byte> input;
    for (std::size_t value = 0; value < 40; ++value) {
        input.push_back(static_cast<std::byte>(value));
    }

    auto fragments = shardrecover::FragmentGenerator::generate(input, 8, 4);
    shardrecover::FragmentEmitter::shuffle(fragments, 42);
    const auto filenames = shardrecover::FragmentEmitter::opaque_filenames(fragments.size(), 42);
    check(filenames.size() == fragments.size(), "opaque fixture naming failed");

    const auto first = std::find_if(fragments.begin(), fragments.end(), [](const auto& fragment) {
        return fragment.index == 0;
    });
    const auto second = std::find_if(fragments.begin(), fragments.end(), [](const auto& fragment) {
        return fragment.index == 1;
    });
    check(first != fragments.end() && second != fragments.end(), "shuffled fixture lost fragments");

    check(overlap(first->data, second->data) == 4,
          "raw-byte overlap failed after shuffle and opaque naming");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

}  // namespace

int main()
{
    const std::array tests{
        TestCase{"basic overlap", test_basic_overlap},
        TestCase{"no overlap", test_no_overlap},
        TestCase{"complete equality", test_complete_equality},
        TestCase{"right is suffix of left", test_right_is_suffix_of_left},
        TestCase{"left smaller than right", test_left_smaller_than_right},
        TestCase{"one-byte overlap", test_one_byte_overlap},
        TestCase{"direction matters", test_direction_matters},
        TestCase{"empty inputs", test_empty_inputs},
        TestCase{"binary zero bytes", test_binary_zero_bytes},
        TestCase{"long exact overlap", test_long_exact_overlap},
        TestCase{"false partial match", test_false_partial_match},
        TestCase{"generated fragment property", test_generated_fragment_property},
        TestCase{"shuffled opaque compatibility", test_shuffled_opaque_compatibility},
    };

    std::size_t failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }

    if (failures != 0) {
        std::cerr << failures << " test case(s) failed\n";
        return 1;
    }

    std::cout << tests.size() << " test cases passed\n";
    return 0;
}
