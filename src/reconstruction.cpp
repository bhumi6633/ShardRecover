#include "shardrecover/reconstruction.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

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

std::vector<std::size_t> select_beam_start_nodes(const FragmentGraph& graph)
{
    std::vector<std::size_t> starts;
    for (std::size_t node_id = 0; node_id < graph.node_count(); ++node_id) {
        if (graph.incoming_edges(node_id).empty()) {
            starts.push_back(node_id);
        }
    }
    if (starts.empty() && graph.node_count() != 0) {
        starts.push_back(select_start_node(graph));
    }
    return starts;
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

ReconstructionResult materialize_result(std::vector<ReconstructionStep> steps,
                                        std::span<const BinaryFile> fragments,
                                        bool complete)
{
    ReconstructionResult result;
    result.steps = std::move(steps);
    result.complete = complete;
    if (result.steps.empty()) {
        return result;
    }

    const auto first = fragments[result.steps.front().node_id].bytes();
    result.bytes.assign(first.begin(), first.end());
    for (std::size_t index = 1; index < result.steps.size(); ++index) {
        const auto& step = result.steps[index];
        const auto fragment = fragments[step.node_id].bytes();
        if (step.overlap_from_previous > fragment.size()) {
            throw std::logic_error("Graph overlap exceeds destination fragment size");
        }

        const auto extension = fragment.subspan(step.overlap_from_previous);
        result.bytes.insert(result.bytes.end(), extension.begin(), extension.end());
        result.total_overlap_bytes += step.overlap_from_previous;
        result.overlap_mismatches += step.mismatches;
        if (!step.exact) {
            ++result.approximate_joins;
        }
    }
    return result;
}

struct BeamState {
    std::vector<ReconstructionStep> steps;
    std::vector<bool> used;
    std::size_t total_overlap = 0;
    std::size_t total_mismatches = 0;
};

bool state_path_less(const FragmentGraph& graph, const BeamState& left, const BeamState& right)
{
    return std::lexicographical_compare(
        left.steps.begin(), left.steps.end(), right.steps.begin(), right.steps.end(),
        [&](const auto& left_step, const auto& right_step) {
            if (left_step.node_id == right_step.node_id) {
                return false;
            }
            return path_less(graph, left_step.node_id, right_step.node_id);
        });
}

bool state_better(const FragmentGraph& graph,
                  const BeamState& left,
                  const BeamState& right,
                  std::size_t node_count)
{
    const bool left_complete = left.steps.size() == node_count;
    const bool right_complete = right.steps.size() == node_count;
    if (left_complete != right_complete) {
        return left_complete;
    }
    if (left.steps.size() != right.steps.size()) {
        return left.steps.size() > right.steps.size();
    }
    if (left.total_overlap != right.total_overlap) {
        return left.total_overlap > right.total_overlap;
    }
    if (left.total_mismatches != right.total_mismatches) {
        return left.total_mismatches < right.total_mismatches;
    }
    return state_path_less(graph, left, right);
}

std::size_t recovered_size(const BeamState& state, std::span<const BinaryFile> fragments)
{
    if (state.steps.empty()) {
        return 0;
    }

    std::size_t size = fragments[state.steps.front().node_id].size();
    for (std::size_t index = 1; index < state.steps.size(); ++index) {
        const auto& step = state.steps[index];
        const auto fragment_size = fragments[step.node_id].size();
        if (step.overlap_from_previous > fragment_size) {
            throw std::logic_error("Graph overlap exceeds destination fragment size");
        }
        size += fragment_size - step.overlap_from_previous;
    }
    return size;
}

bool evidence_better(const FragmentGraph& graph,
                     const ReconstructionCandidateResult& left,
                     const ReconstructionCandidateResult& right)
{
    if (left.complete != right.complete) {
        return left.complete;
    }
    if (left.evidence.format.has_value() && right.evidence.format.has_value()
        && left.evidence.format->ranking_keys != right.evidence.format->ranking_keys) {
        return std::lexicographical_compare(
            right.evidence.format->ranking_keys.begin(), right.evidence.format->ranking_keys.end(),
            left.evidence.format->ranking_keys.begin(), left.evidence.format->ranking_keys.end());
    }
    if (left.steps.size() != right.steps.size()) {
        return left.steps.size() > right.steps.size();
    }
    if (left.total_overlap_bytes != right.total_overlap_bytes) {
        return left.total_overlap_bytes > right.total_overlap_bytes;
    }

    if (left.overlap_mismatches != right.overlap_mismatches) {
        return left.overlap_mismatches < right.overlap_mismatches;
    }
    const BeamState left_state{left.steps, {}, left.total_overlap_bytes, left.overlap_mismatches};
    const BeamState right_state{right.steps, {}, right.total_overlap_bytes, right.overlap_mismatches};
    return state_path_less(graph, left_state, right_state);
}

}  // namespace

