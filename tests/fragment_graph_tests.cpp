#include "shardrecover/binary_file.hpp"
#include "shardrecover/fragment_emitter.hpp"
#include "shardrecover/fragment_generator.hpp"
#include "shardrecover/fragment_graph.hpp"

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
                          / ("shardrecover-fragment-graph-tests-" + std::to_string(timestamp));
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
        throw std::runtime_error("Failed to create graph test file: " + path.string());
    }
    if (!data.empty()) {
        output.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size()));
    }
    output.close();
    if (!output) {
        throw std::runtime_error("Failed to write graph test file: " + path.string());
    }
}

std::vector<shardrecover::BinaryFile> load_inputs(const std::filesystem::path& directory,
                                                  const std::vector<TestInput>& inputs)
{
    std::vector<shardrecover::BinaryFile> files;
    files.reserve(inputs.size());
    for (const auto& input : inputs) {
        const auto path = directory / input.name;
        write_file(path, input.data);
        files.push_back(shardrecover::BinaryFile::load(path));
    }
    return files;
}

shardrecover::FragmentGraph build_graph(const std::filesystem::path& directory,
                                        const std::vector<TestInput>& inputs,
                                        std::size_t minimum_overlap)
{
    const auto files = load_inputs(directory, inputs);
    return shardrecover::FragmentGraph::build(
        std::span<const shardrecover::BinaryFile>{files}, minimum_overlap);
}

bool has_edge(const shardrecover::FragmentGraph& graph,
              std::size_t from,
              std::size_t to,
              std::size_t overlap)
{
    return std::any_of(graph.edges().begin(), graph.edges().end(), [&](const auto& edge) {
        return edge.from == from && edge.to == to && edge.overlap == overlap;
    });
}

void test_nodes_edges_and_adjacency(const std::filesystem::path& directory)
{
    const auto graph = build_graph(directory,
                                   {{"a.bin", bytes("ABCDEF")},
                                    {"b.bin", bytes("DEFGHI")},
                                    {"c.bin", bytes("EFZZ")}},
                                   2);

    check(graph.node_count() == 3, "graph node count was incorrect");
    check(graph.edge_count() == 2, "graph edge count was incorrect");
    check(graph.nodes()[0].id == 0 && graph.nodes()[0].size == 6,
          "graph node metadata was incorrect");
    check(has_edge(graph, 0, 1, 3), "expected A to B edge was missing");
    check(has_edge(graph, 0, 2, 2), "expected A to C edge was missing");
    check(graph.outgoing_edges(0).size() == 2, "outgoing adjacency was incorrect");
    check(graph.outgoing_edges(0)[0].overlap == 3
              && graph.outgoing_edges(0)[1].overlap == 2,
          "outgoing edges were not sorted by descending overlap");
    check(graph.incoming_edges(1).size() == 1 && graph.incoming_edges(1)[0].from == 0,
          "incoming adjacency was incorrect");
}

void test_threshold_filtering(const std::filesystem::path& directory)
{
    const std::vector<TestInput> inputs{
        {"threshold-a.bin", bytes("ABCDEF")},
        {"threshold-b.bin", bytes("CDEFZZ")},
    };
    check(build_graph(directory, inputs, 4).edge_count() == 1,
          "edge at the minimum threshold was excluded");
    check(build_graph(directory, inputs, 5).edge_count() == 0,
          "edge below the minimum threshold was included");
}

void test_no_self_edges_and_no_duplicates(const std::filesystem::path& directory)
{
    const auto graph = build_graph(directory,
                                   {{"duplicate-a.bin", bytes("AAAA")},
                                    {"duplicate-b.bin", bytes("AAAA")},
                                    {"duplicate-c.bin", bytes("AAAA")}},
                                   1);
    std::unordered_set<std::string> pairs;
    for (const auto& edge : graph.edges()) {
        check(edge.from != edge.to, "graph contained a self edge");
        const auto key = std::to_string(edge.from) + ":" + std::to_string(edge.to);
        check(pairs.insert(key).second, "graph contained a duplicate directed edge");
    }
    check(graph.edge_count() == 6, "complete directed graph edge count was incorrect");
}

void test_directionality(const std::filesystem::path& directory)
{
    const auto graph = build_graph(directory,
                                   {{"direction-a.bin", bytes("ABCDEF")},
                                    {"direction-b.bin", bytes("DEFGHI")}},
                                   3);
    check(graph.edge_count() == 1, "directional graph had an unexpected edge count");
    check(has_edge(graph, 0, 1, 3), "forward directional edge was missing");
    check(!has_edge(graph, 1, 0, 3), "reverse directional edge should not exist");
}

