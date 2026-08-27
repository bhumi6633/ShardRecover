#include "shardrecover/png/reconstruction_evaluator.hpp"

#include "shardrecover/png/analyzer.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace shardrecover::png {
namespace {

bool is_type(const ChunkView& chunk, std::string_view expected)
{
    return std::equal(chunk.type.begin(), chunk.type.end(), expected.begin(), expected.end());
}

bool has_issue(const AnalysisResult& result, IssueCode code)
{
    return std::any_of(result.issues.begin(), result.issues.end(), [&](const auto& issue) {
        return issue.code == code;
    });
}

std::int64_t bounded_count(std::size_t count)
{
    const auto maximum = static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max());
    return static_cast<std::int64_t>(std::min(count, maximum));
}

std::string validity(bool valid)
{
    return valid ? "valid" : "invalid";
}

}  // namespace

FormatEvidence ReconstructionEvaluator::evaluate(std::span<const std::byte> candidate) const
{
    const auto analysis = Analyzer::analyze(candidate);
    const bool ihdr_valid = analysis.ihdr.has_value()
                            && !has_issue(analysis, IssueCode::invalid_ihdr_length)
                            && !has_issue(analysis, IssueCode::invalid_dimensions);
    const bool idat_present = std::any_of(analysis.chunks.begin(), analysis.chunks.end(),
                                         [](const auto& chunk) { return is_type(chunk, "IDAT"); });
    const bool iend_present = std::any_of(analysis.chunks.begin(), analysis.chunks.end(),
                                         [](const auto& chunk) { return is_type(chunk, "IEND"); });
    const auto total_crcs = analysis.valid_crc_count + analysis.invalid_crc_count;

    FormatEvidence evidence;
    evidence.format = "png";
    // Priority: structure, all CRCs, fewer failures, semantics, useful parsed facts.
    evidence.ranking_keys = {
        analysis.structurally_valid ? 1 : 0,
        analysis.all_crcs_valid ? 1 : 0,
        -bounded_count(analysis.invalid_crc_count),
        analysis.semantically_valid ? 1 : 0,
        analysis.signature_valid ? 1 : 0,
        ihdr_valid ? 1 : 0,
        idat_present ? 1 : 0,
        iend_present ? 1 : 0,
        bounded_count(analysis.chunks.size()),
    };
    evidence.facts = {
        {"PNG signature", validity(analysis.signature_valid)},
        {"PNG structure", validity(analysis.structurally_valid)},
        {"PNG semantics", validity(analysis.semantically_valid)},
        {"PNG IHDR", validity(ihdr_valid)},
        {"PNG IDAT", idat_present ? "present" : "missing"},
        {"PNG IEND", iend_present ? "present" : "missing"},
        {"PNG chunks", std::to_string(analysis.chunks.size())},
        {"CRC", std::to_string(analysis.valid_crc_count) + " / "
                    + std::to_string(total_crcs) + " valid"},
        {"CRC failures", std::to_string(analysis.invalid_crc_count)},
    };
    return evidence;
}

}  // namespace shardrecover::png
