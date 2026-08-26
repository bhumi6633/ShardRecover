#include "shardrecover/fragment_graph.hpp"

#include "shardrecover/overlap.hpp"

#include <algorithm>
#include <stdexcept>

namespace shardrecover {
namespace {

bool edge_less(const FragmentEdge& left,
               const FragmentEdge& right,
               std::span<const FragmentNode> nodes)
{
    if (left.overlap != right.overlap) {
        return left.overlap > right.overlap;
    }

    const auto& left_source = nodes[left.from].path;
    const auto& right_source = nodes[right.from].path;
    if (left_source != right_source) {
        return left_source.generic_string() < right_source.generic_string();
    }

    const auto& left_destination = nodes[left.to].path;
    const auto& right_destination = nodes[right.to].path;
    if (left_destination != right_destination) {
        return left_destination.generic_string() < right_destination.generic_string();
    }

    return left.from != right.from ? left.from < right.from : left.to < right.to;
}

}  // namespace

FragmentGraph FragmentGraph::build(std::span<const BinaryFile> fragments,
                                   std::size_t minimum_overlap)
{
    if (minimum_overlap == 0) {
        throw std::invalid_argument("Minimum overlap must be greater than zero");
    }

    FragmentGraph graph;
    graph.nodes_.reserve(fragments.size());
    for (std::size_t id = 0; id < fragments.size(); ++id) {
        graph.nodes_.push_back(FragmentNode{id, fragments[id].path(), fragments[id].size()});
    }

    for (std::size_t from = 0; from < fragments.size(); ++from) {
        for (std::size_t to = 0; to < fragments.size(); ++to) {
            if (from == to) {
                continue;
            }

            const auto result = find_suffix_prefix_overlap(fragments[from].bytes(),
                                                           fragments[to].bytes());
            if (result.length >= minimum_overlap) {
                graph.edges_.push_back(FragmentEdge{from, to, result.length});
            }
        }
    }

    const auto nodes = std::span<const FragmentNode>{graph.nodes_};
    std::sort(graph.edges_.begin(), graph.edges_.end(), [nodes](const auto& left, const auto& right) {
        return edge_less(left, right, nodes);
    });

    graph.outgoing_.resize(graph.nodes_.size());
    graph.incoming_.resize(graph.nodes_.size());
    for (const auto& edge : graph.edges_) {
        graph.outgoing_[edge.from].push_back(edge);
        graph.incoming_[edge.to].push_back(edge);
    }

    for (auto& adjacency : graph.outgoing_) {
        std::sort(adjacency.begin(), adjacency.end(), [nodes](const auto& left, const auto& right) {
            return edge_less(left, right, nodes);
        });
    }
    for (auto& adjacency : graph.incoming_) {
        std::sort(adjacency.begin(), adjacency.end(), [nodes](const auto& left, const auto& right) {
            return edge_less(left, right, nodes);
        });
    }

    return graph;
}

std::span<const FragmentNode> FragmentGraph::nodes() const noexcept
{
    return nodes_;
}

std::span<const FragmentEdge> FragmentGraph::edges() const noexcept
{
    return edges_;
}

std::span<const FragmentEdge> FragmentGraph::outgoing_edges(std::size_t node_id) const
{
    if (node_id >= outgoing_.size()) {
        throw std::out_of_range("Fragment node ID is out of range");
    }
    return outgoing_[node_id];
}

std::span<const FragmentEdge> FragmentGraph::incoming_edges(std::size_t node_id) const
{
    if (node_id >= incoming_.size()) {
        throw std::out_of_range("Fragment node ID is out of range");
    }
    return incoming_[node_id];
}

std::size_t FragmentGraph::node_count() const noexcept
{
    return nodes_.size();
}

std::size_t FragmentGraph::edge_count() const noexcept
{
    return edges_.size();
}

}  // namespace shardrecover
