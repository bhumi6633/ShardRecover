#include "shardrecover/binary_file.hpp"
#include "shardrecover/fragment_emitter.hpp"
#include "shardrecover/fragment_generator.hpp"
#include "shardrecover/fragment_graph.hpp"
#include "shardrecover/reconstruction.hpp"

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
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto base = std::filesystem::temp_directory_path()
                          / ("shardrecover-indexed-graph-tests-" + std::to_string(stamp));
        for (int suffix = 0; suffix < 100; ++suffix) {
            auto candidate = base;
            candidate += "-" + std::to_string(suffix);
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = std::move(candidate);
                return;
            }
            if (error) {
                throw std::runtime_error("Failed to create temporary directory: "
                                         + error.message());
            }
        }
        throw std::runtime_error("Failed to find an unused temporary directory name");
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

void write_file(const std::filesystem::path& path, std::span<const std::byte> data)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to create indexed graph test file");
    }
    if (!data.empty()) {
        output.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size()));
    }
    output.close();
    if (!output) {
        throw std::runtime_error("Failed to write indexed graph test file");
    }
}

std::vector<shardrecover::BinaryFile> load_files(
    const std::filesystem::path& directory,
    std::span<const std::vector<std::byte>> payloads)
{
    std::vector<shardrecover::BinaryFile> files;
    files.reserve(payloads.size());
    for (std::size_t index = 0; index < payloads.size(); ++index) {
        const auto path = directory / ("shard-" + std::to_string(index) + ".bin");
        write_file(path, payloads[index]);
        files.push_back(shardrecover::BinaryFile::load(path));
    }
    return files;
}

void check_edge_equal(const shardrecover::FragmentEdge& left,
                      const shardrecover::FragmentEdge& right)
{
    check(left.from == right.from && left.to == right.to,
          "indexed graph changed edge endpoints");
    check(left.overlap == right.overlap && left.matches == right.matches
              && left.mismatches == right.mismatches && left.exact == right.exact,
          "indexed graph changed edge metadata");
    check(left.mismatch_details.size() == right.mismatch_details.size(),
          "indexed graph changed mismatch detail count");
    for (std::size_t index = 0; index < left.mismatch_details.size(); ++index) {
        const auto& a = left.mismatch_details[index];
        const auto& b = right.mismatch_details[index];
        check(a.overlap_offset == b.overlap_offset && a.left_byte == b.left_byte
                  && a.right_byte == b.right_byte,
              "indexed graph changed mismatch details");
    }
}

void check_equivalent(std::span<const shardrecover::BinaryFile> files,
                      std::size_t minimum_overlap)
{
    shardrecover::GraphBuildStats exhaustive_stats;
    shardrecover::GraphBuildStats indexed_stats;
    const auto exhaustive = shardrecover::FragmentGraph::build(
        files,
        {minimum_overlap, 0, shardrecover::GraphBuildStrategy::exhaustive},
        &exhaustive_stats);
    const auto indexed = shardrecover::FragmentGraph::build(
        files,
        {minimum_overlap, 0, shardrecover::GraphBuildStrategy::indexed},
        &indexed_stats);

    check(exhaustive.node_count() == indexed.node_count(),
          "indexed graph changed node count");
    check(exhaustive.edge_count() == indexed.edge_count(),
          "indexed graph changed edge count");
    for (std::size_t index = 0; index < exhaustive.nodes().size(); ++index) {
        const auto& left = exhaustive.nodes()[index];
        const auto& right = indexed.nodes()[index];
        check(left.id == right.id && left.path == right.path && left.size == right.size,
              "indexed graph changed node metadata");
    }
    for (std::size_t index = 0; index < exhaustive.edges().size(); ++index) {
        check_edge_equal(exhaustive.edges()[index], indexed.edges()[index]);
    }
    check(exhaustive_stats.full_overlap_checks == exhaustive_stats.theoretical_pairs,
          "exhaustive statistics did not cover every directed pair");
    check(indexed_stats.full_overlap_checks <= exhaustive_stats.full_overlap_checks,
          "indexed strategy performed more full checks than exhaustive strategy");
    check(indexed_stats.edges_created == indexed.edge_count(),
          "indexed edge statistic was incorrect");
}

