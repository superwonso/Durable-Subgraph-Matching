#ifndef V3_EXISTING_CAPACITY_H
#define V3_EXISTING_CAPACITY_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "TDTree.h"
#include "fixed_plan_feasibility.h"
#include "validated_graph_snapshot.h"

// Native, allocator-capacity evidence used by the formula-V3 admission gate.
// These receipts deliberately describe only owning capacity. Shared snapshot
// pointees/control storage are charged once by validated_snapshot_bytes;
// shared_ptr handles embedded in other objects remain part of their sizeof.

struct FixedPlanCapacityReceiptV1 {
    bool complete = false;
    bool overflowed = false;
    std::uint64_t object_bytes = 0;
    std::uint64_t order_count = 0;
    std::uint64_t order_capacity = 0;
    std::uint64_t order_bytes = 0;
    std::uint64_t parent_count = 0;
    std::uint64_t parent_capacity = 0;
    std::uint64_t parent_bytes = 0;
    std::uint64_t dynamic_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t query_fingerprint = 0;
    std::uint64_t plan_fingerprint = 0;
    std::uint64_t ownership_fingerprint = 0;

    bool invariantHolds() const noexcept;
};

FixedPlanCapacityReceiptV1 fixedPlanCapacityReceiptV1(
    const FixedPlan& plan) noexcept;

// Complete retained row shape reserved for the weighted cost-V2 patch. Every
// row that crosses candidate reservation must live in the selected carrier's
// vector below, so its actual native capacity cannot escape E accounting.
struct V3RetainedPlanScoreRowV1 {
    std::uint64_t plan_fingerprint = 0;
    std::uint64_t raw_root_candidate_attempts = 0;
    std::uint64_t raw_extension_probes = 0;
    std::uint64_t weighted_subtotal = 0;
};

struct V3RetainedSelectedScoreV1 {
    bool available = false;
    bool exact = false;
    std::uint64_t plan_fingerprint = 0;
    std::uint64_t root_candidates = 0;
    std::uint64_t raw_attempted_extensions = 0;
    std::uint64_t objective = 0;
};

// This is the only candidate-planning carrier retained across reservation.
// Proposal/admitted copies are destroyed first. The complete per-plan evidence
// vector is intentionally present now even while the legacy unit-cost planner
// leaves it empty; the weighted-objective patch must populate this same owner.
struct V3SelectedCandidateCarrierV1 {
    bool available = false;
    FixedPlan plan;
    CandidateBuildQuote quote;
    V3RetainedSelectedScoreV1 score;
    std::vector<V3RetainedPlanScoreRowV1> retained_score_rows;
};

struct PlanContainerCapacityDetailV1 {
    bool complete = false;
    bool overflowed = false;
    std::uint64_t container_object_bytes = 0;
    std::uint64_t element_count = 0;
    std::uint64_t element_capacity = 0;
    std::uint64_t element_width_bytes = 0;
    std::uint64_t outer_elements_bytes = 0;
    std::uint64_t plan_count = 0;
    std::uint64_t plan_order_count = 0;
    std::uint64_t plan_order_capacity = 0;
    std::uint64_t plan_order_bytes = 0;
    std::uint64_t plan_parent_count = 0;
    std::uint64_t plan_parent_capacity = 0;
    std::uint64_t plan_parent_bytes = 0;
    std::uint64_t score_row_count = 0;
    std::uint64_t score_row_capacity = 0;
    std::uint64_t score_row_width_bytes = 0;
    std::uint64_t score_row_bytes = 0;
    std::uint64_t selected_plan_fingerprint = 0;
    std::uint64_t dynamic_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t ownership_fingerprint = 0;

    bool invariantHolds() const noexcept;
};

PlanContainerCapacityDetailV1 proposalPlanCapacityDetailV1(
    const std::vector<FixedPlan>& plans) noexcept;
PlanContainerCapacityDetailV1 admittedCandidateCapacityDetailV1(
    const std::vector<V3SelectedCandidateCarrierV1>& candidates) noexcept;
