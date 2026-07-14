#include "TDTree.h"

#include <algorithm>
#include <deque>
#include <fstream>
#include <iostream>
#include <queue>
#include <sstream>
#include <unordered_map>

TDTree::TDTree(const Graph& temporal_graph, const Graph& query_graph, const QueryDecomposition& decomposition, int k)
    : G(temporal_graph),
      Q(query_graph),
      QD(decomposition),
      k_threshold(k),
      candidate_expander_(std::make_unique<CandidateExpander>(temporal_graph, decomposition, k)) {
    build();
}

void TDTree::build() {
    initTree(QD.spanning_tree_adj);
    std::cout << "Candidate expansion backend: "
              << (candidate_expander_->isGpuEnabled() ? "GPU" : "CPU fallback")
              << std::endl;
    growTDTree();
    trimTDTree();
    std::cout << "TD-Tree built successfully!" << std::endl;
}

void TDTree::initTree(const std::vector<std::vector<int>>& query_tree) {
    nodes.clear();
    nodes.reserve(query_tree.size());
    for (size_t i = 0; i < query_tree.size(); ++i) {
        auto node = std::make_unique<TDTreeNode>(static_cast<int>(i));
        node->isLeaf = query_tree[i].empty();
        nodes.push_back(std::move(node));
    }
    std::cout << "TD-Tree initialized with " << nodes.size() << " nodes." << std::endl;
}

int TDTree::getRootQueryVertex() const {
    if (QD.root >= 0 && QD.root < Q.num_vertices) {
        return QD.root;
    }

    std::vector<int> indegree(Q.num_vertices, 0);
    for (int u = 0; u < static_cast<int>(QD.spanning_tree_adj.size()); ++u) {
        for (int v : QD.spanning_tree_adj[u]) {
            if (v >= 0 && v < Q.num_vertices) {
                indegree[v]++;
            }
        }
    }
    for (int i = 0; i < Q.num_vertices; ++i) {
        if (indegree[i] == 0) {
            return i;
        }
    }
    return -1;
}

std::vector<TDTreeNode*> TDTree::buildNodeIndex() const {
    std::vector<TDTreeNode*> node_by_qid(Q.num_vertices, nullptr);
    for (const auto& node_ptr : nodes) {
        if (node_ptr && node_ptr->query_vertex_id >= 0 && node_ptr->query_vertex_id < Q.num_vertices) {
            node_by_qid[node_ptr->query_vertex_id] = node_ptr.get();
        }
    }
    return node_by_qid;
}

std::vector<int> TDTree::buildTraversalOrder() const {
    if (!QD.traversal_order.empty()) {
        return QD.traversal_order;
    }

    std::vector<int> order;
    const int root_qid = getRootQueryVertex();
    if (root_qid < 0) {
        return order;
    }

    std::queue<int> q;
    std::vector<bool> visited(Q.num_vertices, false);
    q.push(root_qid);
    visited[root_qid] = true;

    while (!q.empty()) {
        const int current = q.front();
        q.pop();
        order.push_back(current);

        for (int child_qid : QD.spanning_tree_adj[current]) {
            if (child_qid >= 0 && child_qid < Q.num_vertices && !visited[child_qid]) {
                visited[child_qid] = true;
                q.push(child_qid);
            }
        }
    }

    return order;
}

std::vector<int> TDTree::buildReverseTraversalOrder() const {
    std::vector<int> order = buildTraversalOrder();
    std::reverse(order.begin(), order.end());
    return order;
}

void TDTree::fillRoot() {
    const int root_qid = getRootQueryVertex();
    if (root_qid < 0 || root_qid >= static_cast<int>(nodes.size())) {
        std::cout << "Error: Failed to identify root query vertex." << std::endl;
        return;
    }

    TDTreeNode* root = nodes[root_qid].get();
    const std::vector<int> root_candidates = candidate_expander_->filterRootCandidates(root_qid);

    for (int v : root_candidates) {
        TDTreeBlock block(-1);
        block.V_cand.push_back(v);
        block.TS = computeVertexTimeInstances(v);
        root->blocks.emplace_back(std::move(block));
        root->bloom->add(v);
    }

    std::cout << "Root node filled with " << root->blocks.size() << " candidate blocks." << std::endl;
}

