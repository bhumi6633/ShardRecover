#include "shardrecover/reconstruction.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace shardrecover {
namespace {

std::size_t strongest_outgoing_overlap(const FragmentGraph& graph, std::size_t node_id)
{
    const auto outgoing = graph.outgoing_edges(node_id);
    return outgoing.empty() ? 0 : outgoing.front().overlap;
}

bool path_less(const FragmentGraph& graph, std::size_t left, std::size_t right)
{
    const auto& left_path = graph.nodes()[left].path;
    const auto& right_path = graph.nodes()[right].path;
    if (left_path != right_path) {
        return left_path.generic_string() < right_path.generic_string();
    }
    return left < right;
}

std::size_t select_start_node(const FragmentGraph& graph)
{
    std::vector<std::size_t> candidates;
    for (std::size_t node_id = 0; node_id < graph.node_count(); ++node_id) {
        if (graph.incoming_edges(node_id).empty()) {
            candidates.push_back(node_id);
        }
    }

    if (!candidates.empty()) {
        return *std::min_element(candidates.begin(), candidates.end(), [&](auto left, auto right) {
            const auto left_strength = strongest_outgoing_overlap(graph, left);
            const auto right_strength = strongest_outgoing_overlap(graph, right);
            if (left_strength != right_strength) {
                return left_strength > right_strength;
            }
            return path_less(graph, left, right);
        });
    }

    std::size_t selected = 0;
    std::size_t selected_incoming_count = std::numeric_limits<std::size_t>::max();
    std::size_t selected_incoming_overlap = std::numeric_limits<std::size_t>::max();
    for (std::size_t node_id = 0; node_id < graph.node_count(); ++node_id) {
        const auto incoming = graph.incoming_edges(node_id);
        std::size_t incoming_overlap = 0;
        for (const auto& edge : incoming) {
            incoming_overlap += edge.overlap;
        }

        const bool better_count = incoming.size() < selected_incoming_count;
        const bool equal_count = incoming.size() == selected_incoming_count;
        const bool better_overlap = equal_count && incoming_overlap < selected_incoming_overlap;
        const bool equal_evidence = equal_count && incoming_overlap == selected_incoming_overlap;
        const auto outgoing_strength = strongest_outgoing_overlap(graph, node_id);
        const auto selected_strength = strongest_outgoing_overlap(graph, selected);
        const bool better_outgoing = equal_evidence && outgoing_strength > selected_strength;
        const bool equal_strength = equal_evidence && outgoing_strength == selected_strength;

        if (better_count || better_overlap || better_outgoing
            || (equal_strength && path_less(graph, node_id, selected))) {
            selected = node_id;
            selected_incoming_count = incoming.size();
            selected_incoming_overlap = incoming_overlap;
        }
    }
    return selected;
}

void validate_inputs(const FragmentGraph& graph, std::span<const BinaryFile> fragments)
{
    if (graph.node_count() != fragments.size()) {
        throw std::invalid_argument("Fragment graph and payload count do not match");
    }
    for (std::size_t node_id = 0; node_id < graph.node_count(); ++node_id) {
        const auto& node = graph.nodes()[node_id];
        if (node.id != node_id || node.path != fragments[node_id].path()
            || node.size != fragments[node_id].size()) {
            throw std::invalid_argument("Fragment graph does not match supplied payloads");
        }
    }
}

}  // namespace

ReconstructionResult GreedyReconstructor::reconstruct(const FragmentGraph& graph,
                                                       std::span<const BinaryFile> fragments)
{
    validate_inputs(graph, fragments);

    ReconstructionResult result;
    if (fragments.empty()) {
        result.complete = true;
        return result;
    }

    std::vector<bool> used(fragments.size(), false);
    auto current = select_start_node(graph);
    used[current] = true;
    result.steps.push_back(ReconstructionStep{current, 0});
    const auto first_bytes = fragments[current].bytes();
    result.bytes.assign(first_bytes.begin(), first_bytes.end());

    while (result.steps.size() < fragments.size()) {
        const auto outgoing = graph.outgoing_edges(current);
        const auto next = std::find_if(outgoing.begin(), outgoing.end(), [&](const auto& edge) {
            return !used[edge.to];
        });
        if (next == outgoing.end()) {
            break;
        }

        const auto next_bytes = fragments[next->to].bytes();
        if (next->overlap > next_bytes.size()) {
            throw std::logic_error("Graph overlap exceeds destination fragment size");
        }

        const auto extension = next_bytes.subspan(next->overlap);
        result.bytes.insert(result.bytes.end(), extension.begin(), extension.end());
        result.total_overlap_bytes += next->overlap;
        current = next->to;
        used[current] = true;
        result.steps.push_back(ReconstructionStep{current, next->overlap});
    }

    result.complete = result.steps.size() == fragments.size();
    return result;
}

}  // namespace shardrecover
