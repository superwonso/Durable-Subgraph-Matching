#ifndef QUERY_DECOMPOSITION_H
#define QUERY_DECOMPOSITION_H

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

#include "Utils.h"

enum class QueryTreeMode {
    Dfs,
    Bfs
};

const char* queryTreeModeName(QueryTreeMode mode);

// Native phase boundaries exposed to versioned, source-bound integrations.
// The observer owns timing and receipt policy; the original planner and
// TD-tree never inspect a clock and never write an integration artifact.
enum class OriginalNativePhase {
    Planning,
    TDConstruction,
    Enumeration,
    OutputFinalization
};

class OriginalNativePhaseObserver {
public:
    virtual ~OriginalNativePhaseObserver() = default;
    virtual void onOriginalNativePhase(OriginalNativePhase phase) = 0;
};

struct QueryDecomposition {
    std::vector<std::vector<int>> spanning_tree_adj;
    // One original directed query arc for every weak DFS-tree link. If a
    // reciprocal arc also exists, the other direction remains a non-tree arc.
    std::vector<std::pair<int, int>> tree_edges;
    std::vector<std::pair<int, int>> non_tree_edges;
    std::vector<Label> vertex_labels;
    std::vector<int> parent;
    std::vector<int> dfs_order;
    std::vector<double> topological_selectivity;
    int root = -1;
    bool connected = false;
    QueryTreeMode query_tree_mode = QueryTreeMode::Dfs;
};

QueryDecomposition decomposeQuery(
    const Graph& query_graph,
    const std::array<std::size_t, kLabelCount>& label_vertex_counts,
    QueryTreeMode query_tree_mode = QueryTreeMode::Dfs);

// Versioned adapter entrypoint.  The temporal graph must already contain the
// validated explicit A--E runtime labels.  Counting those labels happens only
// after the native planning notification, so the scan cannot escape planning
// phase accounting.  The historical array-based API above is unchanged.
QueryDecomposition decomposeQueryWithExplicitRuntimeLabels(
    const Graph& query_graph,
    const Graph& explicitly_labeled_temporal_graph,
    OriginalNativePhaseObserver& phase_observer,
    QueryTreeMode query_tree_mode = QueryTreeMode::Dfs);

#endif // QUERY_DECOMPOSITION_H