void TDTree::growTDTree() {
    fillRoot();

    size_t traversed_neighbor_edges = 0;
    size_t rejected_non_tree = 0;
    size_t missing_time_instances = 0;
    size_t rejected_duration = 0;
    size_t rejected_duration_size_lt_k = 0;
    size_t rejected_duration_non_consecutive = 0;
    size_t accepted_candidates = 0;
    std::unordered_map<int, size_t> child_attempts;
    std::unordered_map<int, size_t> child_accepts;
    std::unordered_map<int, size_t> child_reject_non_tree;
    std::unordered_map<int, size_t> child_reject_duration_size_lt_k;
    std::unordered_map<int, size_t> child_reject_duration_non_consecutive;
    std::unordered_map<int, size_t> child_reject_duration_non_consecutive_ts_sum;
    std::unordered_map<int, size_t> child_reject_duration_non_consecutive_maxrun_sum;

    auto max_consecutive_run = [](const std::unordered_set<int>& ts_set) -> int {
        if (ts_set.empty()) {
            return 0;
        }
        std::vector<int> times(ts_set.begin(), ts_set.end());
        std::sort(times.begin(), times.end());
        int best = 1;
        int current = 1;
        for (size_t i = 1; i < times.size(); ++i) {
            if (times[i] == times[i - 1] + 1) {
                current++;
            } else {
                current = 1;
            }
            best = std::max(best, current);
        }
        return best;
    };

    const std::vector<int> traversal_order = buildTraversalOrder();
    if (traversal_order.empty()) {
        std::cout << "Error: traversal order is empty." << std::endl;
        return;
    }

    std::vector<TDTreeNode*> node_by_qid = buildNodeIndex();

    for (int qid : traversal_order) {
        TDTreeNode* current_node = (qid >= 0 && qid < static_cast<int>(node_by_qid.size())) ? node_by_qid[qid] : nullptr;
        if (current_node == nullptr) {
            continue;
        }

        for (const auto& block : current_node->blocks) {
            for (int v : block.V_cand) {
                for (int child_query_id : QD.spanning_tree_adj[current_node->query_vertex_id]) {
                    TDTreeNode* child_node =
                        (child_query_id >= 0 && child_query_id < static_cast<int>(node_by_qid.size()))
                            ? node_by_qid[child_query_id]
                            : nullptr;
                    if (child_node == nullptr) {
                        continue;
                    }

                    const std::vector<int> raw_candidates =
                        candidate_expander_->filterChildCandidates(v, child_query_id);
                    traversed_neighbor_edges += raw_candidates.size();
                    child_attempts[child_query_id] += raw_candidates.size();

                    for (int v_prime : raw_candidates) {
                        if (!nonTreeEdgeTest(v_prime, child_node)) {
                            rejected_non_tree++;
                            child_reject_non_tree[child_query_id]++;
                            continue;
                        }

                        auto it_edge_time_instances = G.edge_time_instances.find({v, v_prime});
                        if (it_edge_time_instances == G.edge_time_instances.end()) {
                            it_edge_time_instances = G.edge_time_instances.find({v_prime, v});
                        }
                        if (it_edge_time_instances == G.edge_time_instances.end()) {
                            missing_time_instances++;
                            continue;
                        }

                        std::unordered_set<int> ts_intersection =
                            intersectTimeSets(it_edge_time_instances->second, block.TS);
                        if (!checkMinimumConsecutiveDuration(ts_intersection)) {
                            rejected_duration++;
                            const size_t ts_size = ts_intersection.size();
                            if (ts_size < static_cast<size_t>(k_threshold)) {
                                rejected_duration_size_lt_k++;
                                child_reject_duration_size_lt_k[child_query_id]++;
                            } else {
                                rejected_duration_non_consecutive++;
                                child_reject_duration_non_consecutive[child_query_id]++;
                                const int max_run = max_consecutive_run(ts_intersection);
                                child_reject_duration_non_consecutive_ts_sum[child_query_id] += ts_size;
                                child_reject_duration_non_consecutive_maxrun_sum[child_query_id] +=
                                    static_cast<size_t>(max_run);
                            }
                            continue;
                        }

                        TDTreeBlock child_block(v);
                        child_block.V_cand.push_back(v_prime);
                        child_block.TS = std::move(ts_intersection);
                        child_node->blocks.emplace_back(std::move(child_block));
                        child_node->bloom->add(v_prime);
                        accepted_candidates++;
                        child_accepts[child_query_id]++;
                    }
                }
            }
        }
    }

    std::cout << "TD-Tree grown with " << nodes.size() << " nodes." << std::endl;
    std::cout << "[Grow Debug] traversed_neighbor_edges=" << traversed_neighbor_edges
              << ", rejected_non_tree=" << rejected_non_tree
              << ", missing_time_instances=" << missing_time_instances
              << ", rejected_duration=" << rejected_duration
              << " (size_lt_k=" << rejected_duration_size_lt_k
              << ", non_consecutive=" << rejected_duration_non_consecutive << ")"
              << ", accepted_candidates=" << accepted_candidates << std::endl;

    for (int qid = 0; qid < Q.num_vertices; ++qid) {
        TDTreeNode* node = (qid >= 0 && qid < static_cast<int>(node_by_qid.size())) ? node_by_qid[qid] : nullptr;
        size_t block_count = 0;
        size_t cand_count = 0;
        if (node != nullptr) {
            block_count = node->blocks.size();
            for (const auto& block : node->blocks) {
                cand_count += block.V_cand.size();
            }
        }

        std::cout << "[Grow Debug] q" << qid
                  << " blocks=" << block_count
                  << " candidates=" << cand_count
                  << " attempts=" << child_attempts[qid]
                  << " accepts=" << child_accepts[qid]
                  << " reject_non_tree=" << child_reject_non_tree[qid]
                  << " reject_ts_size_lt_k=" << child_reject_duration_size_lt_k[qid]
                  << " reject_ts_non_consecutive=" << child_reject_duration_non_consecutive[qid]
                  << std::endl;

        const size_t non_consecutive_count = child_reject_duration_non_consecutive[qid];
        if (non_consecutive_count > 0) {
            const double avg_ts_size = static_cast<double>(
                                           child_reject_duration_non_consecutive_ts_sum[qid]) /
                                       static_cast<double>(non_consecutive_count);
            const double avg_max_run = static_cast<double>(
                                           child_reject_duration_non_consecutive_maxrun_sum[qid]) /
                                       static_cast<double>(non_consecutive_count);
            std::cout << "[Grow Debug] q" << qid
                      << " non_consecutive_stats avg_ts_size=" << avg_ts_size
                      << " avg_max_run=" << avg_max_run
                      << std::endl;
        }
    }
}

