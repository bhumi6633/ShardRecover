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
#include <set>
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
                          / ("shardrecover-parallel-graph-tests-" + std::to_string(stamp));
        for (int suffix = 0; suffix < 100; ++suffix) {
            auto candidate = base;
            candidate += "-" + std::to_string(suffix);
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = std::move(candidate);
                return;
            }
            if (error) {
                throw std::runtime_error("Failed to create test directory: " + error.message());
            }
        }
        throw std::runtime_error("Failed to find an unused test directory");
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

std::vector<shardrecover::BinaryFile> load_files(
    const std::filesystem::path& directory,
    std::string_view prefix,
    std::span<const std::vector<std::byte>> payloads)
{
    std::vector<shardrecover::BinaryFile> files;
    for (std::size_t index = 0; index < payloads.size(); ++index) {
        const auto path = directory / (std::string(prefix) + std::to_string(index) + ".bin");
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Failed to create parallel graph test file");
        }
        const auto& payload = payloads[index];
        if (!payload.empty()) {
            output.write(reinterpret_cast<const char*>(payload.data()),
                         static_cast<std::streamsize>(payload.size()));
        }
        output.close();
        if (!output) {
            throw std::runtime_error("Failed to write parallel graph test file");
        }
        files.push_back(shardrecover::BinaryFile::load(path));
    }
    return files;
}

void check_edge(const shardrecover::FragmentEdge& left,
                const shardrecover::FragmentEdge& right)
{
    check(left.from == right.from && left.to == right.to
              && left.overlap == right.overlap && left.matches == right.matches
              && left.mismatches == right.mismatches && left.exact == right.exact,
          "parallel build changed edge metadata or ordering");
    check(left.mismatch_details.size() == right.mismatch_details.size(),
          "parallel build changed mismatch details");
    for (std::size_t index = 0; index < left.mismatch_details.size(); ++index) {
        const auto& a = left.mismatch_details[index];
        const auto& b = right.mismatch_details[index];
        check(a.overlap_offset == b.overlap_offset && a.left_byte == b.left_byte
                  && a.right_byte == b.right_byte,
              "parallel build changed mismatch evidence");
    }
}

void check_graph(const shardrecover::FragmentGraph& serial,
                 const shardrecover::FragmentGraph& parallel)
{
    check(serial.node_count() == parallel.node_count(), "parallel build changed node count");
    check(serial.edge_count() == parallel.edge_count(), "parallel build changed edge count");
    for (std::size_t index = 0; index < serial.nodes().size(); ++index) {
        const auto& left = serial.nodes()[index];
        const auto& right = parallel.nodes()[index];
        check(left.id == right.id && left.path == right.path && left.size == right.size,
              "parallel build changed node metadata");
        const auto serial_outgoing = serial.outgoing_edges(index);
        const auto parallel_outgoing = parallel.outgoing_edges(index);
        const auto serial_incoming = serial.incoming_edges(index);
        const auto parallel_incoming = parallel.incoming_edges(index);
        check(serial_outgoing.size() == parallel_outgoing.size()
                  && serial_incoming.size() == parallel_incoming.size(),
              "parallel build changed adjacency size");
        for (std::size_t edge = 0; edge < serial_outgoing.size(); ++edge) {
            check_edge(serial_outgoing[edge], parallel_outgoing[edge]);
        }
        for (std::size_t edge = 0; edge < serial_incoming.size(); ++edge) {
            check_edge(serial_incoming[edge], parallel_incoming[edge]);
        }
    }
    std::set<std::pair<std::size_t, std::size_t>> pairs;
    for (std::size_t index = 0; index < serial.edges().size(); ++index) {
        check_edge(serial.edges()[index], parallel.edges()[index]);
        check(pairs.emplace(parallel.edges()[index].from, parallel.edges()[index].to).second,
              "parallel build emitted a duplicate edge");
    }
}

void compare_threads(std::span<const shardrecover::BinaryFile> files,
                     shardrecover::GraphBuildStrategy strategy,
                     std::size_t minimum_overlap)
{
    shardrecover::GraphBuildStats serial_stats;
    shardrecover::GraphBuildStats parallel_stats;
    const auto serial = shardrecover::FragmentGraph::build(
        files, {minimum_overlap, 0, strategy, 1}, &serial_stats);
    const auto parallel = shardrecover::FragmentGraph::build(
        files, {minimum_overlap, 0, strategy, 4}, &parallel_stats);
    check_graph(serial, parallel);
    check(serial_stats.candidate_pairs == parallel_stats.candidate_pairs
              && serial_stats.full_overlap_checks == parallel_stats.full_overlap_checks
              && serial_stats.edges_created == parallel_stats.edges_created,
          "thread count changed graph statistics");
    if (serial_stats.full_overlap_checks != 0) {
        check(serial_stats.threads_used == 1 && parallel_stats.threads_used == 4,
              "graph statistics reported the wrong thread count");
    }
}

