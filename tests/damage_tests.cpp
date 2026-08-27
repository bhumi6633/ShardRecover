#include "shardrecover/damage.hpp"
#include "shardrecover/fragment_emitter.hpp"
#include "shardrecover/fragment_generator.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using shardrecover::CorruptionRecord;
using shardrecover::DamageConfig;
using shardrecover::DamageResult;
using shardrecover::DamageSimulator;
using shardrecover::Fragment;

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::vector<std::byte> sample_bytes(std::size_t count = 128)
{
    std::vector<std::byte> bytes;
    bytes.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        bytes.push_back(static_cast<std::byte>((index * 73U + 19U) & 0xffU));
    }
    return bytes;
}

std::vector<Fragment> originals(std::size_t size = 16, std::size_t overlap = 4)
{
    const auto bytes = sample_bytes();
    return shardrecover::FragmentGenerator::generate(bytes, size, overlap);
}

bool same_fragment(const Fragment& left, const Fragment& right)
{
    return left.index == right.index && left.offset == right.offset && left.data == right.data;
}

bool same_fragments(const std::vector<Fragment>& left, const std::vector<Fragment>& right)
{
    return left.size() == right.size()
           && std::equal(left.begin(), left.end(), right.begin(), same_fragment);
}

bool same_corruption(const CorruptionRecord& left, const CorruptionRecord& right)
{
    return left.dataset_id == right.dataset_id && left.byte_offset == right.byte_offset
           && left.original_value == right.original_value
           && left.corrupted_value == right.corrupted_value;
}

bool same_result(const DamageResult& left, const DamageResult& right)
{
    if (!same_fragments(left.fragments, right.fragments)
        || left.original_fragment_count != right.original_fragment_count
        || left.surviving_original_ids != right.surviving_original_ids
        || left.dropped_original_ids != right.dropped_original_ids
        || left.noise_fragment_ids != right.noise_fragment_ids
        || left.duplicates.size() != right.duplicates.size()
        || left.corruptions.size() != right.corruptions.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.duplicates.size(); ++index) {
        if (left.duplicates[index].dataset_id != right.duplicates[index].dataset_id
            || left.duplicates[index].source_fragment_id
                   != right.duplicates[index].source_fragment_id) {
            return false;
        }
    }
    return std::equal(left.corruptions.begin(), left.corruptions.end(),
                      right.corruptions.begin(), same_corruption);
}

const Fragment& fragment_with_id(const std::vector<Fragment>& fragments, std::size_t id)
{
    const auto found = std::find_if(fragments.begin(), fragments.end(), [&](const auto& fragment) {
        return fragment.index == id;
    });
    if (found == fragments.end()) {
        throw std::runtime_error("Expected fragment identity was absent");
    }
    return *found;
}

void test_no_damage_backward_compatible()
{
    const auto clean = originals();
    const auto result = DamageSimulator::apply(clean, DamageConfig{.seed = 42});
    check(same_fragments(result.fragments, clean), "zero damage changed clean fragments");
    check(result.original_fragment_count == clean.size()
              && result.surviving_original_ids.size() == clean.size()
              && result.dropped_original_ids.empty() && result.duplicates.empty()
              && result.noise_fragment_ids.empty() && result.corruptions.empty(),
          "zero-damage ground truth was incorrect");
}

void test_duplicates_and_opaque_names()
{
    const auto clean = originals();
    const auto result = DamageSimulator::apply(
        clean, DamageConfig{.duplicate_count = 3, .seed = 42});
    check(result.fragments.size() == clean.size() + 3 && result.duplicates.size() == 3,
          "duplicate count was incorrect");

    std::unordered_set<std::size_t> identities;
    for (const auto& fragment : result.fragments) {
        check(identities.insert(fragment.index).second, "duplicate reused a dataset identity");
    }
    for (const auto& record : result.duplicates) {
        const auto& copy = fragment_with_id(result.fragments, record.dataset_id);
        const auto& source = fragment_with_id(clean, record.source_fragment_id);
        check(copy.data == source.data && copy.offset == source.offset,
              "duplicate bytes or source offset changed");
        check(copy.index != source.index, "duplicate identity matched its source");
    }

    const auto names = shardrecover::FragmentEmitter::opaque_filenames(result.fragments.size(), 42);
    std::unordered_set<std::string> unique_names(names.begin(), names.end());
    check(unique_names.size() == names.size(), "opaque duplicate names were not distinct");
    for (const auto& name : names) {
        check(name.starts_with("shard_") && name.ends_with(".bin"),
              "opaque name exposed a damage-specific label");
        check(name.find("copy") == std::string::npos
                  && name.find("duplicate") == std::string::npos,
              "opaque name revealed a duplicate relationship");
    }
}