bool TDTree::nonTreeEdgeTest(int v_prime, TDTreeNode* current_node) const {
    if (current_node == nullptr) {
        return false;
    }

    const int qid = current_node->query_vertex_id;
    for (const auto& edge : QD.non_tree_edges) {
        int other_qid = -1;
        if (edge.first == qid) {
            other_qid = edge.second;
        } else if (edge.second == qid) {
            other_qid = edge.first;
        } else {
            continue;
        }

        const TDTreeNode* other_node = nullptr;
        for (const auto& node_ptr : nodes) {
            if (node_ptr && node_ptr->query_vertex_id == other_qid) {
                other_node = node_ptr.get();
                break;
            }
        }

        if (other_node == nullptr || other_node->bloom == nullptr || other_node->blocks.empty()) {
            continue;
        }

        bool has_possible_neighbor = false;
        for (const auto& g_edge : G.adj[v_prime]) {
            if (other_node->bloom->possiblyContains(g_edge.to)) {
                has_possible_neighbor = true;
                break;
            }
        }

        if (!has_possible_neighbor) {
            return false;
        }
    }

    return true;
}

void TDTree::trimTDTree() {
    auto print_stage_summary = [&](const std::string& stage) {
        std::cout << "\n[Trim Debug] " << stage << std::endl;
        for (const auto& node_ptr : nodes) {
            const TDTreeNode* node = node_ptr.get();
            size_t candidate_count = 0;
            for (const auto& block : node->blocks) {
                candidate_count += block.V_cand.size();
            }
            std::cout << "  q" << node->query_vertex_id
                      << " blocks=" << node->blocks.size()
                      << " candidates=" << candidate_count
                      << std::endl;
        }
    };

    print_stage_summary("before top-down");
    trimTopDown();
    print_stage_summary("after top-down");
    trimBottomUp();
    print_stage_summary("after bottom-up");
    std::cout << "TD-Tree trimmed successfully!" << std::endl;
}

