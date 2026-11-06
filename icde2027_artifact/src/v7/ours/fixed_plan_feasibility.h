#ifndef FIXED_PLAN_FEASIBILITY_H
#define FIXED_PLAN_FEASIBILITY_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "Utils.h"

struct QueryDecomposition;
class TDTree;
class ValidatedGraphSnapshot;
struct FixedPlan;
struct CandidateBuildAccounting;
struct CandidateBuildQuote;
struct CandidateBuildBudget;
struct CandidateBuildBudgetFit;
struct CandidateBuildReservationAttempt;
class CoverAwareTDTreeBuilder;
class ExistingCapacityAdmissionSealV1;

// Exact owning-capacity receipt for CandidateCover.  sizeof(CandidateCover)
// includes the inline shared_ptr handle, but the shared snapshot pointee and
// its control block are intentionally excluded because the retained snapshot
// component accounts for them once.
struct CandidateCoverCapacityReceipt {
    bool complete = false;
    bool overflowed = false;
    std::uint64_t object_bytes = 0;
    std::uint64_t domain_count = 0;
    std::uint64_t domain_capacity = 0;
    std::uint64_t outer_domains_bytes = 0;
    std::uint64_t value_count = 0;
    std::uint64_t value_capacity = 0;
    std::uint64_t nested_values_bytes = 0;
    std::uint64_t dynamic_bytes = 0;
    std::uint64_t total_bytes = 0;

    bool invariantHolds() const noexcept;
};

#ifdef DSM_TCC_V3_TESTING
bool checkedCandidateCoverCapacityAddProductForTesting(
    std::uint64_t initial,
    std::uint64_t count,
    std::uint64_t width,
    std::uint64_t& result) noexcept;
#endif

// Immutable, owning projection of the candidate domains that remain in a
// completely built incumbent TD-tree.  It deliberately exposes no mutable
// relation storage and carries enough identity to reject cross-query/graph/k
// reuse before a future cover-aware build starts.
class CandidateCover {
public:
    CandidateCover(const CandidateCover&) = default;
    CandidateCover(CandidateCover&&) noexcept = default;
    CandidateCover& operator=(const CandidateCover&) = delete;
    CandidateCover& operator=(CandidateCover&&) = delete;

    std::size_t queryVertexCount() const noexcept;
    const std::vector<std::vector<int>>& values() const noexcept;
    const std::vector<int>& domain(std::size_t query_vertex) const;

    std::uint64_t graphFingerprint() const noexcept;
    std::uint64_t queryFingerprint() const noexcept;
    int minimumDuration() const noexcept;
    std::uint64_t incumbentPlanFingerprint() const noexcept;
    std::uint64_t coverFingerprint() const noexcept;
    std::uint64_t sourceRelationEntries() const noexcept;
    CandidateCoverCapacityReceipt capacityReceipt() const noexcept;
    bool hasValidatedSnapshotAuthority() const noexcept;
    std::uint64_t snapshotToken() const noexcept;
    bool isBoundToValidatedSnapshot(
        const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot)
        const noexcept;
    bool isBoundTo(
        const Graph& data,
        const Graph& query) const noexcept;

private:
    friend class TDTree;
    friend struct CandidateBuildQuote;
    friend CandidateBuildQuote quoteCoverAwareBuild(
        const Graph&,
        const Graph&,
        const CandidateCover&,
        const FixedPlan&,
        CandidateBuildAccounting);
    friend CandidateBuildQuote quoteCoverAwareBuild(
        const std::shared_ptr<const ValidatedGraphSnapshot>&,
        const CandidateCover&,
        const FixedPlan&,
        CandidateBuildAccounting);

    CandidateCover(
        std::vector<std::vector<int>> values,
        const Graph* source_graph,
        const Graph* source_query,
        std::uint64_t graph_fingerprint,
        std::uint64_t query_fingerprint,
        int minimum_duration,
        std::uint64_t incumbent_plan_fingerprint,
        std::uint64_t source_relation_entries);
    CandidateCover(
        std::vector<std::vector<int>> values,
        std::shared_ptr<const ValidatedGraphSnapshot> snapshot,
        int minimum_duration,
        std::uint64_t incumbent_plan_fingerprint,
        std::uint64_t source_relation_entries);

