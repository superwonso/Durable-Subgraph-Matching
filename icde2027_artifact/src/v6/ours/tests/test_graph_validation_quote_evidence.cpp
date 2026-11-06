#include "../Utils.h"
#include "../v3_canonical_json.h"
#include "../validated_graph_snapshot.h"

#include <algorithm>
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

Graph makeQuery() {
    Graph query;
    query.num_vertices = 3;
    query.vertex_labels = {
        labelFromString("A"), labelFromString("B"), labelFromString("C")};
    query.external_ids = {0, 1, 2};
    query.adj.resize(3);
    query.in_adj.resize(3);
    addQueryArc(query, 0, 1);
    addQueryArc(query, 1, 2);
    for (auto& edges : query.adj) {
        std::sort(edges.begin(), edges.end(), [](const Edge& lhs, const Edge& rhs) {
            return lhs.to < rhs.to;
        });
    }
    for (auto& edges : query.in_adj) {
        std::sort(edges.begin(), edges.end(), [](const Edge& lhs, const Edge& rhs) {
            return lhs.to < rhs.to;
        });
    }
    return query;
}

void addTemporalEdge(
    Graph& graph,
    int source,
    int target,
    std::vector<TimeInterval> intervals) {
    std::int64_t active = 0;
    for (const TimeInterval& interval : intervals) {
        active += interval.length();
    }
    const int edge_id = static_cast<int>(graph.temporal_edges.size());
    graph.temporal_edges.emplace_back(
        source, target, std::move(intervals), active);
    graph.adj[static_cast<std::size_t>(source)].push_back({target, edge_id});
    graph.in_adj[static_cast<std::size_t>(target)].push_back({source, edge_id});
}

Graph makeData() {
    Graph data;
    data.num_vertices = 4;
    data.vertex_labels = {
        labelFromString("A"), labelFromString("B"),
        labelFromString("C"), labelFromString("A")};
    data.external_ids = {10, 11, 12, 13};
    data.adj.resize(4);
    data.in_adj.resize(4);
    addTemporalEdge(data, 0, 1, {{1, 2}, {5, 7}});
    addTemporalEdge(data, 1, 2, {{2, 4}});
    addTemporalEdge(data, 3, 2, {{9, 10}});
    for (auto& edges : data.adj) {
        std::sort(edges.begin(), edges.end(), [](const Edge& lhs, const Edge& rhs) {
            return lhs.to < rhs.to;
        });
    }
    for (auto& edges : data.in_adj) {
        std::sort(edges.begin(), edges.end(), [](const Edge& lhs, const Edge& rhs) {
            return lhs.to < rhs.to;
        });
    }
    return data;
}

void reseal(GraphValidationQuoteEvidenceV1& evidence) {
    evidence.quote_sha256 = graphValidationQuoteEvidenceSha256V1(evidence);
}

void testAccountingModelArtifact() {
    const GraphValidationAccountingModelArtifactV1 artifact =
        graphValidationAccountingModelArtifactV1();
    require(artifact.complete(), "graph accounting artifact is complete");
    require(
        artifact.sha256 == dsm::v3::sha256Hex(artifact.canonical_bytes),
        "graph accounting SHA binds its canonical descriptor");
    const dsm::v3::Json parsed =
        dsm::v3::Json::parse(artifact.canonical_bytes);
    require(
        dsm::v3::canonicalJsonBytes(parsed) == artifact.canonical_bytes &&
            parsed.at("schema").string() ==
                "icde2027_v3_graph_accounting_model" &&
            parsed.at("schema_version").unsignedInteger() == 1 &&
            parsed.at("source_bundle_sha256").string().size() == 64 &&
            parsed.at("compiler_identity_sha256").string().size() == 64 &&
            parsed.at("included_storage_fields").arrayValue().size() >= 10 &&
            parsed.at("excluded_ownership").arrayValue().size() == 4,
        "descriptor binds schema, native ownership, source, and compiler inputs");
}