ReconstructionResult GreedyReconstructor::reconstruct(const FragmentGraph& graph,
                                                       std::span<const BinaryFile> fragments)
{
    validate_inputs(graph, fragments);

    if (fragments.empty()) {
        ReconstructionResult result;
        result.complete = true;
        return result;
    }

    std::vector<bool> used(fragments.size(), false);
    auto current = select_start_node(graph);
    used[current] = true;
    std::vector<ReconstructionStep> steps;
    steps.push_back(ReconstructionStep{current, 0, 0, 0, true, {}});

    while (steps.size() < fragments.size()) {
        const auto outgoing = graph.outgoing_edges(current);
        const auto next = std::find_if(outgoing.begin(), outgoing.end(), [&](const auto& edge) {
            return !used[edge.to];
        });
        if (next == outgoing.end()) {
            break;
        }

        current = next->to;
        used[current] = true;
        steps.push_back(ReconstructionStep{current,
                                           next->overlap,
                                           next->matches,
                                           next->mismatches,
                                           next->exact,
                                           next->mismatch_details});
    }

    const bool complete = steps.size() == fragments.size();
    return materialize_result(std::move(steps), fragments, complete);
}

BeamReconstructionResult BeamReconstructor::reconstruct(const FragmentGraph& graph,
                                                         std::span<const BinaryFile> fragments,
                                                         std::size_t beam_width,
                                                         const CandidateEvaluator* evaluator)
{
    validate_inputs(graph, fragments);
    if (beam_width == 0) {
        throw std::invalid_argument("Beam width must be greater than zero");
    }

    BeamReconstructionResult result;
    if (fragments.empty()) {
        result.selected.complete = true;
        ReconstructionCandidateResult candidate;
        candidate.complete = true;
        candidate.evidence.complete = true;
        if (evaluator != nullptr) {
            candidate.evidence.format = evaluator->evaluate({});
        }
        result.candidates.push_back(std::move(candidate));
        return result;
    }

    std::vector<BeamState> beam;
    for (const auto node_id : select_beam_start_nodes(graph)) {
        BeamState state;
        state.steps.push_back(ReconstructionStep{node_id, 0, 0, 0, true, {}});
        state.used.assign(fragments.size(), false);
        state.used[node_id] = true;
        beam.push_back(std::move(state));
        ++result.statistics.states_generated;
    }

    std::sort(beam.begin(), beam.end(), [&](const auto& left, const auto& right) {
        return state_better(graph, left, right, fragments.size());
    });
    if (beam.size() > beam_width) {
        beam.resize(beam_width);
    }
    result.statistics.maximum_beam_size = beam.size();

    std::vector<BeamState> finals;
    while (!beam.empty()) {
        std::vector<BeamState> expanded;
        bool found_complete = false;

        for (const auto& state : beam) {
            ++result.statistics.states_expanded;
            bool generated_child = false;
            const auto current = state.steps.back().node_id;
            for (const auto& edge : graph.outgoing_edges(current)) {
                if (state.used[edge.to]) {
                    continue;
                }

                generated_child = true;
                BeamState child = state;
                child.steps.push_back(ReconstructionStep{edge.to,
                                                         edge.overlap,
                                                         edge.matches,
                                                         edge.mismatches,
                                                         edge.exact,
                                                         edge.mismatch_details});
                child.used[edge.to] = true;
                child.total_overlap += edge.overlap;
                child.total_mismatches += edge.mismatches;
                ++result.statistics.states_generated;

                if (child.steps.size() == fragments.size()) {
                    ++result.statistics.complete_candidates_found;
                    found_complete = true;
                    finals.push_back(std::move(child));
                } else {
                    expanded.push_back(std::move(child));
                }
            }

            if (!generated_child) {
                if (state.steps.size() == fragments.size()) {
                    ++result.statistics.complete_candidates_found;
                    found_complete = true;
                }
                finals.push_back(state);
            }
        }

        if (found_complete) {
            break;
        }

        std::sort(expanded.begin(), expanded.end(), [&](const auto& left, const auto& right) {
            return state_better(graph, left, right, fragments.size());
        });
        if (expanded.size() > beam_width) {
            expanded.resize(beam_width);
        }
        beam = std::move(expanded);
        result.statistics.maximum_beam_size = std::max(result.statistics.maximum_beam_size,
                                                       beam.size());
    }

    if (finals.empty()) {
        throw std::logic_error("Beam search produced no reconstruction candidates");
    }

    result.candidates.reserve(finals.size());
    for (const auto& candidate : finals) {
        ReconstructionCandidateResult summary;
        summary.steps = candidate.steps;
        summary.total_overlap_bytes = candidate.total_overlap;
        summary.overlap_mismatches = candidate.total_mismatches;
        summary.approximate_joins = static_cast<std::size_t>(std::count_if(
            summary.steps.begin(), summary.steps.end(), [](const auto& step) {
                return !step.exact;
            }));
        summary.recovered_size = recovered_size(candidate, fragments);
        summary.complete = candidate.steps.size() == fragments.size();
        summary.evidence.complete = summary.complete;
        summary.evidence.fragments_used = summary.steps.size();
        summary.evidence.total_overlap = summary.total_overlap_bytes;
        if (evaluator != nullptr) {
            const auto materialized = materialize_result(candidate.steps, fragments, summary.complete);
            summary.evidence.format = evaluator->evaluate(materialized.bytes);
        }
        result.candidates.push_back(std::move(summary));
    }

    std::sort(result.candidates.begin(), result.candidates.end(), [&](const auto& left,
                                                                      const auto& right) {
        return evidence_better(graph, left, right);
    });

    const auto& selected = result.candidates.front();
    result.selected = materialize_result(selected.steps,
                                         fragments,
                                         selected.complete);
    return result;
}

}  // namespace shardrecover