    std::vector<std::vector<int>> values_;
    std::shared_ptr<const ValidatedGraphSnapshot> snapshot_owner_;
    // TDTree itself keeps const references to these same objects.  Like the
    // incumbent tree, a cover is valid only while they remain alive and are
    // not mutated.  Binding their identities avoids an O(|G|+|E_G|) graph
    // validation/fingerprint pass for every candidate-plan quote.
    const Graph* source_graph_ = nullptr;
    const Graph* source_query_ = nullptr;
    std::uint64_t graph_fingerprint_ = 0;
    std::uint64_t query_fingerprint_ = 0;
    int minimum_duration_ = 0;
    std::uint64_t incumbent_plan_fingerprint_ = 0;
    std::uint64_t cover_fingerprint_ = 0;
    std::uint64_t source_relation_entries_ = 0;
};

struct FixedPlan {
    int root = -1;
    std::vector<int> order;
    std::vector<int> parent;
    std::uint64_t query_fingerprint = 0;
    std::uint64_t fingerprint = 0;
};

std::uint64_t queryGraphContentFingerprint(const Graph& query);

#ifdef DSM_TCC_V3_TESTING
void resetQueryGraphContentFingerprintCallCount() noexcept;
std::uint64_t queryGraphContentFingerprintCallCount() noexcept;
#endif

std::uint64_t fixedPlanFingerprint(
    std::uint64_t query_fingerprint,
    int root,
    const std::vector<int>& order,
    const std::vector<int>& parent) noexcept;

FixedPlan makeFixedPlan(
    const Graph& query,
    int root,
    std::vector<int> order,
    std::vector<int> parent);

FixedPlan makeFixedPlan(
    const Graph& query,
    const QueryDecomposition& decomposition);

FixedPlan makeFixedPlan(
    const ValidatedGraphSnapshot& snapshot,
    int root,
    std::vector<int> order,
    std::vector<int> parent);

FixedPlan makeFixedPlan(
    const ValidatedGraphSnapshot& snapshot,
    const QueryDecomposition& decomposition);

// Validates all topology fields that TDTree construction consumes, not only
// root/order/parent.  The spanning tree and directed non-tree constraints must
// be exactly those induced by the fixed plan and query graph.  Both overloads
// use an allocation-free O(q^2 + |E_Q|^2) comparison: candidate materialization
// has no hidden q-by-q temporary-memory dimension to quote.
bool queryDecompositionTopologyIsCanonical(
    const Graph& query,
    const QueryDecomposition& decomposition) noexcept;

bool queryDecompositionTopologyIsCanonical(
    const ValidatedGraphSnapshot& snapshot,
    const QueryDecomposition& decomposition) noexcept;

enum class FixedPlanValidationStatus {
    Valid,
    InvalidQueryTopology,
    RootOutOfRange,
    OrderSizeMismatch,
    ParentSizeMismatch,
    RootNotFirst,
    OrderVertexOutOfRange,
    DuplicateOrderVertex,
    RootParentNotSentinel,
    NonRootParentOutOfRange,
    ParentDoesNotPrecedeChild,
    ParentChildNotAdjacent,
    QueryFingerprintMismatch,
    PlanFingerprintMismatch
};

struct FixedPlanValidation {
    bool valid = false;
    FixedPlanValidationStatus status =
        FixedPlanValidationStatus::InvalidQueryTopology;
    int query_vertex = -1;
};

const char* fixedPlanValidationStatusName(
    FixedPlanValidationStatus status);

FixedPlanValidation validateFixedPlan(
    const Graph& query,
    const FixedPlan& plan);

