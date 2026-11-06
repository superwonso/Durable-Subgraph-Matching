#ifndef COVER_AWARE_TDTREE_BUILDER_H
#define COVER_AWARE_TDTREE_BUILDER_H

#include <cstdint>
#include <memory>

#include "fixed_plan_feasibility.h"

class TDTree;
class ValidatedGraphSnapshot;

enum class CoverAwareBuildStatus {
    Complete,
    MissingReservation,
    InactiveReservation,
    QuoteContractMismatch,
    SourceIdentityMismatch,
    FixedPlanMismatch,
    CapacityContractViolation,
    QuoteViolation,
    AllocationFailure,
    InjectedFailure,
    InternalFailure
};

enum class CoverAwareBuildFailurePoint {
    None,
    AllocationBeforeValidation,
    AfterRoot,
    AfterRelations,
    AfterBottomUp,
    AfterTopDown,
    AllocationAfterRoot
};

const char* coverAwareBuildStatusName(
    CoverAwareBuildStatus status);

struct CandidateBuildCounters {
    std::uint64_t adjacency_probes = 0;
    std::uint64_t reverse_edge_lookups = 0;
    std::uint64_t arc_checks = 0;
    std::uint64_t root_self_loop_checks = 0;
    std::uint64_t relation_entries = 0;
    std::uint64_t candidate_value_occurrences = 0;
    std::uint64_t nonempty_blocks = 0;
    std::uint64_t semijoin_entry_visits = 0;
    std::uint64_t index_entry_visits = 0;
    std::uint64_t selected_tree_arcs = 0;
    std::uint64_t non_tree_prefilter_checks = 0;
    std::uint64_t peak_payload_capacity_bytes = 0;
    std::uint64_t final_payload_capacity_bytes = 0;
    std::uint64_t reserved_payload_bytes = 0;
    std::uint64_t quote_violations = 0;
    std::uint64_t partial_tree_handoffs = 0;
    std::uint64_t graph_validation_rescans = 0;
    std::uint64_t data_fingerprint_rescans = 0;
    std::uint64_t query_fingerprint_rescans = 0;
};

struct CoverAwareBuildResult {
    CoverAwareBuildStatus status =
        CoverAwareBuildStatus::InternalFailure;
    CandidateBuildCounters counters;
    std::unique_ptr<TDTree> tree;

    bool complete() const noexcept;
};

// Consumes the reservation. Failure rolls its complete ledger debit back
// before returning; success commits it and returns the only complete tree.
CoverAwareBuildResult buildCoverAwareTDTree(
    const Graph& data,
    const Graph& query,
    const CandidateCover& cover,
    const FixedPlan& plan,
    std::unique_ptr<CandidateBuildReservation> reservation);

// V3-authorized materializer. Source identity is shared ownership of the
// immutable snapshot; no validation or fingerprint scan occurs here.
CoverAwareBuildResult buildCoverAwareTDTree(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot,
    const CandidateCover& cover,
    const FixedPlan& plan,
    std::unique_ptr<CandidateBuildReservation> reservation);

#ifdef DSM_TCC_V3_TESTING
CoverAwareBuildResult buildCoverAwareTDTreeForTesting(
    const Graph& data,
    const Graph& query,
    const CandidateCover& cover,
    const FixedPlan& plan,
    std::unique_ptr<CandidateBuildReservation> reservation,
    CoverAwareBuildFailurePoint failure_point);
#endif

#endif // COVER_AWARE_TDTREE_BUILDER_H
