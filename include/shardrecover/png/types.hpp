#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace shardrecover::png {

enum class IssueCode {
    invalid_signature,
    truncated_chunk_header,
    truncated_chunk,
    invalid_chunk_type,
    ihdr_not_first,
    missing_ihdr,
    duplicate_ihdr,
    invalid_ihdr_length,
    invalid_dimensions,
    missing_idat,
    missing_iend,
    duplicate_iend,
    invalid_iend_length,
    trailing_data,
    crc_mismatch,
    invalid_compression_method,
    invalid_filter_method,
    invalid_interlace_method,
    invalid_color_type_bit_depth,
    nonconsecutive_idat,
};

struct AnalysisIssue {
    IssueCode code;
    std::size_t offset;
    std::string message;
};

struct ChunkView {
    std::size_t offset;
    std::uint32_t length;
    std::array<char, 4> type;
    std::span<const std::byte> data;
    std::uint32_t stored_crc;
    std::uint32_t computed_crc;
    bool crc_valid;
};

struct Ihdr {
    std::uint32_t width;
    std::uint32_t height;
    std::uint8_t bit_depth;
    std::uint8_t color_type;
    std::uint8_t compression_method;
    std::uint8_t filter_method;
    std::uint8_t interlace_method;
};

struct AnalysisResult {
    bool signature_valid = false;
    bool parsing_completed = false;
    bool structurally_valid = false;
    bool semantically_valid = false;
    std::size_t valid_crc_count = 0;
    std::size_t invalid_crc_count = 0;
    bool all_crcs_valid = false;
    std::vector<ChunkView> chunks;
    std::optional<Ihdr> ihdr;
    std::vector<AnalysisIssue> issues;
};

}  // namespace shardrecover::png