void TDTree::trimTopDown() {
    const int root_qid = getRootQueryVertex();
    if (root_qid < 0) {
        return;
    }

    std::vector<TDTreeNode*> node_by_qid = buildNodeIndex();
    std::queue<int> q;
    q.push(root_qid);

    while (!q.empty()) {
        const int parent_qid = q.front();
        q.pop();

        TDTreeNode* parent_node =
            (parent_qid >= 0 && parent_qid < static_cast<int>(node_by_qid.size())) ? node_by_qid[parent_qid] : nullptr;
        if (parent_node == nullptr) {
            continue;
        }

        std::unordered_set<int> parent_candidates;
        for (const auto& block : parent_node->blocks) {
            parent_candidates.insert(block.V_cand.begin(), block.V_cand.end());
        }

        for (int child_qid : QD.spanning_tree_adj[parent_qid]) {
            TDTreeNode* child_node =
                (child_qid >= 0 && child_qid < static_cast<int>(node_by_qid.size())) ? node_by_qid[child_qid] : nullptr;
            if (child_node == nullptr) {
                continue;
            }

            for (size_t i = 0; i < child_node->blocks.size();) {
                const TDTreeBlock& block = child_node->blocks[i];
                if (block.v_par == -1 || parent_candidates.find(block.v_par) != parent_candidates.end()) {
                    ++i;
                } else {
                    child_node->blocks.erase(child_node->blocks.begin() + static_cast<std::ptrdiff_t>(i));
                }
            }

            q.push(child_qid);
        }
    }
}

void TDTree::trimBottomUp() {
    std::vector<TDTreeNode*> node_by_qid = buildNodeIndex();
    const std::vector<int> reverse_order = buildReverseTraversalOrder();

    for (int qid : reverse_order) {
        if (qid < 0 || qid >= static_cast<int>(node_by_qid.size())) {
            continue;
        }

        TDTreeNode* node = node_by_qid[qid];
        if (node == nullptr || QD.spanning_tree_adj[qid].empty()) {
            continue;
        }

        std::unordered_map<int, std::unordered_set<int>> child_support;
        for (int child_qid : QD.spanning_tree_adj[qid]) {
            TDTreeNode* child_node =
                (child_qid >= 0 && child_qid < static_cast<int>(node_by_qid.size())) ? node_by_qid[child_qid] : nullptr;
            std::unordered_set<int> support;
            if (child_node != nullptr) {
                for (const auto& child_block : child_node->blocks) {
                    if (!child_block.V_cand.empty()) {
                        support.insert(child_block.v_par);
                    }
                }
            }
            child_support.emplace(child_qid, std::move(support));
        }

        for (size_t block_index = 0; block_index < node->blocks.size();) {
            TDTreeBlock& block = node->blocks[block_index];
            block.V_cand.erase(
                std::remove_if(
                    block.V_cand.begin(),
                    block.V_cand.end(),
                    [&](int v_cand) {
                        for (int child_qid : QD.spanning_tree_adj[qid]) {
                            auto it = child_support.find(child_qid);
                            if (it == child_support.end() || it->second.find(v_cand) == it->second.end()) {
                                return true;
                            }
                        }
                        return false;
                    }),
                block.V_cand.end());

            if (block.V_cand.empty()) {
                node->blocks.erase(node->blocks.begin() + static_cast<std::ptrdiff_t>(block_index));
            } else {
                ++block_index;
            }
        }
    }
}

void TDTree::removeBlock(TDTreeNode* node, int block_index) {
    if (node == nullptr || block_index < 0) {
        return;
    }

    const size_t idx = static_cast<size_t>(block_index);
    if (idx < node->blocks.size()) {
        node->blocks.erase(node->blocks.begin() + static_cast<std::ptrdiff_t>(idx));
    }
}

