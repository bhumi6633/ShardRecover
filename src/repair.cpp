#include "shardrecover/repair.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace shardrecover {
namespace {

std::vector<std::size_t> infer_starts(const ReconstructionResult& baseline,
                                      std::span<const BinaryFile> fragments)
{
    std::vector<std::size_t> starts;
    starts.reserve(baseline.steps.size());
    if (baseline.steps.empty()) {
        return starts;
    }

    starts.push_back(0);
    for (std::size_t index = 1; index < baseline.steps.size(); ++index) {
        const auto previous_id = baseline.steps[index - 1].node_id;
        if (previous_id >= fragments.size()) {
            throw std::invalid_argument("Repair path contains an invalid fragment ID");
        }
        const auto previous_size = fragments[previous_id].size();
        const auto overlap = baseline.steps[index].overlap_from_previous;
        if (overlap > previous_size) {
            throw std::invalid_argument("Repair path overlap exceeds previous fragment size");
        }
        const auto advance = previous_size - overlap;
        if (starts.back() > std::numeric_limits<std::size_t>::max() - advance) {
            throw std::overflow_error("Repaired fragment placement overflows size_t");
        }
        starts.push_back(starts.back() + advance);
    }
    return starts;
}

}  // namespace

RepairResult ConsensusRepairer::repair(const ReconstructionResult& baseline,
                                       std::span<const BinaryFile> fragments)
{
    RepairResult result;
    result.bytes = baseline.bytes;
    const auto starts = infer_starts(baseline, fragments);
    if (starts.size() != baseline.steps.size()) {
        throw std::logic_error("Repair placement count does not match reconstruction path");
    }

    for (std::size_t index = 0; index < baseline.steps.size(); ++index) {
        const auto node_id = baseline.steps[index].node_id;
        if (node_id >= fragments.size()) {
            throw std::invalid_argument("Repair path contains an invalid fragment ID");
        }
        const auto size = fragments[node_id].size();
        if (starts[index] > result.bytes.size() || size > result.bytes.size() - starts[index]) {
            throw std::invalid_argument("Repair fragment placement exceeds baseline output");
        }
    }

    for (std::size_t position = 0; position < result.bytes.size(); ++position) {
        std::array<std::size_t, 256> counts{};
        std::size_t total = 0;
        for (std::size_t index = 0; index < baseline.steps.size(); ++index) {
            const auto start = starts[index];
            const auto fragment = fragments[baseline.steps[index].node_id].bytes();
            if (position < start || position - start >= fragment.size()) {
                continue;
            }
            const auto value = fragment[position - start];
            ++counts[std::to_integer<std::size_t>(value)];
            ++total;
        }
        if (total < 2) {
            continue;
        }
        ++result.redundant_positions;

        std::size_t distinct = 0;
        std::size_t winning_value = 0;
        std::size_t winning_count = 0;
        for (std::size_t value = 0; value < counts.size(); ++value) {
            if (counts[value] == 0) {
                continue;
            }
            ++distinct;
            if (counts[value] > winning_count) {
                winning_value = value;
                winning_count = counts[value];
            }
        }
        if (distinct > 1) {
            ++result.conflicting_positions;
        }

        if (winning_count > total / 2) {
            ++result.corroborated_positions;
            const auto consensus = static_cast<std::byte>(winning_value);
            if (consensus != result.bytes[position]) {
                result.repairs.push_back(ByteRepair{position,
                                                    result.bytes[position],
                                                    consensus,
                                                    winning_count,
                                                    total});
                result.bytes[position] = consensus;
            }
        } else if (distinct > 1) {
            AmbiguousByte ambiguity{position, result.bytes[position], total, {}};
            ambiguity.votes.reserve(distinct);
            for (std::size_t value = 0; value < counts.size(); ++value) {
                if (counts[value] != 0) {
                    ambiguity.votes.push_back(
                        ByteVote{static_cast<std::byte>(value), counts[value]});
                }
            }
            result.ambiguities.push_back(std::move(ambiguity));
        }
    }
    return result;
}

}  // namespace shardrecover
