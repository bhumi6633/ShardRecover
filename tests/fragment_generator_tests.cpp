#include "shardrecover/fragment_generator.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
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
                     const std::vector<std::size_t>& expected_sizes)
{
    const auto fragments = shardrecover::FragmentGenerator::generate(input, fragment_size);
    check(fragments.size() == expected_sizes.size(), "fragment count was incorrect");

    std::size_t expected_offset = 0;
    std::vector<std::byte> reconstructed;
    reconstructed.reserve(input.size());

    for (std::size_t index = 0; index < fragments.size(); ++index) {
        const auto& fragment = fragments[index];
        check(fragment.index == index, "fragment index was not sequential");
        check(fragment.offset == expected_offset, "fragment offset was incorrect");
        check(fragment.data.size() == expected_sizes[index], "fragment size was incorrect");

        reconstructed.insert(reconstructed.end(), fragment.data.begin(), fragment.data.end());
        expected_offset += fragment.data.size();
    }

    check(expected_offset == input.size(), "fragment offsets did not cover the input exactly");
    check(reconstructed == input, "fragment payloads did not preserve the exact input bytes");
}

void test_exact_division()
{
    check_fragments(make_bytes(16), 4, {4, 4, 4, 4});
}

void test_partial_final_fragment()
{
    check_fragments(make_bytes(10), 4, {4, 4, 2});
}

void test_fragment_larger_than_input()
{
    check_fragments(make_bytes(5), 100, {5});
}

void test_fragment_size_one()
{
    check_fragments(make_bytes(6), 1, {1, 1, 1, 1, 1, 1});
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

void test_empty_input()
{
    const std::vector<std::byte> input;
    const auto fragments = shardrecover::FragmentGenerator::generate(input, 4);
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
        TestCase{"exact division", test_exact_division},
        TestCase{"partial final fragment", test_partial_final_fragment},
        TestCase{"fragment larger than input", test_fragment_larger_than_input},
        TestCase{"fragment size one", test_fragment_size_one},
        TestCase{"zero fragment size", test_zero_fragment_size},
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
