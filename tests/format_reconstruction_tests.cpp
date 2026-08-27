#include "shardrecover/binary_file.hpp"
#include "shardrecover/fragment_emitter.hpp"
#include "shardrecover/fragment_generator.hpp"
#include "shardrecover/fragment_graph.hpp"
#include "shardrecover/png/analyzer.hpp"
#include "shardrecover/png/reconstruction_evaluator.hpp"
#include "shardrecover/reconstruction.hpp"

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
                          / ("shardrecover-format-tests-" + std::to_string(stamp));
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
        throw std::runtime_error("Failed to find a temporary directory name");
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

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

std::uint32_t update_crc(std::uint32_t crc, std::byte value)
{
    crc ^= std::to_integer<std::uint8_t>(value);
    for (int bit = 0; bit < 8; ++bit) {
        crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc;
}

std::uint32_t crc(std::string_view type, std::span<const std::byte> data)
{
    std::uint32_t value = 0xffffffffU;
    for (const auto character : type) {
        value = update_crc(value,
                           static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    for (const auto byte : data) {
        value = update_crc(value, byte);
    }
    return value ^ 0xffffffffU;
}

Bytes chunk(std::string_view type, const Bytes& data = {}, bool valid_crc = true)
{
    Bytes result;
    append_u32(result, static_cast<std::uint32_t>(data.size()));
    for (const auto character : type) {
        result.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    result.insert(result.end(), data.begin(), data.end());
    auto checksum = crc(type, data);
    if (!valid_crc) {
        checksum ^= 1U;
    }
    append_u32(result, checksum);
    return result;
}

Bytes valid_png(const Bytes& idat_data = {})
{
    Bytes result{std::byte{0x89}, std::byte{0x50}, std::byte{0x4e}, std::byte{0x47},
                 std::byte{0x0d}, std::byte{0x0a}, std::byte{0x1a}, std::byte{0x0a}};
    Bytes ihdr_data;
    append_u32(ihdr_data, 1);
    append_u32(ihdr_data, 1);
    const std::array fields{std::byte{8}, std::byte{6}, std::byte{0},
                            std::byte{0}, std::byte{0}};
    ihdr_data.insert(ihdr_data.end(), fields.begin(), fields.end());
    for (const auto& part : {chunk("IHDR", ihdr_data), chunk("IDAT", idat_data), chunk("IEND")}) {
        result.insert(result.end(), part.begin(), part.end());
    }
    return result;
}

void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to create test fragment: " + path.string());
    }
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    output.close();
    if (!output) {
        throw std::runtime_error("Failed to write test fragment: " + path.string());
    }
}

struct Input {
    std::string name;
    Bytes bytes;
};

shardrecover::BeamReconstructionResult reconstruct(const std::filesystem::path& directory,
                                                    const std::vector<Input>& inputs,
                                                    std::size_t minimum_overlap,
                                                    std::size_t beam_width,
                                                    const shardrecover::CandidateEvaluator* evaluator)
{
    std::vector<shardrecover::BinaryFile> files;
    for (const auto& input : inputs) {
        const auto path = directory / input.name;
        write_file(path, input.bytes);
        files.push_back(shardrecover::BinaryFile::load(path));
    }
    const auto file_span = std::span<const shardrecover::BinaryFile>{files};
    const auto graph = shardrecover::FragmentGraph::build(file_span, minimum_overlap);
    return shardrecover::BeamReconstructor::reconstruct(graph, file_span, beam_width, evaluator);
}

std::vector<Input> ambiguous_png_fragments(const Bytes& png)
{
    constexpr std::size_t ihdr_start = 8;
    constexpr std::size_t idat_start = 33;
    const auto idat_length = static_cast<std::size_t>(
        (std::to_integer<std::uint32_t>(png[idat_start]) << 24U)
        | (std::to_integer<std::uint32_t>(png[idat_start + 1]) << 16U)
        | (std::to_integer<std::uint32_t>(png[idat_start + 2]) << 8U)
        | std::to_integer<std::uint32_t>(png[idat_start + 3]));
    const auto iend_start = idat_start + 12 + idat_length;
    const auto slice = [&](std::size_t first, std::size_t last) {
        return Bytes(png.begin() + static_cast<std::ptrdiff_t>(first),
                     png.begin() + static_cast<std::ptrdiff_t>(last));
    };
    return {
        {"ambiguous-00-start.bin", slice(0, ihdr_start + 1)},
        {"ambiguous-30-ihdr.bin", slice(ihdr_start, idat_start + 1)},
        {"ambiguous-10-idat.bin", slice(idat_start, iend_start + 4)},
        {"ambiguous-40-iend.bin", slice(iend_start + 3, png.size())},
    };
}

void test_valid_png_beats_higher_overlap(const std::filesystem::path& directory)
{
    const auto png = valid_png(Bytes{std::byte{0x00}, std::byte{0xff}, std::byte{0x80},
                                     std::byte{0x13}, std::byte{0x7a}});
    const auto inputs = ambiguous_png_fragments(png);
    shardrecover::png::ReconstructionEvaluator evaluator;
    const auto generic = reconstruct(directory, inputs, 1, 16, nullptr);
    const auto aware = reconstruct(directory, inputs, 1, 16, &evaluator);

    check(generic.selected.complete && aware.selected.complete,
          "ambiguous candidates were not complete");
    check(generic.selected.bytes != png && generic.selected.total_overlap_bytes > 3,
          "generic ranking did not choose the stronger malformed overlap path");
    check(aware.selected.bytes == png && aware.selected.total_overlap_bytes == 3,
          "PNG evidence did not choose the lower-overlap valid PNG");
    check(aware.candidates.front().evidence.format.has_value(),
          "selected candidate did not expose format evidence");
    std::cout << "[INFO] adversarial overlap: generic="
              << generic.selected.total_overlap_bytes << ", PNG-aware="
              << aware.selected.total_overlap_bytes << '\n';
}

void test_crc_and_structure_ranking(const std::filesystem::path& directory)
{
    shardrecover::png::ReconstructionEvaluator evaluator;
    const auto good = valid_png();
    auto bad_crc = good;
    bad_crc[44] ^= std::byte{1};
    check(shardrecover::png::Analyzer::analyze(bad_crc).invalid_crc_count == 1,
          "bad-CRC fixture was not detected by the PNG analyzer");
    const auto crc_ranked = reconstruct(
        directory,
        {{"crc-a-invalid.png", bad_crc}, {"crc-z-valid.png", good}},
        1024,
        2,
        &evaluator);
    check(crc_ranked.selected.bytes == good, "valid CRC candidate did not win");
    check(crc_ranked.candidates.front().evidence.format->ranking_keys
              > crc_ranked.candidates.back().evidence.format->ranking_keys,
          "CRC evidence did not affect ranking keys");
    std::cout << "[INFO] CRC evidence selected the all-valid candidate\n";

    auto missing_iend = good;
    missing_iend.resize(missing_iend.size() - 12);
    auto duplicate_ihdr = valid_png();
    const auto duplicate = chunk("IHDR", Bytes(13));
    duplicate_ihdr.insert(duplicate_ihdr.begin() + 33, duplicate.begin(), duplicate.end());
    for (const auto& malformed : {missing_iend, duplicate_ihdr}) {
        const auto ranked = reconstruct(
            directory,
            {{"structure-a-invalid.png", malformed}, {"structure-z-valid.png", good}},
            2048,
            2,
            &evaluator);
        check(ranked.selected.bytes == good, "valid structure did not beat malformed PNG");
    }
}

void test_non_png_and_determinism(const std::filesystem::path& directory)
{
    shardrecover::png::ReconstructionEvaluator evaluator;
    const Bytes binary{std::byte{0x00}, std::byte{0xff}, std::byte{0x80},
                       std::byte{0x13}, std::byte{0x7a}};
    const std::vector<Input> inputs{{"nonpng-a.bin", binary},
                                    {"nonpng-b.bin", Bytes{std::byte{0x7a}, std::byte{0x42}}}};
    const auto generic = reconstruct(directory, inputs, 1, 2, nullptr);
    const auto first = reconstruct(directory, inputs, 1, 2, &evaluator);
    const auto second = reconstruct(directory, inputs, 1, 2, &evaluator);
    check(generic.selected.bytes == Bytes({std::byte{0x00}, std::byte{0xff}, std::byte{0x80},
                                          std::byte{0x13}, std::byte{0x7a}, std::byte{0x42}}),
          "generic binary reconstruction changed");
    check(first.selected.bytes == second.selected.bytes,
          "explicit PNG evaluation of non-PNG data was nondeterministic");
    check(first.candidates.front().evidence.format.has_value()
              && first.candidates.front().evidence.format->ranking_keys.front() == 0,
          "non-PNG input did not expose invalid PNG evidence");
}

void test_same_evidence_uses_overlap(const std::filesystem::path& directory)
{
    shardrecover::png::ReconstructionEvaluator evaluator;
    const std::vector<Input> inputs{
        {"fallback-a.bin", Bytes{std::byte{'X'}, std::byte{'A'}, std::byte{'B'}, std::byte{'C'}}},
        {"fallback-b.bin", Bytes{std::byte{'A'}, std::byte{'B'}, std::byte{'C'}, std::byte{'Y'}}},
        {"fallback-c.bin", Bytes{std::byte{'B'}, std::byte{'C'}, std::byte{'Z'}}},
    };
    const auto result = reconstruct(directory, inputs, 2, 2, &evaluator);
    check(result.candidates.size() >= 2
              && result.candidates[0].evidence.format->ranking_keys
                     == result.candidates[1].evidence.format->ranking_keys
              && result.candidates[0].total_overlap_bytes
                     > result.candidates[1].total_overlap_bytes,
          "generic overlap did not break equivalent PNG-evidence tie");
}

void test_valid_png_roundtrip(const std::filesystem::path& directory)
{
    Bytes payload;
    for (std::size_t index = 0; index < 300; ++index) {
        payload.push_back(static_cast<std::byte>((index * 73U + 19U) & 0xffU));
    }
    const auto png = valid_png(payload);
    auto generated = shardrecover::FragmentGenerator::generate(png, 64, 16);
    shardrecover::FragmentEmitter::shuffle(generated, 42);
    const auto names = shardrecover::FragmentEmitter::opaque_filenames(generated.size(), 42);
    std::vector<Input> inputs;
    for (std::size_t index = 0; index < generated.size(); ++index) {
        inputs.push_back(Input{"roundtrip-" + names[index], generated[index].data});
    }

    shardrecover::png::ReconstructionEvaluator evaluator;
    const auto result = reconstruct(directory, inputs, 16, 8, &evaluator);
    check(result.selected.complete && result.selected.bytes == png,
          "PNG-aware shuffled opaque roundtrip was not exact");
    const auto analysis = shardrecover::png::Analyzer::analyze(result.selected.bytes);
    check(analysis.structurally_valid && analysis.all_crcs_valid,
          "roundtrip result did not remain a valid PNG");
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
            TestCase{"valid PNG beats higher overlap", test_valid_png_beats_higher_overlap},
            TestCase{"CRC and structure ranking", test_crc_and_structure_ranking},
            TestCase{"non-PNG safety and determinism", test_non_png_and_determinism},
            TestCase{"same evidence overlap fallback", test_same_evidence_uses_overlap},
            TestCase{"valid PNG roundtrip", test_valid_png_roundtrip},
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
            std::cerr << failures << " format-aware test(s) failed\n";
            return 1;
        }
        std::cout << tests.size() << " format-aware test groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FATAL] " << error.what() << '\n';
        return 1;
    }
}
