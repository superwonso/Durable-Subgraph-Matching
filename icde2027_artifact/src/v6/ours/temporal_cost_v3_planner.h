#ifndef TEMPORAL_COST_V3_PLANNER_H
#define TEMPORAL_COST_V3_PLANNER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "fixed_plan_feasibility.h"
#include "v3_existing_capacity.h"
#include "v3_plan_handoff.h"

class TDTree;
class ValidatedGraphSnapshot;

enum class V3TerminalDecision : std::uint8_t {
    IncumbentEmptyOrDisconnected,
    IncumbentOnly,
    NoNonincumbentCompleteProposal,
    NoBuildFeasiblePlan,
    LazyTransitionBudget,
    SampleEmpty,
    CandidateBuildBudget,
    CandidateBuildQuoteViolation,
    GlobalMemoryAdmission,
    CandidateReservationAllocationFailure,
    CandidateRescoreBudget,
    MetadataBudget,
    SamePlan,
    GainBelowMargin,
    CandidateSelected,
    IncumbentOriginMismatch
};

const char* v3TerminalDecisionName(
    V3TerminalDecision decision) noexcept;

enum class V3ProposalStatus : std::uint8_t {
    Complete,
    MissingSnapshot,
    MissingSnapshotAuthority,
    InvalidIncumbent,
    StateBudget,
    CompletePlanBudget,
    ArithmeticOverflow,
    AllocationFailure
};

const char* v3ProposalStatusName(V3ProposalStatus status) noexcept;

struct V3ProposalBudget {
    std::uint64_t maximum_states_expanded = 4096;
    std::uint64_t maximum_complete_plans = 256;
    std::uint64_t maximum_quote_cover_value_visits = 1000000;
};

struct V3LazyBudget {
    std::uint64_t maximum_block_requests = 100000;
    std::uint64_t maximum_adjacency_probes = 1000000;
    std::uint64_t maximum_reverse_lookups = 1000000;
    std::uint64_t maximum_arc_checks = 2000000;
    std::uint64_t maximum_relation_entries = 1000000;
    std::uint64_t maximum_cache_entries = 100000;
    std::uint64_t maximum_cache_capacity_bytes = 67108864;
    std::uint64_t maximum_extension_probes = 1000000;
    std::uint64_t maximum_interval_intersections = 1000000;
    std::size_t prefix_sample_cap = 256;
};

// This cap is copied for each side.  Incumbent and materialized candidate are
// therefore rescored under identical, independent pilot ledgers.
struct V3RescoreBudget {
    std::uint64_t maximum_block_requests = 100000;
    std::uint64_t maximum_extension_probes = 1000000;
    std::uint64_t maximum_interval_intersections = 1000000;
    std::size_t prefix_sample_cap = 256;
};

struct TemporalCostV3Options {
    V3ProposalBudget proposal_budget;
    V3LazyBudget lazy_budget;
    V3RescoreBudget rescore_budget;
    CandidateBuildAccounting candidate_build_accounting =
        defaultCandidateBuildAccounting();
    std::uint64_t metadata_payload_cap = 67108864;
    std::uint64_t maximum_trace_entries = 100000;
    bool profitability_gate_enabled = true;
    // Formula-V3 only. Legacy/static synthetic callers leave this false and
    // cannot claim an existing-capacity receipt. A future production formula
    // projection must supply the exact build-bound model SHA, M, nonzero phase
    // generation, and both already-captured source-release witnesses.
    bool global_memory_admission_enabled = false;
    std::uint64_t process_memory_max_bytes = 0;
    std::uint64_t existing_capacity_phase_generation = 0;
    std::string existing_capacity_accounting_model_sha256;
    SourceGraphReleaseReceiptV1 source_data_graph_release;
    SourceGraphReleaseReceiptV1 source_query_graph_release;
};

struct V3PlannerTelemetry {
    std::uint64_t v3_proposal_states_expanded = 0;
    std::uint64_t v3_complete_plan_proposals = 0;
    std::uint64_t v3_nonincumbent_plan_proposals = 0;
    std::uint64_t v3_duplicate_plan_proposals = 0;
    std::uint64_t v3_invalid_plan_proposals = 0;

    std::uint64_t v3_quote_cover_value_visits = 0;
    std::uint64_t v3_build_feasible_plan_quotes = 0;
    std::uint64_t v3_build_infeasible_plan_quotes = 0;

    std::uint64_t v3_lazy_block_requests = 0;
    std::uint64_t v3_lazy_block_hits = 0;
    std::uint64_t v3_lazy_block_misses = 0;
    std::uint64_t v3_lazy_blocks_started = 0;
    std::uint64_t v3_lazy_blocks_completed = 0;
    std::uint64_t v3_lazy_budget_rejected_before_start = 0;
    std::uint64_t v3_lazy_adjacency_probes = 0;
    std::uint64_t v3_lazy_reverse_lookups = 0;
    std::uint64_t v3_lazy_arc_checks = 0;
    std::uint64_t v3_lazy_relation_entries = 0;
    std::uint64_t v3_lazy_cache_capacity_bytes = 0;
    std::uint64_t v3_lazy_extension_probes = 0;
    std::uint64_t v3_lazy_interval_intersections = 0;
    std::uint64_t v3_implicit_parent_relation_lookups = 0;
    std::uint64_t v3_lazy_prefix_sample_cap = 0;
    std::uint64_t v3_lazy_maximum_prefix_samples_observed = 0;

