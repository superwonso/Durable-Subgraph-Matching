#ifndef QUERY_DECOMPOSITION_H
#define QUERY_DECOMPOSITION_H

#include <vector>
#include <string>
#include <unordered_map>
#include <utility>
#include <limits>
#include <stack>
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
};

// Function prototype for query decomposition
QueryDecomposition decomposeQuery(const Graph& Q, const std::unordered_map<std::string, int>& label_counts);

#endif // QUERY_DECOMPOSITION_H