void TDTree::print() const {
    for (const auto& node_ptr : nodes) {
        const TDTreeNode* node = node_ptr.get();
        std::cout << "Query Vertex ID: " << node->query_vertex_id << std::endl;
        for (const auto& block : node->blocks) {
            std::cout << "  Parent Vertex: " << block.v_par << " | Candidates: ";
            for (int v : block.V_cand) {
                std::cout << v << ' ';
            }
            if (!block.TS.empty()) {
                std::cout << "| TS: { ";
                for (int ts : block.TS) {
                    std::cout << ts << ' ';
                }
                std::cout << "}";
            }
            std::cout << std::endl;
        }
    }
}

size_t TDTree::getMemoryUsage() const {
    size_t total = 0;
    for (const auto& node_ptr : nodes) {
        total += sizeof(TDTreeNode);
        if (node_ptr->bloom) {
            total += sizeof(BloomFilter) + (1000 / 8);
        }
        for (const auto& block : node_ptr->blocks) {
            total += sizeof(TDTreeBlock);
            total += block.V_cand.capacity() * sizeof(int);
            total += block.TS.size() * sizeof(int);
        }
    }
    return total;
}

void TDTree::print_res() const {
    for (const auto& node_ptr : nodes) {
        const TDTreeNode* node = node_ptr.get();
        std::vector<int> candidates;
        for (const auto& block : node->blocks) {
            candidates.insert(candidates.end(), block.V_cand.begin(), block.V_cand.end());
        }
        if (candidates.empty()) {
            continue;
        }

        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        std::cout << "Query Vertex ID: " << node->query_vertex_id << " - Candidates: ";
        for (int v : candidates) {
            std::cout << v << ' ';
        }
        std::cout << std::endl;
    }
}

