#ifndef V3_PLAN_HANDOFF_H
#define V3_PLAN_HANDOFF_H

#include <cstdint>
#include <memory>
#include <vector>

#include "fixed_plan_feasibility.h"
#include "query_decomposition.h"

class TDTree;
class ValidatedGraphSnapshot;
struct V3PlanHandoffResult;

enum class PlannerDecision : std::uint8_t {
    IncumbentOnly,
    CandidateSelected
};

struct PlannerDecisionDiagnostics {
    std::uint64_t complete_plan_proposals = 0;
    std::uint64_t scored_plan_proposals = 0;
    std::uint64_t materialized_complete_plans = 0;
    std::uint64_t rescored_complete_plans = 0;
    std::uint64_t partial_tree_handoffs = 0;
    std::uint64_t graph_validation_passes = 0;
    std::uint64_t graph_validation_rescans = 0;
    std::uint64_t data_fingerprint_rescans = 0;
    std::uint64_t query_fingerprint_rescans = 0;
};

struct SelectedPlanMetadata {
    SelectivityMode reporting_mode = SelectivityMode::Paper;
    std::vector<double> root_scores;
    std::vector<double> reporting_selectivity;
    std::vector<std::uint64_t> candidate_counts;
    PlannerDecision decision = PlannerDecision::IncumbentOnly;
    PlannerDecisionDiagnostics diagnostics;
};

enum class V3MetadataQuoteStatus {
    Complete,
    MissingSnapshot,
    MissingSnapshotAuthority,
    InvalidFixedPlan,
    TreePlanMismatch,
    VectorShapeMismatch,
    InvalidDiagnostics,
    ArithmeticOverflow,
    PayloadCapExceeded,
    AllocationFailure
};

const char* v3MetadataQuoteStatusName(
    V3MetadataQuoteStatus status) noexcept;

struct V3MetadataBudget {
    // Caps newly owned SelectedPlanReceipt object + exact vector storage.
    // Caller plan/metadata buffers are pre-existing source-live storage;
    // commit never adopts them and does not debit them here.
    std::uint64_t payload_bytes = 0;
    std::uint64_t receipt_tokens = 0;
};

struct V3MetadataQuote {
    bool complete = false;
    V3MetadataQuoteStatus status =
        V3MetadataQuoteStatus::MissingSnapshot;
    std::uint64_t snapshot_token = 0;
    std::uint64_t data_fingerprint = 0;
    std::uint64_t query_fingerprint = 0;
    std::uint64_t plan_fingerprint = 0;
    int minimum_duration = 0;
    std::uint64_t query_vertices = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t payload_cap = 0;
    std::uint64_t metadata_fingerprint = 0;
    std::uint64_t contract_fingerprint = 0;

    bool fits(const V3MetadataBudget& budget) const noexcept;
    bool isBoundToValidatedSnapshot(
        const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot)
        const noexcept;

private:
    friend V3MetadataQuote quoteV3PlanHandoff(
        const std::shared_ptr<const ValidatedGraphSnapshot>&,
        const TDTree&,
        const FixedPlan&,
        const SelectedPlanMetadata&,
        std::uint64_t) noexcept;

    // A token or content fingerprint alone is not snapshot authority.  Keep
    // the exact control block and pointee used by quote construction so an
    // independently sealed, byte-identical snapshot cannot reuse the quote.
    std::shared_ptr<const ValidatedGraphSnapshot> snapshot_owner_;
};

V3MetadataQuote quoteV3PlanHandoff(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot,
    const TDTree& selected_tree,
    const FixedPlan& plan,
    const SelectedPlanMetadata& metadata,
    std::uint64_t payload_cap) noexcept;

class SelectedPlanReceipt {
public:
    SelectedPlanReceipt(const SelectedPlanReceipt&) = delete;
    SelectedPlanReceipt& operator=(const SelectedPlanReceipt&) = delete;
    SelectedPlanReceipt(SelectedPlanReceipt&&) noexcept = default;
    SelectedPlanReceipt& operator=(SelectedPlanReceipt&&) = delete;

    const FixedPlan& plan() const noexcept;
    SelectivityMode reportingMode() const noexcept;
    const std::vector<double>& rootScores() const noexcept;
    const std::vector<double>& reportingSelectivity() const noexcept;
    const std::vector<std::uint64_t>& candidateCounts() const noexcept;
    PlannerDecision decision() const noexcept;
    const PlannerDecisionDiagnostics& diagnostics() const noexcept;
    std::uint64_t snapshotToken() const noexcept;
    std::uint64_t payloadBytes() const noexcept;
    std::uint64_t observedPayloadBytes() const noexcept;
    std::uint64_t payloadCap() const noexcept;

private:
    friend struct V3PlanHandoffResult;
    friend V3PlanHandoffResult commitV3PlanHandoff(
        const std::shared_ptr<const ValidatedGraphSnapshot>&,
        std::unique_ptr<TDTree>&,
        const FixedPlan&,
        const SelectedPlanMetadata&,
        const V3MetadataQuote&,
        V3MetadataBudget&);

    SelectedPlanReceipt() = default;

    std::shared_ptr<const ValidatedGraphSnapshot> snapshot_owner_;
    FixedPlan plan_;
    SelectivityMode reporting_mode_ = SelectivityMode::Paper;
    std::vector<double> root_scores_;
    std::vector<double> reporting_selectivity_;
    std::vector<std::uint64_t> candidate_counts_;
    PlannerDecision decision_ = PlannerDecision::IncumbentOnly;
    PlannerDecisionDiagnostics diagnostics_;
    std::uint64_t payload_bytes_ = 0;
    std::uint64_t observed_payload_bytes_ = 0;
    std::uint64_t payload_cap_ = 0;
};

enum class V3PlanHandoffStatus {
    Complete,
    QuoteIncomplete,
    QuoteContractMismatch,
    MetadataBudget,
    TreePlanMismatch,
    AllocationFailure
};

struct V3PlanHandoffResult {
    V3PlanHandoffStatus status =
        V3PlanHandoffStatus::QuoteIncomplete;
    std::unique_ptr<TDTree> selected_tree;
    std::unique_ptr<SelectedPlanReceipt> receipt;

    bool complete() const noexcept;
};

// Allocation and ownership commit are transactional. On every failure the
// caller keeps selected_tree and the budget byte-for-byte unchanged. Success
// uses a one-way dedicated metadata debit retained by the immutable receipt.
V3PlanHandoffResult commitV3PlanHandoff(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot,
    std::unique_ptr<TDTree>& selected_tree,
    const FixedPlan& plan,
    const SelectedPlanMetadata& metadata,
    const V3MetadataQuote& quote,
    V3MetadataBudget& budget);

#ifdef DSM_TCC_V3_TESTING
void setV3PlanHandoffAllocationFailureCountdown(
    std::int64_t countdown) noexcept;
#endif

#endif // V3_PLAN_HANDOFF_H
