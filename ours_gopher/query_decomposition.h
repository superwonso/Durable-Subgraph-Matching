#ifndef QUERY_DECOMPOSITION_H
#define QUERY_DECOMPOSITION_H

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "Utils.h"

enum class SelectivityMode {
    // 2022 TD-Tree topological selectivity:
    // |V_L(u)| / deg_Q(u).
    TDTree,
    // 260408_cdh.pdf, Eq. (3.2), as printed:
    // deg_Q(u) / (|V_L(u)| * A_dur(u)).
    Paper,
    // The formula consistent with the 2022 TD-Tree numerator/denominator and
    // the surrounding 260408 prose (few candidates, high degree, long life):
    // |V_L(u)| / (deg_Q(u) * A_dur(u)).
    Intended
};

const char* selectivityModeName(SelectivityMode mode);
bool parseSelectivityMode(const std::string& text, SelectivityMode& mode);

struct QueryDecomposition {
    std::vector<std::vector<int>> spanning_tree_adj;
    std::vector<std::pair<int, int>> non_tree_edges;
    std::vector<Label> vertex_labels;
    std::vector<int> parent;
    std::vector<int> dfs_order;
    std::vector<double> temporal_selectivity;
    SelectivityMode selectivity_mode = SelectivityMode::Paper;
    int root = -1;
    bool connected = false;
};

QueryDecomposition decomposeQuery(
    const Graph& query_graph,
    const std::array<std::size_t, kLabelCount>& label_vertex_counts,
    const std::array<double, kLabelCount>& label_average_lifespans,
    SelectivityMode selectivity_mode = SelectivityMode::Paper);

#endif // QUERY_DECOMPOSITION_H
