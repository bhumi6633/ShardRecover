#include "shardrecover/fragment_emitter.hpp"
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
#include <unordered_set>
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
        bytes.push_back(static_cast<std::byte>((index * 17U + 3U) % 256U));
    }
    return bytes;
}

bool same_fragment(const shardrecover::Fragment& left, const shardrecover::Fragment& right)
{
    return left.index == right.index && left.offset == right.offset && left.data == right.data;
}

void check_same_logical_set(const std::vector<shardrecover::Fragment>& expected,
                            const std::vector<shardrecover::Fragment>& actual)
{
    check(actual.size() == expected.size(), "shuffle changed the fragment count");
    std::vector<bool> seen(expected.size(), false);

    for (const auto& fragment : actual) {
        check(fragment.index < expected.size(), "shuffle produced an invalid fragment index");
        check(!seen[fragment.index], "shuffle duplicated a fragment");
        check(same_fragment(fragment, expected[fragment.index]),
              "shuffle changed fragment metadata or bytes");
        seen[fragment.index] = true;
    }

    check(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }),
          "shuffle lost a fragment");
}

std::vector<std::size_t> fragment_indices(const std::vector<shardrecover::Fragment>& fragments)
{
    std::vector<std::size_t> indices;
    indices.reserve(fragments.size());
    for (const auto& fragment : fragments) {
        indices.push_back(fragment.index);
    }
    return indices;
}

void test_shuffle_preserves_fragment_set()
{
    const auto input = make_bytes(80);
    const auto original = shardrecover::FragmentGenerator::generate(input, 8);
    auto shuffled = original;

    shardrecover::FragmentEmitter::shuffle(shuffled, 42);

    check_same_logical_set(original, shuffled);
    check(fragment_indices(shuffled) != fragment_indices(original),
          "seeded shuffle did not change emission order");
}

void test_deterministic_seed()
{
    const auto input = make_bytes(80);
    const auto original = shardrecover::FragmentGenerator::generate(input, 8, 4);
    auto first = original;
    auto second = original;

    shardrecover::FragmentEmitter::shuffle(first, 42);
    shardrecover::FragmentEmitter::shuffle(second, 42);
    const auto first_names = shardrecover::FragmentEmitter::opaque_filenames(first.size(), 42);
    const auto second_names = shardrecover::FragmentEmitter::opaque_filenames(second.size(), 42);

    check(first_names == second_names, "identical seeds produced different opaque filenames");
    check(first.size() == second.size(), "identical runs produced different fragment counts");
    for (std::size_t position = 0; position < first.size(); ++position) {
        check(same_fragment(first[position], second[position]),
              "identical seeds produced different emission ordering");
        check(first_names[position] == second_names[position],
              "identical seeds produced different output records");
    }
}

void test_different_seeds()
{
    const auto input = make_bytes(160);
    const auto original = shardrecover::FragmentGenerator::generate(input, 8, 4);
    auto seed_42 = original;
    auto seed_99 = original;

    shardrecover::FragmentEmitter::shuffle(seed_42, 42);
    shardrecover::FragmentEmitter::shuffle(seed_99, 99);
    const auto names_42 = shardrecover::FragmentEmitter::opaque_filenames(original.size(), 42);
    const auto names_99 = shardrecover::FragmentEmitter::opaque_filenames(original.size(), 99);

    check(fragment_indices(seed_42) != fragment_indices(seed_99) || names_42 != names_99,
          "fixed different seeds produced identical randomized output");
}

void test_opaque_filename_properties()
{
    const auto names = shardrecover::FragmentEmitter::opaque_filenames(1000, 42);
    const std::unordered_set<std::string> unique_names(names.begin(), names.end());
    check(unique_names.size() == names.size(), "opaque filenames were not unique");

    for (const auto& name : names) {
        check(name.starts_with("shard_") && name.ends_with(".bin"),
              "opaque filename did not use the safe shard format");
        check(name.size() == 26, "opaque filename did not contain a 16-digit identifier");
        check(std::all_of(name.begin() + 6, name.end() - 4, [](char character) {
                  return (character >= '0' && character <= '9')
                         || (character >= 'a' && character <= 'f');
              }),
              "opaque filename identifier was not lowercase hexadecimal");
        check(!name.starts_with("fragment_"), "opaque filename exposed sequential naming");
    }
}

void test_unshuffled_legacy_order()
{
    const auto fragments = shardrecover::FragmentGenerator::generate(make_bytes(20), 8, 4);
    const std::array expected_offsets{std::size_t{0}, std::size_t{4}, std::size_t{8},
                                      std::size_t{12}};
    check(fragments.size() == expected_offsets.size(), "legacy fragment count changed");
    for (std::size_t index = 0; index < fragments.size(); ++index) {
        check(fragments[index].index == index, "legacy fragment order changed");
        check(fragments[index].offset == expected_offsets[index], "legacy offset order changed");
    }
}

void test_shuffle_with_overlap()
{
    const auto input = make_bytes(20);
    const auto original = shardrecover::FragmentGenerator::generate(input, 8, 4);
    auto shuffled = original;
    shardrecover::FragmentEmitter::shuffle(shuffled, 42);

    check_same_logical_set(original, shuffled);
    const std::span<const std::byte> input_bytes{input};
    for (const auto& fragment : shuffled) {
        const auto expected = input_bytes.subspan(fragment.offset, fragment.data.size());
        check(std::equal(fragment.data.begin(), fragment.data.end(), expected.begin()),
              "shuffled overlap fragment no longer matched its original range");
    }

    for (std::size_t index = 1; index < original.size(); ++index) {
        const std::span<const std::byte> previous{original[index - 1].data};
        const std::span<const std::byte> current{original[index].data};
        check(std::equal(previous.last(4).begin(), previous.last(4).end(), current.first(4).begin()),
              "overlap relationship changed after enabling shuffle support");
    }
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

}  // namespace

int main()
{
    const std::array tests{
        TestCase{"shuffle preserves fragment set", test_shuffle_preserves_fragment_set},
        TestCase{"deterministic seed", test_deterministic_seed},
        TestCase{"different seeds", test_different_seeds},
        TestCase{"opaque filename properties", test_opaque_filename_properties},
        TestCase{"unshuffled legacy order", test_unshuffled_legacy_order},
        TestCase{"shuffle with overlap", test_shuffle_with_overlap},
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
