#include "shardrecover/png/repair.hpp"

#include "shardrecover/png/analyzer.hpp"
#include "shardrecover/png/crc.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace shardrecover::png {
namespace {

ValidationSummary summarize(const AnalysisResult& analysis)
{
    return ValidationSummary{
        analysis.signature_valid,
        analysis.structurally_valid,
        analysis.semantically_valid,
        analysis.chunks.size(),
        analysis.valid_crc_count,
        analysis.invalid_crc_count,
        analysis.all_crcs_valid,
    };
}

bool in_range(std::size_t position, std::size_t start, std::size_t length)
{
    return position >= start && position - start < length;
}

}  // namespace

CrcRepairResult CrcGuidedRepairer::repair(const shardrecover::RepairResult& consensus)
{
    CrcRepairResult result;
    result.bytes = consensus.bytes;
    const auto initial = Analyzer::analyze(result.bytes);
    result.before = summarize(initial);

    std::vector<std::vector<std::size_t>> chunk_ambiguities(initial.chunks.size());
    for (std::size_t ambiguity_index = 0;
         ambiguity_index < consensus.ambiguities.size();
         ++ambiguity_index) {
        const auto position = consensus.ambiguities[ambiguity_index].position;
        std::optional<std::size_t> containing_chunk;
        for (std::size_t chunk_index = 0; chunk_index < initial.chunks.size(); ++chunk_index) {
            const auto& chunk = initial.chunks[chunk_index];
            const auto type_start = chunk.offset + 4;
            const auto data_start = chunk.offset + 8;
            if (in_range(position, type_start, 4)
                || in_range(position, data_start, chunk.length)) {
                containing_chunk = chunk_index;
                break;
            }
        }
        if (!containing_chunk.has_value()) {
            result.unresolved.push_back(CrcGuidedUnresolved{
                position,
                "position is outside CRC-covered PNG chunk type/data",
            });
            continue;
        }
        ++result.eligible_ambiguous_bytes;
        chunk_ambiguities[*containing_chunk].push_back(ambiguity_index);
    }

    for (std::size_t chunk_index = 0; chunk_index < initial.chunks.size(); ++chunk_index) {
        const auto& ambiguity_indices = chunk_ambiguities[chunk_index];
        if (ambiguity_indices.empty()) {
            continue;
        }
        if (ambiguity_indices.size() != 1) {
            for (const auto ambiguity_index : ambiguity_indices) {
                result.unresolved.push_back(CrcGuidedUnresolved{
                    consensus.ambiguities[ambiguity_index].position,
                    "multiple ambiguous positions occur in the same PNG chunk",
                });
            }
            continue;
        }

        ++result.chunks_evaluated;
        const auto& ambiguity = consensus.ambiguities[ambiguity_indices.front()];
        const auto& chunk = initial.chunks[chunk_index];
        std::vector<std::byte> matching_values;
        for (const auto& vote : ambiguity.votes) {
            auto type = chunk.type;
            std::vector<std::byte> data(chunk.data.begin(), chunk.data.end());
            const auto type_start = chunk.offset + 4;
            const auto data_start = chunk.offset + 8;
            if (in_range(ambiguity.position, type_start, 4)) {
                type[ambiguity.position - type_start] = static_cast<char>(
                    std::to_integer<unsigned char>(vote.value));
            } else {
                data[ambiguity.position - data_start] = vote.value;
            }
            if (compute_chunk_crc(type, data) == chunk.stored_crc) {
                matching_values.push_back(vote.value);
            }
        }

        if (matching_values.size() != 1) {
            result.unresolved.push_back(CrcGuidedUnresolved{
                ambiguity.position,
                matching_values.empty() ? "no observed candidate matches the stored PNG CRC"
                                        : "multiple observed candidates match the stored PNG CRC",
            });
            continue;
        }

        const auto previous = result.bytes[ambiguity.position];
        const auto selected = matching_values.front();
        result.bytes[ambiguity.position] = selected;
        result.repairs.push_back(CrcGuidedRepair{
            ambiguity.position,
            previous,
            selected,
            chunk.stored_crc,
            chunk.stored_crc,
            ambiguity.votes.size(),
            previous != selected,
        });
    }

    result.after = summarize(Analyzer::analyze(result.bytes));
    return result;
}

}  // namespace shardrecover::png