void testExactEvidenceAndProductionSeal() {
    Graph data = makeData();
    Graph query = makeQuery();
    const GraphValidationQuoteEvidenceV1 expected =
        quoteGraphValidationEvidenceV1(data, query);
    require(
        expected.complete && !expected.overflowed &&
            expected.status == GraphValidationQuoteStatus::Complete &&
            expected.derivation_inputs.filtered_vertices == 4 &&
            expected.derivation_inputs.filtered_edges == 3 &&
            expected.derivation_inputs.interval_records == 4 &&
            expected.derivation_inputs.query_vertices == 3 &&
            expected.derivation_inputs.query_edges == 2 &&
            expected.traversal_counters.storage_quote_graphs_visited == 2 &&
            expected.traversal_counters.storage_quote_vertices_visited == 7 &&
            expected.quote_sha256 ==
                graphValidationQuoteEvidenceSha256V1(expected),
        "external quote carries exact derivation, budget, traversal, and SHA");
    const dsm::v3::Json quote_core = dsm::v3::Json::parse(
        canonicalGraphValidationQuoteEvidenceCoreV1(expected));
    require(
        quote_core.find("quote_sha256") == nullptr &&
            quote_core.find("seal_reverification") == nullptr &&
            quote_core.at("graph_accounting_model_sha256").string() ==
                expected.graph_accounting_model_sha256,
        "quote SHA core matches the Python receipt schema boundary");

    const GraphSnapshotSealResult result =
        ValidatedGraphSnapshot::seal(data, query, expected);
    require(result.complete(), "expected-evidence production seal succeeds");
    require(
        result.receipt.production_expected_quote_binding &&
            result.receipt.expected_quote_reverification_passes == 1 &&
            result.receipt.expected_quote_exact_equality &&
            result.receipt.expected_quote_sha256 == expected.quote_sha256 &&
            result.receipt.observed_quote_sha256 == expected.quote_sha256 &&
            result.receipt.graph_accounting_model_sha256 ==
                expected.graph_accounting_model_sha256 &&
            result.receipt.storage_quote_passes == 1 &&
            result.receipt.storage_quote_graphs_visited == 2 &&
            result.receipt.storage_accounting_passes == 2 &&
            result.receipt.storage_accounting_graphs_visited == 4 &&
            result.receipt.graph_validation_calls == 1,
        "external scan and one seal requote/validation scan remain explicit");
    require(
        result.receipt.snapshot_storage_bytes ==
                expected.exact_budget.snapshot_storage_bytes &&
            result.receipt.temporary_validation_capacity_bytes ==
                expected.exact_budget.temporary_validation_bytes &&
            result.receipt.seal_peak_live_bytes ==
                expected.exact_budget.seal_peak_live_bytes,
        "completed allocation exactly realizes the native quote");
}

void requirePreallocationRejection(
    const GraphValidationQuoteEvidenceV1& evidence,
    GraphValidationStatus status,
    const std::string& message) {
    const GraphSnapshotSealResult result =
        ValidatedGraphSnapshot::seal(makeData(), makeQuery(), evidence);
    require(
        !result.complete() && !result.snapshot &&
            result.receipt.status == status &&
            result.receipt.graph_validation_calls == 0 &&
            result.receipt.snapshot_storage_bytes == 0 &&
            result.receipt.temporary_validation_capacity_bytes == 0,
        message);
}

void testTamperAndModelFailures() {
    const GraphValidationQuoteEvidenceV1 exact =
        quoteGraphValidationEvidenceV1(makeData(), makeQuery());

    GraphValidationQuoteEvidenceV1 invalid_hash = exact;
    invalid_hash.quote_sha256.assign(64, '0');
    requirePreallocationRejection(
        invalid_hash, GraphValidationStatus::InvalidExpectedQuote,
        "quote SHA tamper fails before proportional allocation");

    GraphValidationQuoteEvidenceV1 wrong_model = exact;
    wrong_model.graph_accounting_model_sha256.assign(64, 'a');
    if (wrong_model.graph_accounting_model_sha256 ==
        exact.graph_accounting_model_sha256) {
        wrong_model.graph_accounting_model_sha256.assign(64, 'b');
    }
    reseal(wrong_model);
    requirePreallocationRejection(
        wrong_model, GraphValidationStatus::GraphAccountingModelMismatch,
        "accounting-model tamper fails before requote/allocation");

    GraphValidationQuoteEvidenceV1 derivation_tamper = exact;
    ++derivation_tamper.derivation_inputs.filtered_vertices;
    reseal(derivation_tamper);
    requirePreallocationRejection(
        derivation_tamper, GraphValidationStatus::ExpectedQuoteMismatch,
        "derivation tamper is caught by independent requote equality");

    GraphValidationQuoteEvidenceV1 budget_tamper = exact;
    --budget_tamper.exact_budget.snapshot_storage_bytes;
    reseal(budget_tamper);
    requirePreallocationRejection(
        budget_tamper, GraphValidationStatus::ExpectedQuoteMismatch,
        "budget cap-minus-one with a self-consistent hash cannot become authority");

    GraphValidationQuoteEvidenceV1 traversal_tamper = exact;
    ++traversal_tamper.traversal_counters.storage_quote_vertices_visited;
    reseal(traversal_tamper);
    requirePreallocationRejection(
        traversal_tamper, GraphValidationStatus::ExpectedQuoteMismatch,
        "traversal tamper is caught by typed exact equality");

    GraphValidationQuoteEvidenceV1 control_tamper = exact;
    ++control_tamper.snapshot_owner_control_bytes;
    reseal(control_tamper);
    requirePreallocationRejection(
        control_tamper, GraphValidationStatus::ExpectedQuoteMismatch,
        "owner-control tamper is caught by typed exact equality");

    GraphValidationQuoteEvidenceV1 invalid_schema = exact;
    invalid_schema.schema_version = 2;
    reseal(invalid_schema);
    requirePreallocationRejection(
        invalid_schema, GraphValidationStatus::InvalidExpectedQuote,
        "schema-version drift is rejected before requote/allocation");
}

