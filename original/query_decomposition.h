#ifndef QUERY_DECOMPOSITION_H
#define QUERY_DECOMPOSITION_H

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

#include "Utils.h"

struct QueryDecomposition {
    std::vector<std::vector<int>> spanning_tree_adj;
    std::vector<std::pair<int, int>> non_tree_edges;
    std::vector<Label> vertex_labels;
    std::vector<int> parent;
    std::vector<int> dfs_order;
    std::vector<double> topological_selectivity;
    int root = -1;
    bool connected = false;
};

QueryDecomposition decomposeQuery(
    const Graph& query_graph,
    const std::array<std::size_t, kLabelCount>& label_vertex_counts);

#endif // QUERY_DECOMPOSITION_H
