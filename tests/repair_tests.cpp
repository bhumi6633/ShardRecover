#include "shardrecover/binary_file.hpp"
#include "shardrecover/damage.hpp"
#include "shardrecover/fragment_generator.hpp"
#include "shardrecover/fragment_graph.hpp"
#include "shardrecover/reconstruction.hpp"
#include "shardrecover/repair.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<std::byte>;
using shardrecover::BinaryFile;
using shardrecover::ConsensusRepairer;
using shardrecover::ReconstructionResult;
using shardrecover::ReconstructionStep;
using shardrecover::RepairResult;

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

Bytes text_bytes(std::string_view text)
{
    Bytes result;
    for (const auto character : text) {
        result.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return result;
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto base = std::filesystem::temp_directory_path()
                          / ("shardrecover-repair-tests-" + std::to_string(stamp));
        for (int suffix = 0; suffix < 100; ++suffix) {
            auto candidate = base;
            candidate += "-" + std::to_string(suffix);
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = std::move(candidate);
                return;
            }
            if (error) {
                throw std::runtime_error("Failed to create temporary directory: " + error.message());
            }
        }
        throw std::runtime_error("Failed to find temporary directory name");
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to create repair test file");
    }
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    output.close();
    if (!output) {
        throw std::runtime_error("Failed to write repair test file");
    }
}

std::vector<BinaryFile> load_fragments(const std::filesystem::path& directory,
                                       std::string_view prefix,
                                       const std::vector<Bytes>& bytes)
{
    std::vector<BinaryFile> files;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto path = directory / (std::string(prefix) + std::to_string(index) + ".bin");
        write_file(path, bytes[index]);
        files.push_back(BinaryFile::load(path));
    }
    return files;
}

ReconstructionResult baseline_for(std::span<const BinaryFile> fragments,
                                  std::span<const std::size_t> overlaps)
{
    check(fragments.size() == overlaps.size(), "test overlap count did not match fragments");
    ReconstructionResult baseline;
    baseline.complete = true;
    if (fragments.empty()) {
        return baseline;
    }
    baseline.bytes.assign(fragments[0].bytes().begin(), fragments[0].bytes().end());
    baseline.steps.push_back(ReconstructionStep{0, 0, 0, 0, true, {}});
    for (std::size_t index = 1; index < fragments.size(); ++index) {
        check(overlaps[index] <= fragments[index].size(), "test overlap exceeded fragment");
        baseline.steps.push_back(ReconstructionStep{index, overlaps[index], 0, 0, false, {}});
        const auto extension = fragments[index].bytes().subspan(overlaps[index]);
        baseline.bytes.insert(baseline.bytes.end(), extension.begin(), extension.end());
        baseline.total_overlap_bytes += overlaps[index];
    }
    return baseline;
}

RepairResult repair_fixture(const std::filesystem::path& directory,
                            std::string_view prefix,
                            const std::vector<Bytes>& bytes,
                            const std::vector<std::size_t>& overlaps,
                            ReconstructionResult* baseline_output = nullptr)
{
    const auto files = load_fragments(directory, prefix, bytes);
    const auto baseline = baseline_for(files, overlaps);
    if (baseline_output != nullptr) {
        *baseline_output = baseline;
    }
    return ConsensusRepairer::repair(baseline, files);
}

void test_majority_and_two_way_agreement(const std::filesystem::path& directory)
{
    ReconstructionResult baseline;
    const auto majority = repair_fixture(
        directory,
        "majority-",
        {{std::byte{0x91}}, {std::byte{0xcc}}, {std::byte{0xcc}}},
        {0, 1, 1},
        &baseline);
    check(baseline.bytes == Bytes{std::byte{0x91}}, "majority baseline fixture was wrong");
    check(majority.bytes == Bytes{std::byte{0xcc}} && majority.repairs.size() == 1,
          "three-way majority did not repair baseline");
    const auto& repair = majority.repairs.front();
    check(repair.position == 0 && repair.baseline_value == std::byte{0x91}
              && repair.repaired_value == std::byte{0xcc}
              && repair.supporting_votes == 2 && repair.total_observations == 3,
          "three-way repair details were incorrect");

    const auto agreement = repair_fixture(
        directory, "agreement-", {{std::byte{0x7a}}, {std::byte{0x7a}}}, {0, 1});
    check(agreement.bytes == Bytes{std::byte{0x7a}} && agreement.repairs.empty()
              && agreement.corroborated_positions == 1
              && agreement.redundant_positions == 1,
          "two-way agreement was not recognized without mutation");
}

