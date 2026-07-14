#include "query_decomposition.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>

QueryDecomposition decomposeQuery(
    const Graph& query_graph,
    const std::array<std::size_t, kLabelCount>& label_vertex_counts) {
    QueryDecomposition result;
    const int query_vertex_count = query_graph.num_vertices;
    result.vertex_labels = query_graph.vertex_labels;
    result.spanning_tree_adj.resize(static_cast<std::size_t>(query_vertex_count));
    result.parent.assign(static_cast<std::size_t>(query_vertex_count), -1);
    result.topological_selectivity.assign(
        static_cast<std::size_t>(query_vertex_count),
        std::numeric_limits<double>::infinity());

    std::size_t best_candidate_count = std::numeric_limits<std::size_t>::max();
    for (int query_vertex = 0; query_vertex < query_vertex_count; ++query_vertex) {
        const Label label = query_graph.vertex_labels[static_cast<std::size_t>(query_vertex)];
        const std::size_t degree =
            query_graph.adj[static_cast<std::size_t>(query_vertex)].size() +
            query_graph.in_adj[static_cast<std::size_t>(query_vertex)].size();
        if (label >= kLabelCount || degree == 0) continue;

        const std::size_t candidate_count = label_vertex_counts[label];

        // Original topological selectivity:
        // S_topo(u) = |V_L(u)| / (out_degree_Q(u) + in_degree_Q(u)).
        // The minimum score is the root. A missing data label therefore gets
        // score zero and produces an immediate, correct zero-candidate root.
        const double score = static_cast<double>(candidate_count) /
            static_cast<double>(degree);
        result.topological_selectivity[static_cast<std::size_t>(query_vertex)] = score;

        const bool smaller = result.root < 0 ||
            score < result.topological_selectivity[static_cast<std::size_t>(result.root)];
        const bool tied = result.root >= 0 &&
            std::abs(score - result.topological_selectivity[static_cast<std::size_t>(result.root)]) <= 1e-15;
        if (smaller || (tied && candidate_count < best_candidate_count) ||
            (tied && candidate_count == best_candidate_count && query_vertex < result.root)) {
            result.root = query_vertex;
            best_candidate_count = candidate_count;
        }
    }

    // No finite score means that no query label currently has data candidates.
    // The structurally valid query should still run and report Count: 0 rather
    // than fail, so choose a deterministic fallback root.
    if (result.root < 0) {
        for (int query_vertex = 0; query_vertex < query_vertex_count; ++query_vertex) {
            if (!query_graph.adj[static_cast<std::size_t>(query_vertex)].empty() ||
                !query_graph.in_adj[static_cast<std::size_t>(query_vertex)].empty()) {
                result.root = query_vertex;
                break;
            }
        }
    }
    if (result.root < 0) return result;

    std::vector<bool> visited(static_cast<std::size_t>(query_vertex_count), false);
    std::set<std::pair<int, int>> tree_edges;
    std::function<void(int)> dfs = [&](int query_vertex) {
        visited[static_cast<std::size_t>(query_vertex)] = true;
        result.dfs_order.push_back(query_vertex);

        std::vector<int> neighbors;
        neighbors.reserve(
            query_graph.adj[static_cast<std::size_t>(query_vertex)].size() +
            query_graph.in_adj[static_cast<std::size_t>(query_vertex)].size());
        for (const auto& edge : query_graph.adj[static_cast<std::size_t>(query_vertex)]) {
            neighbors.push_back(edge.to);
        }
        for (const auto& edge : query_graph.in_adj[static_cast<std::size_t>(query_vertex)]) {
            neighbors.push_back(edge.to);
        }
        std::sort(neighbors.begin(), neighbors.end(), [&](int lhs, int rhs) {
            const double lhs_score = result.topological_selectivity[static_cast<std::size_t>(lhs)];
            const double rhs_score = result.topological_selectivity[static_cast<std::size_t>(rhs)];
            if (lhs_score != rhs_score) return lhs_score < rhs_score;
            return lhs < rhs;
        });
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());

        for (int neighbor : neighbors) {
            if (visited[static_cast<std::size_t>(neighbor)]) continue;
            result.parent[static_cast<std::size_t>(neighbor)] = query_vertex;
            result.spanning_tree_adj[static_cast<std::size_t>(query_vertex)].push_back(neighbor);
            tree_edges.emplace(std::min(query_vertex, neighbor), std::max(query_vertex, neighbor));
            dfs(neighbor);
        }
    };
    dfs(result.root);

    result.connected = result.dfs_order.size() == static_cast<std::size_t>(query_vertex_count);
    if (!result.connected) return result;

    for (int query_vertex = 0; query_vertex < query_vertex_count; ++query_vertex) {
        for (const auto& edge : query_graph.adj[static_cast<std::size_t>(query_vertex)]) {
            const std::pair<int, int> normalized{
                std::min(query_vertex, edge.to), std::max(query_vertex, edge.to)};
            if (tree_edges.find(normalized) == tree_edges.end()) {
                // Preserve the original query-arc direction. The spanning tree
                // is only a weak-connectivity search skeleton.
                result.non_tree_edges.emplace_back(query_vertex, edge.to);
            }
        }
    }
    return result;
}