void testLegacyCapMinusOneEmptyAndInvalidQuery() {
    Graph data = makeData();
    Graph query = makeQuery();
    const GraphValidationQuote quote = quoteGraphValidation(data, query);
    GraphValidationBudget cap_minus_one = quote.exact_budget;
    --cap_minus_one.seal_peak_live_bytes;
    const GraphSnapshotSealResult rejected =
        ValidatedGraphSnapshot::seal(data, query, cap_minus_one);
    require(
        !rejected.complete() && !rejected.snapshot &&
            rejected.receipt.status == GraphValidationStatus::BudgetExceeded &&
            rejected.receipt.graph_validation_calls == 0,
        "legacy synthetic cap-minus-one remains fail-closed preallocation");

    Graph empty;
    empty.num_vertices = 0;
    const GraphValidationQuoteEvidenceV1 empty_quote =
        quoteGraphValidationEvidenceV1(empty, query);
    require(
        empty_quote.complete &&
            empty_quote.derivation_inputs.filtered_vertices == 0 &&
            empty_quote.derivation_inputs.filtered_edges == 0 &&
            empty_quote.exact_budget.temporary_validation_bytes == 0,
        "empty filtered graph keeps legal zero-work/zero-temporary evidence");
    const GraphSnapshotSealResult empty_result =
        ValidatedGraphSnapshot::seal(empty, query, empty_quote);
    require(
        empty_result.complete() &&
            empty_result.receipt.temporary_validation_capacity_bytes == 0,
        "empty filtered graph seals through production expected evidence");

    Graph temporal_query = query;
    temporal_query.temporal_edges.emplace_back(
        0, 1, std::vector<TimeInterval>{{1, 1}}, 1);
    const GraphValidationQuoteEvidenceV1 invalid_query =
        quoteGraphValidationEvidenceV1(data, temporal_query);
    require(
        !invalid_query.complete && !invalid_query.overflowed &&
            invalid_query.status ==
                GraphValidationQuoteStatus::InvalidQueryGraph,
        "query temporal history remains typed invalid-query evidence");
    const GraphSnapshotSealResult invalid_result =
        ValidatedGraphSnapshot::seal(data, temporal_query, invalid_query);
    require(
        invalid_result.receipt.status ==
            GraphValidationStatus::InvalidExpectedQuote,
        "incomplete expected evidence cannot enter production seal");
}

void testCheckedOverflowPrimitive() {
    std::uint64_t result = 0;
    require(
        graphValidationCheckedStorageAddForTesting(5, 7, 11, result) &&
            result == 82,
        "checked storage primitive accepts exact finite multiplication/addition");
    require(
        !graphValidationCheckedStorageAddForTesting(
            0, std::numeric_limits<std::uint64_t>::max(), 2, result),
        "checked storage primitive rejects multiplication overflow");
    require(
        !graphValidationCheckedStorageAddForTesting(
            std::numeric_limits<std::uint64_t>::max(), 1, 1, result),
        "checked storage primitive rejects addition overflow");
}

} // namespace

int main() {
    try {
        testAccountingModelArtifact();
        testExactEvidenceAndProductionSeal();
        testTamperAndModelFailures();
        testLegacyCapMinusOneEmptyAndInvalidQuery();
        testCheckedOverflowPrimitive();
        std::cout << "Graph quote/hash expected-evidence tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
