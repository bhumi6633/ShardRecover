#include "shardrecover/file_load_strategy.hpp"
#include "shardrecover/fragment_generator.hpp"
#include "shardrecover/fragment_graph.hpp"
#include "shardrecover/fragment_loader.hpp"
#include "shardrecover/png/analyzer.hpp"
#include "shardrecover/reconstruction.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
                / ("shardrecover-mapped-io-tests-" + std::to_string(stamp));
        if (!std::filesystem::create_directory(path_)) {
            throw std::runtime_error("Failed to create mapped-I/O test directory");
        }
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
    if (!data.empty()) {
        output.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size()));
    }
    output.close();
    if (!output) {
        throw std::runtime_error("Failed to write mapped-I/O fixture");
    }
}

void check_edge(const shardrecover::FragmentEdge& left,
                const shardrecover::FragmentEdge& right)
{
    check(left.from == right.from && left.to == right.to
              && left.overlap == right.overlap && left.matches == right.matches
              && left.mismatches == right.mismatches && left.exact == right.exact,
          "mapped I/O changed graph edge metadata or order");
}

void check_graph(const shardrecover::FragmentGraph& left,
                 const shardrecover::FragmentGraph& right)
{
    check(left.node_count() == right.node_count() && left.edge_count() == right.edge_count(),
          "mapped I/O changed graph size");
    for (std::size_t index = 0; index < left.nodes().size(); ++index) {
        check(left.nodes()[index].id == right.nodes()[index].id
                  && left.nodes()[index].path == right.nodes()[index].path
                  && left.nodes()[index].size == right.nodes()[index].size,
              "mapped I/O changed graph nodes");
    }
    for (std::size_t index = 0; index < left.edges().size(); ++index) {
        check_edge(left.edges()[index], right.edges()[index]);
    }
}

std::pair<std::vector<shardrecover::BinaryFile>, std::vector<shardrecover::BinaryFile>>
load_both(const std::filesystem::path& directory)
{
    auto buffered = shardrecover::load_fragment_directory(
        directory, shardrecover::FileLoadStrategy::buffered);
    auto mapped = shardrecover::load_fragment_directory(
        directory, shardrecover::FileLoadStrategy::mapped);
    check(buffered.size() == mapped.size(), "directory strategies returned different counts");
    for (std::size_t index = 0; index < buffered.size(); ++index) {
        check(buffered[index].path() == mapped[index].path()
                  && std::equal(buffered[index].bytes().begin(), buffered[index].bytes().end(),
                                mapped[index].bytes().begin()),
              "directory strategies returned different fragments");
        check(!buffered[index].mapped() && mapped[index].mapped(),
              "directory loader did not preserve requested ownership strategy");
    }
    return {std::move(buffered), std::move(mapped)};
}

void test_directory_graph_and_reconstruction(const std::filesystem::path& root)
{
    const auto directory = root / "fragments";
    std::filesystem::create_directory(directory);
    Bytes source(1024);
    for (std::size_t index = 0; index < source.size(); ++index) {
        source[index] = static_cast<std::byte>((index * 37U) & 0xffU);
    }
    const auto fragments = shardrecover::FragmentGenerator::generate(source, 64, 16);
    for (const auto& fragment : fragments) {
        write_file(directory / ("opaque-" + std::to_string(fragments.size() - fragment.index)
                                + ".bin"),
                   fragment.data);
    }
    auto [buffered, mapped] = load_both(directory);
    const auto buffered_graph = shardrecover::FragmentGraph::build(
        buffered, {16, 0, shardrecover::GraphBuildStrategy::exhaustive, 1});
    const auto mapped_graph = shardrecover::FragmentGraph::build(
        mapped, {16, 0, shardrecover::GraphBuildStrategy::exhaustive, 4});
    check_graph(buffered_graph, mapped_graph);

    const auto buffered_indexed = shardrecover::FragmentGraph::build(
        buffered, {16, 0, shardrecover::GraphBuildStrategy::indexed, 1});
    const auto mapped_indexed = shardrecover::FragmentGraph::build(
        mapped, {16, 0, shardrecover::GraphBuildStrategy::indexed, 4});
    check_graph(buffered_indexed, mapped_indexed);
    const auto buffered_result = shardrecover::GreedyReconstructor::reconstruct(
        buffered_indexed, buffered);
    const auto mapped_result = shardrecover::GreedyReconstructor::reconstruct(
        mapped_indexed, mapped);
    check(buffered_result.bytes == mapped_result.bytes
              && buffered_result.complete == mapped_result.complete,
          "mapped I/O changed reconstruction output");

    const auto buffered_approximate = shardrecover::FragmentGraph::build(
        buffered, {16, 1, shardrecover::GraphBuildStrategy::exhaustive, 1});
    const auto mapped_approximate = shardrecover::FragmentGraph::build(
        mapped, {16, 1, shardrecover::GraphBuildStrategy::exhaustive, 4});
    check_graph(buffered_approximate, mapped_approximate);
}

