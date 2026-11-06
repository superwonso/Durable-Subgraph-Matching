#include "../TDTree.h"
#include "../Utils.h"
#include "../fixed_plan_feasibility.h"
#include "../query_decomposition.h"
#include "../v3_existing_capacity.h"
#include "../validated_graph_snapshot.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void addQueryArc(Graph& query, int source, int target) {
    query.adj[static_cast<std::size_t>(source)].push_back({target, -1});
    query.in_adj[static_cast<std::size_t>(target)].push_back({source, -1});
}

void addTemporalEdge(Graph& graph, int source, int target) {
    const int edge_id = static_cast<int>(graph.temporal_edges.size());
    graph.temporal_edges.emplace_back(
        source, target, std::vector<TimeInterval>{{1, 5}}, 5);
    graph.adj[static_cast<std::size_t>(source)].push_back({target, edge_id});
    graph.in_adj[static_cast<std::size_t>(target)].push_back({source, edge_id});
}

struct Prepared {
    std::shared_ptr<const ValidatedGraphSnapshot> snapshot;
    FixedPlan incumbent_plan;
    std::unique_ptr<TDTree> incumbent;
    CandidateCover cover;
    FixedPlan candidate_plan;
    CandidateBuildQuote quote;

    Prepared(
        std::shared_ptr<const ValidatedGraphSnapshot> snapshot_value,
        FixedPlan incumbent_plan_value,
        std::unique_ptr<TDTree> incumbent_value,
        CandidateCover cover_value,
        FixedPlan candidate_plan_value,
        CandidateBuildQuote quote_value)
        : snapshot(std::move(snapshot_value)),
          incumbent_plan(std::move(incumbent_plan_value)),
          incumbent(std::move(incumbent_value)),
          cover(std::move(cover_value)),
          candidate_plan(std::move(candidate_plan_value)),
          quote(std::move(quote_value)) {}
};

Prepared prepare() {
    Graph data;
    data.num_vertices = 4;
    data.vertex_labels = {
        labelFromString("A"), labelFromString("B"),
        labelFromString("A"), labelFromString("B")};
    data.adj.resize(4);
    data.in_adj.resize(4);
    addTemporalEdge(data, 0, 1);
    addTemporalEdge(data, 2, 3);
    data.external_ids = {100, 101, 102, 103};
    data.vertex_active_durations.assign(4, 8);
    data.neighbor_label_counts.assign(4, {});
    data.incoming_neighbor_label_counts.assign(4, {});
    for (std::size_t vertex = 0; vertex < data.adj.size(); ++vertex) {
        for (const Edge& edge : data.adj[vertex]) {
            ++data.neighbor_label_counts[vertex][
                data.vertex_labels[static_cast<std::size_t>(edge.to)]];
        }
        for (const Edge& edge : data.in_adj[vertex]) {
            ++data.incoming_neighbor_label_counts[vertex][
                data.vertex_labels[static_cast<std::size_t>(edge.to)]];
        }
    }

    Graph query;
    query.num_vertices = 2;
    query.vertex_labels = {labelFromString("A"), labelFromString("B")};
    query.external_ids = {0, 1};
    query.adj.resize(2);
    query.in_adj.resize(2);
    addQueryArc(query, 0, 1);

    GraphSnapshotSealResult sealed = ValidatedGraphSnapshot::seal(
        data, query, GraphValidationBudget::unbounded());
    require(sealed.complete(), "capacity fixture snapshot seals");
    std::array<std::size_t, kLabelCount> counts{};
    std::array<double, kLabelCount> means{};
    counts[labelFromString("A")] = 2;
    counts[labelFromString("B")] = 2;
    means[labelFromString("A")] = 8.0;
    means[labelFromString("B")] = 8.0;
    QueryDecomposition decomposition = decomposeQuery(
        sealed.snapshot->query(), counts, means, SelectivityMode::Paper);
    FixedPlan incumbent_plan = makeFixedPlan(*sealed.snapshot, decomposition);
    std::unique_ptr<TDTree> incumbent = TDTree::buildValidatedIncumbent(
        sealed.snapshot, decomposition, 3);
    require(
        incumbent && incumbent->isBoundToValidatedSnapshot(sealed.snapshot) &&
            incumbent->executionPlanFingerprint() == incumbent_plan.fingerprint,
        "capacity fixture incumbent binds the sealed fixed plan");
    CandidateCover cover = incumbent->exportCandidateCover();
    FixedPlan candidate_plan = makeFixedPlan(
        *sealed.snapshot,
        incumbent_plan.root == 0 ? 1 : 0,
        std::vector<int>{incumbent_plan.root == 0 ? 1 : 0,
                         incumbent_plan.root == 0 ? 0 : 1},
        incumbent_plan.root == 0
            ? std::vector<int>{1, -1}
            : std::vector<int>{-1, 0});
    require(
        validateFixedPlan(*sealed.snapshot, candidate_plan).valid &&
            candidate_plan.fingerprint != incumbent_plan.fingerprint,
        "capacity fixture has one nonincumbent plan");
    CandidateBuildQuote quote = quoteCoverAwareBuild(
        sealed.snapshot, cover, candidate_plan);
    require(quote.complete, "capacity fixture quote completes");
    return Prepared(
        sealed.snapshot, std::move(incumbent_plan), std::move(incumbent),
        std::move(cover), std::move(candidate_plan), std::move(quote));
}

