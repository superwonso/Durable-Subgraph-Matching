#include "v3_plan_handoff.h"

#include "TDTree.h"
#include "validated_graph_snapshot.h"

#include <limits>
#include <new>
#include <atomic>
#include <cmath>
#include <cstring>
#include <utility>

namespace {

#ifdef DSM_TCC_V3_TESTING
std::atomic<std::int64_t> g_handoff_allocation_failure_countdown{-1};

bool injectedAllocationFailure() noexcept {
    std::int64_t expected =
        g_handoff_allocation_failure_countdown.load();
    while (expected >= 0) {
        if (expected == 0) return true;
        if (g_handoff_allocation_failure_countdown
                .compare_exchange_weak(expected, expected - 1)) {
            return false;
        }
    }
    return false;
}
#else
bool injectedAllocationFailure() noexcept { return false; }
#endif

constexpr std::uint64_t kFnvOffset =
    UINT64_C(14695981039346656037);
constexpr std::uint64_t kFnvPrime = UINT64_C(1099511628211);

void appendU64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & UINT64_C(0xff);
        hash *= kFnvPrime;
    }
}

bool checkedAdd(
    std::uint64_t& value,
    std::uint64_t increment) noexcept {
    if (increment >
        std::numeric_limits<std::uint64_t>::max() - value) {
        return false;
    }
    value += increment;
    return true;
}

bool addVectorBytes(
    std::uint64_t& total,
    std::size_t count,
    std::size_t width) noexcept {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (count > static_cast<std::size_t>(
                std::numeric_limits<std::uint64_t>::max()) ||
            width > static_cast<std::size_t>(
                std::numeric_limits<std::uint64_t>::max())) {
            return false;
        }
    }
    const std::uint64_t count_u64 =
        static_cast<std::uint64_t>(count);
    const std::uint64_t width_u64 =
        static_cast<std::uint64_t>(width);
    if (count_u64 != 0 &&
        width_u64 >
            std::numeric_limits<std::uint64_t>::max() / count_u64) {
        return false;
    }
    return checkedAdd(total, count_u64 * width_u64);
}

bool vectorHasQOrZeroShape(
    std::size_t size,
    std::size_t query_vertices) noexcept {
    return size == 0 || size == query_vertices;
}

bool diagnosticsAreConsistent(
    const PlannerDecisionDiagnostics& diagnostics,
    PlannerDecision decision) noexcept {
    if (diagnostics.complete_plan_proposals <
            diagnostics.scored_plan_proposals ||
        diagnostics.scored_plan_proposals <
            diagnostics.materialized_complete_plans ||
        diagnostics.materialized_complete_plans <
            diagnostics.rescored_complete_plans ||
        diagnostics.partial_tree_handoffs != 0 ||
        diagnostics.graph_validation_passes != 1 ||
        diagnostics.graph_validation_rescans != 0 ||
        diagnostics.data_fingerprint_rescans != 0 ||
        diagnostics.query_fingerprint_rescans != 0) {
        return false;
    }
    return decision != PlannerDecision::CandidateSelected ||
        (diagnostics.materialized_complete_plans == 1 &&
         diagnostics.rescored_complete_plans == 1);
}

bool reportingModeIsValid(SelectivityMode mode) noexcept {
    switch (mode) {
        case SelectivityMode::TDTree:
        case SelectivityMode::Paper:
        case SelectivityMode::LegacyReciprocal:
        case SelectivityMode::Intended:
        case SelectivityMode::LegacyExtractedProduct:
        case SelectivityMode::Structural:
        case SelectivityMode::Survival:
        case SelectivityMode::TemporalCost:
        case SelectivityMode::TemporalCostV2:
            return true;
    }
    return false;
}

bool decisionIsValid(PlannerDecision decision) noexcept {
    return decision == PlannerDecision::IncumbentOnly ||
        decision == PlannerDecision::CandidateSelected;
}

std::uint64_t doubleBits(double value) noexcept {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value),
                  "double digest requires 64-bit double");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool appendDoubleVector(
    std::uint64_t& hash,
    const std::vector<double>& values) noexcept {
    appendU64(hash, static_cast<std::uint64_t>(values.size()));
    for (double value : values) {
        if (!std::isfinite(value)) return false;
        appendU64(hash, doubleBits(value));
    }
    return true;
}

