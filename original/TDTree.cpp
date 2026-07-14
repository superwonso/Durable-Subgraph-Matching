#include "TDTree.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <unordered_map>

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

const TDTreeBlock* TDTreeNode::findBlock(int parent_vertex) const {
    const auto found = block_index.find(parent_vertex);
    if (found == block_index.end() || found->second >= blocks.size()) return nullptr;
    return &blocks[found->second];
}

void TDTreeNode::rebuildBlockIndex() {
    std::sort(blocks.begin(), blocks.end(), [](const TDTreeBlock& lhs, const TDTreeBlock& rhs) {
        return lhs.v_par < rhs.v_par;
    });

    std::vector<TDTreeBlock> normalized;
    normalized.reserve(blocks.size());
    for (auto& block : blocks) {
        if (!normalized.empty() && normalized.back().v_par == block.v_par) {
            auto& candidates = normalized.back().V_cand;
            candidates.insert(
                candidates.end(), block.V_cand.begin(), block.V_cand.end());
        } else {
            normalized.push_back(std::move(block));
        }
    }
    for (auto& block : normalized) {
        std::sort(block.V_cand.begin(), block.V_cand.end());
        block.V_cand.erase(
            std::unique(block.V_cand.begin(), block.V_cand.end()),
            block.V_cand.end());
    }
    blocks.swap(normalized);

    block_index.clear();
    block_index.reserve(blocks.size());
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        block_index.emplace(blocks[i].v_par, i);
    }
}

TDTree::TDTree(
    const Graph& temporal_graph,
    const Graph& query_graph,
    const QueryDecomposition& decomposition,
    int minimum_duration)
    : G(temporal_graph),
      Q(query_graph),
      QD(decomposition),
      k_threshold(minimum_duration) {
    build();
}

void TDTree::build() {
    initializeNodes();
    initializeQueryRequirements();
    fillRoot();
    buildCandidateRelations();

    // A bottom-up semijoin removes unsupported parents. The following
    // top-down semijoin removes orphaned descendant blocks in linear time.
    trimBottomUp();
    trimTopDown();
    rebuildBlockIndexes();
}

void TDTree::initializeNodes() {
    nodes.resize(static_cast<std::size_t>(Q.num_vertices));
    for (int query_vertex = 0; query_vertex < Q.num_vertices; ++query_vertex) {
        auto& node = nodes[static_cast<std::size_t>(query_vertex)];
        node.query_vertex_id = query_vertex;
        node.isRoot = query_vertex == QD.root;
        node.isLeaf = QD.spanning_tree_adj[static_cast<std::size_t>(query_vertex)].empty();
    }
}