// Uses the already validated immutable query topology/fingerprint.  This is an
// allocation-free validation path and does not call
// queryGraphContentFingerprint() or queryTopologyIsValid().
FixedPlanValidation validateFixedPlan(
    const ValidatedGraphSnapshot& snapshot,
    const FixedPlan& plan) noexcept;

enum class RequiredArcMask : std::uint8_t {
    None = 0,
    ParentToChild = 1,
    ChildToParent = 2,
    Reciprocal = 3
};

RequiredArcMask requiredArcMask(
    const Graph& query,
    int query_parent,
    int query_child);

// Accounting constants are explicit so the quote can bind its native payload
// model.  The defaults describe the flat/exact-reserve cover-aware builder and
// incumbent TDTree payloads.  Overrides may only increase these lower bounds.
struct CandidateBuildAccounting {
    std::uint64_t tree_object_payload_bytes = 0;
    std::uint64_t node_payload_bytes = 0;
    std::uint64_t relation_entry_bytes = 0;
    std::uint64_t block_payload_bytes = 0;
    std::uint64_t plan_vertex_payload_bytes = 0;
    std::uint64_t tree_child_entry_bytes = 0;
    std::uint64_t non_tree_edge_payload_bytes = 0;
    std::uint64_t data_vertex_marker_bytes = 0;
    std::uint64_t semijoin_visits_per_candidate_value = 0;
    std::uint64_t semijoin_visits_per_block = 0;
    std::uint64_t index_entries_per_block = 0;
};

CandidateBuildAccounting defaultCandidateBuildAccounting();

enum class CandidateBuildQuoteStatus {
    Complete,
    InvalidDataGraph,
    InvalidQueryGraph,
    InvalidAccounting,
    InvalidMinimumDuration,
    CoverQueryCardinalityMismatch,
    DataGraphIdentityMismatch,
    QueryGraphIdentityMismatch,
    CoverDomainNotSortedUnique,
    CoverCandidateOutOfRange,
    InvalidFixedPlan,
    IncumbentPlanRejected,
    ArithmeticOverflow
};

const char* candidateBuildQuoteStatusName(
    CandidateBuildQuoteStatus status);

class CandidateBuildReservation;
class CandidateBuildLedger;
struct CandidateBuildLedgerState;

struct CandidateBuildQuote {
    bool complete = false;
    bool overflowed = false;
    CandidateBuildQuoteStatus status =
        CandidateBuildQuoteStatus::InvalidFixedPlan;
    FixedPlanValidation plan_validation;

    std::uint64_t graph_fingerprint = 0;
    std::uint64_t query_fingerprint = 0;
    std::uint64_t cover_fingerprint = 0;
    std::uint64_t plan_fingerprint = 0;
    std::uint64_t validation_snapshot_token = 0;
    std::uint64_t graph_validation_rescans = 0;
    std::uint64_t data_fingerprint_rescans = 0;
    std::uint64_t query_fingerprint_rescans = 0;

    std::uint64_t adjacency_probes = 0;
    std::uint64_t reverse_edge_lookups = 0;
    std::uint64_t maximum_arc_checks = 0;
    // Cover cardinality of the proposed root before a builder's local/root
    // predicates and reachability trim; not a materialized TDTree root count.
    std::uint64_t proposed_root_cover_cardinality = 0;
    std::uint64_t maximum_relation_entries = 0;
    // Root cover cardinality plus the relation-entry upper bound.  This is an
    // accounting unit, not the number of unique candidate vertices.
    std::uint64_t maximum_candidate_value_occurrences = 0;
    std::uint64_t maximum_blocks = 0;
    std::uint64_t maximum_tree_child_entries = 0;
    std::uint64_t maximum_non_tree_edges = 0;
    std::uint64_t maximum_root_self_loop_checks = 0;
    std::uint64_t maximum_semijoin_entry_visits = 0;
    std::uint64_t maximum_index_entries = 0;
    std::uint64_t maximum_payload_bytes = 0;