PlanContainerCapacityDetailV1 selectedCandidateCapacityDetailV1(
    const V3SelectedCandidateCarrierV1& candidate) noexcept;

struct ReleasedPlanContainerCapacityReceiptV1 {
    bool complete = false;
    bool overflowed = false;
    bool released_before_existing_capture = false;
    PlanContainerCapacityDetailV1 last_live_capacity;
    std::uint64_t current_owned_bytes = 0;
    std::uint64_t release_fingerprint = 0;

    bool invariantHolds() const noexcept;
};

ReleasedPlanContainerCapacityReceiptV1
releasedPlanContainerCapacityReceiptV1(
    const PlanContainerCapacityDetailV1& last_live) noexcept;

struct V3LazyCacheKeyStorageV1 {
    std::uint64_t snapshot_token = 0;
    std::uint64_t cover_fingerprint = 0;
    int query_parent = -1;
    int query_child = -1;
    int parent_data = -1;
    RequiredArcMask mask = RequiredArcMask::None;
};

struct V3LazyCacheEntryStorageV1 {
    V3LazyCacheKeyStorageV1 key;
    std::vector<int> values;
};

struct LazyCacheCapacityDetailV1 {
    bool complete = false;
    bool overflowed = false;
    std::uint64_t container_object_bytes = 0;
    std::uint64_t entry_count = 0;
    std::uint64_t entry_capacity = 0;
    std::uint64_t entry_width_bytes = 0;
    std::uint64_t outer_entries_bytes = 0;
    std::uint64_t nested_value_count = 0;
    std::uint64_t nested_value_capacity = 0;
    std::uint64_t nested_value_width_bytes = 0;
    std::uint64_t nested_value_bytes = 0;
    std::uint64_t dynamic_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t ownership_fingerprint = 0;

    bool invariantHolds() const noexcept;
};

LazyCacheCapacityDetailV1 lazyCacheCapacityDetailV1(
    const std::vector<V3LazyCacheEntryStorageV1>& cache) noexcept;

struct ReleasedLazyCacheCapacityReceiptV1 {
    bool complete = false;
    bool overflowed = false;
    bool released_before_existing_capture = false;
    LazyCacheCapacityDetailV1 last_live_capacity;
    std::uint64_t current_owned_bytes = 0;
    std::uint64_t release_fingerprint = 0;

    bool invariantHolds() const noexcept;
};

ReleasedLazyCacheCapacityReceiptV1 releasedLazyCacheCapacityReceiptV1(
    const LazyCacheCapacityDetailV1& last_live) noexcept;

struct PlannerTraceCapacityReceiptV1 {
    bool complete = false;
    bool overflowed = false;
    bool frozen_before_reservation = false;
    std::uint64_t object_bytes = 0;
    std::uint64_t entry_count = 0;
    std::uint64_t entry_capacity = 0;
    std::uint64_t entry_width_bytes = 0;
    std::uint64_t dynamic_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t content_fingerprint = 0;
    std::uint64_t ownership_fingerprint = 0;

    bool invariantHolds() const noexcept;
};

PlannerTraceCapacityReceiptV1 plannerTraceCapacityReceiptV1(
    const std::vector<std::uint64_t>& trace,
    bool frozen_before_reservation) noexcept;