void test_empty_and_many_small_directories(const std::filesystem::path& root)
{
    const auto empty = root / "empty-directory";
    std::filesystem::create_directory(empty);
    check(shardrecover::load_fragment_directory(
              empty, shardrecover::FileLoadStrategy::mapped).empty(),
          "empty mapped fragment directory was not empty");

    const auto many = root / "many-small";
    std::filesystem::create_directory(many);
    for (int index = 0; index < 128; ++index) {
        const Bytes value{static_cast<std::byte>(index)};
        write_file(many / ("tiny-" + std::to_string(index) + ".bin"), value);
    }
    auto [buffered, mapped] = load_both(many);
    check(buffered.size() == 128 && mapped.size() == 128,
          "many-small-files mapping count was incorrect");
}

void append_u32(Bytes& output, std::uint32_t value)
{
    output.push_back(static_cast<std::byte>((value >> 24U) & 0xffU));
    output.push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::byte>(value & 0xffU));
}

std::uint32_t crc(std::span<const std::byte> bytes)
{
    std::uint32_t value = 0xffffffffU;
    for (const auto byte : bytes) {
        value ^= std::to_integer<std::uint8_t>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            value = (value >> 1U) ^ (0xedb88320U & (0U - (value & 1U)));
        }
    }
    return value ^ 0xffffffffU;
}

void append_chunk(Bytes& png, std::string_view type, const Bytes& data)
{
    append_u32(png, static_cast<std::uint32_t>(data.size()));
    const auto crc_begin = png.size();
    for (const auto character : type) {
        png.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    png.insert(png.end(), data.begin(), data.end());
    append_u32(png, crc(std::span<const std::byte>{png}.subspan(crc_begin)));
}

void test_png_analysis(const std::filesystem::path& root)
{
    Bytes png{std::byte{0x89}, std::byte{0x50}, std::byte{0x4e}, std::byte{0x47},
              std::byte{0x0d}, std::byte{0x0a}, std::byte{0x1a}, std::byte{0x0a}};
    Bytes ihdr;
    append_u32(ihdr, 1);
    append_u32(ihdr, 1);
    ihdr.insert(ihdr.end(), {std::byte{8}, std::byte{6}, std::byte{0},
                             std::byte{0}, std::byte{0}});
    append_chunk(png, "IHDR", ihdr);
    append_chunk(png, "IDAT", {});
    append_chunk(png, "IEND", {});
    const auto path = root / "valid.png";
    write_file(path, png);
    const auto buffered = shardrecover::BinaryFile::load(path);
    const auto mapped = shardrecover::BinaryFile::map(path);
    const auto left = shardrecover::png::Analyzer::analyze(buffered.bytes());
    const auto right = shardrecover::png::Analyzer::analyze(mapped.bytes());
    check(left.structurally_valid && right.structurally_valid
              && left.semantically_valid == right.semantically_valid
              && left.valid_crc_count == right.valid_crc_count
              && left.chunks.size() == right.chunks.size(),
          "mapped input changed valid PNG analysis");
}

}  // namespace

int main()
{
    try {
        const TemporaryDirectory directory;
        test_directory_graph_and_reconstruction(directory.path());
        test_empty_and_many_small_directories(directory.path());
        test_png_analysis(directory.path());
        std::cout << "All mapped I/O tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Mapped I/O test failure: " << error.what() << '\n';
        return 1;
    }
}
