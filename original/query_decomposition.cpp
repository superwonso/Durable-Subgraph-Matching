#include "query_decomposition.h"
#include <algorithm>
#include <iostream>

// Query decomposition function implementation
QueryDecomposition decomposeQuery(const Graph& Q, const std::unordered_map<std::string, int>& label_counts) {
    QueryDecomposition result;

    // Copy vertex labels from the query graph
    result.vertex_labels = Q.vertex_labels;
    // Calculate selectivity for each query vertex
    std::cout << "Calculating selectivity for query vertices..." << std::endl;
    std::cout << "Size of Query Tree... " << Q.num_vertices << std::endl;
    std::vector<double> selectivity(Q.num_vertices, 0.0);
    for (int u = 0; u < Q.num_vertices; ++u) {
        int degree = Q.adj[u].size();
        if (degree == 0) {
            selectivity[u] = std::numeric_limits<double>::max();
        } else {
            const std::string& label = Q.vertex_labels[u];
            auto it = label_counts.find(label);
            if (it != label_counts.end()) {
                selectivity[u] = static_cast<double>(it->second) / degree;
            } else {
                selectivity[u] = std::numeric_limits<double>::max();
            }
        }
    }

    // Select root with the lowest selectivity
    int root = 0;
    double min_selectivity = selectivity[0];
    for (int u = 1; u < Q.num_vertices; ++u) {
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

    // Perform DFS to build spanning tree and identify non-tree edges
    while (!s.empty()) {
        int u = s.top();
        s.pop();

        // Get neighbors sorted by selectivity
        std::vector<int> neighbors;
        for (const auto& edge : Q.adj[u]) {
            neighbors.push_back(edge.to);
        }
        std::sort(neighbors.begin(), neighbors.end(), [&](const int a, const int b) -> bool {
            return selectivity[a] < selectivity[b];
        });

        for (auto& v : neighbors) {
            if (!visited[v]) {
                result.spanning_tree_adj[u].push_back(v);
                result.spanning_tree_adj[v].push_back(u);
                tree_edges.emplace(std::make_pair(u, v));
                tree_edges.emplace(std::make_pair(v, u));

                visited[v] = true;
                s.push(v);
            } else {
                if (tree_edges.find(std::make_pair(u, v)) == tree_edges.end()) {
                    if (u < v) {
                        result.non_tree_edges.emplace_back(std::make_pair(u, v));
                    } else {
                        result.non_tree_edges.emplace_back(std::make_pair(v, u));
                    }
                }
            }
        }
    }
    return result;
}