// Exact capacity projection of a source Graph owner. total_owned_bytes is
// dynamic capacity only: the small, still-live inline Graph handle is covered
// by H. A valid release witness requires the full default Graph state and zero
// capacity in every outer and nested owner.
struct SourceGraphReleaseReceiptV1 {
    bool complete = false;
    bool overflowed = false;
    bool released_before_incumbent = false;
    std::string owner_id;
    std::uint64_t vertex_count = 0;
    std::uint64_t adjacency_outer_capacity = 0;
    std::uint64_t adjacency_outer_bytes = 0;
    std::uint64_t adjacency_nested_capacity = 0;
    std::uint64_t adjacency_nested_bytes = 0;
    std::uint64_t incoming_outer_capacity = 0;
    std::uint64_t incoming_outer_bytes = 0;
    std::uint64_t incoming_nested_capacity = 0;
    std::uint64_t incoming_nested_bytes = 0;
    std::uint64_t vertex_labels_capacity = 0;
    std::uint64_t vertex_labels_bytes = 0;
    std::uint64_t external_ids_capacity = 0;
    std::uint64_t external_ids_bytes = 0;
    std::uint64_t temporal_edges_capacity = 0;
    std::uint64_t temporal_edges_bytes = 0;
    std::uint64_t temporal_intervals_capacity = 0;
    std::uint64_t temporal_intervals_bytes = 0;
    std::uint64_t vertex_active_durations_capacity = 0;
    std::uint64_t vertex_active_durations_bytes = 0;
    std::uint64_t neighbor_label_counts_capacity = 0;
    std::uint64_t neighbor_label_counts_bytes = 0;
    std::uint64_t incoming_neighbor_label_counts_capacity = 0;
    std::uint64_t incoming_neighbor_label_counts_bytes = 0;
    std::uint64_t total_owned_bytes = 0;
    std::uint64_t release_fingerprint = 0;

    bool invariantHolds() const noexcept;
};

SourceGraphReleaseReceiptV1 sourceGraphReleaseReceiptV1(
    const Graph& released_graph,
    const std::string& owner_id) noexcept;

struct SnapshotCapacityComponentReceiptV1 {
    bool complete = false;
    bool overflowed = false;
    std::uint64_t snapshot_token = 0;
    std::uint64_t data_fingerprint = 0;
    std::uint64_t query_fingerprint = 0;
    std::uint64_t sealed_snapshot_storage_bytes = 0;
    std::uint64_t quoted_snapshot_storage_bytes = 0;
    std::string graph_accounting_model_sha256;
    std::string expected_quote_sha256;
    std::uint64_t total_bytes = 0;
    std::uint64_t ownership_fingerprint = 0;

    bool invariantHolds() const noexcept;
};

SnapshotCapacityComponentReceiptV1 snapshotCapacityComponentReceiptV1(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot) noexcept;

struct TDTreeCapacityComponentReceiptV1 {
    bool complete = false;
    bool overflowed = false;
    bool sorted_flat_blocks_only = false;
    std::uint64_t snapshot_token = 0;
    std::uint64_t plan_fingerprint = 0;
    std::uint64_t tree_content_fingerprint = 0;
    TDTreeCapacityReceipt native;
    std::uint64_t total_bytes = 0;
    std::uint64_t ownership_fingerprint = 0;

    bool invariantHolds() const noexcept;
};

TDTreeCapacityComponentReceiptV1 tdTreeCapacityComponentReceiptV1(
    const TDTree& tree) noexcept;

struct CandidateCoverCapacityComponentReceiptV1 {
    bool complete = false;
    bool overflowed = false;
    std::uint64_t snapshot_token = 0;
    std::uint64_t cover_fingerprint = 0;
    std::uint64_t incumbent_plan_fingerprint = 0;
    CandidateCoverCapacityReceipt native;
    std::uint64_t total_bytes = 0;
    std::uint64_t ownership_fingerprint = 0;

    bool invariantHolds() const noexcept;
};

CandidateCoverCapacityComponentReceiptV1
candidateCoverCapacityComponentReceiptV1(
    const CandidateCover& cover) noexcept;

struct ExistingCapacityAccountingModelArtifactV1 {
    std::string canonical_bytes;
    std::string sha256;
    bool complete() const noexcept;
};

ExistingCapacityAccountingModelArtifactV1
existingCapacityAccountingModelArtifactV1();

struct ExistingCapacityReceiptV1 {
    std::string schema = "icde2027_v3_existing_capacity_receipt";
    std::uint64_t schema_version = 1;
    bool complete = false;
    bool overflowed = false;
    std::string accounting_model_sha256;
    std::uint64_t phase_generation = 0;

