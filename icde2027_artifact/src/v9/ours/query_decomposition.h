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
    // Corrected durability-aware score used by the current method:
    // |V'_L(u)| / (deg_Q(u) * A_dur(u)), where V' is the graph presented
    // after the current method's filter.  The local continuation draft
    // inverted the TD-Tree structural ratio; this mode restores the 2022
    // direction while retaining the draft's long-lived-anchor preference.
    Paper,
    // Literal equation printed in the local 260408 continuation draft:
    // deg_Q(u) / (|V_L(u)| * A_dur(u)).  This is a historical reproduction
    // mode, not the current method, because it reverses TD-Tree's c/d ratio.
    LegacyReciprocal,
    // Backward-compatible alias for the corrected durability-aware score.
    // New experiments should use `paper`; archived experiment manifests may
    // continue to use `intended` without changing their arithmetic.
    Intended,
    // Historical parser-audit error caused by flattening a typeset fraction:
    // (deg_Q(u) / |V_L(u)|) * A_dur(u).  Retained solely to reproduce builds
    // made between the first text-extraction audit and this correction.
    LegacyExtractedProduct,
    // Control arm for the durability-aware formula:
    // C_struct(u, k) / deg_Q(u).
    Structural,
    // Durability-aware candidate-volume estimate:
    // C_struct(u, k) / deg_Q(u) * rho_k(u), where rho_k is the fraction of
    // direction/label-compatible data edges that survive for at least k
    // consecutive snapshots.
    Survival,
    // Plan-level Temporal Cost Criterion (TCC). Unlike the legacy modes, this is
    // computed from TD-tree blocks and sampled common-k-window prefix states
    // by planTemporalCostQuery(), not by decomposeQuery().
    TemporalCost,
    // Opt-in KCI-guarded TCC v2.  The planner starts from the deterministic
    // corrected durability-aware decomposition and may replace it only with a complete,
    // fully rescored TCC plan within deterministic work budgets.
    TemporalCostV2
};

const char* selectivityModeName(SelectivityMode mode);
const char* selectivityFormulaId(SelectivityMode mode);
bool parseSelectivityMode(const std::string& text, SelectivityMode& mode);

struct SurvivalSelectivityStatistics {
    std::vector<std::size_t> structural_candidate_counts;
    std::vector<std::size_t> compatible_edge_counts;
    std::vector<std::size_t> durable_compatible_edge_counts;
    std::vector<double> durability_survival_rates;
};

struct QueryDecomposition {
    std::vector<std::vector<int>> spanning_tree_adj;
    std::vector<std::pair<int, int>> non_tree_edges;
    std::vector<Label> vertex_labels;
    std::vector<int> parent;
    std::vector<int> dfs_order;
    std::vector<double> temporal_selectivity;
    std::vector<std::size_t> structural_candidate_counts;
    std::vector<std::size_t> compatible_edge_counts;
    std::vector<std::size_t> durable_compatible_edge_counts;
    std::vector<double> durability_survival_rates;
    SelectivityMode selectivity_mode = SelectivityMode::Paper;
    int root = -1;
    bool connected = false;
};

SurvivalSelectivityStatistics computeSurvivalSelectivityStatistics(
    const Graph& temporal_graph,
    const Graph& query_graph,
    int minimum_duration);

QueryDecomposition decomposeQuery(
    const Graph& query_graph,
    const std::array<std::size_t, kLabelCount>& label_vertex_counts,
    const std::array<double, kLabelCount>&
        label_mean_vertex_active_snapshots,
    SelectivityMode selectivity_mode = SelectivityMode::Paper,
    const SurvivalSelectivityStatistics* survival_statistics = nullptr);

#endif // QUERY_DECOMPOSITION_H