bool metadataFingerprint(
    const FixedPlan& plan,
    const SelectedPlanMetadata& metadata,
    std::uint64_t& fingerprint) noexcept {
    if (!reportingModeIsValid(metadata.reporting_mode) ||
        !decisionIsValid(metadata.decision)) {
        return false;
    }
    std::uint64_t hash = kFnvOffset;
    appendU64(hash, UINT64_C(0x4d4554412d563301));
    appendU64(hash, plan.fingerprint);
    appendU64(hash, static_cast<std::uint64_t>(plan.root));
    appendU64(hash, static_cast<std::uint64_t>(plan.order.size()));
    for (int value : plan.order) {
        appendU64(hash, static_cast<std::uint32_t>(value));
    }
    appendU64(hash, static_cast<std::uint64_t>(plan.parent.size()));
    for (int value : plan.parent) {
        appendU64(hash, static_cast<std::uint32_t>(value));
    }
    appendU64(hash, static_cast<std::uint64_t>(metadata.reporting_mode));
    appendU64(hash, static_cast<std::uint64_t>(metadata.decision));
    if (!appendDoubleVector(hash, metadata.root_scores) ||
        !appendDoubleVector(hash, metadata.reporting_selectivity)) {
        return false;
    }
    appendU64(
        hash,
        static_cast<std::uint64_t>(metadata.candidate_counts.size()));
    for (std::uint64_t value : metadata.candidate_counts) {
        appendU64(hash, value);
    }
    const PlannerDecisionDiagnostics& diagnostics =
        metadata.diagnostics;
    appendU64(hash, diagnostics.complete_plan_proposals);
    appendU64(hash, diagnostics.scored_plan_proposals);
    appendU64(hash, diagnostics.materialized_complete_plans);
    appendU64(hash, diagnostics.rescored_complete_plans);
    appendU64(hash, diagnostics.partial_tree_handoffs);
    appendU64(hash, diagnostics.graph_validation_passes);
    appendU64(hash, diagnostics.graph_validation_rescans);
    appendU64(hash, diagnostics.data_fingerprint_rescans);
    appendU64(hash, diagnostics.query_fingerprint_rescans);
    fingerprint = hash;
    return true;
}

std::uint64_t handoffContractFingerprint(
    const V3MetadataQuote& quote,
    PlannerDecision decision,
    SelectivityMode mode) noexcept {
    std::uint64_t hash = kFnvOffset;
    appendU64(hash, UINT64_C(0x48414e444f464631));
    appendU64(hash, static_cast<std::uint64_t>(quote.complete));
    appendU64(hash, static_cast<std::uint64_t>(quote.status));
    appendU64(hash, quote.snapshot_token);
    appendU64(hash, quote.data_fingerprint);
    appendU64(hash, quote.query_fingerprint);
    appendU64(hash, quote.plan_fingerprint);
    appendU64(
        hash,
        static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(quote.minimum_duration)));
    appendU64(hash, quote.query_vertices);
    appendU64(hash, quote.payload_bytes);
    appendU64(hash, quote.payload_cap);
    appendU64(hash, quote.metadata_fingerprint);
    appendU64(hash, static_cast<std::uint64_t>(decision));
    appendU64(hash, static_cast<std::uint64_t>(mode));
    return hash;
}

bool quoteContractMatches(
    const V3MetadataQuote& quote,
    const FixedPlan& plan,
    const SelectedPlanMetadata& metadata) noexcept {
    std::uint64_t expected_metadata_fingerprint = 0;
    return metadataFingerprint(
            plan, metadata, expected_metadata_fingerprint) &&
        expected_metadata_fingerprint != 0 &&
        expected_metadata_fingerprint == quote.metadata_fingerprint &&
        quote.complete &&
        quote.status == V3MetadataQuoteStatus::Complete &&
        quote.snapshot_token != 0 &&
        quote.contract_fingerprint != 0 &&
        quote.contract_fingerprint ==
            handoffContractFingerprint(
                quote, metadata.decision,
                metadata.reporting_mode);
}

