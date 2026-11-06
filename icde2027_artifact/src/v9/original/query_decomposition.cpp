#include "query_decomposition.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <set>

const char* queryTreeModeName(QueryTreeMode mode) {
    return mode == QueryTreeMode::Bfs ? "bfs" : "dfs";
}

QueryDecomposition decomposeQuery(
    const Graph& query_graph,
    const std::array<std::size_t, kLabelCount>& label_vertex_counts,
    QueryTreeMode query_tree_mode) {
    QueryDecomposition result;
    result.query_tree_mode = query_tree_mode;
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

        // TD-Tree topological selectivity (Li et al., Sec. 4):
        // S_topo(u) = |V_L(u)| / (out_degree_Q(u) + in_degree_Q(u)).
        // The paper says to choose the lowest selectivity; this implementation
        // formalizes that rule as a deterministic minimum with explicit ties.
        // A missing data label therefore gets
        // score zero and produces an immediate, correct zero-candidate root.
        const double score = static_cast<double>(candidate_count) /
            static_cast<double>(degree);
        result.topological_selectivity[static_cast<std::size_t>(query_vertex)] = score;

        const bool smaller = result.root < 0 ||
            score < result.topological_selectivity[static_cast<std::size_t>(result.root)];
        // A tie must be exact: an epsilon comparison can replace the numeric
        // argmin with a slightly larger score because its candidate domain is
        // smaller.  Exact equality also handles equal infinities directly.
        const bool tied = result.root >= 0 &&
            score == result.topological_selectivity[
                static_cast<std::size_t>(result.root)];
        if (smaller || (tied && candidate_count < best_candidate_count) ||
            (tied && candidate_count == best_candidate_count && query_vertex < result.root)) {
            result.root = query_vertex;
            best_candidate_count = candidate_count;
        }
    }

    // If no scored vertex was found (for example, an externally constructed
    // query with only isolated vertices), choose a deterministic fallback.
    // Normal file-backed queries contain at least one edge.
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
    std::set<std::pair<int, int>> directed_tree_edges;
    auto ordered_neighbors = [&](int query_vertex) {
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
        return neighbors;
    };
    auto add_tree_link = [&](int parent, int child) {
        result.parent[static_cast<std::size_t>(child)] = parent;
        result.spanning_tree_adj[static_cast<std::size_t>(parent)].push_back(child);
        const std::pair<int, int> directed_tree_edge =
            GraphUtils::hasEdge(query_graph.adj, parent, child)
            ? std::make_pair(parent, child)
            : std::make_pair(child, parent);
        result.tree_edges.push_back(directed_tree_edge);
        directed_tree_edges.insert(directed_tree_edge);
    };
    std::function<void(int)> dfs = [&](int query_vertex) {
        visited[static_cast<std::size_t>(query_vertex)] = true;
        result.dfs_order.push_back(query_vertex);
        for (int neighbor : ordered_neighbors(query_vertex)) {
            if (visited[static_cast<std::size_t>(neighbor)]) continue;
            add_tree_link(query_vertex, neighbor);
            dfs(neighbor);
        }
    };
    if (query_tree_mode == QueryTreeMode::Dfs) {
        dfs(result.root);
    } else {
        std::queue<int> frontier;
        visited[static_cast<std::size_t>(result.root)] = true;
        frontier.push(result.root);
        while (!frontier.empty()) {
            const int query_vertex = frontier.front();
            frontier.pop();
            result.dfs_order.push_back(query_vertex);
            for (int neighbor : ordered_neighbors(query_vertex)) {
                if (visited[static_cast<std::size_t>(neighbor)]) continue;
                visited[static_cast<std::size_t>(neighbor)] = true;
                add_tree_link(query_vertex, neighbor);
                frontier.push(neighbor);
            }
        }
    }

    result.connected = result.dfs_order.size() == static_cast<std::size_t>(query_vertex_count);
    if (!result.connected) return result;

    for (int query_vertex = 0; query_vertex < query_vertex_count; ++query_vertex) {
        for (const auto& edge : query_graph.adj[static_cast<std::size_t>(query_vertex)]) {
            const std::pair<int, int> directed_edge{query_vertex, edge.to};
            if (directed_tree_edges.find(directed_edge) ==
                directed_tree_edges.end()) {
                // Preserve the original query-arc direction. In particular,
                // only one direction of a reciprocal pair belongs to T; the
                // other direction is a non-tree edge in S.
                result.non_tree_edges.emplace_back(query_vertex, edge.to);
            }
        }
    }
    return result;
}

QueryDecomposition decomposeQueryWithExplicitRuntimeLabels(
    const Graph& query_graph,
    const Graph& explicitly_labeled_temporal_graph,
    OriginalNativePhaseObserver& phase_observer,
    QueryTreeMode query_tree_mode) {
    phase_observer.onOriginalNativePhase(OriginalNativePhase::Planning);

    std::array<std::size_t, kLabelCount> label_vertex_counts{};
    for (const Label label : explicitly_labeled_temporal_graph.vertex_labels) {
        if (label < kLabelCount) {
            ++label_vertex_counts[static_cast<std::size_t>(label)];
        }
    }
    return decomposeQuery(
        query_graph, label_vertex_counts, query_tree_mode);
}
