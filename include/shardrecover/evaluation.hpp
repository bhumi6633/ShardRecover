#pragma once

#include "shardrecover/damage.hpp"
#include "shardrecover/fragment_graph.hpp"
#include "shardrecover/png/types.hpp"
#include "shardrecover/reconstruction.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace shardrecover::evaluation {

// This metadata belongs exclusively to evaluation code after reconstruction completes.
struct EmittedFragmentTruth {
    std::filesystem::path path;
    std::size_t dataset_id;
};

struct RecoveryMetrics {
    bool exact_recovery = false;
    std::size_t original_bytes = 0;
    std::size_t reconstructed_bytes = 0;
    std::int64_t size_delta = 0;

    std::size_t surviving_originals_represented = 0;
    std::size_t surviving_originals_total = 0;
    std::size_t selected_genuine_originals = 0;
    std::size_t selected_duplicates = 0;
    std::size_t selected_noise = 0;
    std::size_t emitted_noise = 0;

    std::size_t ordered_original_pairs_correct = 0;
    std::size_t comparable_original_pairs = 0;
    std::size_t selected_genuine_joins = 0;
    std::size_t ground_truth_consistent_joins = 0;
    std::size_t inconsistent_joins = 0;

    std::size_t repair_events = 0;
    std::optional<png::AnalysisResult> png_analysis;

    std::optional<double> surviving_original_recall() const noexcept;
    std::optional<double> noise_selection_rate() const noexcept;
    std::optional<double> relative_order_accuracy() const noexcept;
};

class RecoveryEvaluator {
public:
    // Ground truth enters only here, after graph/search/repair have produced final_output.
    static RecoveryMetrics evaluate(std::span<const std::byte> original,
                                    std::span<const Fragment> original_fragments,
                                    const DamageResult& damage,
                                    std::span<const EmittedFragmentTruth> emissions,
                                    const FragmentGraph& graph,
                                    const ReconstructionResult& selected_path,
                                    std::span<const std::byte> final_output,
                                    std::size_t repair_events,
                                    bool analyze_png);
};

struct AggregateMetrics {
    std::size_t runs = 0;
    std::size_t exact_recoveries = 0;
    double recall_sum = 0;
    std::size_t recall_runs = 0;
    double order_sum = 0;
    std::size_t order_runs = 0;
    std::size_t selected_noise_sum = 0;

    void add(const RecoveryMetrics& metrics) noexcept;
    std::optional<double> mean_recall() const noexcept;
    std::optional<double> mean_order_accuracy() const noexcept;
    double mean_selected_noise() const noexcept;
};

}  // namespace shardrecover::evaluation
