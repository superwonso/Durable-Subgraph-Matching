#ifndef QUERY_DECOMPOSITION_H
#define QUERY_DECOMPOSITION_H

#include <vector>
#include <string>
#include <unordered_map>
#include <utility>
#include <limits>
#include <queue>
#include <set>
#include "Utils.h"

// Structure to hold the result of query decomposition
struct QueryDecomposition {
    // Adjacency list of the spanning tree
    std::vector<std::vector<int>> spanning_tree_adj;
    // List of non-tree edges (pairs of vertices)
    std::vector<std::pair<int, int>> non_tree_edges;
    // Vertex labels for the query graph
    std::vector<std::string> vertex_labels;
    // BFS traversal order of the query spanning tree
    std::vector<int> traversal_order;
    // Parent query vertex for each query node in the spanning tree
    std::vector<int> parent;
    // BFS level of each query vertex
    std::vector<int> level;
    // Root of the spanning tree
    int root = -1;
};

// Function prototype for query decomposition
QueryDecomposition decomposeQuery(const Graph& Q, const std::unordered_map<std::string, int>& label_counts, const std::unordered_map<std::string, double>& label_average_lifespans);
#endif // QUERY_DECOMPOSITION_H
