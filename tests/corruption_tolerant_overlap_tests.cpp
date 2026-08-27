#include "shardrecover/binary_file.hpp"
#include "shardrecover/damage.hpp"
#include "shardrecover/fragment_generator.hpp"
#include "shardrecover/fragment_graph.hpp"
#include "shardrecover/overlap.hpp"
#include "shardrecover/reconstruction.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<std::byte>;
using shardrecover::BinaryFile;
using shardrecover::FragmentGraph;
using shardrecover::OverlapResult;

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

Bytes text_bytes(std::string_view text)
{
    Bytes result;
    for (const auto character : text) {
        result.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return result;
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto base = std::filesystem::temp_directory_path()
                          / ("shardrecover-tolerant-tests-" + std::to_string(stamp));
        for (int suffix = 0; suffix < 100; ++suffix) {
            auto candidate = base;
            candidate += "-" + std::to_string(suffix);
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = std::move(candidate);
                return;
            }
            if (error) {
                throw std::runtime_error("Failed to create temporary directory: " + error.message());
            }
        }
        throw std::runtime_error("Failed to find temporary directory name");
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to create overlap test file");
    }
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
}

std::vector<BinaryFile> load_inputs(const std::filesystem::path& directory,
                                    std::string_view prefix,
                                    const std::vector<std::pair<std::string, Bytes>>& inputs)
{
    std::vector<BinaryFile> files;
    for (const auto& [name, bytes] : inputs) {
        const auto path = directory / (std::string(prefix) + name);
        write_file(path, bytes);
        files.push_back(BinaryFile::load(path));
    }
    return files;
}

OverlapResult tolerant(std::span<const std::byte> left,
                       std::span<const std::byte> right,
                       std::size_t minimum,
                       std::size_t budget)
{
    return shardrecover::find_tolerant_suffix_prefix_overlap(left, right, minimum, budget);
}

void test_exact_and_single_mismatch(const std::filesystem::path&)
{
    const auto left = text_bytes("ABCDEFG");
    const auto right = text_bytes("EFGHIJK");
    const auto exact = tolerant(left, right, 3, 0);
    check(exact.length == 3 && exact.matches == 3 && exact.mismatches == 0 && exact.exact,
          "exact overlap behavior changed");

    const Bytes damaged_left{std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc},
                             std::byte{0xdd}, std::byte{0xee}};
    const Bytes damaged_right{std::byte{0xaa}, std::byte{0xbb}, std::byte{0x91},
                              std::byte{0xdd}, std::byte{0xee}};
    const auto approximate = tolerant(damaged_left, damaged_right, 5, 1);
    check(approximate.length == 5 && approximate.matches == 4
              && approximate.mismatches == 1 && !approximate.exact,
          "single substitution was not detected");
    check(approximate.mismatch_details.size() == 1
              && approximate.mismatch_details[0].overlap_offset == 2
              && approximate.mismatch_details[0].left_byte == std::byte{0xcc}
              && approximate.mismatch_details[0].right_byte == std::byte{0x91},
          "single substitution location or byte values were incorrect");
}

void test_budgets_longest_and_minimum(const std::filesystem::path&)
{
    const auto left = text_bytes("XXABCDEF");
    const auto right = text_bytes("ABQDEYZZ");
    const auto two = tolerant(left, right, 4, 2);
    check(two.length == 6 && two.matches == 4 && two.mismatches == 2,
          "two substitutions within budget were not accepted");
    const auto one = tolerant(left, right, 4, 1);
    check(one.length != 6, "candidate exceeding mismatch budget was accepted");

    const auto longest = tolerant(text_bytes("PPABCDE"), text_bytes("ABXDEQ"), 3, 2);
    check(longest.length == 5, "longest acceptable approximate overlap was not selected");
    const auto below_minimum = tolerant(text_bytes("XXABCD"), text_bytes("ABXDY"), 5, 1);
    check(below_minimum.length == 0, "overlap shorter than minimum was accepted");
}

