#include "TDTree.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace {

constexpr std::size_t kNoBlock = std::numeric_limits<std::size_t>::max();

struct CellPosition {
    bool root = false;
    std::size_t block_index = 0;
    std::size_t cell_index = 0;
};

} // namespace

const char* matchOutputModeName(MatchOutputMode mode) {
    return mode == MatchOutputMode::CountOnly ? "count-only" : "full";
}

bool CheckedMatchCounter::increment() {
    if (value == std::numeric_limits<std::uint64_t>::max()) {
        overflowed = true;
        return false;
    }
    ++value;
    return true;
}

TDTree::TDTree(
    const Graph& temporal_graph,
    const Graph& query_graph,
    const QueryDecomposition& decomposition,
    int minimum_duration,
    DurationMode requested_duration_mode)
    : G(temporal_graph),
      Q(query_graph),
      QD(decomposition),
      k_threshold(minimum_duration),
      duration_mode(requested_duration_mode) {
    build();
}

void TDTree::build() {
    initializeNodes();
    initializeQueryRequirements();
    growAndTrim();
}

void TDTree::initializeNodes() {
    nodes.resize(static_cast<std::size_t>(Q.num_vertices));
    for (int query_vertex = 0; query_vertex < Q.num_vertices; ++query_vertex) {
        auto& node = nodes[static_cast<std::size_t>(query_vertex)];
        node.query_vertex_id = query_vertex;
        node.isRoot = query_vertex == QD.root;
        node.isLeaf =
            QD.spanning_tree_adj[static_cast<std::size_t>(query_vertex)].empty();
    }
}

void TDTree::initializeQueryRequirements() {
    query_out_neighbor_label_requirements.assign(
        static_cast<std::size_t>(Q.num_vertices), {});
    query_in_neighbor_label_requirements.assign(
        static_cast<std::size_t>(Q.num_vertices), {});
    for (int query_vertex = 0; query_vertex < Q.num_vertices; ++query_vertex) {
        auto& out_requirements =
            query_out_neighbor_label_requirements[static_cast<std::size_t>(query_vertex)];
        for (const auto& edge : Q.adj[static_cast<std::size_t>(query_vertex)]) {
            const Label neighbor_label =
                Q.vertex_labels[static_cast<std::size_t>(edge.to)];
            if (neighbor_label < kLabelCount) ++out_requirements[neighbor_label];
        }

        auto& in_requirements =
            query_in_neighbor_label_requirements[static_cast<std::size_t>(query_vertex)];
        for (const auto& edge : Q.in_adj[static_cast<std::size_t>(query_vertex)]) {
            const Label neighbor_label =
                Q.vertex_labels[static_cast<std::size_t>(edge.to)];
            if (neighbor_label < kLabelCount) ++in_requirements[neighbor_label];
        }
    }
}

bool TDTree::isDataVertexCandidate(int data_vertex, int query_vertex) const {
    if (data_vertex < 0 || query_vertex < 0 ||
        data_vertex >= G.num_vertices || query_vertex >= Q.num_vertices) {
        return false;
    }

    const std::size_t data_index = static_cast<std::size_t>(data_vertex);
    const std::size_t query_index = static_cast<std::size_t>(query_vertex);
    if (G.vertex_labels[data_index] != Q.vertex_labels[query_index]) return false;
    const std::size_t data_degree =
        G.adj[data_index].size() + G.in_adj[data_index].size();
    const std::size_t query_degree =
        Q.adj[query_index].size() + Q.in_adj[query_index].size();
    if (data_degree < query_degree) return false;
    if (data_index >= G.vertex_active_durations.size() ||
        G.vertex_active_durations[data_index] < k_threshold) {
        return false;
    }

    const auto& out_available = G.neighbor_label_counts[data_index];
    const auto& out_required =
        query_out_neighbor_label_requirements[query_index];
    const auto& in_available = G.incoming_neighbor_label_counts[data_index];
    const auto& in_required =
        query_in_neighbor_label_requirements[query_index];
    std::size_t data_distinct_neighbor_labels = 0;
    std::size_t query_distinct_neighbor_labels = 0;
    for (std::size_t label = 0; label < kLabelCount; ++label) {
        if (out_available[label] > 0 || in_available[label] > 0) {
            ++data_distinct_neighbor_labels;
        }
        if (out_required[label] > 0 || in_required[label] > 0) {
            ++query_distinct_neighbor_labels;
        }
    }
    return data_distinct_neighbor_labels >= query_distinct_neighbor_labels;
}

std::vector<std::size_t> TDTree::captureBlockSizes() const {
    std::vector<std::size_t> sizes;
    sizes.reserve(nodes.size());
    for (const auto& node : nodes) sizes.push_back(node.blocks.size());
    return sizes;
}

void TDTree::rollbackBlocks(const std::vector<std::size_t>& sizes) {
    const std::size_t count = std::min(nodes.size(), sizes.size());
    for (std::size_t i = 0; i < count; ++i) {
        nodes[i].blocks.resize(sizes[i]);
    }
}