void test_equal_overlap_tie_order(const std::filesystem::path& directory)
{
    const auto graph = build_graph(directory,
                                   {{"tie-source.bin", bytes("AAXY")},
                                    {"tie-b.bin", bytes("XY1")},
                                    {"tie-c.bin", bytes("XY2")}},
                                   2);
    const auto outgoing = graph.outgoing_edges(0);
    check(outgoing.size() == 2, "equal-overlap fixture had an unexpected edge count");
    check(graph.nodes()[outgoing[0].to].path.filename() == "tie-b.bin"
              && graph.nodes()[outgoing[1].to].path.filename() == "tie-c.bin",
          "equal-overlap edges did not use deterministic path ordering");
}

void test_empty_and_single_graphs(const std::filesystem::path& directory)
{
    const std::vector<shardrecover::BinaryFile> empty;
    const auto empty_graph = shardrecover::FragmentGraph::build(empty, 1);
    check(empty_graph.node_count() == 0 && empty_graph.edge_count() == 0,
          "empty graph was not valid");

    const auto single = build_graph(directory, {{"single.bin", bytes("ABC")}}, 1);
    check(single.node_count() == 1 && single.edge_count() == 0,
          "single-node graph contained an edge");
}

void test_binary_bytes(const std::filesystem::path& directory)
{
    const std::vector<std::byte> left{
        std::byte{0xaa}, std::byte{0x00}, std::byte{0xff}, std::byte{0x80}, std::byte{0x13},
    };
    const std::vector<std::byte> right{
        std::byte{0xff}, std::byte{0x80}, std::byte{0x13}, std::byte{0x7a},
    };
    const auto graph = build_graph(directory,
                                   {{"binary-a.bin", left}, {"binary-b.bin", right}},
                                   3);
    check(has_edge(graph, 0, 1, 3), "binary-safe graph edge was missing");
}

void test_opaque_name_independence(const std::filesystem::path& directory)
{
    const std::vector<std::vector<std::byte>> payloads{
        bytes("ABCDEF"), bytes("DEFGHI"), bytes("EFZZ"),
    };
    const std::vector<TestInput> descriptive{
        {"first.bin", payloads[0]}, {"second.bin", payloads[1]}, {"third.bin", payloads[2]},
    };
    const std::vector<TestInput> opaque{
        {"shard_a91f3c.bin", payloads[0]},
        {"shard_281bb7.bin", payloads[1]},
        {"shard_f204ad.bin", payloads[2]},
    };

    const auto descriptive_graph = build_graph(directory, descriptive, 2);
    const auto opaque_graph = build_graph(directory, opaque, 2);
    check(descriptive_graph.edge_count() == opaque_graph.edge_count(),
          "opaque names changed graph edge count");
    for (std::size_t index = 0; index < descriptive_graph.edges().size(); ++index) {
        const auto& left = descriptive_graph.edges()[index];
        const auto& right = opaque_graph.edges()[index];
        check(left.from == right.from && left.to == right.to && left.overlap == right.overlap,
              "opaque names changed graph structure");
    }
}

void test_shuffled_generated_chain(const std::filesystem::path& directory)
{
    std::vector<std::byte> input;
    for (std::size_t value = 0; value < 20; ++value) {
        input.push_back(static_cast<std::byte>(value));
    }

    auto generated = shardrecover::FragmentGenerator::generate(input, 8, 4);
    shardrecover::FragmentEmitter::shuffle(generated, 42);
    const auto names = shardrecover::FragmentEmitter::opaque_filenames(generated.size(), 42);

    std::vector<TestInput> anonymous_inputs;
    anonymous_inputs.reserve(generated.size());
    for (std::size_t position = 0; position < generated.size(); ++position) {
        anonymous_inputs.push_back(TestInput{names[position], generated[position].data});
    }

    const auto graph = build_graph(directory, anonymous_inputs, 4);
    check(graph.node_count() == 4, "generated chain node count was incorrect");
    check(graph.edge_count() == 3, "generated chain edge count was incorrect");
    for (const auto& edge : graph.edges()) {
        check(edge.overlap == 4, "generated chain edge had the wrong overlap");
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
            TestCase{"nodes, edges, and adjacency", test_nodes_edges_and_adjacency},
            TestCase{"threshold filtering", test_threshold_filtering},
            TestCase{"no self edges or duplicates", test_no_self_edges_and_no_duplicates},
            TestCase{"directionality", test_directionality},
            TestCase{"equal-overlap tie order", test_equal_overlap_tie_order},
            TestCase{"empty and single graphs", test_empty_and_single_graphs},
            TestCase{"binary bytes", test_binary_bytes},
            TestCase{"opaque name independence", test_opaque_name_independence},
            TestCase{"shuffled generated chain", test_shuffled_generated_chain},
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
