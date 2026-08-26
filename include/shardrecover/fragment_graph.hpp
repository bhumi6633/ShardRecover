#pragma once

#include "shardrecover/binary_file.hpp"

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
};

class FragmentGraph {
public:
    static FragmentGraph build(std::span<const BinaryFile> fragments,
                               std::size_t minimum_overlap);

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