    SnapshotCapacityComponentReceiptV1 validated_snapshot_detail;
    TDTreeCapacityComponentReceiptV1 incumbent_flat_tdtree_detail;
    FixedPlanCapacityReceiptV1 incumbent_fixed_plan_detail;
    CandidateCoverCapacityComponentReceiptV1 candidate_cover_detail;
    ReleasedPlanContainerCapacityReceiptV1 proposal_plans_detail;
    ReleasedPlanContainerCapacityReceiptV1 admitted_candidates_detail;
    PlanContainerCapacityDetailV1 selected_candidate_plan_detail;
    ReleasedLazyCacheCapacityReceiptV1 lazy_cache_detail;
    PlannerTraceCapacityReceiptV1 planner_trace_detail;
    SourceGraphReleaseReceiptV1 source_data_graph_detail;
    SourceGraphReleaseReceiptV1 source_query_graph_detail;

    // Closed component set. No unknown/remnant component can be injected.
    std::uint64_t validated_snapshot_bytes = 0;
    std::uint64_t incumbent_flat_tdtree_bytes = 0;
    std::uint64_t incumbent_fixed_plan_owned_bytes = 0;
    std::uint64_t candidate_cover_owned_bytes = 0;
    std::uint64_t proposal_plans_owned_bytes = 0;
    std::uint64_t admitted_candidates_owned_bytes = 0;
    std::uint64_t selected_candidate_plan_owned_bytes = 0;
    std::uint64_t lazy_cache_owned_bytes = 0;
    std::uint64_t planner_trace_owned_bytes = 0;
    std::uint64_t source_data_graph_owned_bytes = 0;
    std::uint64_t source_query_graph_owned_bytes = 0;
    std::uint64_t accounted_existing_capacity_bytes = 0;
    std::string receipt_sha256;

    bool invariantHolds() const noexcept;
};

struct ExistingCapacityCaptureInputsV1 {
    std::shared_ptr<const ValidatedGraphSnapshot> snapshot;
    const TDTree* incumbent_tree = nullptr;
    const FixedPlan* incumbent_plan = nullptr;
    const CandidateCover* cover = nullptr;
    const V3SelectedCandidateCarrierV1* selected_candidate = nullptr;
    ReleasedPlanContainerCapacityReceiptV1 proposal_plans;
    ReleasedPlanContainerCapacityReceiptV1 admitted_candidates;
    ReleasedLazyCacheCapacityReceiptV1 lazy_cache;
    const std::vector<std::uint64_t>* planner_trace = nullptr;
    SourceGraphReleaseReceiptV1 source_data_graph;
    SourceGraphReleaseReceiptV1 source_query_graph;
    std::uint64_t phase_generation = 0;
    std::string expected_accounting_model_sha256;
};

ExistingCapacityReceiptV1 captureExistingCapacityReceiptV1(
    const ExistingCapacityCaptureInputsV1& inputs) noexcept;

std::string canonicalExistingCapacityReceiptCoreV1(
    const ExistingCapacityReceiptV1& receipt);
std::string existingCapacityReceiptSha256V1(
    const ExistingCapacityReceiptV1& receipt);

enum class V3GlobalMemoryAdmissionReason : std::uint8_t {
    Admitted,
    ExistingReceiptIncomplete,
    AccountingModelMismatch,
    CandidateQuoteIncomplete,
    CandidateQuoteBindingMismatch,
    CandidatePayloadCeilingExceeded,
    ArithmeticOverflow,
    ProcessMemoryLimitExceeded,
    AlreadyConsumed
};

const char* v3GlobalMemoryAdmissionReasonName(
    V3GlobalMemoryAdmissionReason reason) noexcept;