void test_ties_and_strict_majority(const std::filesystem::path& directory)
{
    const auto disagreement = repair_fixture(
        directory, "disagree-", {{std::byte{0xcc}}, {std::byte{0x91}}}, {0, 1});
    check(disagreement.bytes == Bytes{std::byte{0xcc}} && disagreement.repairs.empty()
              && disagreement.ambiguities.size() == 1
              && disagreement.ambiguities[0].total_observations == 2,
          "two-way disagreement did not preserve baseline as ambiguous");

    const auto four_way = repair_fixture(
        directory,
        "four-tie-",
        {{std::byte{0xcc}}, {std::byte{0xcc}}, {std::byte{0x91}}, {std::byte{0x91}}},
        {0, 1, 1, 1});
    check(four_way.repairs.empty() && four_way.ambiguities.size() == 1,
          "four-way tie was repaired arbitrarily");

    const auto strict = repair_fixture(
        directory,
        "strict-",
        {{std::byte{0xbb}}, {std::byte{0xaa}}, {std::byte{0xaa}},
         {std::byte{0xaa}}, {std::byte{0xbb}}},
        {0, 1, 1, 1, 1});
    check(strict.bytes == Bytes{std::byte{0xaa}} && strict.repairs.size() == 1
              && strict.repairs[0].supporting_votes == 3
              && strict.repairs[0].total_observations == 5,
          "strict three-of-five majority was not applied");
}

void test_no_redundancy_multiple_repairs_and_binary(const std::filesystem::path& directory)
{
    const Bytes binary{std::byte{0x00}, std::byte{0xff}, std::byte{0x80},
                       std::byte{0x13}, std::byte{0x7a}};
    const auto lone = repair_fixture(directory, "lone-", {binary}, {0});
    check(lone.bytes == binary && lone.repairs.empty() && lone.redundant_positions == 0,
          "single-observation bytes were called repaired");

    const Bytes damaged{std::byte{0x91}, std::byte{0x22}, std::byte{0x80},
                        std::byte{0x44}, std::byte{0x01}};
    const auto repaired = repair_fixture(
        directory, "multiple-", {damaged, binary, binary}, {0, binary.size(), binary.size()});
    check(repaired.bytes == binary && repaired.repairs.size() == 4
              && repaired.repairs[0].position == 0
              && repaired.repairs[1].position == 1
              && repaired.repairs[2].position == 3
              && repaired.repairs[3].position == 4,
          "multiple binary-safe repairs were incorrect");
    check(repaired.bytes.size() == damaged.size(), "repair changed output size");
}

void test_relative_mapping_and_no_overlap_duplication(const std::filesystem::path& directory)
{
    const std::vector<Bytes> fragments{
        text_bytes("ABCDEFGH"), text_bytes("EFGHIJKL"),
        text_bytes("IJKLMNOP"), text_bytes("MNOPQRST"),
    };
    ReconstructionResult baseline;
    const auto result = repair_fixture(directory, "mapping-", fragments, {0, 4, 4, 4}, &baseline);
    check(result.bytes == text_bytes("ABCDEFGHIJKLMNOPQRST")
              && baseline.bytes == result.bytes && result.bytes.size() == 20,
          "relative starts 0,4,8,12 or overlap-aware output were incorrect");
    check(result.redundant_positions == 12 && result.corroborated_positions == 12
              && result.repairs.empty() && result.ambiguities.empty(),
          "relative-placement coverage counts were incorrect");
}

void test_wrong_majority_obeys_evidence(const std::filesystem::path& directory)
{
    const auto result = repair_fixture(
        directory,
        "wrong-majority-",
        {{std::byte{0xcc}}, {std::byte{0x91}}, {std::byte{0x91}}},
        {0, 1, 1});
    check(result.bytes == Bytes{std::byte{0x91}} && result.repairs.size() == 1,
          "repair engine did not obey available two-of-three evidence");
}

Bytes deterministic_source()
{
    Bytes source;
    for (std::size_t index = 0; index < 64; ++index) {
        source.push_back(static_cast<std::byte>((index * 73U + 19U) & 0xffU));
    }
    return source;
}