void TDTree::compactLeafBlock(TDTreeBlock& block) const {
    block.leaf_time_union.clear();
    for (const auto& cell : block.candidates) {
        block.leaf_time_union = unionTimeIntervals(
            block.leaf_time_union, cell.leaf_time_instances);
    }
    for (auto& cell : block.candidates) {
        cell.leaf_time_instances = subtractTimeIntervals(
            block.leaf_time_union, cell.leaf_time_instances);
    }
    block.leaf_time_compacted = true;
}

bool TDTree::treeEdgeTimeInstances(
    int parent_query_vertex,
    int child_query_vertex,
    int parent_data_vertex,
    int child_data_vertex,
    std::vector<TimeInterval>& time_instances) const {
    const bool parent_to_child =
        std::find(
            QD.tree_edges.begin(),
            QD.tree_edges.end(),
            std::make_pair(parent_query_vertex, child_query_vertex)) !=
        QD.tree_edges.end();
    const bool child_to_parent =
        std::find(
            QD.tree_edges.begin(),
            QD.tree_edges.end(),
            std::make_pair(child_query_vertex, parent_query_vertex)) !=
        QD.tree_edges.end();
    if (parent_to_child == child_to_parent) return false;

    const TemporalEdge* edge = parent_to_child
        ? G.findTemporalEdge(parent_data_vertex, child_data_vertex)
        : G.findTemporalEdge(child_data_vertex, parent_data_vertex);
    if (edge == nullptr) return false;
    time_instances = edge->active_intervals;
    return hasMinimumDuration(
        time_instances, k_threshold, duration_mode);
}

bool TDTree::passesMappedNonTreeConstraints(
    int data_vertex,
    int query_vertex,
    const std::vector<int>& path_mapping) const {
    for (const auto& query_edge : QD.non_tree_edges) {
        if (query_edge.first == query_vertex &&
            query_edge.second == query_vertex) {
            const TemporalEdge* self_edge =
                G.findTemporalEdge(data_vertex, data_vertex);
            if (self_edge == nullptr ||
                !hasMinimumDuration(
                    self_edge->active_intervals, k_threshold, duration_mode)) {
                return false;
            }
            continue;
        }

        int other_query_vertex = -1;
        int data_source = -1;
        int data_target = -1;
        if (query_edge.first == query_vertex) {
            other_query_vertex = query_edge.second;
            if (other_query_vertex < 0 ||
                static_cast<std::size_t>(other_query_vertex) >= path_mapping.size() ||
                path_mapping[static_cast<std::size_t>(other_query_vertex)] < 0) {
                continue;
            }
            data_source = data_vertex;
            data_target =
                path_mapping[static_cast<std::size_t>(other_query_vertex)];
        } else if (query_edge.second == query_vertex) {
            other_query_vertex = query_edge.first;
            if (other_query_vertex < 0 ||
                static_cast<std::size_t>(other_query_vertex) >= path_mapping.size() ||
                path_mapping[static_cast<std::size_t>(other_query_vertex)] < 0) {
                continue;
            }
            data_source =
                path_mapping[static_cast<std::size_t>(other_query_vertex)];
            data_target = data_vertex;
        } else {
            continue;
        }

        const TemporalEdge* temporal_edge =
            G.findTemporalEdge(data_source, data_target);
        if (temporal_edge == nullptr ||
            !hasMinimumDuration(
                temporal_edge->active_intervals, k_threshold, duration_mode)) {
            return false;
        }
    }
    return true;
}