void test_direction_exact_first_binary_and_empty(const std::filesystem::path&)
{
    const auto left = text_bytes("XXABCDE");
    const auto right = text_bytes("ABXDEYY");
    const auto forward = tolerant(left, right, 4, 1);
    const auto reverse = tolerant(right, left, 4, 1);
    check(forward.length == 5 && reverse.length == 0,
          "approximate overlap directionality was incorrect");

    const auto exact_first = tolerant(text_bytes("ZZABC"), text_bytes("ABCQQ"), 3, 3);
    check(exact_first.exact && exact_first.length == 3 && exact_first.mismatches == 0,
          "qualifying exact overlap was not preferred");

    const Bytes binary_left{std::byte{0x44}, std::byte{0x00}, std::byte{0xff},
                            std::byte{0x80}, std::byte{0x13}, std::byte{0x7a}};
    const Bytes binary_right{std::byte{0x00}, std::byte{0xff}, std::byte{0x81},
                             std::byte{0x13}, std::byte{0x7a}, std::byte{0x55}};
    const auto binary = tolerant(binary_left, binary_right, 5, 1);
    check(binary.length == 5 && binary.mismatch_details[0].overlap_offset == 2,
          "binary approximate overlap was not safe");

    const Bytes empty;
    check(tolerant(empty, empty, 1, 1).length == 0
              && tolerant(binary_left, empty, 1, 1).length == 0
              && tolerant(empty, binary_right, 1, 1).length == 0,
          "empty tolerant overlap was unsafe");
}

void test_graph_metadata_and_compatibility(const std::filesystem::path& directory)
{
    const auto files = load_inputs(
        directory,
        "metadata-",
        {{"a.bin", text_bytes("XXABCDE")}, {"b.bin", text_bytes("ABXDEYY")}});
    const auto exact_default = FragmentGraph::build(files, 5);
    const auto exact_explicit = FragmentGraph::build(files, 5, 0);
    check(exact_default.edge_count() == exact_explicit.edge_count()
              && exact_default.edge_count() == 0,
          "exact-only graph compatibility changed");

    const auto approximate = FragmentGraph::build(files, 5, 1);
    check(approximate.edge_count() == 1, "approximate graph edge was not created");
    const auto& edge = approximate.edges().front();
    check(edge.from == 0 && edge.to == 1 && edge.overlap == 5 && edge.matches == 4
              && edge.mismatches == 1 && !edge.exact && edge.mismatch_details.size() == 1
              && edge.mismatch_details[0].overlap_offset == 2,
          "approximate graph metadata was incorrect");
}

void test_graph_ordering_and_opaque_names(const std::filesystem::path& directory)
{
    const auto files = load_inputs(
        directory,
        "opaque-shard_",
        {{"source.bin", text_bytes("QQABCDEFG")},
         {"long-two.bin", text_bytes("ABXDEYG")},
         {"short-exact.bin", text_bytes("CDEFGZ")},
         {"long-one.bin", text_bytes("ABCXEFG")}});
    const auto graph = FragmentGraph::build(files, 5, 2);
    const auto outgoing = graph.outgoing_edges(0);
    check(outgoing.size() >= 3 && outgoing[0].overlap == 7 && outgoing[0].mismatches == 1
              && outgoing[1].overlap == 7 && outgoing[1].mismatches == 2
              && outgoing[2].overlap == 5 && outgoing[2].mismatches == 0,
          "approximate adjacency ordering was not length then mismatch count");

    const auto renamed = load_inputs(
        directory,
        "renamed-",
        {{"z.bin", text_bytes("XXABCDE")}, {"a.bin", text_bytes("ABXDEYY")}});
    const auto renamed_graph = FragmentGraph::build(renamed, 5, 1);
    check(renamed_graph.edge_count() == 1 && renamed_graph.edges()[0].overlap == 5,
          "opaque filenames changed approximate inference");
}

void test_damage_simulator_integration(const std::filesystem::path& directory)
{
    const auto source = text_bytes("ABCDEFGHIJKL");
    const auto clean = shardrecover::FragmentGenerator::generate(source, 8, 4);
    shardrecover::DamageResult damaged;
    bool found_overlap_corruption = false;
    for (std::uint64_t seed = 0; seed < 1000 && !found_overlap_corruption; ++seed) {
        damaged = shardrecover::DamageSimulator::apply(
            clean, shardrecover::DamageConfig{.corrupt_byte_count = 1, .seed = seed});
        const auto& record = damaged.corruptions.front();
        found_overlap_corruption = (record.dataset_id == 0 && record.byte_offset >= 4)
                                   || (record.dataset_id == 1 && record.byte_offset < 4);
    }
    check(found_overlap_corruption, "could not construct deterministic overlap corruption");

    std::vector<std::pair<std::string, Bytes>> inputs;
    for (const auto& fragment : damaged.fragments) {
        inputs.push_back({"fragment-" + std::to_string(fragment.index) + ".bin", fragment.data});
    }
    const auto files = load_inputs(directory, "damage-", inputs);
    check(FragmentGraph::build(files, 4, 0).edge_count() == 0,
          "exact graph retained the corrupted relationship");
    const auto tolerant_graph = FragmentGraph::build(files, 4, 1);
    check(tolerant_graph.edge_count() >= 1
              && std::any_of(tolerant_graph.edges().begin(), tolerant_graph.edges().end(),
                             [](const auto& edge) {
                                 return edge.from == 0 && edge.to == 1 && edge.overlap == 4
                                        && edge.mismatches == 1;
                             }),
          "tolerant graph did not recover damage-simulator relationship");
}