struct V3GlobalMemoryAdmissionReceiptV1 {
    std::string schema = "icde2027_v3_global_memory_admission";
    std::uint64_t schema_version = 1;
    bool complete = false;
    bool overflowed = false;
    bool fits = false;
    std::string admission_scope =
        "pre_reservation_closed_11_components_v1";
    bool full_process_memory_proof = false;
    bool production_authorization = false;
    V3GlobalMemoryAdmissionReason reason =
        V3GlobalMemoryAdmissionReason::ExistingReceiptIncomplete;
    std::string existing_capacity_receipt_sha256;
    std::string existing_capacity_accounting_model_sha256;
    std::uint64_t candidate_quote_contract_fingerprint = 0;
    std::uint64_t selected_plan_fingerprint = 0;
    std::uint64_t accounted_existing_capacity_bytes = 0;
    std::uint64_t candidate_payload_quote_bytes = 0;
    std::uint64_t candidate_payload_ceiling_bytes = 0;
    std::uint64_t fixed_memory_headroom_bytes = 0;
    std::uint64_t process_memory_max_bytes = 0;
    std::uint64_t required_capacity_bytes = 0;
    bool exact_boundary = false;
    std::string admission_sha256;

    bool invariantHolds() const noexcept;
};

V3GlobalMemoryAdmissionReceiptV1 evaluateV3GlobalMemoryAdmissionV1(
    const ExistingCapacityReceiptV1& existing,
    const CandidateBuildQuote& quote,
    std::uint64_t candidate_payload_ceiling_bytes,
    std::uint64_t fixed_memory_headroom_bytes,
    std::uint64_t process_memory_max_bytes) noexcept;

std::string canonicalV3GlobalMemoryAdmissionCoreV1(
    const V3GlobalMemoryAdmissionReceiptV1& receipt);
std::string v3GlobalMemoryAdmissionSha256V1(
    const V3GlobalMemoryAdmissionReceiptV1& receipt);

// Synthetic/unit-test factory. It creates a self-consistent receipt from the
// exact closed component values without requiring impossible live allocations.
// Production capture must use captureExistingCapacityReceiptV1 instead.
#ifdef DSM_TCC_V3_TESTING
V3GlobalMemoryAdmissionReceiptV1
evaluateV3GlobalMemoryAdmissionValuesForTesting(
    std::uint64_t accounted_existing_capacity_bytes,
    std::uint64_t candidate_payload_quote_bytes,
    std::uint64_t candidate_payload_ceiling_bytes,
    std::uint64_t fixed_memory_headroom_bytes,
    std::uint64_t process_memory_max_bytes);
#endif

// Noncopyable, one-shot gate consumed by the reservation overload only after
// reservation-owner allocation succeeds. It does not debit H or any E byte
// from the candidate ledger.
class ExistingCapacityAdmissionSealV1 {
public:
    ExistingCapacityAdmissionSealV1(
        ExistingCapacityReceiptV1 existing,
        V3GlobalMemoryAdmissionReceiptV1 admission);
    ExistingCapacityAdmissionSealV1(
        const ExistingCapacityAdmissionSealV1&) = delete;
    ExistingCapacityAdmissionSealV1& operator=(
        const ExistingCapacityAdmissionSealV1&) = delete;

    const ExistingCapacityReceiptV1& existing() const noexcept;
    const V3GlobalMemoryAdmissionReceiptV1& admission() const noexcept;
    bool consumableFor(const CandidateBuildQuote& quote) const noexcept;
    bool consumed() const noexcept;

private:
    friend CandidateBuildReservationAttempt tryReserveCandidateBuild(
        const CandidateBuildQuote&,
        CandidateBuildLedger&,
        ExistingCapacityAdmissionSealV1&);
    void consume() noexcept;

    ExistingCapacityReceiptV1 existing_;
    V3GlobalMemoryAdmissionReceiptV1 admission_;
    bool consumed_ = false;
};

#ifdef DSM_TCC_V3_TESTING
bool checkedExistingCapacityAddProductForTesting(
    std::uint64_t initial,
    std::uint64_t count,
    std::uint64_t width,
    std::uint64_t& result) noexcept;
#endif

#endif // V3_EXISTING_CAPACITY_H