template <typename T>
bool copyExact(
    const std::vector<T>& source,
    std::vector<T>& destination) {
    destination.reserve(source.size());
    if (destination.capacity() != source.size()) return false;
    destination.insert(
        destination.end(), source.begin(), source.end());
    return destination.capacity() == destination.size();
}

} // namespace

const char* v3MetadataQuoteStatusName(
    V3MetadataQuoteStatus status) noexcept {
    switch (status) {
        case V3MetadataQuoteStatus::Complete:
            return "complete";
        case V3MetadataQuoteStatus::MissingSnapshot:
            return "missing-snapshot";
        case V3MetadataQuoteStatus::MissingSnapshotAuthority:
            return "missing-snapshot-authority";
        case V3MetadataQuoteStatus::InvalidFixedPlan:
            return "invalid-fixed-plan";
        case V3MetadataQuoteStatus::TreePlanMismatch:
            return "tree-plan-mismatch";
        case V3MetadataQuoteStatus::VectorShapeMismatch:
            return "vector-shape-mismatch";
        case V3MetadataQuoteStatus::InvalidDiagnostics:
            return "invalid-diagnostics";
        case V3MetadataQuoteStatus::ArithmeticOverflow:
            return "arithmetic-overflow";
        case V3MetadataQuoteStatus::PayloadCapExceeded:
            return "payload-cap-exceeded";
        case V3MetadataQuoteStatus::AllocationFailure:
            return "allocation-failure";
    }
    return "unknown";
}

bool V3MetadataQuote::fits(
    const V3MetadataBudget& budget) const noexcept {
    return complete && status == V3MetadataQuoteStatus::Complete &&
        payload_bytes <= budget.payload_bytes &&
        budget.receipt_tokens >= 1;
}

bool V3MetadataQuote::isBoundToValidatedSnapshot(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot)
    const noexcept {
    return sharesValidatedGraphSnapshot(snapshot_owner_, snapshot);
}

V3MetadataQuote quoteV3PlanHandoff(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot,
    const TDTree& selected_tree,
    const FixedPlan& plan,
    const SelectedPlanMetadata& metadata,
    std::uint64_t payload_cap) noexcept {
    V3MetadataQuote quote;
    quote.payload_cap = payload_cap;
    quote.plan_fingerprint = plan.fingerprint;
    quote.minimum_duration = selected_tree.minimumDuration();
    if (!snapshot) return quote;
    quote.snapshot_token = snapshot->snapshotToken();
    quote.data_fingerprint = snapshot->dataFingerprint();
    quote.query_fingerprint = snapshot->queryFingerprint();
    if (!selected_tree.hasValidatedSnapshotAuthority() ||
        !selected_tree.isBoundToValidatedSnapshot(snapshot)) {
        quote.status =
            V3MetadataQuoteStatus::MissingSnapshotAuthority;
        return quote;
    }
    try {
    const FixedPlanValidation validation =
        validateFixedPlan(*snapshot, plan);
    if (!validation.valid) {
        quote.status = V3MetadataQuoteStatus::InvalidFixedPlan;
        return quote;
    }
    if (selected_tree.executionPlanFingerprint() !=
        plan.fingerprint) {
        quote.status = V3MetadataQuoteStatus::TreePlanMismatch;
        return quote;
    }
    const std::size_t query_vertices =
        static_cast<std::size_t>(snapshot->query().num_vertices);
    if (!vectorHasQOrZeroShape(
            metadata.root_scores.size(), query_vertices) ||
        !vectorHasQOrZeroShape(
            metadata.reporting_selectivity.size(),
            query_vertices) ||
        !vectorHasQOrZeroShape(
            metadata.candidate_counts.size(), query_vertices)) {
        quote.status = V3MetadataQuoteStatus::VectorShapeMismatch;
        return quote;
    }
    if (!diagnosticsAreConsistent(
            metadata.diagnostics, metadata.decision)) {
        quote.status = V3MetadataQuoteStatus::InvalidDiagnostics;
        return quote;
    }
    if (!metadataFingerprint(
            plan, metadata, quote.metadata_fingerprint) ||
        quote.metadata_fingerprint == 0) {
        quote.status = V3MetadataQuoteStatus::InvalidDiagnostics;
        return quote;
    }

    quote.query_vertices =
        static_cast<std::uint64_t>(query_vertices);
    std::uint64_t payload = sizeof(SelectedPlanReceipt);
    if (!addVectorBytes(
            payload, plan.order.size(), sizeof(int)) ||
        !addVectorBytes(
            payload, plan.parent.size(), sizeof(int)) ||
        !addVectorBytes(
            payload, metadata.root_scores.size(), sizeof(double)) ||
        !addVectorBytes(
            payload,
            metadata.reporting_selectivity.size(), sizeof(double)) ||
        !addVectorBytes(
            payload,
            metadata.candidate_counts.size(),
            sizeof(std::uint64_t))) {
        quote.status = V3MetadataQuoteStatus::ArithmeticOverflow;
        return quote;
    }
    quote.payload_bytes = payload;
    if (payload > payload_cap) {
        quote.status = V3MetadataQuoteStatus::PayloadCapExceeded;
        return quote;
    }
    quote.complete = true;
    quote.status = V3MetadataQuoteStatus::Complete;
    quote.snapshot_owner_ = snapshot;
    quote.contract_fingerprint = handoffContractFingerprint(
        quote, metadata.decision, metadata.reporting_mode);
    return quote;
    } catch (const std::bad_alloc&) {
        quote.complete = false;
        quote.status = V3MetadataQuoteStatus::AllocationFailure;
        return quote;
    } catch (...) {
        quote.complete = false;
        quote.status = V3MetadataQuoteStatus::InvalidDiagnostics;
        return quote;
    }
}

