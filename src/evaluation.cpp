#include "shardrecover/evaluation.hpp"

#include "shardrecover/png/analyzer.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace shardrecover::evaluation {
namespace {

std::int64_t checked_delta(std::size_t reconstructed, std::size_t original)
{
    const auto limit = static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max());
    if (reconstructed > limit || original > limit) {
        throw std::overflow_error("Recovery size delta exceeds int64_t");
    }
    return static_cast<std::int64_t>(reconstructed) - static_cast<std::int64_t>(original);
}

}  // namespace

std::optional<double> RecoveryMetrics::surviving_original_recall() const noexcept
{
    if (surviving_originals_total == 0) return std::nullopt;
    return static_cast<double>(surviving_originals_represented)
           / static_cast<double>(surviving_originals_total);
}

std::optional<double> RecoveryMetrics::noise_selection_rate() const noexcept
{
    if (emitted_noise == 0) return std::nullopt;
    return static_cast<double>(selected_noise) / static_cast<double>(emitted_noise);
}

std::optional<double> RecoveryMetrics::relative_order_accuracy() const noexcept
{
    if (comparable_original_pairs == 0) return std::nullopt;
    return static_cast<double>(ordered_original_pairs_correct)
           / static_cast<double>(comparable_original_pairs);
}

RecoveryMetrics RecoveryEvaluator::evaluate(
    std::span<const std::byte> original,
    std::span<const Fragment> original_fragments,
    const DamageResult& damage,
    std::span<const EmittedFragmentTruth> emissions,
    const FragmentGraph& graph,
    const ReconstructionResult& selected_path,
    std::span<const std::byte> final_output,
    std::size_t repair_events,
    bool analyze_png)
{
    if (emissions.size() != graph.node_count()) {
        throw std::invalid_argument("Emission truth and graph node counts do not match");
    }
    RecoveryMetrics metrics;
    metrics.exact_recovery = std::equal(original.begin(), original.end(),
                                        final_output.begin(), final_output.end());
    metrics.original_bytes = original.size();
    metrics.reconstructed_bytes = final_output.size();
    metrics.size_delta = checked_delta(final_output.size(), original.size());
    metrics.surviving_originals_total = damage.surviving_original_ids.size();
    metrics.emitted_noise = damage.noise_fragment_ids.size();
    metrics.repair_events = repair_events;
    if (analyze_png) metrics.png_analysis = png::Analyzer::analyze(final_output);

    std::unordered_map<std::size_t, const Fragment*> originals;
    for (const auto& fragment : original_fragments) originals.emplace(fragment.index, &fragment);
    std::unordered_set<std::size_t> surviving(damage.surviving_original_ids.begin(),
                                               damage.surviving_original_ids.end());
    std::unordered_map<std::size_t, std::size_t> duplicate_sources;
    for (const auto& duplicate : damage.duplicates) {
        duplicate_sources.emplace(duplicate.dataset_id, duplicate.source_fragment_id);
    }
    std::unordered_set<std::size_t> noise(damage.noise_fragment_ids.begin(),
                                          damage.noise_fragment_ids.end());
    std::unordered_map<std::string, std::size_t> emitted_by_path;
    for (const auto& emission : emissions) {
        emitted_by_path.emplace(emission.path.generic_string(), emission.dataset_id);
    }

    struct SelectedOriginal { std::size_t source_id; std::size_t offset; std::size_t size; };
    std::vector<SelectedOriginal> selected_originals;
    std::unordered_set<std::size_t> represented;
    for (const auto& step : selected_path.steps) {
        if (step.node_id >= graph.node_count()) {
            throw std::invalid_argument("Selected path contains an invalid graph node");
        }
        const auto found = emitted_by_path.find(graph.nodes()[step.node_id].path.generic_string());
        if (found == emitted_by_path.end()) {
            throw std::invalid_argument("Selected graph node has no emission truth");
        }
        const auto dataset_id = found->second;
        if (noise.contains(dataset_id)) {
            ++metrics.selected_noise;
            continue;
        }
        if (const auto duplicate = duplicate_sources.find(dataset_id);
            duplicate != duplicate_sources.end()) {
            ++metrics.selected_duplicates;
            represented.insert(duplicate->second);
            continue;
        }
        if (!surviving.contains(dataset_id)) {
            throw std::invalid_argument("Selected fragment has an unknown ground-truth role");
        }
        ++metrics.selected_genuine_originals;
        represented.insert(dataset_id);
        const auto source = originals.find(dataset_id);
        if (source == originals.end()) throw std::invalid_argument("Missing original fragment truth");
        selected_originals.push_back(
            {dataset_id, source->second->offset, source->second->data.size()});
    }
    for (const auto id : represented) {
        if (surviving.contains(id)) ++metrics.surviving_originals_represented;
    }

    for (std::size_t left = 0; left < selected_originals.size(); ++left) {
        for (std::size_t right = left + 1; right < selected_originals.size(); ++right) {
            if (selected_originals[left].source_id == selected_originals[right].source_id
                || selected_originals[left].offset == selected_originals[right].offset) continue;
            ++metrics.comparable_original_pairs;
            if (selected_originals[left].offset < selected_originals[right].offset) {
                ++metrics.ordered_original_pairs_correct;
            }
        }
    }

    for (std::size_t index = 1; index < selected_path.steps.size(); ++index) {
        const auto& previous_step = selected_path.steps[index - 1];
        const auto& current_step = selected_path.steps[index];
        const auto previous_id = emitted_by_path.at(
            graph.nodes()[previous_step.node_id].path.generic_string());
        const auto current_id = emitted_by_path.at(
            graph.nodes()[current_step.node_id].path.generic_string());
        if (!surviving.contains(previous_id) || !surviving.contains(current_id)) continue;
        ++metrics.selected_genuine_joins;
        const auto* previous = originals.at(previous_id);
        const auto* current = originals.at(current_id);
        const auto expected = previous->offset + previous->data.size()
                              - current_step.overlap_from_previous;
        if (current->offset == expected) ++metrics.ground_truth_consistent_joins;
        else ++metrics.inconsistent_joins;
    }
    return metrics;
}

void AggregateMetrics::add(const RecoveryMetrics& metrics) noexcept
{
    ++runs;
    if (metrics.exact_recovery) ++exact_recoveries;
    selected_noise_sum += metrics.selected_noise;
    if (const auto recall = metrics.surviving_original_recall()) {
        recall_sum += *recall;
        ++recall_runs;
    }
    if (const auto order = metrics.relative_order_accuracy()) {
        order_sum += *order;
        ++order_runs;
    }
}

std::optional<double> AggregateMetrics::mean_recall() const noexcept
{
    return recall_runs == 0 ? std::nullopt
                            : std::optional<double>{recall_sum / recall_runs};
}

std::optional<double> AggregateMetrics::mean_order_accuracy() const noexcept
{
    return order_runs == 0 ? std::nullopt
                           : std::optional<double>{order_sum / order_runs};
}

double AggregateMetrics::mean_selected_noise() const noexcept
{
    return runs == 0 ? 0.0 : static_cast<double>(selected_noise_sum) / runs;
}

}  // namespace shardrecover::evaluation
