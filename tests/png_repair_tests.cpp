#include "shardrecover/binary_file.hpp"
#include "shardrecover/damage.hpp"
#include "shardrecover/fragment_generator.hpp"
#include "shardrecover/fragment_graph.hpp"
#include "shardrecover/png/analyzer.hpp"
#include "shardrecover/png/crc.hpp"
#include "shardrecover/png/repair.hpp"
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
using shardrecover::AmbiguousByte;
using shardrecover::ByteVote;
using shardrecover::RepairResult;
using shardrecover::png::CrcGuidedRepairer;

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto base = std::filesystem::temp_directory_path()
                          / ("shardrecover-png-repair-tests-" + std::to_string(stamp));
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

void append_u32(Bytes& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::byte>((value >> 24U) & 0xffU));
    bytes.push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::byte>(value & 0xffU));
}

Bytes chunk(std::string_view type, const Bytes& data = {})
{
    check(type.size() == 4, "test PNG chunk type must have four bytes");
    std::array<char, 4> type_bytes{};
    std::copy(type.begin(), type.end(), type_bytes.begin());
    Bytes result;
    append_u32(result, static_cast<std::uint32_t>(data.size()));
    for (const auto value : type_bytes) {
        result.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    result.insert(result.end(), data.begin(), data.end());
    append_u32(result, shardrecover::png::compute_chunk_crc(type_bytes, data));
    return result;
}

Bytes make_png(const Bytes& idat_data = {}, bool ancillary = false)
{
    Bytes result{std::byte{0x89}, std::byte{0x50}, std::byte{0x4e}, std::byte{0x47},
                 std::byte{0x0d}, std::byte{0x0a}, std::byte{0x1a}, std::byte{0x0a}};
    Bytes ihdr;
    append_u32(ihdr, 1);
    append_u32(ihdr, 1);
    const std::array fields{std::byte{8}, std::byte{6}, std::byte{0},
                            std::byte{0}, std::byte{0}};
    ihdr.insert(ihdr.end(), fields.begin(), fields.end());
    for (const auto& bytes : {chunk("IHDR", ihdr), chunk("IDAT", idat_data)}) {
        result.insert(result.end(), bytes.begin(), bytes.end());
    }
    if (ancillary) {
        const auto text = chunk("tEXt", Bytes{std::byte{'k'}, std::byte{0}, std::byte{'v'}});
        result.insert(result.end(), text.begin(), text.end());
    }
    const auto end = chunk("IEND");
    result.insert(result.end(), end.begin(), end.end());
    return result;
}

AmbiguousByte ambiguity(std::size_t position,
                        std::byte baseline,
                        std::initializer_list<std::byte> values)
{
    AmbiguousByte result{position, baseline, values.size(), {}};
    for (const auto value : values) {
        result.votes.push_back(ByteVote{value, 1});
    }
    return result;
}

RepairResult consensus_fixture(Bytes bytes, std::vector<AmbiguousByte> ambiguities)
{
    RepairResult result;
    result.bytes = std::move(bytes);
    result.ambiguities = std::move(ambiguities);
    result.conflicting_positions = result.ambiguities.size();
    return result;
}

std::size_t first_idat_data_position(const Bytes& png)
{
    const auto analysis = shardrecover::png::Analyzer::analyze(png);
    for (const auto& png_chunk : analysis.chunks) {
        if (png_chunk.type == std::array<char, 4>{'I', 'D', 'A', 'T'}) {
            return png_chunk.offset + 8;
        }
    }
    throw std::runtime_error("test PNG had no IDAT chunk");
}

void test_two_way_crc_resolution(const std::filesystem::path&)
{
    const auto pristine = make_png(Bytes{std::byte{0xcc}, std::byte{0x13}});
    const auto position = first_idat_data_position(pristine);
    auto damaged = pristine;
    damaged[position] = std::byte{0x91};
    const auto consensus = consensus_fixture(
        damaged, {ambiguity(position, std::byte{0x91}, {std::byte{0x91}, std::byte{0xcc}})});
    const auto result = CrcGuidedRepairer::repair(consensus);
    check(result.bytes == pristine && result.repairs.size() == 1
              && result.repairs[0].changed
              && result.repairs[0].crc_consistent_value == std::byte{0xcc}
              && result.repairs[0].candidates_tested == 2,
          "two-way CRC tie did not select the unique observed consistent byte");
    check(result.before.invalid_crc_count == 1 && result.after.invalid_crc_count == 0
              && result.after.all_crcs_valid,
          "CRC counts did not improve to full validity");
}

void test_correct_baseline_and_no_match(const std::filesystem::path&)
{
    const auto pristine = make_png(Bytes{std::byte{0xcc}});
    const auto position = first_idat_data_position(pristine);
    const auto correct = CrcGuidedRepairer::repair(consensus_fixture(
        pristine, {ambiguity(position, std::byte{0xcc}, {std::byte{0xcc}, std::byte{0x91}})}));
    check(correct.repairs.size() == 1 && !correct.repairs[0].changed
              && correct.bytes == pristine,
          "correct CRC-consistent baseline was mutated unnecessarily");

    auto damaged = pristine;
    damaged[position] = std::byte{0xaa};
    const auto no_match = CrcGuidedRepairer::repair(consensus_fixture(
        damaged, {ambiguity(position, std::byte{0xaa}, {std::byte{0xaa}, std::byte{0xbb}})}));
    check(no_match.repairs.empty() && no_match.unresolved.size() == 1
              && no_match.bytes == damaged
              && no_match.unresolved[0].reason.find("no observed candidate") != std::string::npos,
          "zero-matching observed candidates were not left unresolved");
}

void test_ineligible_regions(const std::filesystem::path&)
{
    const auto pristine = make_png(Bytes{std::byte{0x42}});
    const auto analysis = shardrecover::png::Analyzer::analyze(pristine);
    const auto& idat = analysis.chunks[1];
    const std::vector<AmbiguousByte> ambiguities{
        ambiguity(0, pristine[0], {pristine[0], std::byte{0}}),
        ambiguity(idat.offset, pristine[idat.offset], {pristine[idat.offset], std::byte{1}}),
        ambiguity(idat.offset + 8 + idat.length,
                  pristine[idat.offset + 8 + idat.length],
                  {pristine[idat.offset + 8 + idat.length], std::byte{0}}),
    };
    const auto result = CrcGuidedRepairer::repair(consensus_fixture(pristine, ambiguities));
    check(result.repairs.empty() && result.unresolved.size() == 3
              && result.eligible_ambiguous_bytes == 0,
          "signature, length, or stored CRC ambiguity was treated as CRC-covered data");
}

void test_chunk_type_repair(const std::filesystem::path&)
{
    const auto pristine = make_png({}, true);
    const auto analysis = shardrecover::png::Analyzer::analyze(pristine);
    const auto text = std::find_if(analysis.chunks.begin(), analysis.chunks.end(), [](const auto& item) {
        return item.type == std::array<char, 4>{'t', 'E', 'X', 't'};
    });
    check(text != analysis.chunks.end(), "test ancillary chunk was absent");
    const auto position = text->offset + 4;
    auto damaged = pristine;
    damaged[position] = std::byte{'u'};
    const auto result = CrcGuidedRepairer::repair(consensus_fixture(
        damaged, {ambiguity(position, std::byte{'u'}, {std::byte{'u'}, std::byte{'t'}})}));
    check(result.bytes == pristine && result.repairs.size() == 1,
          "CRC-covered chunk type byte was not repaired");
}

void test_multiple_same_and_different_chunks(const std::filesystem::path&)
{
    const auto pristine = make_png(Bytes{std::byte{0x10}, std::byte{0x20}}, true);
    const auto idat_position = first_idat_data_position(pristine);
    auto two_damaged = pristine;
    two_damaged[idat_position] = std::byte{0x11};
    two_damaged[idat_position + 1] = std::byte{0x21};
    const auto skipped = CrcGuidedRepairer::repair(consensus_fixture(
        two_damaged,
        {ambiguity(idat_position, std::byte{0x11}, {std::byte{0x11}, std::byte{0x10}}),
         ambiguity(idat_position + 1, std::byte{0x21}, {std::byte{0x21}, std::byte{0x20}})}));
    check(skipped.repairs.empty() && skipped.unresolved.size() == 2
              && skipped.chunks_evaluated == 0,
          "multiple ambiguities in one chunk triggered combinatorial repair");

    const auto analysis = shardrecover::png::Analyzer::analyze(pristine);
    const auto text = std::find_if(analysis.chunks.begin(), analysis.chunks.end(), [](const auto& item) {
        return item.type == std::array<char, 4>{'t', 'E', 'X', 't'};
    });
    const auto text_position = text->offset + 8;
    auto separate = pristine;
    separate[idat_position] = std::byte{0x11};
    separate[text_position] = std::byte{'x'};
    const auto repaired = CrcGuidedRepairer::repair(consensus_fixture(
        separate,
        {ambiguity(idat_position, std::byte{0x11}, {std::byte{0x11}, std::byte{0x10}}),
         ambiguity(text_position, std::byte{'x'}, {std::byte{'x'}, std::byte{'k'}})}));
    check(repaired.bytes == pristine && repaired.repairs.size() == 2
              && repaired.chunks_evaluated == 2 && repaired.after.all_crcs_valid,
          "independent chunk ambiguities were not repaired separately");
}

void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to create PNG repair fragment");
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void test_real_pipeline_roundtrip(const std::filesystem::path& directory)
{
    Bytes payload;
    for (std::size_t index = 0; index < 128; ++index) {
        payload.push_back(static_cast<std::byte>((index * 73U + 19U) & 0xffU));
    }
    const auto pristine = make_png(payload);
    const auto clean = shardrecover::FragmentGenerator::generate(pristine, 32, 16);
    const auto idat_start = first_idat_data_position(pristine);
    const auto idat_end = idat_start + payload.size();

    shardrecover::DamageResult damaged;
    bool found = false;
    for (std::uint64_t seed = 0; seed < 10000 && !found; ++seed) {
        damaged = shardrecover::DamageSimulator::apply(
            clean, shardrecover::DamageConfig{.corrupt_byte_count = 1, .seed = seed});
        const auto& record = damaged.corruptions.front();
        const auto absolute = clean[record.dataset_id].offset + record.byte_offset;
        std::size_t coverage = 0;
        std::size_t earliest = clean.size();
        for (const auto& fragment : clean) {
            if (absolute >= fragment.offset && absolute - fragment.offset < fragment.data.size()) {
                ++coverage;
                earliest = std::min(earliest, fragment.index);
            }
        }
        found = absolute >= idat_start && absolute < idat_end
                && coverage == 2 && record.dataset_id == earliest;
    }
    check(found, "could not construct deterministic CRC-repairable PNG corruption");

    std::vector<shardrecover::BinaryFile> files;
    for (const auto& fragment : damaged.fragments) {
        const auto path = directory / ("roundtrip-" + std::to_string(fragment.index) + ".bin");
        write_file(path, fragment.data);
        files.push_back(shardrecover::BinaryFile::load(path));
    }
    const auto graph = shardrecover::FragmentGraph::build(files, 16, 1);
    const auto reconstruction = shardrecover::BeamReconstructor::reconstruct(graph, files, 16);
    check(reconstruction.selected.complete && reconstruction.selected.bytes != pristine,
          "real pipeline baseline did not retain selected PNG corruption");
    const auto consensus = shardrecover::ConsensusRepairer::repair(reconstruction.selected, files);
    check(consensus.repairs.empty() && consensus.ambiguities.size() == 1
              && consensus.bytes != pristine,
          "two-way PNG conflict was not left for CRC guidance");
    const auto repaired = CrcGuidedRepairer::repair(consensus);
    check(repaired.bytes == pristine && repaired.repairs.size() == 1
              && repaired.before.invalid_crc_count == 1 && repaired.after.all_crcs_valid,
          "real approximate/consensus/PNG pipeline did not restore exact PNG bytes");
    std::cout << "[INFO] PNG CRC valid chunks: " << repaired.before.valid_crc_count << '/'
              << repaired.before.parsed_chunks << " -> " << repaired.after.valid_crc_count
              << '/' << repaired.after.parsed_chunks << '\n';
}

void test_consensus_first_non_png_and_determinism(const std::filesystem::path&)
{
    RepairResult consensus;
    consensus.bytes = Bytes{std::byte{0x91}};
    consensus.repairs.push_back(
        shardrecover::ByteRepair{0, std::byte{0xcc}, std::byte{0x91}, 2, 3});
    const auto no_redo = CrcGuidedRepairer::repair(consensus);
    check(no_redo.repairs.empty() && no_redo.bytes == consensus.bytes,
          "PNG stage redundantly reconsidered consensus-resolved byte");

    consensus.ambiguities.push_back(
        ambiguity(0, std::byte{0x91}, {std::byte{0x91}, std::byte{0xcc}}));
    const auto first = CrcGuidedRepairer::repair(consensus);
    const auto second = CrcGuidedRepairer::repair(consensus);
    check(first.repairs.empty() && first.unresolved.size() == 1
              && first.bytes == second.bytes
              && first.unresolved[0].reason == second.unresolved[0].reason,
          "non-PNG ambiguity was unsafe or nondeterministic");
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
            TestCase{"two-way CRC resolution", test_two_way_crc_resolution},
            TestCase{"correct baseline and no match", test_correct_baseline_and_no_match},
            TestCase{"ineligible regions", test_ineligible_regions},
            TestCase{"chunk type repair", test_chunk_type_repair},
            TestCase{"same and different chunk ambiguities", test_multiple_same_and_different_chunks},
            TestCase{"real pipeline PNG roundtrip", test_real_pipeline_roundtrip},
            TestCase{"consensus first, non-PNG, determinism", test_consensus_first_non_png_and_determinism},
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
            std::cerr << failures << " PNG CRC repair test group(s) failed\n";
            return 1;
        }
        std::cout << tests.size() << " PNG CRC repair test groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FATAL] " << error.what() << '\n';
        return 1;
    }
}
