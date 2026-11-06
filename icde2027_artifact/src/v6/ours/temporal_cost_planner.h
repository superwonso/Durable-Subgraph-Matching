#ifndef TEMPORAL_COST_PLANNER_H
#define TEMPORAL_COST_PLANNER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "Utils.h"
#include "TDTree.h"
#include "query_decomposition.h"

struct TemporalCostPlannerOptions {
    std::size_t beam_width = 64;
    std::size_t sample_cap = 4096;
    std::uint64_t sample_seed = 42;
    // These deterministic budgets are used only by the opt-in v2 policy.
    // A zero budget is valid and deterministically selects the complete KCI
    // incumbent without constructing a partial executable plan.
    std::uint64_t max_catalog_probes = UINT64_C(50000000);
    std::uint64_t max_state_expansions = UINT64_C(4096);
    std::uint64_t max_extension_probes = UINT64_C(1000000);
    // Calls to intersectTimeIntervals() made while evaluating sampled mapped
    // query edges. Initial edge scans/copies are counted separately as
    // temporal-edge applications but do not consume this budget.
    std::uint64_t max_interval_intersections = UINT64_C(10000000);
    std::uint64_t pilot_extension_probe_cap = UINT64_C(4096);
    std::size_t pilot_prefix_cap = 64;
    double opportunity_margin_fraction = 0.05;
    double catalog_probe_cost_weight = 1.0;
    // Development-only diagnostic arm. Production defaults to the guarded
    // policy; disabling it bypasses opportunity/final cost rejection only.
    bool cost_gate_enabled = true;
#ifdef DSM_TCC_TEST_REFERENCE_FULL_SORT
    // Test-only M3 parity switch. Production builds do not expose this field
    // or any CLI surface for selecting the pre-optimization full-sort plus
    // inherited-sample deep-copy path.
    bool test_reference_full_sort = false;
#endif
    std::size_t rescore_top_l = 2;
    std::uint64_t max_candidate_trees_built = UINT64_C(2);
};

struct TemporalTransitionEstimate {
    std::vector<int> prefix_order;
    int parent = -1;
    int next = -1;
    std::size_t sampled_prefixes = 0;
    std::uint64_t sampled_raw_extensions = 0;
    std::uint64_t sampled_accepted_extensions = 0;
    bool input_prefix_population_exact = true;
    bool output_prefix_population_exact = true;
    double raw_per_prefix = 0.0;
    // Combined injectivity, mapped-edge, and common-window pass rate.
    double overall_extension_pass_rate = 0.0;
    double estimated_raw_extensions = 0.0;
    double estimated_next_prefixes = 0.0;
};

struct TemporalRootPlanEstimate {
    int root = -1;
    std::size_t root_candidate_count = 0;
    std::size_t pilot_relation_entries = 0;
    double catalogue_estimated_total_candidates = 0.0;
    double estimated_total_candidates = 0.0;
    bool estimated_total_exact = true;
    std::vector<int> order;
    std::vector<int> parent;
    std::vector<TemporalTransitionEstimate> transitions;
};

struct TemporalFixedPlanEstimate {
    std::uint64_t plan_fingerprint = 0;
    int root = -1;
    bool incumbent = false;
    bool reused_incumbent_score = false;
    std::uint64_t extension_probe_cap = 0;
    std::uint64_t extension_probes = 0;
    double estimated_total_candidates = 0.0;
    bool estimated_total_exact = true;
};

struct TemporalPlanDiagnostics {
    std::vector<TemporalRootPlanEstimate> root_plans;
    double selected_estimated_total_candidates = 0.0;
    long long planning_milliseconds = 0;
    std::uint64_t planning_nanoseconds = 0;
    std::size_t beam_width = 0;
    std::size_t sample_cap = 0;
    std::uint64_t sample_seed = 0;
    bool sampled_baseline_plan_selected = false;
    double sampled_baseline_estimated_total_candidates = 0.0;
    bool sampled_baseline_estimate_exact = true;
    // Root + raw objective components.  Raw is the sum of the estimated
    // attempted TD-block extensions and may be fractional under sampling.
    double selected_root_candidates = 0.0;
    double selected_raw_extension_candidates = 0.0;
    double selected_root_plus_raw_candidates = 0.0;