struct FullEvidence {
    ExistingCapacityReceiptV1 existing;
    V3SelectedCandidateCarrierV1 selected;
};

FullEvidence capture(Prepared& prepared, bool source_released = true) {
    std::vector<FixedPlan> proposals;
    proposals.reserve(2);
    proposals.push_back(prepared.incumbent_plan);
    proposals.push_back(prepared.candidate_plan);
    const PlanContainerCapacityDetailV1 proposal_live =
        proposalPlanCapacityDetailV1(proposals);
    std::vector<FixedPlan>().swap(proposals);

    std::vector<V3SelectedCandidateCarrierV1> admitted;
    admitted.reserve(1);
    V3SelectedCandidateCarrierV1 admitted_value;
    admitted_value.available = true;
    admitted_value.plan = prepared.candidate_plan;
    admitted_value.quote = prepared.quote;
    admitted.push_back(std::move(admitted_value));
    const PlanContainerCapacityDetailV1 admitted_live =
        admittedCandidateCapacityDetailV1(admitted);
    std::vector<V3SelectedCandidateCarrierV1>().swap(admitted);

    std::vector<V3LazyCacheEntryStorageV1> lazy;
    lazy.reserve(2);
    V3LazyCacheEntryStorageV1 lazy_entry;
    lazy_entry.values.reserve(4);
    lazy_entry.values.push_back(1);
    lazy.push_back(std::move(lazy_entry));
    const LazyCacheCapacityDetailV1 lazy_live =
        lazyCacheCapacityDetailV1(lazy);
    std::vector<V3LazyCacheEntryStorageV1>().swap(lazy);

    FullEvidence result;
    result.selected.available = true;
    result.selected.plan = prepared.candidate_plan;
    result.selected.quote = prepared.quote;
    result.selected.score.available = true;
    result.selected.score.exact = true;
    result.selected.score.plan_fingerprint = prepared.candidate_plan.fingerprint;
    result.selected.retained_score_rows.reserve(2);
    result.selected.retained_score_rows.push_back(
        {prepared.candidate_plan.fingerprint, 2, 3, 5});

    std::vector<std::uint64_t> trace;
    trace.reserve(8);
    trace.push_back(7);
    Graph released_data;
    Graph released_query;
    Graph nonreleased;
    nonreleased.adj.reserve(1);
    ExistingCapacityCaptureInputsV1 inputs;
    inputs.snapshot = prepared.snapshot;
    inputs.incumbent_tree = prepared.incumbent.get();
    inputs.incumbent_plan = &prepared.incumbent_plan;
    inputs.cover = &prepared.cover;
    inputs.selected_candidate = &result.selected;
    inputs.proposal_plans =
        releasedPlanContainerCapacityReceiptV1(proposal_live);
    inputs.admitted_candidates =
        releasedPlanContainerCapacityReceiptV1(admitted_live);
    inputs.lazy_cache = releasedLazyCacheCapacityReceiptV1(lazy_live);
    inputs.planner_trace = &trace;
    inputs.source_data_graph = sourceGraphReleaseReceiptV1(
        source_released ? released_data : nonreleased,
        "source_data_graph");
    inputs.source_query_graph = sourceGraphReleaseReceiptV1(
        released_query, "source_query_graph");
    inputs.phase_generation = 1;
    inputs.expected_accounting_model_sha256 =
        existingCapacityAccountingModelArtifactV1().sha256;
    result.existing = captureExistingCapacityReceiptV1(inputs);
    return result;
}

CandidateBuildBudget budgetFor(const CandidateBuildQuote& quote) {
    CandidateBuildBudget budget;
    budget.adjacency_probes = quote.adjacency_probes;
    budget.reverse_edge_lookups = quote.reverse_edge_lookups;
    budget.arc_checks = quote.maximum_arc_checks;
    budget.relation_entries = quote.maximum_relation_entries;
    budget.candidate_value_occurrences =
        quote.maximum_candidate_value_occurrences;
    budget.blocks = quote.maximum_blocks;
    budget.semijoin_entry_visits = quote.maximum_semijoin_entry_visits;
    budget.index_entries = quote.maximum_index_entries;
    budget.payload_memory_bytes = quote.maximum_payload_bytes;
    budget.fixed_memory_headroom_bytes = 4096;
    budget.candidate_tree_tokens = 1;
    return budget;
}