void test_drop_and_invalid_drop()
{
    const auto clean = originals();
    const auto result = DamageSimulator::apply(clean, DamageConfig{.drop_count = 2, .seed = 42});
    check(result.fragments.size() == clean.size() - 2
              && result.dropped_original_ids.size() == 2,
          "drop count was incorrect");
    for (const auto id : result.dropped_original_ids) {
        check(std::none_of(result.fragments.begin(), result.fragments.end(),
                           [&](const auto& fragment) { return fragment.index == id; }),
              "dropped fragment remained in emitted dataset");
    }

    bool rejected = false;
    try {
        (void)DamageSimulator::apply(clean, DamageConfig{.drop_count = clean.size() + 1});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "excessive drop count was accepted");
}

void test_noise_generation()
{
    const auto clean = originals();
    const auto first = DamageSimulator::apply(clean, DamageConfig{.noise_count = 8, .seed = 42});
    const auto second = DamageSimulator::apply(clean, DamageConfig{.noise_count = 8, .seed = 42});
    check(first.noise_fragment_ids.size() == 8 && same_result(first, second),
          "noise count or fixed-seed contents were incorrect");

    const auto minimum_size = std::min_element(clean.begin(), clean.end(), [](const auto& left,
                                                                              const auto& right) {
        return left.data.size() < right.data.size();
    })->data.size();
    const auto maximum_size = std::max_element(clean.begin(), clean.end(), [](const auto& left,
                                                                              const auto& right) {
        return left.data.size() < right.data.size();
    })->data.size();
    bool saw_non_text_byte = false;
    for (const auto id : first.noise_fragment_ids) {
        const auto& noise = fragment_with_id(first.fragments, id);
        check(noise.data.size() >= minimum_size && noise.data.size() <= maximum_size,
              "noise size fell outside surviving original size range");
        saw_non_text_byte = saw_non_text_byte
                            || std::any_of(noise.data.begin(), noise.data.end(), [](auto value) {
                                   const auto byte = std::to_integer<unsigned int>(value);
                                   return byte == 0 || byte > 0x7fU;
                               });
    }
    check(saw_non_text_byte, "noise fixture did not exercise arbitrary binary bytes");
}

void test_corruption_and_metadata()
{
    const auto clean = originals();
    const auto one = DamageSimulator::apply(
        clean, DamageConfig{.corrupt_byte_count = 1, .seed = 42});
    check(one.corruptions.size() == 1, "single corruption count was incorrect");
    const auto& record = one.corruptions.front();
    const auto& before = fragment_with_id(clean, record.dataset_id);
    const auto& after = fragment_with_id(one.fragments, record.dataset_id);
    check(record.byte_offset < before.data.size()
              && before.data[record.byte_offset] == record.original_value
              && after.data[record.byte_offset] == record.corrupted_value
              && record.original_value != record.corrupted_value,
          "single corruption metadata did not match changed byte");

    const auto many = DamageSimulator::apply(
        clean, DamageConfig{.corrupt_byte_count = 25, .seed = 99});
    std::set<std::pair<std::size_t, std::size_t>> positions;
    for (const auto& corruption : many.corruptions) {
        check(positions.emplace(corruption.dataset_id, corruption.byte_offset).second,
              "one byte position was corrupted repeatedly");
        check(corruption.original_value != corruption.corrupted_value,
              "corruption retained the original byte value");
    }
    check(positions.size() == 25, "multiple corruption count was incorrect");

    std::size_t eligible_bytes = 0;
    for (const auto& fragment : clean) {
        eligible_bytes += fragment.data.size();
    }
    bool rejected = false;
    try {
        (void)DamageSimulator::apply(
            clean, DamageConfig{.corrupt_byte_count = eligible_bytes + 1});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "excessive corruption count was accepted");
}

