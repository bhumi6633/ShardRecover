#include "shardrecover/binary_file.hpp"
#include "shardrecover/fragment_emitter.hpp"
#include "shardrecover/fragment_generator.hpp"
#include "shardrecover/fragment_graph.hpp"
#include "shardrecover/reconstruction.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::vector<std::byte> bytes(std::string_view text)
{
    std::vector<std::byte> result;
    result.reserve(text.size());
    for (const auto character : text) {
        result.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return result;
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto base = std::filesystem::temp_directory_path()
                          / ("shardrecover-reconstruction-tests-" + std::to_string(timestamp));
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
        throw std::runtime_error("Failed to find an unused temporary directory name");
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

struct TestInput {
    std::string name;
    std::vector<std::byte> data;
};

void write_file(const std::filesystem::path& path, const std::vector<std::byte>& data)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to create reconstruction test file: " + path.string());
    }
    if (!data.empty()) {
        output.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size()));
    }
    output.close();
    if (!output) {
        throw std::runtime_error("Failed to write reconstruction test file: " + path.string());
    }
}

shardrecover::ReconstructionResult reconstruct(const std::filesystem::path& directory,
                                                const std::vector<TestInput>& inputs,
                                                std::size_t minimum_overlap)
{
    std::vector<shardrecover::BinaryFile> files;
    files.reserve(inputs.size());
    for (const auto& input : inputs) {
        const auto path = directory / input.name;
        write_file(path, input.data);
        files.push_back(shardrecover::BinaryFile::load(path));
    }

    const auto fragment_span = std::span<const shardrecover::BinaryFile>{files};
    const auto graph = shardrecover::FragmentGraph::build(fragment_span, minimum_overlap);
    return shardrecover::GreedyReconstructor::reconstruct(graph, fragment_span);
}

std::vector<std::size_t> path_ids(const shardrecover::ReconstructionResult& result)
{
    std::vector<std::size_t> ids;
    ids.reserve(result.steps.size());
    for (const auto& step : result.steps) {
        ids.push_back(step.node_id);
    }
    return ids;
}

void check_no_reuse(const shardrecover::ReconstructionResult& result)
{
    std::unordered_set<std::size_t> used;
    for (const auto& step : result.steps) {
        check(used.insert(step.node_id).second, "reconstruction reused a fragment");
    }
}

std::vector<TestInput> simple_chain(std::string_view prefix)
{
    return {
        {std::string(prefix) + "a.bin", bytes("ABCDEFG")},
        {std::string(prefix) + "b.bin", bytes("EFGHIJK")},
        {std::string(prefix) + "c.bin", bytes("IJKLMNO")},
    };
}

void test_simple_chain(const std::filesystem::path& directory)
{
    const auto result = reconstruct(directory, simple_chain("simple-"), 3);
    check(result.bytes == bytes("ABCDEFGHIJKLMNO"), "simple chain reconstructed incorrectly");
    check(result.steps.size() == 3 && result.complete, "simple chain was not complete");
    check(result.total_overlap_bytes == 6, "simple chain overlap total was incorrect");
    check_no_reuse(result);
}

void test_shuffled_input_order(const std::filesystem::path& directory)
{
    const auto chain = simple_chain("shuffled-");
    const std::vector<TestInput> shuffled{chain[2], chain[0], chain[1]};
    const auto result = reconstruct(directory, shuffled, 3);
    check(result.bytes == bytes("ABCDEFGHIJKLMNO"), "shuffled input changed reconstruction");
    check(result.complete, "shuffled chain was not complete");
}

void test_opaque_filename_independence(const std::filesystem::path& directory)
{
    const auto descriptive = reconstruct(directory, simple_chain("named-"), 3);
    const auto opaque = reconstruct(directory,
                                    {{"shard_f204ad.bin", bytes("IJKLMNO")},
                                     {"shard_a91f3c.bin", bytes("ABCDEFG")},
                                     {"shard_281bb7.bin", bytes("EFGHIJK")}},
                                    3);
    check(descriptive.bytes == opaque.bytes, "opaque filenames changed reconstructed bytes");
    check(opaque.complete, "opaque chain was not complete");
}

void test_generated_dataset_exact_recovery(const std::filesystem::path& directory)
{
    std::mt19937_64 engine(20260826);
    std::vector<std::byte> original;
    original.reserve(512);
    for (std::size_t index = 0; index < 512; ++index) {
        original.push_back(static_cast<std::byte>(engine() & 0xffU));
    }

    auto fragments = shardrecover::FragmentGenerator::generate(original, 64, 16);
    shardrecover::FragmentEmitter::shuffle(fragments, 42);
    const auto names = shardrecover::FragmentEmitter::opaque_filenames(fragments.size(), 42);

    std::vector<TestInput> inputs;
    inputs.reserve(fragments.size());
    for (std::size_t index = 0; index < fragments.size(); ++index) {
        inputs.push_back(TestInput{"generated-" + names[index], fragments[index].data});
    }

    const auto result = reconstruct(directory, inputs, 16);
    check(result.complete, "generated dataset reconstruction was incomplete");
    check(result.bytes == original, "generated dataset did not recover exact original bytes");
    check_no_reuse(result);
}