void test_varied_exact_graphs(const std::filesystem::path& directory)
{
    const std::vector<std::vector<std::byte>> chain{
        bytes("ABCDEFGH"), bytes("EFGHIJKL"), bytes("IJKLMNOP"), bytes("MNOPQRST"),
    };
    const auto chain_files = load_files(directory, chain);
    for (const auto threshold : {1U, 2U, 4U, 7U, 8U, 9U}) {
        check_equivalent(chain_files, threshold);
    }

    const std::vector<std::vector<std::byte>> edge_cases{
        {}, bytes("A"), bytes("AA"), bytes("AAAA"), bytes("AAAA"),
        {std::byte{0x00}, std::byte{0xff}, std::byte{0x80}},
        {std::byte{0xff}, std::byte{0x80}, std::byte{0x7f}},
    };
    const auto edge_files = load_files(directory, edge_cases);
    check_equivalent(edge_files, 1);
    check_equivalent(edge_files, 3);

    const std::vector<shardrecover::BinaryFile> empty;
    check_equivalent(empty, 1);
}

void test_generated_shuffled_dataset(const std::filesystem::path& directory)
{
    std::mt19937_64 engine(8675309);
    std::vector<std::byte> source(4096);
    for (auto& value : source) {
        value = static_cast<std::byte>(engine() & 0xffU);
    }
    auto fragments = shardrecover::FragmentGenerator::generate(source, 128, 32);
    shardrecover::FragmentEmitter::shuffle(fragments, 73);
    std::vector<std::vector<std::byte>> payloads;
    payloads.reserve(fragments.size());
    for (auto& fragment : fragments) {
        payloads.push_back(std::move(fragment.data));
    }
    const auto files = load_files(directory, payloads);
    check_equivalent(files, 16);
    check_equivalent(files, 32);
    check_equivalent(files, 64);

    shardrecover::GraphBuildStats exhaustive_stats;
    shardrecover::GraphBuildStats indexed_stats;
    const auto exhaustive = shardrecover::FragmentGraph::build(
        files, {32, 0, shardrecover::GraphBuildStrategy::exhaustive}, &exhaustive_stats);
    const auto indexed = shardrecover::FragmentGraph::build(
        files, {32, 0, shardrecover::GraphBuildStrategy::indexed}, &indexed_stats);
    check(indexed_stats.full_overlap_checks < exhaustive_stats.full_overlap_checks,
          "indexed strategy did not reduce full checks on high-entropy data");

    const auto file_span = std::span<const shardrecover::BinaryFile>{files};
    const auto exhaustive_result = shardrecover::GreedyReconstructor::reconstruct(
        exhaustive, file_span);
    const auto indexed_result = shardrecover::GreedyReconstructor::reconstruct(indexed, file_span);
    check(exhaustive_result.bytes == indexed_result.bytes
              && exhaustive_result.steps.size() == indexed_result.steps.size()
              && exhaustive_result.complete == indexed_result.complete,
          "indexed graph changed reconstruction output");
}

void test_approximate_fallback(const std::filesystem::path& directory)
{
    const std::vector<std::vector<std::byte>> payloads{
        bytes("ABCDEFGH"), bytes("EFXHIJKL"), bytes("IJKLMNOP"),
    };
    const auto files = load_files(directory, payloads);
    shardrecover::GraphBuildStats requested_indexed;
    const auto fallback = shardrecover::FragmentGraph::build(
        files, {3, 1, shardrecover::GraphBuildStrategy::indexed}, &requested_indexed);
    const auto exhaustive = shardrecover::FragmentGraph::build(files, 3, 1);
    check(requested_indexed.requested_strategy == shardrecover::GraphBuildStrategy::indexed
              && requested_indexed.effective_strategy
                     == shardrecover::GraphBuildStrategy::exhaustive
              && requested_indexed.approximate_fallback,
          "approximate indexed request did not report exhaustive fallback");
    check(fallback.edge_count() == exhaustive.edge_count(),
          "approximate fallback changed edge count");
    for (std::size_t index = 0; index < fallback.edges().size(); ++index) {
        check_edge_equal(fallback.edges()[index], exhaustive.edges()[index]);
    }
}

}  // namespace

int main()
{
    try {
        const TemporaryDirectory directory;
        test_varied_exact_graphs(directory.path());
        test_generated_shuffled_dataset(directory.path());
        test_approximate_fallback(directory.path());
        std::cout << "All indexed graph tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Indexed graph test failure: " << error.what() << '\n';
        return 1;
    }
}