    bool materializerContractBound() const noexcept;
    bool validatedSnapshotBound() const noexcept;
    bool isBoundToValidatedSnapshot(
        const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot)
        const noexcept;
    const char* payloadModel() const noexcept;
    std::uint64_t accountingFingerprint() const noexcept;
    std::uint64_t contractFingerprint() const noexcept;

private:
    friend CandidateBuildQuote quoteCoverAwareBuild(
        const Graph&,
        const Graph&,
        const CandidateCover&,
        const FixedPlan&,
        CandidateBuildAccounting);
    friend CandidateBuildQuote quoteCoverAwareBuild(
        const std::shared_ptr<const ValidatedGraphSnapshot>&,
        const CandidateCover&,
        const FixedPlan&,
        CandidateBuildAccounting);
    friend CandidateBuildBudgetFit evaluateCandidateBuildBudget(
        const CandidateBuildQuote&,
        const CandidateBuildBudget&);
    friend class CandidateBuildReservation;
    friend class CoverAwareTDTreeBuilder;
    friend class CandidateBuildLedger;
    friend bool candidateBuildQuoteContractIsValid(
        const CandidateBuildQuote&);

    static CandidateBuildQuote quoteImpl(
        const Graph& data,
        const Graph& query,
        const CandidateCover& cover,
        const FixedPlan& plan,
        CandidateBuildAccounting accounting,
        std::shared_ptr<const ValidatedGraphSnapshot> snapshot);

    bool materializer_contract_bound_ = false;
    std::shared_ptr<const ValidatedGraphSnapshot> snapshot_owner_;
    CandidateBuildAccounting accounting_;
    std::uint64_t accounting_fingerprint_ = 0;
    std::uint64_t contract_fingerprint_ = 0;
};

CandidateBuildQuote quoteCoverAwareBuild(
    const Graph& data,
    const Graph& query,
    const CandidateCover& cover,
    const FixedPlan& plan,
    CandidateBuildAccounting accounting =
        defaultCandidateBuildAccounting());

// V3-authorized zero-rescan overload. The snapshot, cover, quote and eventual
// candidate tree retain the same immutable owner/control identity.
CandidateBuildQuote quoteCoverAwareBuild(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot,
    const CandidateCover& cover,
    const FixedPlan& plan,
    CandidateBuildAccounting accounting =
        defaultCandidateBuildAccounting());

// Fail-closed hard-budget admission.  Every field defaults to zero so callers
// must reserve every resource explicitly; there is no implicit unlimited cap.
// payload_memory_bytes is the candidate payload ceiling P.  The immutable
// fixed_memory_headroom_bytes is carried as evidence for the separate global
// admission predicate E + Q_B + H <= M; it is never subtracted from P here.
struct CandidateBuildBudget {
    std::uint64_t adjacency_probes = 0;
    std::uint64_t reverse_edge_lookups = 0;
    std::uint64_t arc_checks = 0;
    std::uint64_t relation_entries = 0;
    std::uint64_t candidate_value_occurrences = 0;
    std::uint64_t blocks = 0;
    std::uint64_t semijoin_entry_visits = 0;
    std::uint64_t index_entries = 0;
    std::uint64_t payload_memory_bytes = 0;
    std::uint64_t fixed_memory_headroom_bytes = 0;
    std::uint64_t candidate_tree_tokens = 0;
};

enum class CandidateBuildBudgetFitReason {
    FitsQuote,
    QuoteIncomplete,
    QuoteOverflow,
    NoCandidateTreeToken,
    AdjacencyProbeBudget,
    ReverseLookupBudget,
    ArcCheckBudget,
    RelationEntryBudget,
    CandidateValueBudget,
    BlockBudget,
    SemijoinVisitBudget,
    IndexEntryBudget,
    PayloadMemoryBudget,
    GlobalMemoryAdmission
};

struct CandidateBuildBudgetFit {
    bool fits = false;
    CandidateBuildBudgetFitReason reason =
        CandidateBuildBudgetFitReason::QuoteIncomplete;
    std::uint64_t required_payload_bytes = 0;
};

const char* candidateBuildBudgetFitReasonName(
    CandidateBuildBudgetFitReason reason);