const FixedPlan& SelectedPlanReceipt::plan() const noexcept {
    return plan_;
}

SelectivityMode SelectedPlanReceipt::reportingMode() const noexcept {
    return reporting_mode_;
}

const std::vector<double>&
SelectedPlanReceipt::rootScores() const noexcept {
    return root_scores_;
}

const std::vector<double>&
SelectedPlanReceipt::reportingSelectivity() const noexcept {
    return reporting_selectivity_;
}

const std::vector<std::uint64_t>&
SelectedPlanReceipt::candidateCounts() const noexcept {
    return candidate_counts_;
}

PlannerDecision SelectedPlanReceipt::decision() const noexcept {
    return decision_;
}

const PlannerDecisionDiagnostics&
SelectedPlanReceipt::diagnostics() const noexcept {
    return diagnostics_;
}

std::uint64_t SelectedPlanReceipt::snapshotToken() const noexcept {
    return snapshot_owner_ == nullptr
        ? 0
        : snapshot_owner_->snapshotToken();
}

std::uint64_t SelectedPlanReceipt::payloadBytes() const noexcept {
    return payload_bytes_;
}

std::uint64_t SelectedPlanReceipt::observedPayloadBytes() const noexcept {
    return observed_payload_bytes_;
}

std::uint64_t SelectedPlanReceipt::payloadCap() const noexcept {
    return payload_cap_;
}

bool V3PlanHandoffResult::complete() const noexcept {
    return status == V3PlanHandoffStatus::Complete &&
        selected_tree != nullptr && receipt != nullptr;
}

#ifdef DSM_TCC_V3_TESTING
void setV3PlanHandoffAllocationFailureCountdown(
    std::int64_t countdown) noexcept {
    g_handoff_allocation_failure_countdown.store(countdown);
}
#endif

