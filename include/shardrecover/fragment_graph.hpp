#pragma once

#include "shardrecover/binary_file.hpp"
#include "shardrecover/overlap.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace shardrecover {

struct FragmentNode {
    std::size_t id;
    std::filesystem::path path;
    std::size_t size;
};

struct FragmentEdge {
    std::size_t from;
    std::size_t to;
    std::size_t overlap;
    std::size_t matches;
    std::size_t mismatches;
    bool exact;
    std::vector<OverlapMismatch> mismatch_details;
};

enum class GraphBuildStrategy {
    exhaustive,
    indexed,
};

struct GraphBuildConfig {
    std::size_t minimum_overlap;
    std::size_t max_mismatches = 0;
    GraphBuildStrategy strategy = GraphBuildStrategy::exhaustive;
    std::size_t threads = 1;
};

struct GraphBuildStats {
    GraphBuildStrategy requested_strategy = GraphBuildStrategy::exhaustive;
    GraphBuildStrategy effective_strategy = GraphBuildStrategy::exhaustive;
    bool approximate_fallback = false;
    std::size_t theoretical_pairs = 0;
    std::size_t candidate_pairs = 0;
    std::size_t full_overlap_checks = 0;
    std::size_t edges_created = 0;
    std::size_t threads_used = 1;
};

class FragmentGraph {
public:
    static FragmentGraph build(std::span<const BinaryFile> fragments,
                               std::size_t minimum_overlap,
                               std::size_t max_mismatches = 0);
    static FragmentGraph build(std::span<const BinaryFile> fragments,
                               const GraphBuildConfig& config,
                               GraphBuildStats* statistics = nullptr);

    std::span<const FragmentNode> nodes() const noexcept;
    std::span<const FragmentEdge> edges() const noexcept;
    std::span<const FragmentEdge> outgoing_edges(std::size_t node_id) const;
    std::span<const FragmentEdge> incoming_edges(std::size_t node_id) const;
    std::size_t node_count() const noexcept;
    std::size_t edge_count() const noexcept;

private:
    std::vector<FragmentNode> nodes_;
    std::vector<FragmentEdge> edges_;
    std::vector<std::vector<FragmentEdge>> outgoing_;
    std::vector<std::vector<FragmentEdge>> incoming_;
};

}  // namespace shardrecover
