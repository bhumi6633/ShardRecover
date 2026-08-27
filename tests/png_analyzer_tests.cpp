#include "shardrecover/png/analyzer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Bytes = std::vector<std::byte>;
using shardrecover::png::AnalysisResult;
using shardrecover::png::IssueCode;

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

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

Bytes make_chunk(std::string_view type, const Bytes& data = {}, bool valid_crc = true)
{
    check(type.size() == 4, "test chunk type must have four bytes");
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

Bytes ihdr(std::uint32_t width = 1,
           std::uint32_t height = 1,
           std::uint8_t bit_depth = 8,
           std::uint8_t color_type = 6,
           std::uint8_t compression = 0,
           std::uint8_t filter = 0,
           std::uint8_t interlace = 0)
{
    Bytes data;
    append_u32(data, width);
    append_u32(data, height);
    data.push_back(static_cast<std::byte>(bit_depth));
    data.push_back(static_cast<std::byte>(color_type));
    data.push_back(static_cast<std::byte>(compression));
    data.push_back(static_cast<std::byte>(filter));
    data.push_back(static_cast<std::byte>(interlace));
    return make_chunk("IHDR", data);
}

Bytes signature()
{
    return {std::byte{0x89}, std::byte{0x50}, std::byte{0x4e}, std::byte{0x47},
            std::byte{0x0d}, std::byte{0x0a}, std::byte{0x1a}, std::byte{0x0a}};
}

void append(Bytes& destination, const Bytes& source)
{
    destination.insert(destination.end(), source.begin(), source.end());
}

Bytes png_with(const std::vector<Bytes>& chunks)
{
    auto result = signature();
    for (const auto& chunk : chunks) {
        append(result, chunk);
    }
    return result;
}

Bytes minimal_png(const Bytes& idat_data = {}, std::uint8_t interlace = 0)
{
    return png_with({ihdr(1, 1, 8, 6, 0, 0, interlace),
                     make_chunk("IDAT", idat_data),
                     make_chunk("IEND")});
}

AnalysisResult analyze(const Bytes& bytes)
{
    return shardrecover::png::Analyzer::analyze(bytes);
}

bool has_issue(const AnalysisResult& result, IssueCode code)
{
    return std::any_of(result.issues.begin(), result.issues.end(), [&](const auto& issue) {
        return issue.code == code;
    });
}

void test_valid_minimal_png()
{
    const auto result = analyze(minimal_png());
    check(result.signature_valid && result.parsing_completed && result.structurally_valid,
          "minimal PNG was not structurally valid");
    check(result.semantically_valid && result.all_crcs_valid && result.chunks.size() == 3,
          "minimal PNG semantic or CRC validation failed");
}

void test_bad_and_truncated_signatures()
{
    auto bad = minimal_png();
    bad[0] = std::byte{0x00};
    check(has_issue(analyze(bad), IssueCode::invalid_signature), "bad signature was accepted");
    check(has_issue(analyze(Bytes{std::byte{0x89}, std::byte{0x50}}),
                    IssueCode::invalid_signature),
          "truncated signature was accepted");
    check(has_issue(analyze({}), IssueCode::invalid_signature), "empty input was accepted");
}

void test_truncated_headers_and_lengths()
{
    auto header = signature();
    header.push_back(std::byte{0x00});
    check(has_issue(analyze(header), IssueCode::truncated_chunk_header),
          "truncated chunk header was accepted");

    auto oversized = signature();
    append_u32(oversized, 20);
    append(oversized, Bytes{std::byte{'I'}, std::byte{'D'}, std::byte{'A'}, std::byte{'T'}});
    check(has_issue(analyze(oversized), IssueCode::truncated_chunk),
          "out-of-range declared length was accepted");

    auto malicious = signature();
    append_u32(malicious, 0xffffffffU);
    append(malicious, Bytes{std::byte{'I'}, std::byte{'D'}, std::byte{'A'}, std::byte{'T'}});
    check(has_issue(analyze(malicious), IssueCode::truncated_chunk),
          "maximum malicious length was not rejected safely");

    auto crc_truncated = png_with({ihdr(), make_chunk("IDAT")});
    append_u32(crc_truncated, 0);
    append(crc_truncated, Bytes{std::byte{'I'}, std::byte{'E'}, std::byte{'N'}, std::byte{'D'}});
    check(has_issue(analyze(crc_truncated), IssueCode::truncated_chunk),
          "truncated CRC field was accepted");
}

void test_ihdr_structure_rules()
{
    check(has_issue(analyze(png_with({make_chunk("IDAT"), ihdr(), make_chunk("IEND")})),
                    IssueCode::ihdr_not_first),
          "non-first IHDR was accepted");
    check(has_issue(analyze(png_with({ihdr(), ihdr(), make_chunk("IDAT"), make_chunk("IEND")})),
                    IssueCode::duplicate_ihdr),
          "duplicate IHDR was accepted");
    check(has_issue(analyze(png_with({make_chunk("IHDR", Bytes(12)),
                                      make_chunk("IDAT"), make_chunk("IEND")})),
                    IssueCode::invalid_ihdr_length),
          "invalid IHDR length was accepted");
    check(has_issue(analyze(png_with({ihdr(0, 1), make_chunk("IDAT"), make_chunk("IEND")})),
                    IssueCode::invalid_dimensions),
          "zero width was accepted");
    check(has_issue(analyze(png_with({ihdr(1, 0), make_chunk("IDAT"), make_chunk("IEND")})),
                    IssueCode::invalid_dimensions),
          "zero height was accepted");
}

void test_required_and_final_chunks()
{
    check(has_issue(analyze(png_with({ihdr(), make_chunk("IEND")})), IssueCode::missing_idat),
          "missing IDAT was accepted");
    check(has_issue(analyze(png_with({ihdr(), make_chunk("IDAT")})), IssueCode::missing_iend),
          "missing IEND was accepted");
    check(has_issue(analyze(png_with({ihdr(), make_chunk("IDAT"),
                                      make_chunk("IEND", Bytes{std::byte{1}})})),
                    IssueCode::invalid_iend_length),
          "nonempty IEND was accepted");
    auto trailing = minimal_png();
    trailing.push_back(std::byte{0});
    check(has_issue(analyze(trailing), IssueCode::trailing_data),
          "bytes after IEND were accepted");
}

void test_crc_rules()
{
    const auto valid = analyze(minimal_png());
    check(valid.valid_crc_count == 3 && valid.invalid_crc_count == 0 && valid.all_crcs_valid,
          "valid CRC aggregate was incorrect");

    auto bad_crc_png = png_with({ihdr(), make_chunk("IDAT", {}, false), make_chunk("IEND")});
    const auto invalid = analyze(bad_crc_png);
    check(invalid.invalid_crc_count == 1 && !invalid.all_crcs_valid
              && has_issue(invalid, IssueCode::crc_mismatch),
          "invalid CRC was not reported");

    const auto idat = analyze(png_with({ihdr(), make_chunk("IDAT"), make_chunk("IEND")}));
    const auto jdat = analyze(png_with({ihdr(), make_chunk("JDAT"), make_chunk("IEND")}));
    check(idat.chunks[1].computed_crc != jdat.chunks[1].computed_crc,
          "CRC did not change when chunk type changed");
    check(idat.chunks[1].crc_valid,
          "length bytes were incorrectly included in the CRC calculation");
}

void test_idat_order_and_binary_payload()
{
    const auto consecutive = analyze(png_with(
        {ihdr(), make_chunk("IDAT", Bytes{std::byte{1}}),
         make_chunk("IDAT", Bytes{std::byte{2}}), make_chunk("IEND")}));
    check(consecutive.semantically_valid, "consecutive IDAT chunks were rejected");

    const auto interrupted = analyze(png_with(
        {ihdr(), make_chunk("IDAT"), make_chunk("tEXt"),
         make_chunk("IDAT"), make_chunk("IEND")}));
    check(has_issue(interrupted, IssueCode::nonconsecutive_idat),
          "interrupted IDAT sequence was accepted");

    const Bytes binary{std::byte{0x00}, std::byte{0xff}, std::byte{0x80},
                       std::byte{0x13}, std::byte{0x7a}};
    const auto binary_png = minimal_png(binary);
    const auto binary_result = analyze(binary_png);
    check(binary_result.structurally_valid && binary_result.all_crcs_valid
              && binary_result.chunks[1].data.size() == binary.size()
              && std::equal(binary.begin(), binary.end(), binary_result.chunks[1].data.begin()),
          "binary IDAT payload was not preserved safely");
}

void test_ihdr_fields_and_semantics()
{
    const auto dimensions = analyze(png_with(
        {ihdr(640, 480), make_chunk("IDAT"), make_chunk("IEND")}));
    check(dimensions.ihdr.has_value() && dimensions.ihdr->width == 640
              && dimensions.ihdr->height == 480,
          "IHDR dimensions were decoded incorrectly");
    check(analyze(minimal_png({}, 0)).semantically_valid, "interlace zero was rejected");
    check(analyze(minimal_png({}, 1)).semantically_valid, "interlace one was rejected");
    check(has_issue(analyze(minimal_png({}, 2)), IssueCode::invalid_interlace_method),
          "invalid interlace method was accepted");
    check(has_issue(analyze(png_with({ihdr(1, 1, 4, 6), make_chunk("IDAT"),
                                      make_chunk("IEND")})),
                    IssueCode::invalid_color_type_bit_depth),
          "invalid color type and bit depth combination was accepted");
    check(has_issue(analyze(png_with({ihdr(1, 1, 8, 6, 1), make_chunk("IDAT"),
                                      make_chunk("IEND")})),
                    IssueCode::invalid_compression_method),
          "invalid compression method was accepted");
    check(has_issue(analyze(png_with({ihdr(1, 1, 8, 6, 0, 1), make_chunk("IDAT"),
                                      make_chunk("IEND")})),
                    IssueCode::invalid_filter_method),
          "invalid filter method was accepted");
}

void test_invalid_chunk_type()
{
    check(has_issue(analyze(png_with({ihdr(), make_chunk("ID1T"), make_chunk("IEND")})),
                    IssueCode::invalid_chunk_type),
          "nonalphabetic chunk type was accepted");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

}  // namespace

int main()
{
    const std::array tests{
        TestCase{"valid minimal PNG", test_valid_minimal_png},
        TestCase{"bad and truncated signatures", test_bad_and_truncated_signatures},
        TestCase{"truncated headers and lengths", test_truncated_headers_and_lengths},
        TestCase{"IHDR structure rules", test_ihdr_structure_rules},
        TestCase{"required and final chunks", test_required_and_final_chunks},
        TestCase{"CRC rules", test_crc_rules},
        TestCase{"IDAT ordering and binary payload", test_idat_order_and_binary_payload},
        TestCase{"IHDR fields and semantics", test_ihdr_fields_and_semantics},
        TestCase{"invalid chunk type", test_invalid_chunk_type},
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
        std::cerr << failures << " test group(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " PNG test groups passed\n";
    return 0;
}
