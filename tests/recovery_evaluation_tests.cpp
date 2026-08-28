#include "shardrecover/binary_file.hpp"
#include "shardrecover/damage.hpp"
#include "shardrecover/evaluation.hpp"
#include "shardrecover/fragment_generator.hpp"
#include "shardrecover/fragment_graph.hpp"
#include "shardrecover/reconstruction.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace shardrecover;

struct TestFailure : std::runtime_error { using std::runtime_error::runtime_error; };
#define CHECK(condition) do { if (!(condition)) throw TestFailure(std::string("CHECK failed: ") + #condition); } while (false)

class TempDirectory {
public:
    TempDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
                / ("shardrecover-evaluation-" + std::to_string(stamp));
        std::filesystem::create_directory(path_);
    }
    ~TempDirectory() { std::error_code error; std::filesystem::remove_all(path_, error); }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

std::vector<std::byte> bytes(std::size_t count)
{
    std::vector<std::byte> result(count);
    std::mt19937_64 engine(0x5348415244ULL);
    for (auto& value : result) value = std::byte(engine() & 0xffU);
    return result;
}

void write(const std::filesystem::path& path, std::span<const std::byte> data)
{
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!output) throw TestFailure("could not write fixture");
}

void test_clean_pipeline()
{
    const auto original = bytes(1024);
    const auto fragments = FragmentGenerator::generate(original, 128, 64);
    const auto damage = DamageSimulator::apply(fragments, DamageConfig{.seed = 41});
    TempDirectory temp;
    std::vector<evaluation::EmittedFragmentTruth> truth;
    for (std::size_t i = 0; i < damage.fragments.size(); ++i) {
        const auto path = temp.path() / ("opaque-" + std::to_string((i * 11) % 17) + ".bin");
        write(path, damage.fragments[i].data);
        truth.push_back({path, damage.fragments[i].index});
    }
    std::reverse(truth.begin(), truth.end());
    std::vector<BinaryFile> loaded;
    for (const auto& item : truth) loaded.push_back(BinaryFile::load(item.path));
    GraphBuildConfig config{64, 0, GraphBuildStrategy::indexed, 2};
    const auto graph = FragmentGraph::build(loaded, config);
    const auto recovered = BeamReconstructor::reconstruct(graph, loaded, 8).selected;
    const auto metrics = evaluation::RecoveryEvaluator::evaluate(
        original, fragments, damage, truth, graph, recovered, recovered.bytes, 0, false);
    CHECK(metrics.exact_recovery);
    CHECK(metrics.surviving_originals_represented == fragments.size());
    CHECK(metrics.surviving_original_recall() == 1.0);
    CHECK(metrics.relative_order_accuracy() == 1.0);
    CHECK(metrics.selected_noise == 0);
    CHECK(metrics.inconsistent_joins == 0);
}

void test_roles_order_and_size_metrics()
{
    const auto original = bytes(16);
    const auto fragments = FragmentGenerator::generate(original, 4, 0);
    DamageResult damage;
    damage.fragments = fragments;
    damage.original_fragment_count = 4;
    damage.surviving_original_ids = {0, 1, 2, 3};
    damage.fragments.push_back(Fragment{4, 0, fragments[0].data});
    damage.duplicates.push_back({4, 0});
    damage.fragments.push_back(Fragment{5, 0, bytes(4)});
    damage.noise_fragment_ids.push_back(5);

    TempDirectory temp;
    std::vector<BinaryFile> loaded;
    std::vector<evaluation::EmittedFragmentTruth> truth;
    for (std::size_t i = 0; i < damage.fragments.size(); ++i) {
        const auto path = temp.path() / (std::to_string(i) + ".bin");
        write(path, damage.fragments[i].data);
        loaded.push_back(BinaryFile::load(path));
        truth.push_back({path, i});
    }
    const auto graph = FragmentGraph::build(loaded, 1);
    ReconstructionResult selected;
    for (const auto id : {0U, 2U, 1U, 3U, 4U, 5U}) {
        selected.steps.push_back(ReconstructionStep{.node_id = id,
                                                    .overlap_from_previous = 0});
    }
    auto wrong = original;
    wrong.back() ^= std::byte{1};
    const auto metrics = evaluation::RecoveryEvaluator::evaluate(
        original, fragments, damage, truth, graph, selected, wrong, 2, false);
    CHECK(!metrics.exact_recovery);
    CHECK(metrics.size_delta == 0);
    CHECK(metrics.selected_genuine_originals == 4);
    CHECK(metrics.selected_duplicates == 1);
    CHECK(metrics.selected_noise == 1);
    CHECK(metrics.surviving_originals_represented == 4);
    CHECK(metrics.ordered_original_pairs_correct == 5);
    CHECK(metrics.comparable_original_pairs == 6);
    CHECK(metrics.repair_events == 2);

    wrong.pop_back();
    const auto shorter = evaluation::RecoveryEvaluator::evaluate(
        original, fragments, damage, truth, graph, selected, wrong, 0, false);
    CHECK(shorter.size_delta == -1);
}

void test_empty_and_aggregate_metrics()
{
    DamageResult damage;
    FragmentGraph graph = FragmentGraph::build(std::span<const BinaryFile>{}, 1);
    ReconstructionResult selected;
    const auto metrics = evaluation::RecoveryEvaluator::evaluate(
        {}, {}, damage, {}, graph, selected, {}, 0, false);
    CHECK(metrics.exact_recovery);
    CHECK(!metrics.surviving_original_recall());
    CHECK(!metrics.relative_order_accuracy());
    CHECK(!metrics.noise_selection_rate());

    evaluation::AggregateMetrics aggregate;
    aggregate.add(metrics);
    auto failed = metrics;
    failed.exact_recovery = false;
    failed.surviving_originals_total = 2;
    failed.surviving_originals_represented = 1;
    failed.comparable_original_pairs = 4;
    failed.ordered_original_pairs_correct = 3;
    failed.selected_noise = 2;
    aggregate.add(failed);
    CHECK(aggregate.runs == 2);
    CHECK(aggregate.exact_recoveries == 1);
    CHECK(aggregate.mean_recall() == 0.5);
    CHECK(aggregate.mean_order_accuracy() == 0.75);
    CHECK(aggregate.mean_selected_noise() == 1.0);
}

}  // namespace

int main()
{
    const std::vector<std::pair<std::string, void(*)()>> tests = {
        {"clean shuffled opaque pipeline", test_clean_pipeline},
        {"roles, order, and size metrics", test_roles_order_and_size_metrics},
        {"empty and aggregate metrics", test_empty_and_aggregate_metrics},
    };
    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS: " << name << '\n'; }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