bool TDTree::buildChildren(
    int query_vertex,
    TDTreeCell& parent_cell,
    std::vector<int>& path_mapping,
    const std::vector<TimeInterval>& path_time_instances,
    bool has_path_time_instances) {
    const auto& children =
        QD.spanning_tree_adj[static_cast<std::size_t>(query_vertex)];
    parent_cell.child_block_indices.assign(children.size(), kNoBlock);
    if (children.empty()) return true;

    // This snapshot implements Algorithm 2's cascading removal without
    // invalidating auxiliary indexes: if one required child has no supported
    // block, every block created below this cell is rolled back together.
    const std::vector<std::size_t> initial_block_sizes = captureBlockSizes();

    for (std::size_t child_position = 0;
         child_position < children.size();
         ++child_position) {
        const int child_query_vertex = children[child_position];
        const bool parent_to_child =
            std::find(
                QD.tree_edges.begin(),
                QD.tree_edges.end(),
                std::make_pair(query_vertex, child_query_vertex)) !=
            QD.tree_edges.end();
        const bool child_to_parent =
            std::find(
                QD.tree_edges.begin(),
                QD.tree_edges.end(),
                std::make_pair(child_query_vertex, query_vertex)) !=
            QD.tree_edges.end();
        if (parent_to_child == child_to_parent) {
            rollbackBlocks(initial_block_sizes);
            return false;
        }

        TDTreeBlock block;
        block.v_par = parent_cell.data_vertex;
        const auto& expansion_edges = parent_to_child
            ? G.adj[static_cast<std::size_t>(parent_cell.data_vertex)]
            : G.in_adj[static_cast<std::size_t>(parent_cell.data_vertex)];

        for (const auto& edge_ref : expansion_edges) {
            const int candidate = edge_ref.to;
            if (candidate < 0 || candidate >= G.num_vertices) continue;
            if (std::find(path_mapping.begin(), path_mapping.end(), candidate) !=
                path_mapping.end()) {
                continue;
            }
            if (!isDataVertexCandidate(candidate, child_query_vertex)) continue;

            std::vector<TimeInterval> tree_edge_time_instances;
            if (!treeEdgeTimeInstances(
                    query_vertex,
                    child_query_vertex,
                    parent_cell.data_vertex,
                    candidate,
                    tree_edge_time_instances)) {
                continue;
            }

            std::vector<TimeInterval> next_path_time_instances =
                has_path_time_instances
                ? intersectTimeIntervals(
                      path_time_instances, tree_edge_time_instances)
                : std::move(tree_edge_time_instances);
            if (!hasMinimumDuration(
                    next_path_time_instances, k_threshold, duration_mode)) {
                continue;
            }

            path_mapping[static_cast<std::size_t>(child_query_vertex)] = candidate;
            if (!passesMappedNonTreeConstraints(
                    candidate, child_query_vertex, path_mapping)) {
                path_mapping[static_cast<std::size_t>(child_query_vertex)] = -1;
                continue;
            }

            TDTreeCell child_cell;
            child_cell.data_vertex = candidate;
            if (nodes[static_cast<std::size_t>(child_query_vertex)].isLeaf) {
                child_cell.leaf_time_instances = next_path_time_instances;
            }
            if (buildChildren(
                    child_query_vertex,
                    child_cell,
                    path_mapping,
                    next_path_time_instances,
                    true)) {
                block.candidates.push_back(std::move(child_cell));
            }
            path_mapping[static_cast<std::size_t>(child_query_vertex)] = -1;
        }

        if (block.candidates.empty()) {
            rollbackBlocks(initial_block_sizes);
            parent_cell.child_block_indices.assign(children.size(), kNoBlock);
            return false;
        }

        auto& child_node = nodes[static_cast<std::size_t>(child_query_vertex)];
        if (child_node.isLeaf) compactLeafBlock(block);
        child_node.blocks.push_back(std::move(block));
        parent_cell.child_block_indices[child_position] =
            child_node.blocks.size() - 1;
    }
    return true;
}

void TDTree::growAndTrim() {
    if (QD.root < 0 || !QD.connected || QD.root >= Q.num_vertices) return;

    auto& root_node = nodes[static_cast<std::size_t>(QD.root)];
    root_node.root_candidates.reserve(
        static_cast<std::size_t>(G.num_vertices) / kLabelCount + 1);
    std::vector<int> path_mapping(
        static_cast<std::size_t>(Q.num_vertices), -1);
    const std::vector<TimeInterval> empty_time_instances;

    for (int data_vertex = 0; data_vertex < G.num_vertices; ++data_vertex) {
        if (!isDataVertexCandidate(data_vertex, QD.root)) continue;
        path_mapping[static_cast<std::size_t>(QD.root)] = data_vertex;
        if (!passesMappedNonTreeConstraints(
                data_vertex, QD.root, path_mapping)) {
            path_mapping[static_cast<std::size_t>(QD.root)] = -1;
            continue;
        }

        TDTreeCell root_cell;
        root_cell.data_vertex = data_vertex;
        if (buildChildren(
                QD.root,
                root_cell,
                path_mapping,
                empty_time_instances,
                false)) {
            root_node.root_candidates.push_back(std::move(root_cell));
        }
        path_mapping[static_cast<std::size_t>(QD.root)] = -1;
    }
}

std::vector<int> TDTree::generateMatchingOrder() const {
    std::vector<int> order;
    if (QD.root < 0 || !QD.connected) return order;
    order.push_back(QD.root);
    if (Q.num_vertices <= 1) return order;

    struct LeafCardinality {
        int query_vertex;
        std::size_t cardinality;
    };
    std::vector<LeafCardinality> leaves;
    for (int query_vertex = 0; query_vertex < Q.num_vertices; ++query_vertex) {
        if (!nodes[static_cast<std::size_t>(query_vertex)].isLeaf ||
            query_vertex == QD.root) {
            continue;
        }
        std::size_t cardinality = 0;
        for (const auto& block :
             nodes[static_cast<std::size_t>(query_vertex)].blocks) {
            cardinality += block.candidates.size();
        }
        leaves.push_back({query_vertex, cardinality});
    }
    std::sort(
        leaves.begin(),
        leaves.end(),
        [](const LeafCardinality& lhs, const LeafCardinality& rhs) {
            // Section 7.1 sorts query paths in decreasing matching
            // cardinality; query ID makes ties reproducible.
            if (lhs.cardinality != rhs.cardinality) {
                return lhs.cardinality > rhs.cardinality;
            }
            return lhs.query_vertex < rhs.query_vertex;
        });

    std::vector<std::uint8_t> added(
        static_cast<std::size_t>(Q.num_vertices), 0);
    added[static_cast<std::size_t>(QD.root)] = 1;
    for (const auto& leaf : leaves) {
        std::vector<int> path;
        for (int current = leaf.query_vertex;
             current >= 0 && current != QD.root;
             current = QD.parent[static_cast<std::size_t>(current)]) {
            path.push_back(current);
        }
        std::reverse(path.begin(), path.end());
        for (int query_vertex : path) {
            if (added[static_cast<std::size_t>(query_vertex)] != 0) continue;
            added[static_cast<std::size_t>(query_vertex)] = 1;
            order.push_back(query_vertex);
        }
    }

    // Every vertex of a connected tree lies on a root-to-leaf path. Keep a
    // defensive fallback so malformed external decompositions cannot silently
    // omit a query vertex.
    for (int query_vertex : QD.dfs_order) {
        if (added[static_cast<std::size_t>(query_vertex)] != 0) continue;
        added[static_cast<std::size_t>(query_vertex)] = 1;
        order.push_back(query_vertex);
    }
    return order;
}