    std::uint64_t v3_scored_plan_proposals = 0;
    std::uint64_t v3_materialized_complete_plans = 0;
    std::uint64_t v3_rescored_complete_plans = 0;
    std::uint64_t v3_candidate_build_quote_violations = 0;
    std::uint64_t v3_partial_candidate_tree_handoffs = 0;
    std::uint64_t v3_rescore_prefix_sample_cap = 0;
    std::uint64_t v3_rescore_maximum_prefix_samples_observed = 0;
    std::uint64_t v3_maximum_prefix_samples_observed = 0;

    std::uint64_t v3_cover_export_entries = 0;
    std::uint64_t v3_candidate_build_work = 0;
    std::uint64_t v3_incumbent_rescore_work = 0;
    std::uint64_t v3_candidate_rescore_work = 0;
    std::uint64_t predicted_incumbent_total = 0;
    std::uint64_t predicted_candidate_total = 0;
    std::uint64_t cost_convention_id = 0;

    std::uint64_t graph_validation_passes = 0;
    std::uint64_t graph_validation_rescans = 0;
    std::uint64_t data_fingerprint_rescans = 0;
    std::uint64_t query_fingerprint_rescans = 0;
    std::uint64_t extra_full_q_by_n_domain_scans = 0;
    std::uint64_t global_query_edge_catalogue_builds = 0;
    std::uint64_t global_arc_consistency_passes = 0;
    std::uint64_t trace_entries_dropped = 0;
};

struct V3FixedPlanProposalResult {
    V3ProposalStatus status = V3ProposalStatus::MissingSnapshot;
    std::vector<FixedPlan> plans;
    V3PlannerTelemetry telemetry;
    std::vector<std::uint64_t> deterministic_trace;

    bool complete() const noexcept;
};

// Reads query topology and CandidateCover cardinalities only.  It does not
// inspect data adjacency, temporal histories, or any lazy transition block.
V3FixedPlanProposalResult generateV3FixedPlanProposals(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot,
    const CandidateCover& cover,
    const FixedPlan& incumbent_plan,
    const V3ProposalBudget& budget,
    std::uint64_t maximum_trace_entries = 0) noexcept;

struct V3PlanScoreReceipt {
    bool available = false;
    bool exact = false;
    std::uint64_t plan_fingerprint = 0;
    std::uint64_t root_candidates = 0;
    std::uint64_t raw_attempted_extensions = 0;
    std::uint64_t objective = 0;
};

// Scalar evidence retained across reservation/materializer moves.  The
// planner previously discarded both the selected sealed quote and the
// builder's actual counters, which made a complete matcher receipt
// impossible to reconstruct without guessing.  These are the exact nine
// dimensions exposed by the frozen candidate-build receipt schema.
struct V3CandidateBuildWorkVector {
    std::uint64_t adjacency_probes = 0;
    std::uint64_t reverse_edge_lookups = 0;
    std::uint64_t arc_checks = 0;
    std::uint64_t relation_entries = 0;
    std::uint64_t candidate_value_occurrences = 0;
    std::uint64_t blocks = 0;
    std::uint64_t semijoin_entry_visits = 0;
    std::uint64_t index_entries = 0;
    std::uint64_t payload_memory_bytes = 0;
};

struct V3CandidateBuildEvidence {
    bool quote_available = false;
    V3CandidateBuildWorkVector sealed_quote;
    V3CandidateBuildWorkVector actual;
    bool quote_dominance_verified = true;
    std::uint64_t partial_handoffs = 0;

    bool invariantHolds() const noexcept;
};

struct TemporalCostV3Result {
    V3TerminalDecision decision = V3TerminalDecision::IncumbentOnly;
    std::unique_ptr<TDTree> selected_tree;
    std::unique_ptr<SelectedPlanReceipt> receipt;
    V3PlannerTelemetry telemetry;
    V3PlanScoreReceipt incumbent_score;
    V3PlanScoreReceipt candidate_score;
    V3CandidateBuildEvidence candidate_build;
    bool global_memory_admission_required = false;
    bool global_memory_admission_attempted = false;
    bool global_memory_admission_consumed = false;
    ExistingCapacityReceiptV1 existing_capacity;
    V3GlobalMemoryAdmissionReceiptV1 global_memory_admission;
    std::uint64_t incumbent_plan_fingerprint = 0;
    std::uint64_t selected_plan_fingerprint = 0;
    std::vector<std::uint64_t> deterministic_trace;

    bool selectedCandidate() const noexcept;
    bool invariantHolds() const noexcept;
};

// Consumes incumbent_tree into the returned result.  Every failed proposal,
// lazy, build, rescore, metadata, or profitability path returns the exact same
// incumbent TDTree object; no partial candidate is handed off.
TemporalCostV3Result planTemporalCostV3(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot,
    std::unique_ptr<TDTree> incumbent_tree,
    const FixedPlan& incumbent_plan,
    CandidateBuildLedger& candidate_build_ledger,
    V3MetadataBudget& metadata_budget,
    const TemporalCostV3Options& options = {}) noexcept;

#endif // TEMPORAL_COST_V3_PLANNER_H
