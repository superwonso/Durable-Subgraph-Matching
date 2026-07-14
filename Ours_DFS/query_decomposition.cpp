#include "query_decomposition.h"
#include <algorithm>
#include <iostream>

// Query decomposition function implementation
QueryDecomposition decomposeQuery(const Graph& Q, const std::unordered_map<std::string, int>& label_counts, const std::unordered_map<std::string, double>& label_average_lifespans) {
    QueryDecomposition result;

    // Copy vertex labels from the query graph
    result.vertex_labels = Q.vertex_labels;
    // Calculate selectivity for each query vertex
    std::cout << "Calculating selectivity for query vertices..." << std::endl;
    std::cout << "Size of Query Tree... " << Q.num_vertices << std::endl;
    std::vector<double> selectivity(Q.num_vertices, 0.0);
    int root = 0;
    double min_selectivity = std::numeric_limits<double>::max();
    double average_lifespan;
    for (int u = 0; u < Q.num_vertices; ++u) {
        int degree = Q.adj[u].size();
        if (degree == 0) {
            selectivity[u] = std::numeric_limits<double>::max();
        } else {
            const std::string& label = Q.vertex_labels[u];
            auto it = label_counts.find(label);
            if (it != label_counts.end()) {
                average_lifespan = label_average_lifespans.at(label);
                selectivity[u] = (static_cast<double>(it->second) * average_lifespan) / degree;
            } else {
                selectivity[u] = std::numeric_limits<double>::max();
            }
        }
        // Determine root with the lowest selectivity
        if (selectivity[u] < min_selectivity) {
            min_selectivity = selectivity[u];
            root = u;
        }
    }

    // Initialize BFS variables
    std::vector<bool> visited(Q.num_vertices, false);
    std::queue<int> q;
    q.push(root);
    visited[root] = true;

    // Initialize spanning tree adjacency list
    result.spanning_tree_adj.resize(Q.num_vertices);
    result.parent.assign(Q.num_vertices, -1);
    result.level.assign(Q.num_vertices, -1);
    result.root = root;
    result.level[root] = 0;

    // Set to keep track of non-tree edges without duplicates
    std::set<std::pair<int, int>> non_tree_edge_set;

    // Perform BFS to build spanning tree and identify non-tree edges
    while (!q.empty()) {
        int u = q.front();
        q.pop();
        result.traversal_order.push_back(u);

        // Get neighbors sorted by selectivity
        std::vector<int> neighbors(Q.adj[u].size());
        std::transform(Q.adj[u].begin(), Q.adj[u].end(), neighbors.begin(), [](const Edge& edge) { return edge.to; });
        std::sort(neighbors.begin(), neighbors.end(), [&](const int a, const int b) -> bool {
            return selectivity[a] < selectivity[b];
        });

        for (auto& v : neighbors) {
            if (!visited[v]) {
                result.spanning_tree_adj[u].push_back(v);

                visited[v] = true;
                result.parent[v] = u;
                result.level[v] = result.level[u] + 1;
                q.push(v);
            } else {
                // parent 정보를 활용해 비트리 간선 판별
                if (v != result.parent[u] && u != result.parent[v]) {
                    const std::pair<int, int> edge = (u < v) ? std::make_pair(u, v) : std::make_pair(v, u);
                    if (non_tree_edge_set.insert(edge).second) {
                        result.non_tree_edges.push_back(edge);
                    }
                }
            }
        }
    }

    if (result.traversal_order.size() != static_cast<size_t>(Q.num_vertices)) {
        std::cerr << "Warning: query graph is not fully connected; only the component rooted at "
                  << result.root << " was decomposed." << std::endl;
    }

    return result;
}