bool equalBudget(
    const CandidateBuildBudget& lhs,
    const CandidateBuildBudget& rhs) {
    return lhs.adjacency_probes == rhs.adjacency_probes &&
        lhs.reverse_edge_lookups == rhs.reverse_edge_lookups &&
        lhs.arc_checks == rhs.arc_checks &&
        lhs.relation_entries == rhs.relation_entries &&
        lhs.candidate_value_occurrences == rhs.candidate_value_occurrences &&
        lhs.blocks == rhs.blocks &&
        lhs.semijoin_entry_visits == rhs.semijoin_entry_visits &&
        lhs.index_entries == rhs.index_entries &&
        lhs.payload_memory_bytes == rhs.payload_memory_bytes &&
        lhs.fixed_memory_headroom_bytes == rhs.fixed_memory_headroom_bytes &&
        lhs.candidate_tree_tokens == rhs.candidate_tree_tokens;
}

void testCapacityArithmeticAndModel() {
    const ExistingCapacityAccountingModelArtifactV1 model =
        existingCapacityAccountingModelArtifactV1();
    require(model.complete(), "existing-capacity model is canonical/build-bound");
    std::uint64_t result = 0;
    require(
        checkedExistingCapacityAddProductForTesting(5, 3, 7, result) &&
            result == 26,
        "checked capacity product/add exact arithmetic");
    require(
        !checkedExistingCapacityAddProductForTesting(
            std::numeric_limits<std::uint64_t>::max(), 1, 1, result) &&
        !checkedExistingCapacityAddProductForTesting(
            0, std::numeric_limits<std::uint64_t>::max(), 2, result),
        "component add and multiply overflow hooks fail closed");

    FixedPlan plan;
    plan.order.reserve(5);
    plan.parent.reserve(7);
    plan.order = {0, 1};
    plan.parent = {-1, 0};
    plan.query_fingerprint = 11;
    plan.fingerprint = 12;
    const FixedPlanCapacityReceiptV1 fixed = fixedPlanCapacityReceiptV1(plan);
    require(
        fixed.invariantHolds() && fixed.order_capacity == 5 &&
            fixed.parent_capacity == 7 &&
            fixed.order_bytes == 5 * sizeof(int) &&
            fixed.parent_bytes == 7 * sizeof(int),
        "fixed-plan receipt uses actual native capacities");
    FixedPlanCapacityReceiptV1 tampered = fixed;
    ++tampered.order_bytes;
    require(!tampered.invariantHolds(), "fixed-plan arithmetic tamper rejects");
}

void testClosedReceiptAndTamper() {
    Prepared prepared = prepare();
    FullEvidence evidence = capture(prepared);
    require(
        evidence.existing.invariantHolds() &&
            evidence.existing.proposal_plans_owned_bytes == 0 &&
            evidence.existing.admitted_candidates_owned_bytes == 0 &&
            evidence.existing.lazy_cache_owned_bytes == 0 &&
            evidence.existing.source_data_graph_owned_bytes == 0 &&
            evidence.existing.source_query_graph_owned_bytes == 0 &&
            evidence.existing.selected_candidate_plan_detail.score_row_count == 1 &&
            evidence.existing.selected_candidate_plan_detail.score_row_capacity == 2 &&
            evidence.existing.selected_candidate_plan_owned_bytes > 0 &&
            evidence.existing.planner_trace_owned_bytes > 0,
        "closed E receipt counts retained owners and proves released owners zero");

    ExistingCapacityReceiptV1 tampered = evidence.existing;
    ++tampered.candidate_cover_owned_bytes;
    tampered.receipt_sha256 = existingCapacityReceiptSha256V1(tampered);
    require(
        !tampered.invariantHolds(),
        "coordinated component+canonical reseal cannot bypass detailed arithmetic");
    tampered = evidence.existing;
    tampered.selected_candidate_plan_detail.score_row_capacity = 3;
    tampered.receipt_sha256 = existingCapacityReceiptSha256V1(tampered);
    require(
        !tampered.invariantHolds(),
        "score-row count/capacity coordinated tamper rejects");
    tampered = evidence.existing;
    tampered.incumbent_flat_tdtree_detail.sorted_flat_blocks_only = false;
    tampered.receipt_sha256 = existingCapacityReceiptSha256V1(tampered);
    require(!tampered.invariantHolds(), "flat-only incumbent invariant is closed");
    tampered = evidence.existing;
    ++tampered.validated_snapshot_detail.total_bytes;
    tampered.receipt_sha256 = existingCapacityReceiptSha256V1(tampered);
    require(!tampered.invariantHolds(), "snapshot storage mismatch rejects");

    FullEvidence unreleased = capture(prepared, false);
    require(
        !unreleased.existing.complete &&
            !unreleased.existing.invariantHolds(),
        "source owner not released rejects capture");
}