void TDTree::save_res(const std::string& filename) const {
    std::ofstream out(filename);
    if (!out.is_open()) {
        return;
    }

    out << "[Candidate Summary]\n";
    for (const auto& node_ptr : nodes) {
        const TDTreeNode* node = node_ptr.get();
        std::vector<int> candidates;
        for (const auto& block : node->blocks) {
            candidates.insert(candidates.end(), block.V_cand.begin(), block.V_cand.end());
        }
        if (candidates.empty()) {
            continue;
        }

        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        out << "Query Vertex ID: " << node->query_vertex_id << " - Candidates: ";
        for (int v : candidates) {
            out << v << ' ';
        }
        out << '\n';
    }

    std::vector<const TDTreeNode*> node_by_qid(Q.num_vertices, nullptr);
    for (const auto& node_ptr : nodes) {
        if (node_ptr->query_vertex_id >= 0 && node_ptr->query_vertex_id < Q.num_vertices) {
            node_by_qid[node_ptr->query_vertex_id] = node_ptr.get();
        }
    }

    std::vector<int> parent = QD.parent;
    if (parent.size() != static_cast<size_t>(Q.num_vertices)) {
        parent.assign(Q.num_vertices, -1);
        for (int u = 0; u < static_cast<int>(QD.spanning_tree_adj.size()); ++u) {
            for (int v : QD.spanning_tree_adj[u]) {
                if (v >= 0 && v < Q.num_vertices) {
                    parent[v] = u;
                }
            }
        }
    }

    const int root_qid = getRootQueryVertex();
    const TDTreeNode* root_node =
        (root_qid >= 0 && root_qid < static_cast<int>(node_by_qid.size())) ? node_by_qid[root_qid] : nullptr;
    if (root_node == nullptr) {
        out << "\n[Final Matches]\nNo match (root node not found).\n";
        return;
    }

    std::vector<std::pair<int, std::unordered_set<int>>> root_candidates;
    size_t root_blocks_total = root_node->blocks.size();
    size_t root_blocks_with_vpar_minus1 = 0;
    size_t root_blocks_with_candidates = 0;
    for (const auto& block : root_node->blocks) {
        if (block.v_par == -1) {
            root_blocks_with_vpar_minus1++;
        }
        if (!block.V_cand.empty()) {
            root_blocks_with_candidates++;
        }
        if (block.v_par == -1 && !block.V_cand.empty()) {
            root_candidates.emplace_back(block.V_cand.front(), block.TS);
        }
    }

    std::vector<int> order = buildTraversalOrder();
    if (order.empty()) {
        out << "\n[Final Matches]\nNo match (empty traversal order).\n";
        return;
    }

    struct FinalMatch {
        std::vector<int> mapping;
        std::unordered_set<int> ts;
    };

    struct PartialMatchState {
        std::vector<int> mapping;
        std::unordered_set<int> used_data_vertices;
        std::unordered_set<int> current_ts;
        int depth = 0;
    };

    std::vector<FinalMatch> final_matches;
    std::unordered_set<std::string> unique_match_keys;

    auto get_edge_times = [&](int u, int v) -> const std::unordered_set<int>* {
        auto it = G.edge_time_instances.find({u, v});
        if (it != G.edge_time_instances.end()) {
            return &it->second;
        }
        it = G.edge_time_instances.find({v, u});
        if (it != G.edge_time_instances.end()) {
            return &it->second;
        }
        return nullptr;
    };

    auto query_has_edge = [&](int u, int v) -> bool {
        return GraphUtils::hasEdge(Q.adj, u, v) || GraphUtils::hasEdge(Q.adj, v, u);
    };

    struct MatchDebugStats {
        size_t root_blocks_total = 0;
        size_t root_blocks_with_vpar_minus1 = 0;
        size_t root_blocks_with_candidates = 0;
        size_t root_candidates_total = 0;
        size_t root_ts_fail = 0;
        size_t bfs_parent_invalid = 0;
        size_t bfs_parent_unmapped = 0;
        size_t bfs_node_missing = 0;
        size_t bfs_empty_candidates = 0;
        size_t cand_duplicate_vertex = 0;
        size_t cand_missing_tree_edge = 0;
        size_t cand_tree_ts_fail = 0;
        size_t cand_non_tree_missing_edge = 0;
        size_t cand_non_tree_ts_fail = 0;
        size_t bfs_accept_branch = 0;
        size_t final_match_unique = 0;
        size_t final_match_duplicate = 0;
    } dbg;

    std::vector<std::string> dbg_samples;
    const size_t max_dbg_samples = 40;
    auto push_dbg = [&](const std::string& message) {
        if (dbg_samples.size() < max_dbg_samples) {
            dbg_samples.push_back(message);
        }
    };

    dbg.root_blocks_total = root_blocks_total;
    dbg.root_blocks_with_vpar_minus1 = root_blocks_with_vpar_minus1;
    dbg.root_blocks_with_candidates = root_blocks_with_candidates;
    dbg.root_candidates_total = root_candidates.size();

    std::deque<PartialMatchState> frontier;
    for (const auto& root_entry : root_candidates) {
        const int root_data = root_entry.first;
        std::unordered_set<int> root_ts = root_entry.second;
        if (root_ts.empty()) {
            root_ts = computeVertexTimeInstances(root_data);
        }
        if (!checkMinimumConsecutiveDuration(root_ts)) {
            dbg.root_ts_fail++;
            continue;
        }

        PartialMatchState state;
        state.mapping.assign(Q.num_vertices, -1);
        state.mapping[root_qid] = root_data;
        state.used_data_vertices.insert(root_data);
        state.current_ts = std::move(root_ts);
        state.depth = 1;
        frontier.push_back(std::move(state));
    }

    while (!frontier.empty()) {
        PartialMatchState state = std::move(frontier.front());
        frontier.pop_front();

        if (state.depth >= static_cast<int>(order.size())) {
            std::string key;
            key.reserve(static_cast<size_t>(Q.num_vertices) * 16);
            for (int qid = 0; qid < Q.num_vertices; ++qid) {
                key += std::to_string(qid);
                key += "->";
                key += std::to_string(state.mapping[qid]);
                key += ';';
            }

            if (unique_match_keys.insert(key).second) {
                final_matches.push_back({state.mapping, state.current_ts});
                dbg.final_match_unique++;
            } else {
                dbg.final_match_duplicate++;
            }
            continue;
        }

        const int qid = order[state.depth];
        const int parent_qid = (qid >= 0 && qid < static_cast<int>(parent.size())) ? parent[qid] : -1;
        if (parent_qid < 0 || parent_qid >= Q.num_vertices) {
            dbg.bfs_parent_invalid++;
            push_dbg("drop[parent-invalid]: q" + std::to_string(qid));
            continue;
        }

        const int parent_data = state.mapping[parent_qid];
        if (parent_data < 0) {
            dbg.bfs_parent_unmapped++;
            push_dbg("drop[parent-unmapped]: q" + std::to_string(qid) +
                     " parent q" + std::to_string(parent_qid));
            continue;
        }

        const TDTreeNode* node =
            (qid >= 0 && qid < static_cast<int>(node_by_qid.size())) ? node_by_qid[qid] : nullptr;
        if (node == nullptr) {
            dbg.bfs_node_missing++;
            push_dbg("drop[node-missing]: q" + std::to_string(qid));
            continue;
        }

        std::vector<int> candidates;
        for (const auto& block : node->blocks) {
            if (block.v_par == parent_data) {
                candidates.insert(candidates.end(), block.V_cand.begin(), block.V_cand.end());
            }
        }

        if (candidates.empty()) {
            dbg.bfs_empty_candidates++;
            push_dbg("drop[empty-candidates]: q" + std::to_string(qid) +
                     " parent-data " + std::to_string(parent_data));
            continue;
        }

        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        for (int cand : candidates) {
            if (state.used_data_vertices.find(cand) != state.used_data_vertices.end()) {
                dbg.cand_duplicate_vertex++;
                continue;
            }

            const auto* tree_edge_ts = get_edge_times(parent_data, cand);
            if (tree_edge_ts == nullptr) {
                dbg.cand_missing_tree_edge++;
                push_dbg("drop[tree-edge-missing]: q" + std::to_string(qid) +
                         " parent-data " + std::to_string(parent_data) +
                         " cand " + std::to_string(cand));
                continue;
            }

            std::unordered_set<int> next_ts = intersectTimeSets(state.current_ts, *tree_edge_ts);
            if (!checkMinimumConsecutiveDuration(next_ts)) {
                dbg.cand_tree_ts_fail++;
                continue;
            }

            bool ok = true;
            for (int q2 = 0; q2 < Q.num_vertices; ++q2) {
                if (state.mapping[q2] < 0 || q2 == parent_qid || q2 == qid) {
                    continue;
                }
                if (!query_has_edge(qid, q2)) {
                    continue;
                }

                const auto* non_tree_ts = get_edge_times(cand, state.mapping[q2]);
                if (non_tree_ts == nullptr) {
                    dbg.cand_non_tree_missing_edge++;
                    push_dbg("drop[non-tree-edge-missing]: q" + std::to_string(qid) +
                             "->q" + std::to_string(q2) +
                             " data " + std::to_string(cand) +
                             "->" + std::to_string(state.mapping[q2]));
                    ok = false;
                    break;
                }

                next_ts = intersectTimeSets(next_ts, *non_tree_ts);
                if (!checkMinimumConsecutiveDuration(next_ts)) {
                    dbg.cand_non_tree_ts_fail++;
                    ok = false;
                    break;
                }
            }

            if (!ok) {
                continue;
            }

            PartialMatchState next_state = state;
            next_state.mapping[qid] = cand;
            next_state.used_data_vertices.insert(cand);
            next_state.current_ts = std::move(next_ts);
            next_state.depth = state.depth + 1;
            frontier.push_back(std::move(next_state));
            dbg.bfs_accept_branch++;
        }
    }

    out << "\n[Final Matches]\n";
    out << "Count: " << final_matches.size() << '\n';
    for (size_t i = 0; i < final_matches.size(); ++i) {
        out << "Match " << i << ": ";
        for (int qid = 0; qid < Q.num_vertices; ++qid) {
            if (qid > 0) {
                out << ", ";
            }
            out << "q" << qid << "->" << final_matches[i].mapping[qid];
        }
        out << " |TS|=" << final_matches[i].ts.size() << '\n';
    }

    out << "\n[Final Match Debug]\n";
    out << "root_blocks_total: " << dbg.root_blocks_total << '\n';
    out << "root_blocks_with_vpar_minus1: " << dbg.root_blocks_with_vpar_minus1 << '\n';
    out << "root_blocks_with_candidates: " << dbg.root_blocks_with_candidates << '\n';
    out << "root_candidates_total: " << dbg.root_candidates_total << '\n';
    out << "root_ts_fail: " << dbg.root_ts_fail << '\n';
    out << "bfs_parent_invalid: " << dbg.bfs_parent_invalid << '\n';
    out << "bfs_parent_unmapped: " << dbg.bfs_parent_unmapped << '\n';
    out << "bfs_node_missing: " << dbg.bfs_node_missing << '\n';
    out << "bfs_empty_candidates: " << dbg.bfs_empty_candidates << '\n';
    out << "cand_duplicate_vertex: " << dbg.cand_duplicate_vertex << '\n';
    out << "cand_missing_tree_edge: " << dbg.cand_missing_tree_edge << '\n';
    out << "cand_tree_ts_fail: " << dbg.cand_tree_ts_fail << '\n';
    out << "cand_non_tree_missing_edge: " << dbg.cand_non_tree_missing_edge << '\n';
    out << "cand_non_tree_ts_fail: " << dbg.cand_non_tree_ts_fail << '\n';
    out << "bfs_accept_branch: " << dbg.bfs_accept_branch << '\n';
    out << "final_match_unique: " << dbg.final_match_unique << '\n';
    out << "final_match_duplicate: " << dbg.final_match_duplicate << '\n';
    out << "node_survivors:\n";
    for (int qid = 0; qid < Q.num_vertices; ++qid) {
        const TDTreeNode* node =
            (qid >= 0 && qid < static_cast<int>(node_by_qid.size())) ? node_by_qid[qid] : nullptr;
        size_t block_count = 0;
        size_t candidate_count = 0;
        if (node != nullptr) {
            block_count = node->blocks.size();
            for (const auto& block : node->blocks) {
                candidate_count += block.V_cand.size();
            }
        }
        out << "  q" << qid << ": blocks=" << block_count
            << ", candidates=" << candidate_count << '\n';
    }

    if (!dbg_samples.empty()) {
        out << "samples:\n";
        for (const auto& sample : dbg_samples) {
            out << "  - " << sample << '\n';
        }
    }
}