// Pure fit predicate used by the owning reservation factory below.  Direct
// callers do not gain build authority because this function neither reserves
// nor consumes budget.
CandidateBuildBudgetFit evaluateCandidateBuildBudget(
    const CandidateBuildQuote& quote,
    const CandidateBuildBudget& budget);

// Thread-safe all-or-nothing ledger for the materializer partition.  The
// immutable fixed headroom is a guard and is never charged as payload.
class CandidateBuildLedger {
public:
    explicit CandidateBuildLedger(CandidateBuildBudget budget);
    CandidateBuildLedger(const CandidateBuildLedger&) = delete;
    CandidateBuildLedger& operator=(const CandidateBuildLedger&) = delete;

    CandidateBuildBudget remaining() const;

private:
    friend struct CandidateBuildReservationAttempt;
    friend class CandidateBuildReservation;
    friend CandidateBuildReservationAttempt tryReserveCandidateBuild(
        const CandidateBuildQuote&,
        CandidateBuildLedger&);
    friend CandidateBuildReservationAttempt tryReserveCandidateBuild(
        const CandidateBuildQuote&,
        CandidateBuildLedger&,
        ExistingCapacityAdmissionSealV1&);

    std::shared_ptr<CandidateBuildLedgerState> state_;
};

class CandidateBuildReservation {
public:
    ~CandidateBuildReservation();
    CandidateBuildReservation(const CandidateBuildReservation&) = delete;
    CandidateBuildReservation& operator=(
        const CandidateBuildReservation&) = delete;
    CandidateBuildReservation(CandidateBuildReservation&&) = delete;
    CandidateBuildReservation& operator=(
        CandidateBuildReservation&&) = delete;

    const CandidateBuildQuote& quote() const noexcept;
    bool active() const noexcept;

private:
    friend struct CandidateBuildReservationAttempt;
    friend class CoverAwareTDTreeBuilder;
    friend CandidateBuildReservationAttempt tryReserveCandidateBuild(
        const CandidateBuildQuote&,
        CandidateBuildLedger&);
    friend CandidateBuildReservationAttempt tryReserveCandidateBuild(
        const CandidateBuildQuote&,
        CandidateBuildLedger&,
        ExistingCapacityAdmissionSealV1&);

    CandidateBuildReservation(
        const CandidateBuildQuote& quote,
        std::shared_ptr<CandidateBuildLedgerState> state);
    void commit();
    void rollback() noexcept;

    CandidateBuildQuote quote_;
    std::shared_ptr<CandidateBuildLedgerState> state_;
    bool active_ = false;
};

struct CandidateBuildReservationAttempt {
    CandidateBuildBudgetFit fit;
    std::unique_ptr<CandidateBuildReservation> reservation;
};

// Allocates and debits only after the complete ledger snapshot has passed all
// checks.  Allocation failure or any failed cap leaves the ledger unchanged.
CandidateBuildReservationAttempt tryReserveCandidateBuild(
    const CandidateBuildQuote& quote,
    CandidateBuildLedger& ledger);

// Formula-V3 one-shot reservation. The exact E+Q_B+H<=M seal is consumed
// atomically with the candidate ledger token, after reservation-owner
// allocation succeeds. Legacy/synthetic callers retain the overload above;
// production formula authority must use this overload.
CandidateBuildReservationAttempt tryReserveCandidateBuild(
    const CandidateBuildQuote& quote,
    CandidateBuildLedger& ledger,
    ExistingCapacityAdmissionSealV1& admission_seal);

#ifdef DSM_TCC_V3_TESTING
void setCandidateBuildReservationAllocationFailureForTesting(
    bool fail_next) noexcept;
#endif

// Validates the private materializer seal.  Public quote fields are readable
// for telemetry, but mutating any of them makes reservation fail closed.
bool candidateBuildQuoteContractIsValid(
    const CandidateBuildQuote& quote);

#endif // FIXED_PLAN_FEASIBILITY_H