void test_corrupted_path_and_merge_policy(const std::filesystem::path& directory)
{
    const auto files = load_inputs(
        directory,
        "chain-",
        {{"a.bin", text_bytes("XXABCDE")},
         {"b.bin", text_bytes("ABC9EFG")},
         {"c.bin", text_bytes("EFGHI")}});
    const auto exact_graph = FragmentGraph::build(files, 3, 0);
    const auto exact = shardrecover::GreedyReconstructor::reconstruct(exact_graph, files);
    check(!exact.complete, "exact reconstruction unexpectedly retained corrupted chain");

    const auto tolerant_graph = FragmentGraph::build(files, 3, 1);
    const auto result = shardrecover::GreedyReconstructor::reconstruct(tolerant_graph, files);
    check(result.complete && result.steps.size() == 3
              && result.steps[0].node_id == 0 && result.steps[1].node_id == 1
              && result.steps[2].node_id == 2,
          "tolerant reconstruction did not recover chain order");
    check(result.approximate_joins == 1 && result.overlap_mismatches == 1
              && result.steps[1].mismatch_details[0].overlap_offset == 3,
          "reconstruction did not retain mismatch evidence");
    check(result.bytes == text_bytes("XXABCDEFGHI"),
          "merge did not retain left bytes and append only non-overlap suffix");
}

void test_noise_and_reproducibility(const std::filesystem::path& directory)
{
    const auto clean = shardrecover::FragmentGenerator::generate(text_bytes("ABCDEFGHIJKLMNOP"),
                                                                  10,
                                                                  4);
    const auto damaged = shardrecover::DamageSimulator::apply(
        clean, shardrecover::DamageConfig{.noise_count = 1, .seed = 42});
    std::vector<std::pair<std::string, Bytes>> inputs;
    for (const auto& fragment : damaged.fragments) {
        inputs.push_back({"item-" + std::to_string(fragment.index) + ".bin", fragment.data});
    }
    const auto files = load_inputs(directory, "noise-", inputs);
    const auto first = FragmentGraph::build(files, 6, 1);
    const auto second = FragmentGraph::build(files, 6, 1);
    const auto noise_id = files.size() - 1;
    check(std::none_of(first.edges().begin(), first.edges().end(), [&](const auto& edge) {
              return edge.from == noise_id || edge.to == noise_id;
          }),
          "deterministic random noise gained a weak approximate connection");
    check(first.edges().size() == second.edges().size(), "repeated graph size changed");
    for (std::size_t index = 0; index < first.edges().size(); ++index) {
        const auto& left = first.edges()[index];
        const auto& right = second.edges()[index];
        check(left.from == right.from && left.to == right.to && left.overlap == right.overlap
                  && left.matches == right.matches && left.mismatches == right.mismatches
                  && left.exact == right.exact,
              "repeated overlap evidence was nondeterministic");
    }
}

struct TestCase {
    std::string_view name;
    void (*run)(const std::filesystem::path&);
};

}  // namespace

int main()
{
    try {
        const TemporaryDirectory directory;
        const std::array tests{
            TestCase{"exact and single mismatch", test_exact_and_single_mismatch},
            TestCase{"budgets, longest, and minimum", test_budgets_longest_and_minimum},
            TestCase{"direction, exact-first, binary, empty", test_direction_exact_first_binary_and_empty},
            TestCase{"graph metadata and compatibility", test_graph_metadata_and_compatibility},
            TestCase{"graph ordering and opaque names", test_graph_ordering_and_opaque_names},
            TestCase{"damage simulator integration", test_damage_simulator_integration},
            TestCase{"corrupted path and merge policy", test_corrupted_path_and_merge_policy},
            TestCase{"noise and reproducibility", test_noise_and_reproducibility},
        };
        std::size_t failures = 0;
        for (const auto& test : tests) {
            try {
                test.run(directory.path());
                std::cout << "[PASS] " << test.name << '\n';
            } catch (const std::exception& error) {
                ++failures;
                std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
            }
        }
        if (failures != 0) {
            std::cerr << failures << " corruption-tolerant test group(s) failed\n";
            return 1;
        }
        std::cout << tests.size() << " corruption-tolerant test groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FATAL] " << error.what() << '\n';
        return 1;
    }
}