void test_approximate_end_to_end_damage_repair(const std::filesystem::path& directory)
{
    const auto source = deterministic_source();
    const auto clean = shardrecover::FragmentGenerator::generate(source, 16, 12);
    shardrecover::DamageResult damaged;
    bool found_repairable = false;
    for (std::uint64_t seed = 0; seed < 10000 && !found_repairable; ++seed) {
        damaged = shardrecover::DamageSimulator::apply(
            clean, shardrecover::DamageConfig{.corrupt_byte_count = 1, .seed = seed});
        const auto& corruption = damaged.corruptions.front();
        const auto absolute = clean[corruption.dataset_id].offset + corruption.byte_offset;
        std::size_t coverage = 0;
        std::size_t earliest = clean.size();
        for (const auto& fragment : clean) {
            if (absolute >= fragment.offset && absolute - fragment.offset < fragment.data.size()) {
                ++coverage;
                earliest = std::min(earliest, fragment.index);
            }
        }
        found_repairable = coverage >= 3 && corruption.dataset_id == earliest;
    }
    check(found_repairable, "could not construct deterministic repairable simulator damage");

    std::vector<Bytes> damaged_bytes;
    for (const auto& fragment : damaged.fragments) {
        damaged_bytes.push_back(fragment.data);
    }
    const auto files = load_fragments(directory, "end-to-end-", damaged_bytes);
    const auto graph = shardrecover::FragmentGraph::build(files, 12, 1);
    const auto reconstruction = shardrecover::BeamReconstructor::reconstruct(graph, files, 16);
    check(reconstruction.selected.complete && reconstruction.selected.approximate_joins >= 1,
          "approximate graph did not recover full damaged fragment path");
    check(reconstruction.selected.bytes != source,
          "end-to-end baseline did not retain selected corruption");

    const auto repaired = ConsensusRepairer::repair(reconstruction.selected, files);
    check(repaired.bytes == source && repaired.repairs.size() == 1,
          "consensus did not exactly repair deterministic recoverable damage");
    check(repaired.bytes.size() == reconstruction.selected.bytes.size(),
          "end-to-end substitution changed output size");
}

void test_insufficient_redundancy_and_determinism(const std::filesystem::path& directory)
{
    const auto first = repair_fixture(
        directory,
        "insufficient-",
        {{std::byte{0x91}, std::byte{0x13}}, {std::byte{0xcc}, std::byte{0x13}}},
        {0, 2});
    const auto second = repair_fixture(
        directory,
        "insufficient-repeat-",
        {{std::byte{0x91}, std::byte{0x13}}, {std::byte{0xcc}, std::byte{0x13}}},
        {0, 2});
    check(first.bytes == Bytes({std::byte{0x91}, std::byte{0x13}})
              && first.repairs.empty() && first.ambiguities.size() == 1,
          "one-good/one-bad conflict was not preserved as ambiguous");
    check(first.bytes == second.bytes && first.repairs.size() == second.repairs.size()
              && first.ambiguities[0].position == second.ambiguities[0].position,
          "identical consensus repair runs were nondeterministic");
}

void test_path_and_filename_independence(const std::filesystem::path& directory)
{
    const std::vector<Bytes> ordered_bytes{
        text_bytes("ABCDEFGH"), text_bytes("EFGHIJKL"), text_bytes("IJKLMNOP")};
    const auto ordered = load_fragments(directory, "descriptive-", ordered_bytes);
    const auto ordered_graph = shardrecover::FragmentGraph::build(ordered, 4);
    const auto ordered_reconstruction = shardrecover::BeamReconstructor::reconstruct(
        ordered_graph, ordered, 8);
    const auto ordered_repair = ConsensusRepairer::repair(ordered_reconstruction.selected, ordered);

    const std::vector<Bytes> shuffled_bytes{ordered_bytes[2], ordered_bytes[0], ordered_bytes[1]};
    const auto opaque = load_fragments(directory, "shard_f09a-", shuffled_bytes);
    const auto opaque_graph = shardrecover::FragmentGraph::build(opaque, 4);
    const auto opaque_reconstruction = shardrecover::BeamReconstructor::reconstruct(
        opaque_graph, opaque, 8);
    const auto opaque_repair = ConsensusRepairer::repair(opaque_reconstruction.selected, opaque);
    check(ordered_repair.bytes == opaque_repair.bytes
              && ordered_repair.repairs.size() == opaque_repair.repairs.size(),
          "load ordering or opaque filenames changed equivalent repair result");
}

struct TestCase {
    std::string_view name;
    void (*run)(const std::filesystem::path&);
};

}  // namespace

int main()
{
    try {
        const TemporaryDirectory directory;
        const std::array tests{
            TestCase{"majority and agreement", test_majority_and_two_way_agreement},
            TestCase{"ties and strict majority", test_ties_and_strict_majority},
            TestCase{"no redundancy, multiple, binary", test_no_redundancy_multiple_repairs_and_binary},
            TestCase{"relative mapping and merge size", test_relative_mapping_and_no_overlap_duplication},
            TestCase{"wrong majority follows evidence", test_wrong_majority_obeys_evidence},
            TestCase{"approximate end-to-end repair", test_approximate_end_to_end_damage_repair},
            TestCase{"insufficient redundancy and determinism", test_insufficient_redundancy_and_determinism},
            TestCase{"path and filename independence", test_path_and_filename_independence},
        };
        std::size_t failures = 0;
        for (const auto& test : tests) {
            try {
                test.run(directory.path());
                std::cout << "[PASS] " << test.name << '\n';
            } catch (const std::exception& error) {
                ++failures;
                std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
            }
        }
        if (failures != 0) {
            std::cerr << failures << " consensus repair test group(s) failed\n";
            return 1;
        }
        std::cout << tests.size() << " consensus repair test groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FATAL] " << error.what() << '\n';
        return 1;
    }
}