void test_core_datasets(const std::filesystem::path& directory)
{
    const std::vector<std::vector<std::byte>> chain{
        bytes("ABCDEFGH"), bytes("EFGHIJKL"), bytes("IJKLMNOP"), bytes("MNOPQRST"),
    };
    const auto chain_files = load_files(directory, "chain-", chain);
    compare_threads(chain_files, shardrecover::GraphBuildStrategy::exhaustive, 4);
    compare_threads(chain_files, shardrecover::GraphBuildStrategy::indexed, 4);

    const std::vector<std::vector<std::byte>> binary{
        {std::byte{0x00}, std::byte{0xff}, std::byte{0x80}, std::byte{0x13}},
        {std::byte{0x80}, std::byte{0x13}, std::byte{0x7a}},
        {std::byte{0x42}, std::byte{0x43}},
    };
    const auto binary_files = load_files(directory, "binary-", binary);
    compare_threads(binary_files, shardrecover::GraphBuildStrategy::exhaustive, 2);
    compare_threads(binary_files, shardrecover::GraphBuildStrategy::indexed, 2);

    const std::vector<std::vector<std::byte>> unrelated{
        bytes("AAAA"), bytes("BCDE"), bytes("FGHI"),
    };
    const auto unrelated_files = load_files(directory, "unrelated-", unrelated);
    compare_threads(unrelated_files, shardrecover::GraphBuildStrategy::exhaustive, 3);
    compare_threads(unrelated_files, shardrecover::GraphBuildStrategy::indexed, 3);

    const std::vector<shardrecover::BinaryFile> empty;
    compare_threads(empty, shardrecover::GraphBuildStrategy::exhaustive, 1);
    const std::vector<std::vector<std::byte>> single_payload{bytes("single")};
    const auto single = load_files(directory, "single-", single_payload);
    compare_threads(single, shardrecover::GraphBuildStrategy::indexed, 1);
}

void test_generated_repeated_and_reconstruction(const std::filesystem::path& directory)
{
    std::mt19937_64 engine(42);
    std::vector<std::byte> source(2048);
    for (auto& value : source) {
        value = static_cast<std::byte>(engine() & 0xffU);
    }
    auto generated = shardrecover::FragmentGenerator::generate(source, 64, 16);
    shardrecover::FragmentEmitter::shuffle(generated, 99);
    std::vector<std::vector<std::byte>> payloads;
    for (auto& fragment : generated) {
        payloads.push_back(std::move(fragment.data));
    }
    const auto files = load_files(directory, "opaque-a91f-", payloads);
    for (const auto threads : {1U, 2U, 4U}) {
        const auto graph = shardrecover::FragmentGraph::build(
            files, {16, 0, shardrecover::GraphBuildStrategy::indexed, threads});
        const auto reference = shardrecover::FragmentGraph::build(
            files, {16, 0, shardrecover::GraphBuildStrategy::indexed, 1});
        check_graph(reference, graph);
    }

    const auto serial_graph = shardrecover::FragmentGraph::build(
        files, {16, 0, shardrecover::GraphBuildStrategy::indexed, 1});
    const auto parallel_graph = shardrecover::FragmentGraph::build(
        files, {16, 0, shardrecover::GraphBuildStrategy::indexed, 4});
    const auto file_span = std::span<const shardrecover::BinaryFile>{files};
    const auto serial = shardrecover::GreedyReconstructor::reconstruct(serial_graph, file_span);
    const auto parallel = shardrecover::GreedyReconstructor::reconstruct(parallel_graph, file_span);
    check(serial.bytes == parallel.bytes && serial.complete == parallel.complete
              && serial.steps.size() == parallel.steps.size(),
          "parallel graph changed reconstruction result");
    for (std::size_t index = 0; index < serial.steps.size(); ++index) {
        check(serial.steps[index].node_id == parallel.steps[index].node_id
                  && serial.steps[index].overlap_from_previous
                         == parallel.steps[index].overlap_from_previous,
              "parallel graph changed reconstruction path");
    }

    for (int repetition = 0; repetition < 12; ++repetition) {
        const auto repeated = shardrecover::FragmentGraph::build(
            files, {16, 0, shardrecover::GraphBuildStrategy::indexed, 4});
        check_graph(parallel_graph, repeated);
    }
}

void test_approximate_policy(const std::filesystem::path& directory)
{
    const std::vector<std::vector<std::byte>> payloads{
        bytes("ABCDEFGH"), bytes("EFXHIJKL"), bytes("IJKLMNOP"),
    };
    const auto files = load_files(directory, "approximate-", payloads);
    shardrecover::GraphBuildStats serial_stats;
    shardrecover::GraphBuildStats configured_parallel_stats;
    const auto serial = shardrecover::FragmentGraph::build(
        files, {3, 1, shardrecover::GraphBuildStrategy::indexed, 1}, &serial_stats);
    const auto configured_parallel = shardrecover::FragmentGraph::build(
        files, {3, 1, shardrecover::GraphBuildStrategy::indexed, 4},
        &configured_parallel_stats);
    check_graph(serial, configured_parallel);
    check(configured_parallel_stats.approximate_fallback
              && configured_parallel_stats.effective_strategy
                     == shardrecover::GraphBuildStrategy::exhaustive
              && configured_parallel_stats.threads_used == 1,
          "approximate mode did not report its serial exhaustive policy");
}

}  // namespace

int main()
{
    try {
        const TemporaryDirectory directory;
        test_core_datasets(directory.path());
        test_generated_repeated_and_reconstruction(directory.path());
        test_approximate_policy(directory.path());
        std::cout << "All parallel graph tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Parallel graph test failure: " << error.what() << '\n';
        return 1;
    }
}
