#include "shardrecover/fragment_graph.hpp"

#include "shardrecover/overlap.hpp"
#include "shardrecover/thread_pool.hpp"

#include <algorithm>
#include <cstdint>
#include <future>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace shardrecover {
namespace {

bool edge_less(const FragmentEdge& left,
               const FragmentEdge& right,
               std::span<const FragmentNode> nodes)
{
    if (left.overlap != right.overlap) {
        return left.overlap > right.overlap;
    }
    if (left.mismatches != right.mismatches) {
        return left.mismatches < right.mismatches;
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

constexpr std::uint64_t fingerprint_base = 1099511628211ULL;

struct FingerprintKey {
    std::size_t length;
    std::uint64_t value;

    bool operator==(const FingerprintKey&) const = default;
};

struct FingerprintKeyHash {
    std::size_t operator()(const FingerprintKey& key) const noexcept
    {
        return static_cast<std::size_t>(key.value ^ (key.value >> 32U))
               ^ (key.length * 0x9e3779b9U);
    }
};

struct CandidatePair {
    std::size_t from;
    std::size_t to;
};

std::vector<std::uint64_t> prefix_fingerprints(std::span<const std::byte> bytes)
{
    std::vector<std::uint64_t> prefixes(bytes.size() + 1, 0);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        prefixes[index + 1] = prefixes[index] * fingerprint_base
                              + std::to_integer<std::uint8_t>(bytes[index]) + 1U;
    }
    return prefixes;
}

FragmentEdge make_edge(std::size_t from,
                       std::size_t to,
                       const OverlapResult& result)
{
    return FragmentEdge{from,
                        to,
                        result.length,
                        result.matches,
                        result.mismatches,
                        result.exact,
                        result.mismatch_details};
}

std::vector<FragmentEdge> evaluate_exact_pairs(
    std::span<const BinaryFile> fragments,
    std::span<const CandidatePair> pairs)
{
    std::vector<FragmentEdge> edges;
    for (const auto& pair : pairs) {
        const auto result = find_suffix_prefix_overlap(fragments[pair.from].bytes(),
                                                       fragments[pair.to].bytes());
        if (result.length != 0) {
            edges.push_back(make_edge(pair.from, pair.to, result));
        }
    }
    return edges;
}

}  // namespace

FragmentGraph FragmentGraph::build(std::span<const BinaryFile> fragments,
                                   std::size_t minimum_overlap,
                                   std::size_t max_mismatches)
{
    return build(fragments,
                 GraphBuildConfig{minimum_overlap,
                                  max_mismatches,
                                  GraphBuildStrategy::exhaustive,
                                  1});
}

FragmentGraph FragmentGraph::build(std::span<const BinaryFile> fragments,
                                   const GraphBuildConfig& config,
                                   GraphBuildStats* statistics)
{
    if (config.minimum_overlap == 0) {
        throw std::invalid_argument("Minimum overlap must be greater than zero");
    }
    if (config.threads == 0) {
        throw std::invalid_argument("Graph build thread count must be greater than zero");
    }

    GraphBuildStats local_stats;
    local_stats.requested_strategy = config.strategy;
    local_stats.effective_strategy = config.strategy;
    if (config.max_mismatches != 0 && config.strategy == GraphBuildStrategy::indexed) {
        local_stats.effective_strategy = GraphBuildStrategy::exhaustive;
        local_stats.approximate_fallback = true;
    }
    if (!fragments.empty()
        && fragments.size() - 1 > std::numeric_limits<std::size_t>::max() / fragments.size()) {
        throw std::overflow_error("Theoretical graph pair count overflows size_t");
    }
    local_stats.theoretical_pairs = fragments.size() * (fragments.size() - 1);

    FragmentGraph graph;
    graph.nodes_.reserve(fragments.size());
    for (std::size_t id = 0; id < fragments.size(); ++id) {
        graph.nodes_.push_back(FragmentNode{id, fragments[id].path(), fragments[id].size()});
    }

    std::vector<CandidatePair> exact_pairs;
    if (config.max_mismatches == 0
        && local_stats.effective_strategy == GraphBuildStrategy::indexed) {
        std::size_t maximum_size = 0;
        for (const auto& fragment : fragments) {
            maximum_size = std::max(maximum_size, fragment.size());
        }
        std::vector<std::uint64_t> powers(maximum_size + 1, 1);
        for (std::size_t length = 1; length < powers.size(); ++length) {
            powers[length] = powers[length - 1] * fingerprint_base;
        }

        std::unordered_map<FingerprintKey, std::vector<std::size_t>, FingerprintKeyHash>
            prefix_index;
        prefix_index.reserve(fragments.size());
        for (std::size_t node_id = 0; node_id < fragments.size(); ++node_id) {
            const auto bytes = fragments[node_id].bytes();
            const auto prefixes = prefix_fingerprints(bytes);
            for (std::size_t length = config.minimum_overlap; length <= bytes.size(); ++length) {
                prefix_index[FingerprintKey{length, prefixes[length]}].push_back(node_id);
            }
        }
        for (std::size_t from = 0; from < fragments.size(); ++from) {
            const auto source = fragments[from].bytes();
            if (source.size() < config.minimum_overlap) {
                continue;
            }
            const auto prefixes = prefix_fingerprints(source);
            std::vector<bool> checked(fragments.size(), false);
            for (std::size_t length = config.minimum_overlap; length <= source.size(); ++length) {
                const auto suffix_hash = prefixes.back()
                                         - prefixes[source.size() - length] * powers[length];
                const auto bucket = prefix_index.find(FingerprintKey{length, suffix_hash});
                if (bucket == prefix_index.end()) {
                    continue;
                }
                const auto boundary = source.last(length);
                for (const auto to : bucket->second) {
                    if (from == to || checked[to]) {
                        continue;
                    }
                    ++local_stats.candidate_pairs;
                    const auto destination_prefix = fragments[to].bytes().first(length);
                    if (!std::equal(boundary.begin(), boundary.end(),
                                    destination_prefix.begin())) {
                        continue;
                    }
                    checked[to] = true;
                    ++local_stats.full_overlap_checks;
                    exact_pairs.push_back(CandidatePair{from, to});
                }
            }
        }
    } else if (config.max_mismatches == 0) {
        for (std::size_t from = 0; from < fragments.size(); ++from) {
            for (std::size_t to = 0; to < fragments.size(); ++to) {
                if (from == to) {
                    continue;
                }
                ++local_stats.candidate_pairs;
                ++local_stats.full_overlap_checks;
                exact_pairs.push_back(CandidatePair{from, to});
            }
        }
    } else {
        for (std::size_t from = 0; from < fragments.size(); ++from) {
            for (std::size_t to = 0; to < fragments.size(); ++to) {
                if (from == to) {
                    continue;
                }
                ++local_stats.candidate_pairs;
                ++local_stats.full_overlap_checks;
                const auto result = find_tolerant_suffix_prefix_overlap(
                    fragments[from].bytes(),
                    fragments[to].bytes(),
                    config.minimum_overlap,
                    config.max_mismatches);
                if (result.length >= config.minimum_overlap) {
                    graph.edges_.push_back(make_edge(from, to, result));
                }
            }
        }
    }

    local_stats.threads_used = config.max_mismatches == 0 && !exact_pairs.empty()
                                   ? config.threads
                                   : 1;
    if (!exact_pairs.empty()) {
        if (config.threads == 1) {
            graph.edges_ = evaluate_exact_pairs(fragments, exact_pairs);
        } else {
            ThreadPool pool(config.threads);
            const auto target_batches = std::min(
                exact_pairs.size(),
                config.threads <= std::numeric_limits<std::size_t>::max() / 4
                    ? config.threads * 4
                    : exact_pairs.size());
            const auto batch_size = std::max<std::size_t>(
                1, exact_pairs.size() / target_batches
                       + (exact_pairs.size() % target_batches != 0 ? 1 : 0));
            std::vector<std::future<std::vector<FragmentEdge>>> futures;
            for (std::size_t begin = 0; begin < exact_pairs.size(); begin += batch_size) {
                const auto count = std::min(batch_size, exact_pairs.size() - begin);
                futures.push_back(pool.submit([fragments,
                                               pairs = std::span<const CandidatePair>{
                                                   exact_pairs.data() + begin, count}] {
                    return evaluate_exact_pairs(fragments, pairs);
                }));
            }
            for (auto& future : futures) {
                auto edges = future.get();
                graph.edges_.insert(graph.edges_.end(),
                                    std::make_move_iterator(edges.begin()),
                                    std::make_move_iterator(edges.end()));
            }
        }
        graph.edges_.erase(
            std::remove_if(graph.edges_.begin(), graph.edges_.end(), [&](const auto& edge) {
                return edge.overlap < config.minimum_overlap;
            }),
            graph.edges_.end());
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
    local_stats.edges_created = graph.edge_count();
    if (statistics != nullptr) {
        *statistics = local_stats;
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