void test_binary_safe_recovery(const std::filesystem::path& directory)
{
    const std::vector<std::byte> left{
        std::byte{0x00}, std::byte{0xff}, std::byte{0x80}, std::byte{0x13}, std::byte{0x7a},
    };
    const std::vector<std::byte> right{
        std::byte{0x13}, std::byte{0x7a}, std::byte{0x55}, std::byte{0x00},
    };
    auto expected = left;
    expected.insert(expected.end(), {std::byte{0x55}, std::byte{0x00}});

    const auto result = reconstruct(directory,
                                    {{"binary-left.bin", left}, {"binary-right.bin", right}},
                                    2);
    check(result.complete && result.bytes == expected, "binary-safe reconstruction failed");
}

void test_single_and_empty_datasets(const std::filesystem::path& directory)
{
    const auto single = reconstruct(directory, {{"single-reconstruct.bin", bytes("ABC")}}, 1);
    check(single.complete && single.steps.size() == 1 && single.bytes == bytes("ABC"),
          "single fragment reconstruction failed");

    const std::vector<shardrecover::BinaryFile> no_files;
    const auto graph = shardrecover::FragmentGraph::build(no_files, 1);
    const auto empty = shardrecover::GreedyReconstructor::reconstruct(graph, no_files);
    check(empty.complete && empty.steps.empty() && empty.bytes.empty(),
          "empty reconstruction was not safe");
}

void test_disconnected_fragment(const std::filesystem::path& directory)
{
    const auto result = reconstruct(directory,
                                    {{"disconnected-a.bin", bytes("ABCDEFG")},
                                     {"disconnected-b.bin", bytes("EFGHIJK")},
                                     {"disconnected-z.bin", bytes("ZZZ")}},
                                    3);
    check(!result.complete, "disconnected reconstruction was marked complete");
    check(result.steps.size() == 2, "disconnected reconstruction used the wrong fragment count");
    check(result.bytes == bytes("ABCDEFGHIJK"), "disconnected partial bytes were incorrect");
}

void test_greedy_choice(const std::filesystem::path& directory)
{
    const auto result = reconstruct(directory,
                                    {{"greedy-a.bin", bytes("XXABCDEF")},
                                     {"greedy-b.bin", bytes("CDEFGH")},
                                     {"greedy-c.bin", bytes("EFZZ")}},
                                    2);
    check(result.steps.size() >= 2, "greedy fixture did not take an edge");
    check(result.steps[1].node_id == 1 && result.steps[1].overlap_from_previous == 4,
          "greedy strategy did not select the strongest outgoing edge");
}

void test_deterministic_tie_break(const std::filesystem::path& directory)
{
    const std::vector<TestInput> inputs{
        {"tie-source.bin", bytes("AAXY")},
        {"tie-z.bin", bytes("XY2")},
        {"tie-b.bin", bytes("XY1")},
    };
    const auto first = reconstruct(directory, inputs, 2);
    const auto second = reconstruct(directory, inputs, 2);
    check(path_ids(first) == path_ids(second), "equal-overlap tie was not deterministic");
    check(first.steps.size() >= 2 && first.steps[1].node_id == 2,
          "equal-overlap tie did not use destination path ordering");
}

void test_overlap_merge_and_containment(const std::filesystem::path& directory)
{
    const auto merged = reconstruct(directory,
                                    {{"merge-a.bin", bytes("ABCDE")},
                                     {"merge-b.bin", bytes("CDEFG")}},
                                    3);
    check(merged.bytes == bytes("ABCDEFG"), "overlap bytes were duplicated during merge");

    const auto contained = reconstruct(directory,
                                       {{"contain-a.bin", bytes("ABCDEFG")},
                                        {"contain-b.bin", bytes("EFG")}},
                                       3);
    check(contained.complete && contained.bytes == bytes("ABCDEFG"),
          "fully overlapped destination was not handled safely");
}

void test_input_permutation_same_result(const std::filesystem::path& directory)
{
    const auto chain = simple_chain("permutation-");
    const auto ordered = reconstruct(directory, chain, 3);
    const auto permuted = reconstruct(directory, {chain[1], chain[2], chain[0]}, 3);
    check(ordered.bytes == permuted.bytes && ordered.complete == permuted.complete,
          "unambiguous input permutation changed reconstruction result");
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
            TestCase{"simple chain", test_simple_chain},
            TestCase{"shuffled input order", test_shuffled_input_order},
            TestCase{"opaque filename independence", test_opaque_filename_independence},
            TestCase{"generated dataset exact recovery", test_generated_dataset_exact_recovery},
            TestCase{"binary-safe recovery", test_binary_safe_recovery},
            TestCase{"single and empty datasets", test_single_and_empty_datasets},
            TestCase{"disconnected fragment", test_disconnected_fragment},
            TestCase{"greedy choice", test_greedy_choice},
            TestCase{"deterministic tie break", test_deterministic_tie_break},
            TestCase{"overlap merge and containment", test_overlap_merge_and_containment},
            TestCase{"input permutation", test_input_permutation_same_result},
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
            std::cerr << failures << " test case(s) failed\n";
            return 1;
        }

        std::cout << tests.size() << " test cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FATAL] " << error.what() << '\n';
        return 1;
    }
}