std::vector<int> TDTree::uniqueCandidates(const TDTreeNode& node) const {
    std::vector<int> candidates;
    if (node.isRoot) {
        candidates.reserve(node.root_candidates.size());
        for (const auto& cell : node.root_candidates) {
            candidates.push_back(cell.data_vertex);
        }
    } else {
        for (const auto& block : node.blocks) {
            for (const auto& cell : block.candidates) {
                candidates.push_back(cell.data_vertex);
            }
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(
        std::unique(candidates.begin(), candidates.end()), candidates.end());
    return candidates;
}

std::size_t TDTree::uniqueCandidateCount(const TDTreeNode& node) const {
    return uniqueCandidates(node).size();
}

std::size_t TDTree::candidateRelationCount() const {
    std::size_t count = 0;
    for (const auto& node : nodes) {
        if (node.isRoot) {
            count += node.root_candidates.size();
        } else {
            for (const auto& block : node.blocks) {
                count += block.candidates.size();
            }
        }
    }
    return count;
}

void TDTree::print_res() const {
    std::cout << "TD-tree candidate summary:\n";
    for (const auto& node : nodes) {
        std::size_t relation_count = 0;
        if (node.isRoot) {
            relation_count = node.root_candidates.size();
        } else {
            for (const auto& block : node.blocks) {
                relation_count += block.candidates.size();
            }
        }
        std::cout << "  q" << node.query_vertex_id
                  << " label="
                  << labelToString(
                         Q.vertex_labels[static_cast<std::size_t>(
                             node.query_vertex_id)])
                  << " blocks=" << node.blocks.size()
                  << " unique_candidates=" << uniqueCandidateCount(node)
                  << " relation_entries=" << relation_count << '\n';
    }
}

CheckedMatchCounter TDTree::enumerateMatches(
    std::ostream& output,
    MatchOutputMode mode,
    std::size_t requested_threads,
    MatchSummary& summary) const {
    summary.threads_requested =
        std::max<std::size_t>(1, requested_threads);
    summary.threads_used = 1;
    summary.workers_with_tasks = 0;
    summary.root_tasks_claimed = 0;
    summary.worker_root_tasks_min = 0;
    summary.worker_root_tasks_max = 0;

    CheckedMatchCounter empty_counter;
    const std::vector<int> matching_order = generateMatchingOrder();
    if (QD.root < 0 || !QD.connected || matching_order.empty() ||
        matching_order.size() != static_cast<std::size_t>(Q.num_vertices)) {
        return empty_counter;
    }

    struct SearchContext {
        CheckedMatchCounter counter;
        std::vector<int> mapping;
        std::vector<std::uint8_t> used_data_vertices;
        std::vector<CellPosition> positions;
        std::uint64_t dfs_states = 0;
        std::uint64_t generated_candidates = 0;
        std::uint64_t accepted_candidates = 0;
        std::uint64_t completed_matches = 0;
        std::uint64_t pruned_injectivity = 0;
        std::uint64_t pruned_temporal_support = 0;
    };

    auto make_context = [&]() {
        SearchContext context;
        context.mapping.assign(
            static_cast<std::size_t>(Q.num_vertices), -1);
        context.used_data_vertices.assign(
            static_cast<std::size_t>(G.num_vertices), 0);
        context.positions.resize(
            static_cast<std::size_t>(Q.num_vertices));
        return context;
    };

    auto cell_at = [&](int query_vertex,
                       const CellPosition& position) -> const TDTreeCell* {
        const auto& node = nodes[static_cast<std::size_t>(query_vertex)];
        if (position.root) {
            return position.cell_index < node.root_candidates.size()
                ? &node.root_candidates[position.cell_index]
                : nullptr;
        }
        if (position.block_index >= node.blocks.size()) return nullptr;
        const auto& block = node.blocks[position.block_index];
        return position.cell_index < block.candidates.size()
            ? &block.candidates[position.cell_index]
            : nullptr;
    };

    const auto& root_candidates =
        nodes[static_cast<std::size_t>(QD.root)].root_candidates;
    summary.root_candidate_count = root_candidates.size();

    auto merge_statistics = [&](const SearchContext& context) {
        summary.dfs_states += context.dfs_states;
        summary.generated_candidates +=
            context.generated_candidates;
        summary.accepted_candidates +=
            context.accepted_candidates;
        summary.completed_matches +=
            context.completed_matches;
        summary.pruned_injectivity +=
            context.pruned_injectivity;
        summary.pruned_temporal_support +=
            context.pruned_temporal_support;
    };

    auto enumerate_root = [&](std::size_t root_index,
                              SearchContext& context,
                              std::ostream* match_rows) {
        auto expand = [&](auto&& self,
                          std::size_t depth,
                          const std::vector<TimeInterval>&
                              current_time_instances,
                          bool has_time_instances) -> void {
            if (context.counter.overflowed) return;
            ++context.dfs_states;
            if (depth >= matching_order.size()) {
                if (!has_time_instances ||
                    !hasMinimumDuration(
                        current_time_instances,
                        k_threshold,
                        duration_mode)) {
                    return;
                }
                if (!context.counter.increment()) return;
                ++context.completed_matches;
                if (match_rows != nullptr) {
                    *match_rows << "Match "
                                << (context.counter.value - 1)
                                << ": ";
                    for (int query_vertex = 0;
                         query_vertex < Q.num_vertices;
                         ++query_vertex) {
                        if (query_vertex > 0) *match_rows << ", ";
                        *match_rows << 'q' << query_vertex << '('
                                    << labelToString(
                                           Q.vertex_labels[
                                               static_cast<std::size_t>(
                                                   query_vertex)])
                                    << ")->"
                                    << G.externalId(
                                           context.mapping[
                                               static_cast<std::size_t>(
                                                   query_vertex)]);
                    }
                    *match_rows << " | active="
                                << formatIntervals(
                                       current_time_instances)
                                << '\n';
                }
                return;
            }

            const int query_vertex = matching_order[depth];
            const int parent_query_vertex =
                QD.parent[static_cast<std::size_t>(query_vertex)];
            if (parent_query_vertex < 0) return;
            const TDTreeCell* parent_cell =
                cell_at(
                    parent_query_vertex,
                    context.positions[
                        static_cast<std::size_t>(
                            parent_query_vertex)]);
            if (parent_cell == nullptr) return;

            const auto& siblings =
                QD.spanning_tree_adj[
                    static_cast<std::size_t>(
                        parent_query_vertex)];
            const auto child_it =
                std::find(
                    siblings.begin(),
                    siblings.end(),
                    query_vertex);
            if (child_it == siblings.end()) return;
            const std::size_t child_position =
                static_cast<std::size_t>(
                    child_it - siblings.begin());
            if (child_position >=
                parent_cell->child_block_indices.size()) {
                return;
            }
            const std::size_t block_index =
                parent_cell->child_block_indices[child_position];
            const auto& node =
                nodes[static_cast<std::size_t>(query_vertex)];
            if (block_index == kNoBlock ||
                block_index >= node.blocks.size()) {
                return;
            }
            const auto& block = node.blocks[block_index];

            context.generated_candidates +=
                static_cast<std::uint64_t>(
                    block.candidates.size());
            for (std::size_t cell_index = 0;
                 cell_index < block.candidates.size();
                 ++cell_index) {
                const auto& candidate_cell =
                    block.candidates[cell_index];
                const int candidate = candidate_cell.data_vertex;
                if (candidate < 0 ||
                    candidate >= G.num_vertices ||
                    context.used_data_vertices[
                        static_cast<std::size_t>(candidate)] != 0) {
                    ++context.pruned_injectivity;
                    continue;
                }

                std::vector<TimeInterval> next_time_instances =
                    current_time_instances;
                bool next_has_time_instances =
                    has_time_instances;
                auto apply_time_instances =
                    [&](const std::vector<TimeInterval>&
                            edge_time_instances) {
                        if (!next_has_time_instances) {
                            next_time_instances =
                                edge_time_instances;
                            next_has_time_instances = true;
                        } else {
                            next_time_instances =
                                intersectTimeIntervals(
                                    next_time_instances,
                                    edge_time_instances);
                        }
                        return hasMinimumDuration(
                            next_time_instances,
                            k_threshold,
                            duration_mode);
                    };
                auto apply_temporal_edge =
                    [&](int source_data_vertex,
                        int target_data_vertex) {
                        const TemporalEdge* temporal_edge =
                            G.findTemporalEdge(
                                source_data_vertex,
                                target_data_vertex);
                        return temporal_edge != nullptr &&
                            apply_time_instances(
                                temporal_edge->active_intervals);
                    };

                // A leaf cell stores the complete root-to-leaf tree-path TS
                // from Algorithm 1. Intersecting all selected leaf sets
                // implements Algorithm 3, Eq. (1), including nonconsecutive
                // cardinality.
                if (node.isLeaf) {
                    const std::vector<TimeInterval>
                        leaf_time_instances =
                            block.leaf_time_compacted
                            ? subtractTimeIntervals(
                                  block.leaf_time_union,
                                  candidate_cell
                                      .leaf_time_instances)
                            : candidate_cell
                                  .leaf_time_instances;
                    if (!apply_time_instances(
                            leaf_time_instances)) {
                        ++context.pruned_temporal_support;
                        continue;
                    }
                }

                if (GraphUtils::hasEdge(
                        Q.adj, query_vertex, query_vertex) &&
                    !apply_temporal_edge(candidate, candidate)) {
                    ++context.pruned_temporal_support;
                    continue;
                }

                bool valid = true;
                for (const auto& query_edge :
                     Q.adj[
                         static_cast<std::size_t>(
                             query_vertex)]) {
                    const int other_query_vertex =
                        query_edge.to;
                    const bool is_tree_arc =
                        other_query_vertex ==
                            parent_query_vertex &&
                        std::find(
                            QD.tree_edges.begin(),
                            QD.tree_edges.end(),
                            std::make_pair(
                                query_vertex,
                                other_query_vertex)) !=
                            QD.tree_edges.end();
                    if (other_query_vertex == query_vertex ||
                        is_tree_arc) {
                        continue;
                    }
                    const int other_data_vertex =
                        context.mapping[
                            static_cast<std::size_t>(
                                other_query_vertex)];
                    if (other_data_vertex < 0) continue;
                    if (!apply_temporal_edge(
                            candidate,
                            other_data_vertex)) {
                        valid = false;
                        break;
                    }
                }
                if (!valid) {
                    ++context.pruned_temporal_support;
                    continue;
                }
                for (const auto& query_edge :
                     Q.in_adj[
                         static_cast<std::size_t>(
                             query_vertex)]) {
                    const int other_query_vertex =
                        query_edge.to;
                    const bool is_tree_arc =
                        other_query_vertex ==
                            parent_query_vertex &&
                        std::find(
                            QD.tree_edges.begin(),
                            QD.tree_edges.end(),
                            std::make_pair(
                                other_query_vertex,
                                query_vertex)) !=
                            QD.tree_edges.end();
                    if (other_query_vertex == query_vertex ||
                        is_tree_arc) {
                        continue;
                    }
                    const int other_data_vertex =
                        context.mapping[
                            static_cast<std::size_t>(
                                other_query_vertex)];
                    if (other_data_vertex < 0) continue;
                    if (!apply_temporal_edge(
                            other_data_vertex,
                            candidate)) {
                        valid = false;
                        break;
                    }
                }
                if (!valid) {
                    ++context.pruned_temporal_support;
                    continue;
                }

                ++context.accepted_candidates;
                context.mapping[
                    static_cast<std::size_t>(query_vertex)] =
                    candidate;
                context.used_data_vertices[
                    static_cast<std::size_t>(candidate)] = 1;
                context.positions[
                    static_cast<std::size_t>(query_vertex)] =
                    {false, block_index, cell_index};
                self(
                    self,
                    depth + 1,
                    next_time_instances,
                    next_has_time_instances);
                context.used_data_vertices[
                    static_cast<std::size_t>(candidate)] = 0;
                context.mapping[
                    static_cast<std::size_t>(query_vertex)] = -1;
                if (context.counter.overflowed) return;
            }
        };

        const int root_candidate = root_candidates[root_index].data_vertex;
        if (root_candidate < 0 ||
            root_candidate >= G.num_vertices) {
            return;
        }

        std::vector<TimeInterval> root_time_instances;
        bool root_has_time_instances = false;
        if (GraphUtils::hasEdge(Q.adj, QD.root, QD.root)) {
            const TemporalEdge* self_edge =
                G.findTemporalEdge(root_candidate, root_candidate);
            if (self_edge == nullptr ||
                !hasMinimumDuration(
                    self_edge->active_intervals,
                    k_threshold,
                    duration_mode)) {
                return;
            }
            root_time_instances = self_edge->active_intervals;
            root_has_time_instances = true;
        }

        context.mapping[
            static_cast<std::size_t>(QD.root)] = root_candidate;
        context.used_data_vertices[
            static_cast<std::size_t>(root_candidate)] = 1;
        context.positions[static_cast<std::size_t>(QD.root)] =
            {true, 0, root_index};
        expand(
            expand,
            1,
            root_time_instances,
            root_has_time_instances);
        context.used_data_vertices[
            static_cast<std::size_t>(root_candidate)] = 0;
        context.mapping[static_cast<std::size_t>(QD.root)] = -1;
    };

    if (mode == MatchOutputMode::Full ||
        summary.threads_requested == 1) {
        SearchContext context = make_context();
        for (std::size_t root_index = 0;
             root_index < root_candidates.size();
             ++root_index) {
            ++summary.root_tasks_claimed;
            enumerate_root(
                root_index,
                context,
                mode == MatchOutputMode::Full ? &output : nullptr);
            if (context.counter.overflowed) break;
        }
        if (summary.root_tasks_claimed > 0) {
            summary.workers_with_tasks = 1;
            summary.worker_root_tasks_min =
                summary.root_tasks_claimed;
            summary.worker_root_tasks_max =
                summary.root_tasks_claimed;
        }
        merge_statistics(context);
        return context.counter;
    }

    const std::size_t worker_count = summary.threads_requested;
    summary.threads_used = worker_count;
    std::vector<SearchContext> contexts;
    contexts.reserve(worker_count);
    for (std::size_t worker = 0;
         worker < worker_count;
         ++worker) {
        contexts.push_back(make_context());
    }
    std::vector<std::size_t> worker_root_tasks(worker_count, 0);
    std::atomic<std::size_t> next_root{
        std::min(worker_count, root_candidates.size())};
    std::atomic<bool> cancel{false};
    std::mutex exception_mutex;
    std::exception_ptr worker_exception;
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    auto join_workers = [&]() {
        for (auto& worker : workers) {
            if (worker.joinable()) worker.join();
        }
    };

    try {
        for (std::size_t worker = 0;
             worker < worker_count;
             ++worker) {
            workers.emplace_back([&, worker]() {
                try {
                    if (worker < root_candidates.size() &&
                        !cancel.load(std::memory_order_relaxed)) {
                        ++worker_root_tasks[worker];
                        enumerate_root(
                            worker, contexts[worker], nullptr);
                        if (contexts[worker].counter.overflowed) {
                            cancel.store(
                                true, std::memory_order_relaxed);
                        }
                    }
                    while (!cancel.load(std::memory_order_relaxed)) {
                        const std::size_t root_index =
                            next_root.fetch_add(
                                1, std::memory_order_relaxed);
                        if (root_index >= root_candidates.size()) {
                            break;
                        }
                        ++worker_root_tasks[worker];
                        enumerate_root(
                            root_index, contexts[worker], nullptr);
                        if (contexts[worker].counter.overflowed) {
                            cancel.store(
                                true, std::memory_order_relaxed);
                        }
                    }
                } catch (...) {
                    {
                        std::lock_guard<std::mutex> lock(
                            exception_mutex);
                        if (!worker_exception) {
                            worker_exception =
                                std::current_exception();
                        }
                    }
                    cancel.store(true, std::memory_order_relaxed);
                }
            });
        }
    } catch (...) {
        cancel.store(true, std::memory_order_relaxed);
        join_workers();
        throw;
    }
    join_workers();
    if (worker_exception) std::rethrow_exception(worker_exception);

    summary.worker_root_tasks_min =
        std::numeric_limits<std::size_t>::max();
    for (std::size_t tasks : worker_root_tasks) {
        summary.root_tasks_claimed += tasks;
        if (tasks > 0) ++summary.workers_with_tasks;
        summary.worker_root_tasks_min =
            std::min(summary.worker_root_tasks_min, tasks);
        summary.worker_root_tasks_max =
            std::max(summary.worker_root_tasks_max, tasks);
    }

    CheckedMatchCounter total;
    for (const SearchContext& context : contexts) {
        merge_statistics(context);
        if (context.counter.overflowed ||
            context.counter.value >
                std::numeric_limits<std::uint64_t>::max() -
                    total.value) {
            total.value =
                std::numeric_limits<std::uint64_t>::max();
            total.overflowed = true;
            break;
        }
        total.value += context.counter.value;
    }
    return total;
}

MatchSummary TDTree::save_res(
    const std::string& filename,
    MatchOutputMode mode,
    std::size_t threads) const {
    MatchSummary summary;
    summary.threads_requested =
        std::max<std::size_t>(1, threads);
    // Binary mode keeps tellp/seekp offsets stable on Windows (text mode
    // translates '\n' to CRLF and would corrupt the count placeholder).
    std::ofstream output(filename, std::ios::binary);
    if (!output.is_open()) return summary;

    output << "[Run]\nmode: " << matchOutputModeName(mode)
           << "\nselectivity_mode: tdtree"
           << "\nquery_root: " << QD.root
           << "\nroot_selectivity_score: "
           << std::setprecision(17)
           << QD.topological_selectivity[
                  static_cast<std::size_t>(QD.root)]
           << "\nduration_mode: " << durationModeName(duration_mode)
           << "\nminimum_duration_k: " << k_threshold
           << "\nthreads_requested: "
           << summary.threads_requested
           << "\nplan_order:";
    for (int query_vertex : QD.dfs_order) {
        output << ' ' << query_vertex;
    }
    std::array<std::size_t, kLabelCount> label_counts{};
    std::array<long long, kLabelCount> duration_sums{};
    for (std::size_t data_vertex = 0;
         data_vertex < G.vertex_labels.size();
         ++data_vertex) {
        const Label label = G.vertex_labels[data_vertex];
        if (label >= kLabelCount) continue;
        ++label_counts[label];
        duration_sums[label] +=
            G.vertex_active_durations[data_vertex];
    }
    output << "\n\n[Selectivity Summary]\n";
    for (int query_vertex = 0;
         query_vertex < Q.num_vertices;
         ++query_vertex) {
        const Label label =
            Q.vertex_labels[
                static_cast<std::size_t>(query_vertex)];
        const std::size_t degree =
            Q.adj[static_cast<std::size_t>(query_vertex)].size() +
            Q.in_adj[static_cast<std::size_t>(query_vertex)].size();
        const double average_lifespan =
            label < kLabelCount && label_counts[label] > 0
            ? static_cast<double>(duration_sums[label]) /
                static_cast<double>(label_counts[label])
            : 0.0;
        const auto order_it = std::find(
            QD.dfs_order.begin(), QD.dfs_order.end(),
            query_vertex);
        output << 'q' << query_vertex
               << " label=" << labelToString(label)
               << " degree=" << degree
               << " candidate_count="
               << (label < kLabelCount
                       ? label_counts[label]
                       : 0)
               << " average_lifespan="
               << std::setprecision(17)
               << average_lifespan
               << " score="
               << QD.topological_selectivity[
                      static_cast<std::size_t>(
                          query_vertex)]
               << " is_root="
               << (query_vertex == QD.root
                       ? "true"
                       : "false")
               << " order_position="
               << (order_it == QD.dfs_order.end()
                       ? -1
                       : static_cast<long long>(
                             order_it -
                             QD.dfs_order.begin()))
               << '\n';
    }
    output << "\n[Candidate Summary]\n";
    for (const auto& node : nodes) {
        const auto candidates = uniqueCandidates(node);
        std::size_t relation_entries =
            node.root_candidates.size();
        for (const auto& block : node.blocks) {
            relation_entries += block.candidates.size();
        }
        output << "q" << node.query_vertex_id
               << " label="
               << labelToString(
                      Q.vertex_labels[static_cast<std::size_t>(
                          node.query_vertex_id)])
               << " blocks=" << node.blocks.size()
               << " candidates=" << candidates.size()
               << " relation_entries=" << relation_entries
               << " sample=";
        const std::size_t sample_size =
            std::min<std::size_t>(20, candidates.size());
        for (std::size_t i = 0; i < sample_size; ++i) {
            if (i > 0) output << ',';
            output << G.externalId(candidates[i]);
        }
        if (candidates.size() > sample_size) output << ",...";
        output << '\n';
    }

    output << "\n[Final Matches]\nCount: ";
    const std::streampos count_position = output.tellp();
    output << std::setw(20) << 0 << '\n';

    const auto start = std::chrono::steady_clock::now();
    const CheckedMatchCounter counter = enumerateMatches(
        output, mode, summary.threads_requested, summary);
    summary.match_count = counter.value;
    summary.count_overflow = counter.overflowed;
    summary.enumeration_milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();

    const std::streampos end_position = output.tellp();
    output.seekp(count_position);
    if (summary.count_overflow) {
        output << std::setw(20) << "OVERFLOW";
    } else {
        output << std::setw(20) << summary.match_count;
    }
    output.seekp(end_position);
    output << "\n[Statistics]\n"
           << "mode: " << matchOutputModeName(mode) << '\n'
           << "duration_mode: " << durationModeName(duration_mode) << '\n'
           << "count_overflow: "
           << (summary.count_overflow ? "true" : "false") << '\n'
           << "threads_requested: "
           << summary.threads_requested << '\n'
           << "threads_used: " << summary.threads_used << '\n'
           << "workers_with_tasks: "
           << summary.workers_with_tasks << '\n'
           << "root_tasks_claimed: "
           << summary.root_tasks_claimed << '\n'
           << "worker_root_tasks_min: "
           << summary.worker_root_tasks_min << '\n'
           << "worker_root_tasks_max: "
           << summary.worker_root_tasks_max << '\n'
           << "root_candidate_count: "
           << summary.root_candidate_count << '\n'
           << "candidate_relation_entries: " << candidateRelationCount() << '\n'
           << "dfs_states: " << summary.dfs_states << '\n'
           << "metric_schema_version: logical_extensions_v1\n"
           << "raw_extension_candidates: "
           << summary.generated_candidates << '\n'
           << "post_structural_candidates: "
           << summary.generated_candidates << '\n'
           << "generated_candidates: "
           << summary.generated_candidates << '\n'
           << "accepted_candidates: "
           << summary.accepted_candidates << '\n'
           << "accepted_extensions: "
           << summary.accepted_candidates << '\n'
           << "completed_matches: "
           << summary.completed_matches << '\n'
           << "pruned_injectivity: "
           << summary.pruned_injectivity << '\n'
           << "pruned_temporal_support: "
           << summary.pruned_temporal_support << '\n'
           << "enumeration_ms: " << summary.enumeration_milliseconds << '\n';
    output.flush();
    summary.output_written = output.good();
    return summary;
}

std::size_t TDTree::getMemoryUsage() const {
    std::size_t total = sizeof(TDTree);
    total += nodes.capacity() * sizeof(TDTreeNode);
    total += query_out_neighbor_label_requirements.capacity() *
        sizeof(std::array<int, kLabelCount>);
    total += query_in_neighbor_label_requirements.capacity() *
        sizeof(std::array<int, kLabelCount>);
    for (const auto& node : nodes) {
        total += node.root_candidates.capacity() * sizeof(TDTreeCell);
        for (const auto& cell : node.root_candidates) {
            total +=
                cell.leaf_time_instances.capacity() * sizeof(TimeInterval);
            total +=
                cell.child_block_indices.capacity() * sizeof(std::size_t);
        }
        total += node.blocks.capacity() * sizeof(TDTreeBlock);
        for (const auto& block : node.blocks) {
            total +=
                block.leaf_time_union.capacity() * sizeof(TimeInterval);
            total += block.candidates.capacity() * sizeof(TDTreeCell);
            for (const auto& cell : block.candidates) {
                total +=
                    cell.leaf_time_instances.capacity() * sizeof(TimeInterval);
                total +=
                    cell.child_block_indices.capacity() * sizeof(std::size_t);
            }
        }
    }
    return total;
}
