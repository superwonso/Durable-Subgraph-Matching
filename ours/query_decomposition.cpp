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

    // Initialize DFS variables
    std::vector<bool> visited(Q.num_vertices, false);
    std::stack<int> s;
    s.push(root);
    visited[root] = true;

    // Initialize spanning tree adjacency list
    result.spanning_tree_adj.resize(Q.num_vertices);

    // Set to keep track of tree edges
    std::set<std::pair<int, int>> tree_edges;

    // Initialize parent vector
    std::vector<int> parent(Q.num_vertices, -1);

    // Perform DFS to build spanning tree and identify non-tree edges
    while (!s.empty()) {
        int u = s.top();
        s.pop();

        // Get neighbors sorted by selectivity
        std::vector<int> neighbors(Q.adj[u].size());
        std::transform(Q.adj[u].begin(), Q.adj[u].end(), neighbors.begin(), [](const Edge& edge) { return edge.to; });
        std::sort(neighbors.begin(), neighbors.end(), [&](const int a, const int b) -> bool {
            return selectivity[a] < selectivity[b];
        });

        for (auto& v : neighbors) {
            if (!visited[v]) {
                result.spanning_tree_adj[u].push_back(v);
                result.spanning_tree_adj[v].push_back(u);

                visited[v] = true;
                parent[v] = u;
                s.push(v);
            } else {
                // parent 정보를 활용해 비트리 간선 판별
                if (v != parent[u] && u != parent[v]) {
                    if (u < v) {
                        result.non_tree_edges.emplace_back(u, v);
                    } else {
                        result.non_tree_edges.emplace_back(v, u);
                    }
                }
            }
        }
    }
    return result;
}