void TDTree::initializeQueryRequirements() {
    query_out_neighbor_label_requirements.assign(static_cast<std::size_t>(Q.num_vertices), {});
    query_in_neighbor_label_requirements.assign(static_cast<std::size_t>(Q.num_vertices), {});
    for (int query_vertex = 0; query_vertex < Q.num_vertices; ++query_vertex) {
        auto& out_requirements =
            query_out_neighbor_label_requirements[static_cast<std::size_t>(query_vertex)];
        for (const auto& edge : Q.adj[static_cast<std::size_t>(query_vertex)]) {
            const Label neighbor_label = Q.vertex_labels[static_cast<std::size_t>(edge.to)];
            if (neighbor_label < kLabelCount) ++out_requirements[neighbor_label];
        }
        auto& in_requirements =
            query_in_neighbor_label_requirements[static_cast<std::size_t>(query_vertex)];
        for (const auto& edge : Q.in_adj[static_cast<std::size_t>(query_vertex)]) {
            const Label neighbor_label = Q.vertex_labels[static_cast<std::size_t>(edge.to)];
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
    if (G.adj[data_index].size() < Q.adj[query_index].size()) return false;
    if (G.in_adj[data_index].size() < Q.in_adj[query_index].size()) return false;
    if (data_index >= G.vertex_active_durations.size() ||
        G.vertex_active_durations[data_index] < k_threshold) {
        return false;
    }

    const auto& out_available = G.neighbor_label_counts[data_index];
    const auto& out_required = query_out_neighbor_label_requirements[query_index];
    const auto& in_available = G.incoming_neighbor_label_counts[data_index];
    const auto& in_required = query_in_neighbor_label_requirements[query_index];
    for (std::size_t label = 0; label < kLabelCount; ++label) {
        if (out_available[label] < out_required[label] ||
            in_available[label] < in_required[label]) {
            return false;
        }
    }
    return true;
}

void TDTree::fillRoot() {
    if (QD.root < 0 || QD.root >= Q.num_vertices) return;
    auto& root = nodes[static_cast<std::size_t>(QD.root)];
    root.root_candidates.reserve(static_cast<std::size_t>(G.num_vertices) / kLabelCount + 1);
    for (int data_vertex = 0; data_vertex < G.num_vertices; ++data_vertex) {
        if (isDataVertexCandidate(data_vertex, QD.root)) {
            root.root_candidates.push_back(data_vertex);
        }
    }
}

bool TDTree::passesAvailableNonTreeConstraints(
    int data_vertex,
    int query_vertex,
    const std::vector<int>& order_position,
    const std::vector<std::vector<std::uint8_t>>& candidate_flags) const {
    for (const auto& non_tree_edge : QD.non_tree_edges) {
        int other_query_vertex = -1;
        const std::vector<Edge>* candidate_neighbors = nullptr;
        if (non_tree_edge.first == query_vertex) {
            other_query_vertex = non_tree_edge.second;
            candidate_neighbors = &G.adj[static_cast<std::size_t>(data_vertex)];
        } else if (non_tree_edge.second == query_vertex) {
            other_query_vertex = non_tree_edge.first;
            candidate_neighbors = &G.in_adj[static_cast<std::size_t>(data_vertex)];
        } else {
            continue;
        }

        if (order_position[static_cast<std::size_t>(other_query_vertex)] >=
            order_position[static_cast<std::size_t>(query_vertex)]) {
            continue;
        }

        bool found_compatible_neighbor = false;
        for (const auto& edge_ref : *candidate_neighbors) {
            if (candidate_flags[static_cast<std::size_t>(other_query_vertex)]
                               [static_cast<std::size_t>(edge_ref.to)] == 0) {
                continue;
            }
            const auto& temporal_edge = G.temporal_edges[static_cast<std::size_t>(edge_ref.temporal_edge_id)];
            if (hasMinimumConsecutiveDuration(temporal_edge.active_intervals, k_threshold)) {
                found_compatible_neighbor = true;
                break;
            }
        }
        if (!found_compatible_neighbor) return false;
    }
    return true;
}

void TDTree::buildCandidateRelations() {
    if (QD.root < 0 || !QD.connected) return;

    std::vector<int> order_position(static_cast<std::size_t>(Q.num_vertices), -1);
    for (std::size_t i = 0; i < QD.dfs_order.size(); ++i) {
        order_position[static_cast<std::size_t>(QD.dfs_order[i])] = static_cast<int>(i);
    }

    std::vector<std::vector<std::uint8_t>> candidate_flags(
        static_cast<std::size_t>(Q.num_vertices),
        std::vector<std::uint8_t>(static_cast<std::size_t>(G.num_vertices), 0));
    std::vector<std::vector<int>> candidate_lists(static_cast<std::size_t>(Q.num_vertices));

    auto& root_candidates = nodes[static_cast<std::size_t>(QD.root)].root_candidates;
    candidate_lists[static_cast<std::size_t>(QD.root)] = root_candidates;
    for (int candidate : root_candidates) {
        candidate_flags[static_cast<std::size_t>(QD.root)][static_cast<std::size_t>(candidate)] = 1;
    }

    for (std::size_t order_index = 1; order_index < QD.dfs_order.size(); ++order_index) {
        const int query_vertex = QD.dfs_order[order_index];
        const int parent_query_vertex = QD.parent[static_cast<std::size_t>(query_vertex)];
        if (parent_query_vertex < 0) continue;
        const bool parent_to_child =
            GraphUtils::hasEdge(Q.adj, parent_query_vertex, query_vertex);
        const bool child_to_parent =
            GraphUtils::hasEdge(Q.adj, query_vertex, parent_query_vertex);
        if (!parent_to_child && !child_to_parent) continue;

        auto& node = nodes[static_cast<std::size_t>(query_vertex)];
        const auto& parent_candidates = candidate_lists[static_cast<std::size_t>(parent_query_vertex)];
        node.blocks.reserve(parent_candidates.size());

        for (int parent_data_vertex : parent_candidates) {
            TDTreeBlock block;
            block.v_par = parent_data_vertex;
            const auto& expansion_edges = parent_to_child
                ? G.adj[static_cast<std::size_t>(parent_data_vertex)]
                : G.in_adj[static_cast<std::size_t>(parent_data_vertex)];
            for (const auto& edge_ref : expansion_edges) {
                const int candidate = edge_ref.to;
                if (!isDataVertexCandidate(candidate, query_vertex)) continue;

                const TemporalEdge* forward_edge = parent_to_child
                    ? G.findTemporalEdge(parent_data_vertex, candidate)
                    : nullptr;
                const TemporalEdge* reverse_edge = child_to_parent
                    ? G.findTemporalEdge(candidate, parent_data_vertex)
                    : nullptr;
                if ((parent_to_child &&
                     (forward_edge == nullptr || !hasMinimumConsecutiveDuration(
                         forward_edge->active_intervals, k_threshold))) ||
                    (child_to_parent &&
                     (reverse_edge == nullptr || !hasMinimumConsecutiveDuration(
                         reverse_edge->active_intervals, k_threshold)))) {
                    continue;
                }
                if (!passesAvailableNonTreeConstraints(
                        candidate, query_vertex, order_position, candidate_flags)) {
                    continue;
                }

                block.V_cand.push_back(candidate);
                auto& flag = candidate_flags[static_cast<std::size_t>(query_vertex)]
                                            [static_cast<std::size_t>(candidate)];
                if (flag == 0) {
                    flag = 1;
                    candidate_lists[static_cast<std::size_t>(query_vertex)].push_back(candidate);
                }
            }

            if (!block.V_cand.empty()) node.blocks.push_back(std::move(block));
        }
        std::sort(
            candidate_lists[static_cast<std::size_t>(query_vertex)].begin(),
            candidate_lists[static_cast<std::size_t>(query_vertex)].end());
    }
}

void TDTree::trimBottomUp() {
    for (auto order_it = QD.dfs_order.rbegin(); order_it != QD.dfs_order.rend(); ++order_it) {
        const int query_vertex = *order_it;
        const auto& children = QD.spanning_tree_adj[static_cast<std::size_t>(query_vertex)];
        if (children.empty()) continue;

        std::unordered_map<int, int> support_count;
        std::size_t support_entries = 0;
        for (int child : children) support_entries += nodes[static_cast<std::size_t>(child)].blocks.size();
        support_count.reserve(support_entries);
        for (int child : children) {
            const auto& child_blocks = nodes[static_cast<std::size_t>(child)].blocks;
            std::unordered_map<int, bool> seen_parent;
            seen_parent.reserve(child_blocks.size());
            for (const auto& block : child_blocks) {
                if (!block.V_cand.empty()) seen_parent.emplace(block.v_par, true);
            }
            for (const auto& entry : seen_parent) ++support_count[entry.first];
        }

        const int required_support = static_cast<int>(children.size());
        auto supported = [&](int candidate) {
            const auto found = support_count.find(candidate);
            return found != support_count.end() && found->second == required_support;
        };

        auto& node = nodes[static_cast<std::size_t>(query_vertex)];
        if (node.isRoot) {
            node.root_candidates.erase(
                std::remove_if(
                    node.root_candidates.begin(), node.root_candidates.end(),
                    [&](int candidate) { return !supported(candidate); }),
                node.root_candidates.end());
        } else {
            for (auto& block : node.blocks) {
                block.V_cand.erase(
                    std::remove_if(
                        block.V_cand.begin(), block.V_cand.end(),
                        [&](int candidate) { return !supported(candidate); }),
                    block.V_cand.end());
            }
            node.blocks.erase(
                std::remove_if(
                    node.blocks.begin(), node.blocks.end(),
                    [](const TDTreeBlock& block) { return block.V_cand.empty(); }),
                node.blocks.end());
        }
    }
}

void TDTree::trimTopDown() {
    std::vector<int> candidate_marks(static_cast<std::size_t>(G.num_vertices), 0);
    int mark = 0;
    for (int query_vertex : QD.dfs_order) {
        if (mark == std::numeric_limits<int>::max()) {
            std::fill(candidate_marks.begin(), candidate_marks.end(), 0);
            mark = 0;
        }
        ++mark;

        const auto parent_candidates = uniqueCandidates(nodes[static_cast<std::size_t>(query_vertex)]);
        for (int candidate : parent_candidates) {
            candidate_marks[static_cast<std::size_t>(candidate)] = mark;
        }

        for (int child : QD.spanning_tree_adj[static_cast<std::size_t>(query_vertex)]) {
            auto& child_blocks = nodes[static_cast<std::size_t>(child)].blocks;
            child_blocks.erase(
                std::remove_if(
                    child_blocks.begin(), child_blocks.end(),
                    [&](const TDTreeBlock& block) {
                        return candidate_marks[static_cast<std::size_t>(block.v_par)] != mark;
                    }),
                child_blocks.end());
        }
    }
}

void TDTree::rebuildBlockIndexes() {
    for (auto& node : nodes) node.rebuildBlockIndex();
}

std::vector<int> TDTree::uniqueCandidates(const TDTreeNode& node) const {
    if (node.isRoot) return node.root_candidates;

    std::vector<int> candidates;
    for (const auto& block : node.blocks) {
        candidates.insert(candidates.end(), block.V_cand.begin(), block.V_cand.end());
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
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
            for (const auto& block : node.blocks) count += block.V_cand.size();
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
            for (const auto& block : node.blocks) relation_count += block.V_cand.size();
        }
        std::cout << "  q" << node.query_vertex_id
                  << " label=" << labelToString(Q.vertex_labels[static_cast<std::size_t>(node.query_vertex_id)])
                  << " blocks=" << node.blocks.size()
                  << " unique_candidates=" << uniqueCandidateCount(node)
                  << " relation_entries=" << relation_count << '\n';
    }
}

CheckedMatchCounter TDTree::enumerateMatches(
    std::ostream& output,
    MatchOutputMode mode) const {
    CheckedMatchCounter match_counter;
    if (QD.root < 0 || !QD.connected || QD.dfs_order.empty()) {
        return match_counter;
    }

    std::vector<int> mapping(static_cast<std::size_t>(Q.num_vertices), -1);
    std::vector<std::uint8_t> used_data_vertices(static_cast<std::size_t>(G.num_vertices), 0);

    std::function<void(std::size_t, const std::vector<TimeInterval>&, bool)> dfs;
    dfs = [&](std::size_t depth, const std::vector<TimeInterval>& current_intervals, bool has_intervals) {
        if (match_counter.overflowed) return;
        if (depth >= QD.dfs_order.size()) {
            if (!match_counter.increment()) return;
            if (mode == MatchOutputMode::Full) {
                output << "Match " << (match_counter.value - 1) << ": ";
                for (int query_vertex = 0; query_vertex < Q.num_vertices; ++query_vertex) {
                    if (query_vertex > 0) output << ", ";
                    output << 'q' << query_vertex << '(' <<
                        labelToString(Q.vertex_labels[static_cast<std::size_t>(query_vertex)]) << ")->" <<
                        G.externalId(mapping[static_cast<std::size_t>(query_vertex)]);
                }
                output << " | active=" << formatIntervals(current_intervals) << '\n';
            }
            return;
        }

        const int query_vertex = QD.dfs_order[depth];
        const int parent_query_vertex = QD.parent[static_cast<std::size_t>(query_vertex)];
        if (parent_query_vertex < 0) return;
        const int parent_data_vertex = mapping[static_cast<std::size_t>(parent_query_vertex)];
        const TDTreeBlock* block = nodes[static_cast<std::size_t>(query_vertex)].findBlock(parent_data_vertex);
        if (block == nullptr) return;

        for (int candidate : block->V_cand) {
            if (used_data_vertices[static_cast<std::size_t>(candidate)] != 0) continue;

            std::vector<TimeInterval> next_intervals = current_intervals;
            bool next_has_intervals = has_intervals;
            bool valid = true;

            auto apply_temporal_edge = [&](int source_data_vertex, int target_data_vertex) {
                const TemporalEdge* temporal_edge =
                    G.findTemporalEdge(source_data_vertex, target_data_vertex);
                if (temporal_edge == nullptr) return false;

                if (!next_has_intervals) {
                    next_intervals.clear();
                    for (const auto& interval : temporal_edge->active_intervals) {
                        if (interval.length() >= k_threshold) next_intervals.push_back(interval);
                    }
                    next_has_intervals = true;
                } else {
                    next_intervals = intersectTimeIntervals(
                        next_intervals, temporal_edge->active_intervals, k_threshold);
                }
                return !next_intervals.empty();
            };

            // A self-loop is represented in both adj and in_adj, so apply it
            // exactly once before checking arcs to previously mapped vertices.
            if (GraphUtils::hasEdge(Q.adj, query_vertex, query_vertex) &&
                !apply_temporal_edge(candidate, candidate)) {
                continue;
            }

            // Check every outgoing and incoming query arc whose other endpoint
            // is already mapped. Tree orientation never changes arc direction.
            for (const auto& query_edge : Q.adj[static_cast<std::size_t>(query_vertex)]) {
                const int other_query_vertex = query_edge.to;
                if (other_query_vertex == query_vertex) continue;
                const int other_data_vertex = mapping[static_cast<std::size_t>(other_query_vertex)];
                if (other_data_vertex < 0) continue;
                if (!apply_temporal_edge(candidate, other_data_vertex)) {
                    valid = false;
                    break;
                }
            }
            if (!valid) continue;
            for (const auto& query_edge : Q.in_adj[static_cast<std::size_t>(query_vertex)]) {
                const int other_query_vertex = query_edge.to;
                if (other_query_vertex == query_vertex) continue;
                const int other_data_vertex = mapping[static_cast<std::size_t>(other_query_vertex)];
                if (other_data_vertex < 0) continue;
                if (!apply_temporal_edge(other_data_vertex, candidate)) {
                    valid = false;
                    break;
                }
            }
            if (!valid) continue;

            mapping[static_cast<std::size_t>(query_vertex)] = candidate;
            used_data_vertices[static_cast<std::size_t>(candidate)] = 1;
            dfs(depth + 1, next_intervals, next_has_intervals);
            used_data_vertices[static_cast<std::size_t>(candidate)] = 0;
            mapping[static_cast<std::size_t>(query_vertex)] = -1;
            if (match_counter.overflowed) return;
        }
    };

    const auto& root_candidates = nodes[static_cast<std::size_t>(QD.root)].root_candidates;
    for (int root_candidate : root_candidates) {
        std::vector<TimeInterval> root_intervals;
        bool root_has_intervals = false;
        if (GraphUtils::hasEdge(Q.adj, QD.root, QD.root)) {
            const TemporalEdge* self_edge =
                G.findTemporalEdge(root_candidate, root_candidate);
            if (self_edge == nullptr) continue;
            for (const auto& interval : self_edge->active_intervals) {
                if (interval.length() >= k_threshold) {
                    root_intervals.push_back(interval);
                }
            }
            if (root_intervals.empty()) continue;
            root_has_intervals = true;
        }
        mapping[static_cast<std::size_t>(QD.root)] = root_candidate;
        used_data_vertices[static_cast<std::size_t>(root_candidate)] = 1;
        dfs(1, root_intervals, root_has_intervals);
        used_data_vertices[static_cast<std::size_t>(root_candidate)] = 0;
        mapping[static_cast<std::size_t>(QD.root)] = -1;
        if (match_counter.overflowed) break;
    }
    return match_counter;
}

MatchSummary TDTree::save_res(
    const std::string& filename,
    MatchOutputMode mode) const {
    MatchSummary summary;
    // Binary mode keeps tellp/seekp offsets stable on Windows (text mode
    // translates '\n' to CRLF and would corrupt the count placeholder).
    std::ofstream output(filename, std::ios::binary);
    if (!output.is_open()) return summary;

    output << "[Run]\nmode: " << matchOutputModeName(mode)
           << "\n\n[Candidate Summary]\n";
    for (const auto& node : nodes) {
        const auto candidates = uniqueCandidates(node);
        output << "q" << node.query_vertex_id
               << " label=" << labelToString(Q.vertex_labels[static_cast<std::size_t>(node.query_vertex_id)])
               << " blocks=" << node.blocks.size()
               << " candidates=" << candidates.size()
               << " sample=";
        const std::size_t sample_size = std::min<std::size_t>(20, candidates.size());
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
    const CheckedMatchCounter counter = enumerateMatches(output, mode);
    summary.match_count = counter.value;
    summary.count_overflow = counter.overflowed;
    summary.enumeration_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

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
           << "count_overflow: " << (summary.count_overflow ? "true" : "false") << '\n'
           << "candidate_relation_entries: " << candidateRelationCount() << '\n'
           << "enumeration_ms: " << summary.enumeration_milliseconds << '\n';
    output.flush();
    summary.output_written = output.good();
    return summary;
}

std::size_t TDTree::getMemoryUsage() const {
    std::size_t total = nodes.capacity() * sizeof(TDTreeNode);
    total += query_out_neighbor_label_requirements.capacity() *
        sizeof(std::array<int, kLabelCount>);
    total += query_in_neighbor_label_requirements.capacity() *
        sizeof(std::array<int, kLabelCount>);
    for (const auto& node : nodes) {
        total += node.root_candidates.capacity() * sizeof(int);
        total += node.blocks.capacity() * sizeof(TDTreeBlock);
        for (const auto& block : node.blocks) {
            total += block.V_cand.capacity() * sizeof(int);
        }
        total += node.block_index.size() *
            (sizeof(std::pair<const int, std::size_t>) + sizeof(void*) * 2);
    }
    return total;
}