    // KCI-guarded v2 audit trail.  Legacy TCC reports v2_enabled=false and
    // leaves the admission/fallback fields as "not-applicable".
    bool v2_enabled = false;
    std::string v2_admission_reason = "not-applicable";
    std::string v2_fallback_phase = "not-applicable";
    std::string v2_fallback_reason = "not-applicable";
    bool v2_budget_exhausted = false;
    bool v2_kci_incumbent_selected = false;
    bool v2_candidate_selected = false;
    bool v2_cost_gate_enabled = true;
    bool v2_cost_gate_bypassed = false;
    bool v2_forced_candidate_selected = false;
    std::uint64_t max_catalog_probes = 0;
    std::uint64_t max_state_expansions = 0;
    std::uint64_t max_extension_probes = 0;
    std::uint64_t max_interval_intersections = 0;
    std::uint64_t max_candidate_trees_built = 0;
    std::uint64_t catalog_probes = 0;
    std::uint64_t state_expansions = 0;
    std::uint64_t extension_probes = 0;
    std::uint64_t beam_extension_probes = 0;
    std::uint64_t incumbent_extension_probes = 0;
    std::uint64_t candidate_extension_probes = 0;
    std::uint64_t fixed_plan_extension_probes = 0;
    std::uint64_t interval_intersections = 0;
    std::uint64_t temporal_edge_applications = 0;
    std::uint64_t pilot_extension_probe_cap = UINT64_C(4096);
    std::uint64_t pilot_extension_probes = 0;
    std::size_t pilot_prefix_cap = 0;
    bool incumbent_objective_available = false;
    bool selected_objective_available = false;
    double predicted_kci_controllable_cost = 0.0;
    double predicted_tcc_controllable_cost = 0.0;
    double opportunity_safety_margin = 0.0;
    double opportunity_margin_fraction = 0.0;
    double catalog_probe_cost_weight = 0.0;
    double opportunity_pilot_spent_cost = 0.0;
    double opportunity_pilot_state_cost = 0.0;
    double opportunity_pilot_extension_cost = 0.0;
    double opportunity_pilot_interval_cost = 0.0;
    double opportunity_incumbent_build_cost = 0.0;
    double opportunity_catalog_floor_probes = 0.0;
    double opportunity_catalog_floor_cost = 0.0;
    double opportunity_optimistic_exec_cost = 0.0;
    std::size_t incumbent_relation_entries = 0;
    double final_planner_spent_cost = 0.0;
    double final_incumbent_build_cost = 0.0;
    double final_catalog_cost = 0.0;
    double final_state_expansion_cost = 0.0;
    double final_extension_probe_cost = 0.0;
    double final_interval_intersection_cost = 0.0;
    double final_rejected_tree_cost = 0.0;
    bool final_cost_comparison_available = false;
    double final_candidate_build_exec_cost = 0.0;
    double final_candidate_h1_cost = 0.0;
    double final_kci_h1_cost = 0.0;
    std::string v2_decision = "not-applicable";
    std::string v2_decision_reason = "not-applicable";
    std::uint64_t incumbent_plan_fingerprint = 0;
    std::string incumbent_selectivity_formula_id = "not-applicable";
    std::uint64_t selected_plan_fingerprint = 0;
    std::size_t candidate_trees_built = 0;
    long long incumbent_tree_build_milliseconds = 0;
    long long selected_tree_build_milliseconds = 0;
    bool selected_tree_handoff = false;
    std::size_t rescore_top_l = 0;
    std::size_t catalogue_complete_plans = 0;
    std::size_t candidate_plans_rescored = 0;
    std::size_t candidate_tree_relation_entries_built = 0;
    std::size_t rejected_candidate_tree_relation_entries = 0;
    std::string fixed_plan_scoring_method = "not-applicable";
    std::vector<TemporalFixedPlanEstimate> fixed_plan_scores;
    int incumbent_root = -1;
    std::vector<int> incumbent_order;
    std::vector<int> incumbent_parent;
    double incumbent_root_candidates = 0.0;
    double incumbent_raw_extension_candidates = 0.0;
    double incumbent_root_plus_raw_candidates = 0.0;
};

struct TemporalCostPlanResult {
    QueryDecomposition decomposition;
    // V2 transfers the already built selected tree to the caller.  This is
    // null only for empty/disconnected queries or compatibility wrappers that
    // intentionally discard it.
    std::unique_ptr<TDTree> selected_tree;
};

// Estimate the actual logical candidate counter:
//
//   |D_root| + sum_i sum_{mu in prefix_i} |TD-block(mu(parent_i), q_i)|
//
// A prefix sample is accepted only when it is injective and every mapped
// query arc shares a common interval of at least k consecutive snapshots.
// Each possible root is evaluated. A catalogue beam compares every connected
// (next,parent) transition; its best plan per root is then re-scored against
// the actual TD-tree blocks. The corrected durability-aware Ours plan is
// included as a sampled fallback candidate; capped samples do not provide an
// exact no-regression guarantee.
QueryDecomposition planTemporalCostQuery(
    const Graph& temporal_graph,
    const Graph& query_graph,
    int minimum_duration,
    const std::array<std::size_t, kLabelCount>& label_vertex_counts,
    const std::array<double, kLabelCount>&
        label_mean_vertex_active_snapshots,
    const TemporalCostPlannerOptions& options = {},
    TemporalPlanDiagnostics* diagnostics = nullptr);

// KCI-guarded, deterministically budgeted TCC v2.  Legacy temporal-cost
// behavior is intentionally preserved by planTemporalCostQuery().  V2 always
// starts with a complete corrected durability-aware decomposition and returns either
// that incumbent or a complete candidate that has been built and rescored;
// partially explored beam/catalogue state is never exposed as a plan.
QueryDecomposition planTemporalCostV2Query(
    const Graph& temporal_graph,
    const Graph& query_graph,
    int minimum_duration,
    const std::array<std::size_t, kLabelCount>& label_vertex_counts,
    const std::array<double, kLabelCount>&
        label_mean_vertex_active_snapshots,
    const TemporalCostPlannerOptions& options = {},
    TemporalPlanDiagnostics* diagnostics = nullptr);

TemporalCostPlanResult planTemporalCostV2(
    const Graph& temporal_graph,
    const Graph& query_graph,
    int minimum_duration,
    const std::array<std::size_t, kLabelCount>& label_vertex_counts,
    const std::array<double, kLabelCount>&
        label_mean_vertex_active_snapshots,
    const TemporalCostPlannerOptions& options = {},
    TemporalPlanDiagnostics* diagnostics = nullptr);

#endif // TEMPORAL_COST_PLANNER_H