bool TDTree::checkMinimumDuration(int vertex) const {
    std::unordered_set<int> vertex_time_instances;

    for (const auto& edge : G.adj[vertex]) {
        auto time_it = G.edge_time_instances.find({vertex, edge.to});
        if (time_it != G.edge_time_instances.end()) {
            vertex_time_instances.insert(time_it->second.begin(), time_it->second.end());
        }

        time_it = G.edge_time_instances.find({edge.to, vertex});
        if (time_it != G.edge_time_instances.end()) {
            vertex_time_instances.insert(time_it->second.begin(), time_it->second.end());
        }
    }

    if (vertex_time_instances.empty()) {
        return false;
    }

    return static_cast<int>(vertex_time_instances.size()) >= k_threshold;
}

bool TDTree::checkMinimumConsecutiveDuration(const std::unordered_set<int>& time_instances) const {
    if (time_instances.size() < static_cast<size_t>(k_threshold)) {
        return false;
    }

    std::vector<int> times(time_instances.begin(), time_instances.end());
    std::sort(times.begin(), times.end());

    int consecutive_count = 1;
    for (size_t i = 1; i < times.size(); ++i) {
        if (times[i] == times[i - 1] + 1) {
            consecutive_count++;
            if (consecutive_count >= k_threshold) {
                return true;
            }
        } else {
            consecutive_count = 1;
        }
    }

    return false;
}

std::unordered_set<int> TDTree::computeVertexTimeInstances(int vertex) const {
    std::unordered_set<int> vertex_time_instances;

    for (const auto& edge : G.adj[vertex]) {
        auto time_it = G.edge_time_instances.find({vertex, edge.to});
        if (time_it != G.edge_time_instances.end()) {
            vertex_time_instances.insert(time_it->second.begin(), time_it->second.end());
        }

        time_it = G.edge_time_instances.find({edge.to, vertex});
        if (time_it != G.edge_time_instances.end()) {
            vertex_time_instances.insert(time_it->second.begin(), time_it->second.end());
        }
    }

    return vertex_time_instances;
}