void test_seed_reproducibility_and_difference()
{
    const auto clean = originals();
    const DamageConfig config{2, 3, 1, 9, 42};
    const auto first = DamageSimulator::apply(clean, config);
    const auto second = DamageSimulator::apply(clean, config);
    check(same_result(first, second), "identical seed did not reproduce damage ground truth");

    auto first_emission = first.fragments;
    auto second_emission = second.fragments;
    shardrecover::FragmentEmitter::shuffle(first_emission, config.seed);
    shardrecover::FragmentEmitter::shuffle(second_emission, config.seed);
    const auto first_names = shardrecover::FragmentEmitter::opaque_filenames(first_emission.size(),
                                                                              config.seed);
    const auto second_names = shardrecover::FragmentEmitter::opaque_filenames(second_emission.size(),
                                                                               config.seed);
    check(same_fragments(first_emission, second_emission) && first_names == second_names,
          "shuffle or opaque emission was not reproducible");

    const auto different = DamageSimulator::apply(clean, DamageConfig{2, 3, 1, 9, 99});
    check(!same_result(first, different), "different seeds produced identical combined damage");
}

void test_combined_overlap_damage()
{
    const auto input = sample_bytes(100);
    const auto clean = shardrecover::FragmentGenerator::generate(input, 16, 4);
    for (std::size_t index = 0; index < clean.size(); ++index) {
        check(clean[index].offset == index * 12, "overlap stride changed before damage");
        check(clean[index].data == std::vector<std::byte>(
                                       input.begin() + static_cast<std::ptrdiff_t>(clean[index].offset),
                                       input.begin() + static_cast<std::ptrdiff_t>(
                                           clean[index].offset + clean[index].data.size())),
              "clean overlap bytes changed before damage");
    }

    const auto damaged = DamageSimulator::apply(clean, DamageConfig{2, 3, 1, 5, 42});
    check(damaged.original_fragment_count == clean.size()
              && damaged.dropped_original_ids.size() == 1
              && damaged.duplicates.size() == 2
              && damaged.noise_fragment_ids.size() == 3
              && damaged.corruptions.size() == 5
              && damaged.fragments.size() == clean.size() - 1 + 2 + 3,
          "combined damage invariants were incorrect");

    auto emitted = damaged.fragments;
    shardrecover::FragmentEmitter::shuffle(emitted, 42);
    const auto names = shardrecover::FragmentEmitter::opaque_filenames(emitted.size(), 42);
    check(std::unordered_set<std::string>(names.begin(), names.end()).size() == names.size(),
          "combined shuffle produced duplicate opaque names");
    for (std::size_t index = 0; index < emitted.size(); ++index) {
        check(!emitted[index].data.empty(), "raw emitted fragment unexpectedly contained no bytes");
        check(names[index].starts_with("shard_") && names[index].ends_with(".bin")
                  && names[index].find("noise") == std::string::npos
                  && names[index].find("drop") == std::string::npos,
              "opaque filename exposed damage ground truth");
    }
}

void test_empty_and_single_fragment_inputs()
{
    const std::vector<Fragment> empty;
    check(DamageSimulator::apply(empty, DamageConfig{}).fragments.empty(),
          "empty zero-damage input was unsafe");
    bool empty_noise_rejected = false;
    try {
        (void)DamageSimulator::apply(empty, DamageConfig{.noise_count = 1});
    } catch (const std::invalid_argument&) {
        empty_noise_rejected = true;
    }
    check(empty_noise_rejected, "noise on empty input was accepted without a size policy");

    const std::vector<Fragment> single{{0, 0, sample_bytes(8)}};
    const auto duplicate = DamageSimulator::apply(single, DamageConfig{.duplicate_count = 1,
                                                                        .corrupt_byte_count = 2,
                                                                        .seed = 42});
    check(duplicate.fragments.size() == 2 && duplicate.corruptions.size() == 2,
          "single-fragment duplication or corruption failed");
    check(DamageSimulator::apply(single, DamageConfig{.drop_count = 1}).fragments.empty(),
          "single fragment could not be dropped safely");
    bool rejected = false;
    try {
        (void)DamageSimulator::apply(single, DamageConfig{.duplicate_count = 1,
                                                          .drop_count = 1});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "duplication after dropping the only fragment was accepted");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

}  // namespace

int main()
{
    const std::array tests{
        TestCase{"zero damage compatibility", test_no_damage_backward_compatible},
        TestCase{"duplicates and opaque names", test_duplicates_and_opaque_names},
        TestCase{"drop behavior and validation", test_drop_and_invalid_drop},
        TestCase{"noise generation", test_noise_generation},
        TestCase{"corruption and metadata", test_corruption_and_metadata},
        TestCase{"seed reproducibility", test_seed_reproducibility_and_difference},
        TestCase{"combined overlap damage", test_combined_overlap_damage},
        TestCase{"empty and single fragment inputs", test_empty_and_single_fragment_inputs},
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
        std::cerr << failures << " damage test group(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " damage test groups passed\n";
    return 0;
}