void testAdmissionBoundaries() {
    constexpr std::uint64_t existing = 100;
    constexpr std::uint64_t quote = 25;
    constexpr std::uint64_t headroom = 40;
    constexpr std::uint64_t required = existing + quote + headroom;
    V3GlobalMemoryAdmissionReceiptV1 exact =
        evaluateV3GlobalMemoryAdmissionValuesForTesting(
            existing, quote, quote, headroom, required);
    require(
        exact.invariantHolds() && exact.fits && exact.exact_boundary &&
            !exact.full_process_memory_proof &&
            !exact.production_authorization &&
            exact.admission_scope ==
                "pre_reservation_closed_11_components_v1",
        "E+Q+H exact equality is admitted only in diagnostic scope");
    const V3GlobalMemoryAdmissionReceiptV1 minus =
        evaluateV3GlobalMemoryAdmissionValuesForTesting(
            existing, quote, quote, headroom, required - 1);
    require(
        minus.invariantHolds() && !minus.fits &&
            minus.reason ==
                V3GlobalMemoryAdmissionReason::ProcessMemoryLimitExceeded,
        "global limit cap-minus-one rejects");
    const V3GlobalMemoryAdmissionReceiptV1 payload_minus =
        evaluateV3GlobalMemoryAdmissionValuesForTesting(
            existing, quote, quote - 1, headroom, required);
    require(
        payload_minus.invariantHolds() && !payload_minus.fits &&
            payload_minus.reason ==
                V3GlobalMemoryAdmissionReason::CandidatePayloadCeilingExceeded,
        "Q_B>P rejects independently of global equality");
    const V3GlobalMemoryAdmissionReceiptV1 overflow =
        evaluateV3GlobalMemoryAdmissionValuesForTesting(
            std::numeric_limits<std::uint64_t>::max(), 1,
            std::numeric_limits<std::uint64_t>::max(), 0,
            std::numeric_limits<std::uint64_t>::max());
    require(
        !overflow.complete && overflow.overflowed &&
            overflow.reason == V3GlobalMemoryAdmissionReason::ArithmeticOverflow,
        "global checked addition overflow rejects");
    exact.production_authorization = true;
    exact.admission_sha256 = v3GlobalMemoryAdmissionSha256V1(exact);
    require(
        !exact.invariantHolds(),
        "pre-reservation receipt cannot be promoted to production authority");
}

void testOneShotReservationAndAllocationFailure() {
    Prepared prepared = prepare();
    FullEvidence evidence = capture(prepared);
    CandidateBuildBudget initial = budgetFor(prepared.quote);
    const std::uint64_t required =
        evidence.existing.accounted_existing_capacity_bytes +
        prepared.quote.maximum_payload_bytes +
        initial.fixed_memory_headroom_bytes;
    V3GlobalMemoryAdmissionReceiptV1 admission =
        evaluateV3GlobalMemoryAdmissionV1(
            evidence.existing, prepared.quote,
            initial.payload_memory_bytes,
            initial.fixed_memory_headroom_bytes, required);
    require(admission.invariantHolds(), "live exact admission completes");

    CandidateBuildLedger ledger(initial);
    ExistingCapacityAdmissionSealV1 seal(evidence.existing, admission);
    setCandidateBuildReservationAllocationFailureForTesting(true);
    bool allocation_failed = false;
    try {
        (void)tryReserveCandidateBuild(prepared.quote, ledger, seal);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    require(
        allocation_failed && !seal.consumed() &&
            equalBudget(ledger.remaining(), initial),
        "reservation allocation failure leaves seal and ledger byte-identical");

    CandidateBuildReservationAttempt reserved =
        tryReserveCandidateBuild(prepared.quote, ledger, seal);
    require(
        reserved.fit.fits && reserved.reservation && seal.consumed() &&
            ledger.remaining().candidate_tree_tokens == 0 &&
            ledger.remaining().fixed_memory_headroom_bytes ==
                initial.fixed_memory_headroom_bytes,
        "one successful reservation consumes seal/token without debiting H");
    CandidateBuildReservationAttempt second =
        tryReserveCandidateBuild(prepared.quote, ledger, seal);
    require(
        !second.fit.fits && !second.reservation,
        "one-shot admission cannot authorize a second reservation");
    reserved.reservation.reset();
    require(
        equalBudget(ledger.remaining(), initial),
        "uncommitted reservation rollback refunds exact candidate debit only");
}

} // namespace

int main() {
    try {
        testCapacityArithmeticAndModel();
        testClosedReceiptAndTamper();
        testAdmissionBoundaries();
        testOneShotReservationAndAllocationFailure();
        std::cout << "All V3 existing-capacity tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
