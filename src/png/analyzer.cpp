#include "shardrecover/png/analyzer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace shardrecover::png {
namespace {

constexpr std::array<std::byte, 8> signature{
    std::byte{0x89}, std::byte{0x50}, std::byte{0x4e}, std::byte{0x47},
    std::byte{0x0d}, std::byte{0x0a}, std::byte{0x1a}, std::byte{0x0a},
};

std::uint32_t read_u32_be(std::span<const std::byte, 4> bytes)
{
    return (std::to_integer<std::uint32_t>(bytes[0]) << 24U)
           | (std::to_integer<std::uint32_t>(bytes[1]) << 16U)
           | (std::to_integer<std::uint32_t>(bytes[2]) << 8U)
           | std::to_integer<std::uint32_t>(bytes[3]);
}

bool is_type(const std::array<char, 4>& type, std::string_view expected)
{
    return std::equal(type.begin(), type.end(), expected.begin(), expected.end());
}

void add_issue(AnalysisResult& result,
               IssueCode code,
               std::size_t offset,
               std::string message)
{
    result.issues.push_back(AnalysisIssue{code, offset, std::move(message)});
}

bool valid_chunk_type(const std::array<char, 4>& type)
{
    return std::all_of(type.begin(), type.end(), [](char value) {
        return std::isalpha(static_cast<unsigned char>(value)) != 0;
    });
}

Ihdr parse_ihdr(std::span<const std::byte> data)
{
    return Ihdr{
        read_u32_be(std::span<const std::byte, 4>{data.subspan<0, 4>()}),
        read_u32_be(std::span<const std::byte, 4>{data.subspan<4, 4>()}),
        std::to_integer<std::uint8_t>(data[8]),
        std::to_integer<std::uint8_t>(data[9]),
        std::to_integer<std::uint8_t>(data[10]),
        std::to_integer<std::uint8_t>(data[11]),
        std::to_integer<std::uint8_t>(data[12]),
    };
}

}  // namespace

AnalysisResult Analyzer::analyze(std::span<const std::byte> bytes)
{
    AnalysisResult result;
    if (bytes.size() < signature.size()
        || !std::equal(signature.begin(), signature.end(), bytes.begin())) {
        add_issue(result, IssueCode::invalid_signature, 0, "Invalid or truncated PNG signature");
        return result;
    }
    result.signature_valid = true;

    std::size_t offset = signature.size();
    std::size_t ihdr_count = 0;
    std::size_t idat_count = 0;
    std::size_t iend_count = 0;
    bool truncated = false;

    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        if (remaining < 8) {
            add_issue(result, IssueCode::truncated_chunk_header, offset,
                      "Truncated PNG chunk length or type");
            truncated = true;
            break;
        }

        const auto length = read_u32_be(
            std::span<const std::byte, 4>{bytes.subspan(offset, 4)});
        std::array<char, 4> type{};
        for (std::size_t index = 0; index < type.size(); ++index) {
            type[index] = static_cast<char>(std::to_integer<unsigned char>(bytes[offset + 4 + index]));
        }
        if (!valid_chunk_type(type)) {
            add_issue(result, IssueCode::invalid_chunk_type, offset + 4,
                      "PNG chunk type must contain only ASCII letters");
        }

        constexpr std::size_t fixed_chunk_bytes = 12;
        if (remaining < fixed_chunk_bytes
            || static_cast<std::size_t>(length) > remaining - fixed_chunk_bytes) {
            add_issue(result, IssueCode::truncated_chunk, offset,
                      "Declared PNG chunk extends beyond input");
            truncated = true;
            break;
        }

        const auto data_offset = offset + 8;
        const auto crc_offset = data_offset + static_cast<std::size_t>(length);
        const auto data = bytes.subspan(data_offset, length);
        const auto stored_crc = read_u32_be(
            std::span<const std::byte, 4>{bytes.subspan(crc_offset, 4)});
        result.chunks.push_back(ChunkView{offset, length, type, data, stored_crc});

        const bool ihdr = is_type(type, "IHDR");
        const bool idat = is_type(type, "IDAT");
        const bool iend = is_type(type, "IEND");
        if (result.chunks.size() == 1 && !ihdr) {
            add_issue(result, IssueCode::ihdr_not_first, offset, "IHDR must be the first chunk");
        }
        if (ihdr) {
            ++ihdr_count;
            if (ihdr_count > 1) {
                add_issue(result, IssueCode::duplicate_ihdr, offset, "PNG contains duplicate IHDR");
            }
            if (length != 13) {
                add_issue(result, IssueCode::invalid_ihdr_length, offset, "IHDR length must be 13");
            } else {
                result.ihdr = parse_ihdr(data);
                if (result.ihdr->width == 0 || result.ihdr->height == 0) {
                    add_issue(result, IssueCode::invalid_dimensions, offset,
                              "PNG width and height must be nonzero");
                }
            }
        }
        if (idat) {
            ++idat_count;
        }
        if (iend) {
            ++iend_count;
            if (length != 0) {
                add_issue(result, IssueCode::invalid_iend_length, offset, "IEND length must be zero");
            }
        }

        offset = crc_offset + 4;
        if (iend) {
            if (offset != bytes.size()) {
                add_issue(result, IssueCode::trailing_data, offset,
                          "Unexpected bytes follow IEND");
            }
            break;
        }
    }

    if (ihdr_count == 0) {
        add_issue(result, IssueCode::missing_ihdr, signature.size(), "PNG is missing IHDR");
    }
    if (idat_count == 0) {
        add_issue(result, IssueCode::missing_idat, signature.size(), "PNG is missing IDAT");
    }
    if (iend_count == 0) {
        add_issue(result, IssueCode::missing_iend, bytes.size(), "PNG is missing IEND");
    }

    result.parsing_completed = !truncated;
    result.structurally_valid = result.parsing_completed && result.issues.empty();
    return result;
}

}  // namespace shardrecover::png
