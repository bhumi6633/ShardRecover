#include "shardrecover/fragment_generator.hpp"

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

std::vector<std::byte> make_bytes(std::size_t size)
{
    std::vector<std::byte> bytes;
    bytes.reserve(size);
    for (std::size_t index = 0; index < size; ++index) {
        bytes.push_back(static_cast<std::byte>(index % 256));
    }
    return bytes;
}

void check_fragments(const std::vector<std::byte>& input,
                     std::size_t fragment_size,
                     std::size_t overlap,
                     const std::vector<std::size_t>& expected_offsets,
                     const std::vector<std::size_t>& expected_sizes)
{
    const auto fragments = shardrecover::FragmentGenerator::generate(input, fragment_size, overlap);
    check(fragments.size() == expected_sizes.size(), "fragment count was incorrect");
    check(fragments.size() == expected_offsets.size(), "expected offset count was incorrect");

    const std::span<const std::byte> input_bytes{input};
    const auto stride = fragment_size - overlap;

    for (std::size_t index = 0; index < fragments.size(); ++index) {
        const auto& fragment = fragments[index];
        check(fragment.index == index, "fragment index was not sequential");
        check(fragment.offset == expected_offsets[index], "fragment offset was incorrect");
        check(fragment.data.size() == expected_sizes[index], "fragment size was incorrect");

        const auto expected_data = input_bytes.subspan(fragment.offset, fragment.data.size());
        check(std::equal(fragment.data.begin(), fragment.data.end(), expected_data.begin()),
              "fragment payload did not match its original byte range");

        if (index > 0) {
            check(fragment.offset - fragments[index - 1].offset == stride,
                  "adjacent fragment offsets did not match the stride");

            if (overlap > 0) {
                const std::span<const std::byte> previous_data{fragments[index - 1].data};
                const std::span<const std::byte> current_data{fragment.data};
                check(std::equal(previous_data.last(overlap).begin(),
                                 previous_data.last(overlap).end(),
                                 current_data.first(overlap).begin()),
                      "adjacent fragments did not share the expected overlap bytes");
            }
        }
    }

    if (!fragments.empty()) {
        const auto& last = fragments.back();
        check(last.offset + last.data.size() == input.size(),
              "final fragment did not end at the end of the input");
    }
}

void test_zero_overlap_backward_compatibility()
{
    const auto input = make_bytes(16);
    check_fragments(input, 4, 0, {0, 4, 8, 12}, {4, 4, 4, 4});

    const auto default_fragments = shardrecover::FragmentGenerator::generate(input, 4);
    const auto explicit_fragments = shardrecover::FragmentGenerator::generate(input, 4, 0);
    check(default_fragments.size() == explicit_fragments.size(),
          "default and explicit zero overlap produced different fragment counts");
    for (std::size_t index = 0; index < default_fragments.size(); ++index) {
        check(default_fragments[index].index == explicit_fragments[index].index
                  && default_fragments[index].offset == explicit_fragments[index].offset
                  && default_fragments[index].data == explicit_fragments[index].data,
              "default and explicit zero overlap produced different fragments");
    }
}

void test_partial_final_fragment()
{
    check_fragments(make_bytes(11), 8, 4, {0, 4}, {8, 7});
}

void test_fragment_larger_than_input()
{
    check_fragments(make_bytes(5), 100, 20, {0}, {5});
}

void test_standard_overlap()
{
    check_fragments(make_bytes(20), 8, 4, {0, 4, 8, 12}, {8, 8, 8, 8});
}

void test_large_valid_overlap()
{
    check_fragments(make_bytes(10), 8, 7, {0, 1, 2}, {8, 8, 8});
}

void test_fragment_size_one()
{
    check_fragments(make_bytes(6), 1, 0, {0, 1, 2, 3, 4, 5}, {1, 1, 1, 1, 1, 1});
}

void test_zero_fragment_size()
{
    try {
        static_cast<void>(shardrecover::FragmentGenerator::generate(make_bytes(4), 0));
    } catch (const std::invalid_argument&) {
        return;
    }

    throw std::runtime_error("zero fragment size was not rejected with std::invalid_argument");
}

void test_overlap_equal_to_size()
{
    try {
        static_cast<void>(shardrecover::FragmentGenerator::generate(make_bytes(8), 8, 8));
    } catch (const std::invalid_argument&) {
        return;
    }

    throw std::runtime_error("overlap equal to fragment size was not rejected");
}

void test_overlap_greater_than_size()
{
    try {
        static_cast<void>(shardrecover::FragmentGenerator::generate(make_bytes(8), 8, 9));
    } catch (const std::invalid_argument&) {
        return;
    }

    throw std::runtime_error("overlap greater than fragment size was not rejected");
}

void test_empty_input()
{
    const std::vector<std::byte> input;
    const auto fragments = shardrecover::FragmentGenerator::generate(input, 8, 4);
    check(fragments.empty(), "empty input should produce zero fragments");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

}  // namespace

int main()
{
    const std::array tests{
        TestCase{"zero-overlap backward compatibility", test_zero_overlap_backward_compatibility},
        TestCase{"standard overlap", test_standard_overlap},
        TestCase{"large valid overlap", test_large_valid_overlap},
        TestCase{"partial final fragment", test_partial_final_fragment},
        TestCase{"fragment larger than input", test_fragment_larger_than_input},
        TestCase{"fragment size one", test_fragment_size_one},
        TestCase{"zero fragment size", test_zero_fragment_size},
        TestCase{"overlap equal to size", test_overlap_equal_to_size},
        TestCase{"overlap greater than size", test_overlap_greater_than_size},
        TestCase{"empty input", test_empty_input},
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