V3PlanHandoffResult commitV3PlanHandoff(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot,
    std::unique_ptr<TDTree>& selected_tree,
    const FixedPlan& plan,
    const SelectedPlanMetadata& metadata,
    const V3MetadataQuote& quote,
    V3MetadataBudget& budget) {
    V3PlanHandoffResult result;
    if (!quoteContractMatches(quote, plan, metadata)) {
        result.status =
            V3PlanHandoffStatus::QuoteContractMismatch;
        return result;
    }
    if (!snapshot ||
        !quote.isBoundToValidatedSnapshot(snapshot) ||
        quote.snapshot_token != snapshot->snapshotToken() ||
        quote.data_fingerprint != snapshot->dataFingerprint() ||
        quote.query_fingerprint != snapshot->queryFingerprint() ||
        quote.query_vertices != static_cast<std::uint64_t>(
            snapshot->query().num_vertices)) {
        result.status =
            V3PlanHandoffStatus::QuoteContractMismatch;
        return result;
    }
    if (!selected_tree ||
        !selected_tree->isBoundToValidatedSnapshot(snapshot) ||
        selected_tree->validationSnapshotToken() != quote.snapshot_token ||
        selected_tree->minimumDuration() != quote.minimum_duration ||
        selected_tree->executionPlanFingerprint() !=
            quote.plan_fingerprint ||
        plan.fingerprint != quote.plan_fingerprint ||
        plan.query_fingerprint != quote.query_fingerprint) {
        result.status = V3PlanHandoffStatus::TreePlanMismatch;
        return result;
    }
    try {
        if (injectedAllocationFailure()) throw std::bad_alloc();
        const FixedPlanValidation plan_validation =
            validateFixedPlan(*snapshot, plan);
        if (!plan_validation.valid) {
            result.status = V3PlanHandoffStatus::TreePlanMismatch;
            return result;
        }
        if (!quote.fits(budget)) {
            result.status = V3PlanHandoffStatus::MetadataBudget;
            return result;
        }
        std::unique_ptr<SelectedPlanReceipt> receipt(
            new SelectedPlanReceipt());
        receipt->snapshot_owner_ = snapshot;
        receipt->plan_.root = plan.root;
        receipt->plan_.query_fingerprint = plan.query_fingerprint;
        receipt->plan_.fingerprint = plan.fingerprint;
        if (!copyExact(plan.order, receipt->plan_.order) ||
            injectedAllocationFailure()) {
            result.status = V3PlanHandoffStatus::AllocationFailure;
            return result;
        }
        if (!copyExact(plan.parent, receipt->plan_.parent) ||
            injectedAllocationFailure()) {
            result.status = V3PlanHandoffStatus::AllocationFailure;
            return result;
        }
        receipt->reporting_mode_ = metadata.reporting_mode;
        if (!copyExact(
                metadata.root_scores, receipt->root_scores_) ||
            injectedAllocationFailure()) {
            result.status = V3PlanHandoffStatus::AllocationFailure;
            return result;
        }
        if (!copyExact(
                metadata.reporting_selectivity,
                receipt->reporting_selectivity_) ||
            injectedAllocationFailure()) {
            result.status = V3PlanHandoffStatus::AllocationFailure;
            return result;
        }
        if (!copyExact(
                metadata.candidate_counts,
                receipt->candidate_counts_) ||
            injectedAllocationFailure()) {
            result.status = V3PlanHandoffStatus::AllocationFailure;
            return result;
        }
        receipt->decision_ = metadata.decision;
        receipt->diagnostics_ = metadata.diagnostics;
        receipt->payload_bytes_ = quote.payload_bytes;
        receipt->payload_cap_ = quote.payload_cap;
        std::uint64_t observed_payload = sizeof(SelectedPlanReceipt);
        if (!addVectorBytes(
                observed_payload,
                receipt->plan_.order.capacity(), sizeof(int)) ||
            !addVectorBytes(
                observed_payload,
                receipt->plan_.parent.capacity(), sizeof(int)) ||
            !addVectorBytes(
                observed_payload,
                receipt->root_scores_.capacity(), sizeof(double)) ||
            !addVectorBytes(
                observed_payload,
                receipt->reporting_selectivity_.capacity(),
                sizeof(double)) ||
            !addVectorBytes(
                observed_payload,
                receipt->candidate_counts_.capacity(),
                sizeof(std::uint64_t)) ||
            observed_payload > quote.payload_bytes ||
            quote.payload_bytes > quote.payload_cap) {
            result.status = V3PlanHandoffStatus::AllocationFailure;
            return result;
        }
        receipt->observed_payload_bytes_ = observed_payload;
        budget.payload_bytes -= quote.payload_bytes;
        --budget.receipt_tokens;
        result.status = V3PlanHandoffStatus::Complete;
        result.selected_tree = std::move(selected_tree);
        result.receipt = std::move(receipt);
        return result;
    } catch (const std::bad_alloc&) {
        result.status = V3PlanHandoffStatus::AllocationFailure;
        return result;
    }
}
