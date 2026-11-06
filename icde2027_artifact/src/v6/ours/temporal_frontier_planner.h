#ifndef TEMPORAL_FRONTIER_PLANNER_H
#define TEMPORAL_FRONTIER_PLANNER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "query_decomposition.h"

// This module plans only the connected query-vertex order and its spanning-tree
// parents. It deliberately does not depend on TDTree internals, so a caller can
// collect relation statistics from a pilot TD-tree build, a relation catalogue,
// or a sampled temporal join and then pass those statistics here.
enum class TemporalFrontierSearch {
    Greedy,
    Beam,
    Exact
};

const char* temporalFrontierSearchName(TemporalFrontierSearch search);

struct TemporalFrontierRelationStats {
    int parent = -1;
    int child = -1;

    // Number of candidate-relation entries for this oriented expansion. When
    // mean_block_fanout is not supplied, relation_entries / parent_domain_size
    // is used as the mean fanout.
    std::size_t relation_entries = 0;
    std::size_t parent_domain_size = 0;
    double mean_block_fanout =
        std::numeric_limits<double>::quiet_NaN();

    // Fraction of raw block entries expected to pass injectivity and already
    // available structural/back-edge constraints.
    double non_temporal_pass_rate = 1.0;

    // Conditional probability that the extension retains a common k-window
    // with the current prefix. This is intentionally separate from individual
    // edge durability, which should already be reflected in the relation.
    double common_window_pass_rate = 1.0;
};

struct TemporalFrontierPlanStats {
    // Plan-independent root-domain estimates, normally durable/semijoined
    // candidate counts rather than label-wide vertex counts.
    std::vector<double> root_candidate_counts;

    // Supply one row for every parent->child orientation that the planner may
    // choose. Query direction does not restrict the spanning-tree orientation.
    std::vector<TemporalFrontierRelationStats> relations;
};

struct TemporalFrontierPrefixView {
    const std::vector<int>* order = nullptr;
    const std::vector<int>* parents = nullptr;
    const std::vector<std::uint8_t>* selected = nullptr;
    double estimated_partial_states = 0.0;
    double estimated_raw_extensions = 0.0;
    double estimated_total_candidate_examinations = 0.0;
};

struct TemporalFrontierTransitionEstimate {
    // Leave a field as NaN to retain the corresponding static relation value.
    double mean_block_fanout =
        std::numeric_limits<double>::quiet_NaN();
    double non_temporal_pass_rate =
        std::numeric_limits<double>::quiet_NaN();
    double common_window_pass_rate =
        std::numeric_limits<double>::quiet_NaN();
    std::size_t sample_count = 0;
};

using TemporalFrontierTransitionEstimator = std::function<
    TemporalFrontierTransitionEstimate(
        const TemporalFrontierPrefixView& prefix,
        int parent,
        int next,
        const TemporalFrontierRelationStats& relation)>;

struct TemporalFrontierPlannerOptions {
    TemporalFrontierSearch search = TemporalFrontierSearch::Beam;
    std::size_t beam_width = 64;
    std::size_t exact_vertex_limit = 10;
    std::size_t max_expanded_states = 1'000'000;

    // Partial beam states are ranked by:
    //   accumulated total +
    //   downstream_pressure_weight * partial_states * remaining_vertices.
    // It affects pruning/greedy choice only; completed plans are compared by
    // estimated total candidate examinations.
    double downstream_pressure_weight = 1.0;
    double comparison_epsilon = 1e-12;

    // Exact search above exact_vertex_limit falls back to beam search when
    // true. Otherwise planning returns an explanatory error.
    bool exact_fallback_to_beam = true;
};

struct TemporalFrontierStepDiagnostic {
    std::size_t depth = 0;
    int parent = -1;
    int next = -1;
    double input_partial_states = 0.0;
    double mean_block_fanout = 0.0;
    double raw_extension_increment = 0.0;
    double non_temporal_pass_rate = 0.0;
    double common_window_pass_rate = 0.0;
    double output_partial_states = 0.0;
    double accumulated_raw_extensions = 0.0;
    double accumulated_total_candidate_examinations = 0.0;
    std::size_t sample_count = 0;
};

struct TemporalFrontierPlanResult {
    bool valid = false;
    std::string error;
    TemporalFrontierSearch requested_search =
        TemporalFrontierSearch::Beam;
    TemporalFrontierSearch actual_search =
        TemporalFrontierSearch::Beam;

    std::vector<int> order;
    std::vector<int> parents;
    std::vector<TemporalFrontierStepDiagnostic> steps;

    double estimated_root_candidates = 0.0;
    double estimated_raw_extensions = 0.0;
    double estimated_total_candidate_examinations = 0.0;
    double estimated_final_partial_states = 0.0;

    std::size_t expanded_states = 0;
    std::size_t retained_states = 0;
    bool used_transition_estimator = false;
};

// QueryDecomposition supplies weak query topology through its spanning-tree
// edges, parent vector, and non-tree edges. The returned order is a connected
// frontier order: every non-root vertex has an earlier adjacent parent. It need
// not be a recursive DFS preorder, which is not required by TDTree's build,
// semijoin, or enumeration phases.
TemporalFrontierPlanResult planTemporalFrontier(
    const QueryDecomposition& topology,
    const TemporalFrontierPlanStats& statistics,
    const TemporalFrontierPlannerOptions& options = {},
    const TemporalFrontierTransitionEstimator& estimator = {});

#endif // TEMPORAL_FRONTIER_PLANNER_H
