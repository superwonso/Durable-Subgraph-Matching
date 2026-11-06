#include "validated_graph_snapshot.h"

#include <v3_build_identity_generated.h>
#include "v3_canonical_json.h"

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kFnvOffset =
    UINT64_C(14695981039346656037);
constexpr std::uint64_t kFnvPrime = UINT64_C(1099511628211);
// Conservative implementation model for shared_ptr(new T): two pointers,
// strong/weak counters and allocator bookkeeping. It is charged before new.
constexpr std::uint64_t kSnapshotOwnerControlBytes =
    8 * static_cast<std::uint64_t>(sizeof(void*));
constexpr const char* kGraphQuoteEvidenceSchema =
    "icde2027_v3_graph_validation_quote_evidence";
constexpr std::uint64_t kGraphQuoteEvidenceVersion = 1;
constexpr const char* kGraphAccountingModelSchema =
    "icde2027_v3_graph_accounting_model";
constexpr std::uint64_t kGraphAccountingModelVersion = 1;

dsm::v3::Json u64Json(std::uint64_t value) {
    return dsm::v3::Json(value);
}

dsm::v3::Json textJson(const std::string& value) {
    return dsm::v3::Json(value);
}

dsm::v3::Json boolJson(bool value) {
    return dsm::v3::Json(value);
}

dsm::v3::Json stringArrayJson(
    std::initializer_list<const char*> values) {
    dsm::v3::Json::Array result;
    result.reserve(values.size());
    for (const char* value : values) {
        result.emplace_back(std::string(value));
    }
    return dsm::v3::Json::array(std::move(result));
}

dsm::v3::Json graphValidationBudgetJson(
    const GraphValidationBudget& budget) {
    dsm::v3::Json::Object result;
    result.emplace(
        "combined_incoming_adjacency_entries",
        u64Json(budget.combined_incoming_adjacency_entries));
    result.emplace(
        "combined_outgoing_adjacency_entries",
        u64Json(budget.combined_outgoing_adjacency_entries));
    result.emplace("data_vertices", u64Json(budget.data_vertices));
    result.emplace(
        "derived_incident_interval_visits",
        u64Json(budget.derived_incident_interval_visits));
    result.emplace(
        "derived_neighbor_label_updates",
        u64Json(budget.derived_neighbor_label_updates));
    result.emplace(
        "interval_records", u64Json(budget.interval_records));
    result.emplace("query_vertices", u64Json(budget.query_vertices));
    result.emplace(
        "seal_peak_live_bytes", u64Json(budget.seal_peak_live_bytes));
    result.emplace(
        "snapshot_storage_bytes",
        u64Json(budget.snapshot_storage_bytes));
    result.emplace("temporal_edges", u64Json(budget.temporal_edges));
    result.emplace(
        "temporary_validation_bytes",
        u64Json(budget.temporary_validation_bytes));
    return dsm::v3::Json::object(std::move(result));
}

dsm::v3::Json derivationInputsJson(
    const GraphValidationDerivationInputsV1& inputs) {
    dsm::v3::Json::Object result;
    result.emplace("filtered_edges", u64Json(inputs.filtered_edges));
    result.emplace(
        "filtered_vertices", u64Json(inputs.filtered_vertices));
    result.emplace(
        "interval_records", u64Json(inputs.interval_records));
    result.emplace("query_edges", u64Json(inputs.query_edges));
    result.emplace("query_vertices", u64Json(inputs.query_vertices));
    return dsm::v3::Json::object(std::move(result));
}

dsm::v3::Json traversalCountersJson(
    const GraphValidationTraversalCountersV1& counters) {
    dsm::v3::Json::Object result;
    result.emplace(
        "storage_quote_graphs_visited",
        u64Json(counters.storage_quote_graphs_visited));
    result.emplace(
        "storage_quote_incoming_adjacency_entries_visited",
        u64Json(
            counters.storage_quote_incoming_adjacency_entries_visited));
    result.emplace(
        "storage_quote_incident_incoming_entries_visited",
        u64Json(
            counters.storage_quote_incident_incoming_entries_visited));
    result.emplace(
        "storage_quote_incident_outgoing_entries_visited",
        u64Json(
            counters.storage_quote_incident_outgoing_entries_visited));
    result.emplace(
        "storage_quote_interval_records_visited",
        u64Json(counters.storage_quote_interval_records_visited));
    result.emplace(
        "storage_quote_outgoing_adjacency_entries_visited",
        u64Json(
            counters.storage_quote_outgoing_adjacency_entries_visited));
    result.emplace(
        "storage_quote_temporal_edges_visited",
        u64Json(counters.storage_quote_temporal_edges_visited));
    result.emplace(
        "storage_quote_vertices_visited",
        u64Json(counters.storage_quote_vertices_visited));
    return dsm::v3::Json::object(std::move(result));
}

dsm::v3::Json graphQuoteEvidenceCoreJson(
    const GraphValidationQuoteEvidenceV1& evidence) {
    dsm::v3::Json::Object result;
    result.emplace("complete", boolJson(evidence.complete));
    result.emplace(
        "derivation_inputs", derivationInputsJson(evidence.derivation_inputs));
    result.emplace("exact_budget", graphValidationBudgetJson(evidence.exact_budget));
    result.emplace(
        "graph_accounting_model_sha256",
        textJson(evidence.graph_accounting_model_sha256));
    result.emplace(
        "maximum_incident_intervals",
        u64Json(evidence.maximum_incident_intervals));
    result.emplace("overflowed", boolJson(evidence.overflowed));
    result.emplace("schema", textJson(evidence.schema));
    result.emplace("schema_version", u64Json(evidence.schema_version));
    result.emplace(
        "snapshot_owner_control_bytes",
        u64Json(evidence.snapshot_owner_control_bytes));
    result.emplace(
        "source_live_storage_bytes",
        u64Json(evidence.source_live_storage_bytes));
    result.emplace(
        "status",
        textJson(graphValidationQuoteStatusName(evidence.status)));
    result.emplace(
        "traversal_counters",
        traversalCountersJson(evidence.traversal_counters));
    return dsm::v3::Json::object(std::move(result));
}

dsm::v3::Json graphAccountingModelJson() {
    dsm::v3::Json::Object widths;
    widths.emplace(
        "array_int_label_count", u64Json(sizeof(std::array<int, kLabelCount>)));
    widths.emplace("edge", u64Json(sizeof(Edge)));
    widths.emplace("graph", u64Json(sizeof(Graph)));
    widths.emplace("int", u64Json(sizeof(int)));
    widths.emplace("label", u64Json(sizeof(Label)));
    widths.emplace(
        "shared_owner_control_estimate",
        u64Json(kSnapshotOwnerControlBytes));
    widths.emplace("temporal_edge", u64Json(sizeof(TemporalEdge)));
    widths.emplace("time_interval", u64Json(sizeof(TimeInterval)));
    widths.emplace("uint8", u64Json(sizeof(std::uint8_t)));
    widths.emplace(
        "validated_graph_snapshot", u64Json(sizeof(ValidatedGraphSnapshot)));
    widths.emplace("vector_edge", u64Json(sizeof(std::vector<Edge>)));
    widths.emplace("void_pointer", u64Json(sizeof(void*)));

    dsm::v3::Json::Object ownership;
    ownership.emplace(
        "clone_vector_capacity",
        textJson("source_size_exact_reserve_v1"));
    ownership.emplace(
        "shared_owner_control",
        textJson("estimated_as_8_pointer_widths_included_once_v1"));
    ownership.emplace(
        "snapshot_graph",
        textJson("snapshot_inline_plus_all_exact_clone_capacities_v1"));
    ownership.emplace(
        "source_graph",
        textJson("source_inline_plus_all_observed_vector_capacities_v1"));
    ownership.emplace(
        "temporary_validation",
        textJson("simultaneously_live_exact_reserved_capacities_v1"));

    dsm::v3::Json::Object equations;
    equations.emplace(
        "seal_peak_live_bytes",
        textJson(
            "source_live_storage_bytes+snapshot_storage_bytes+temporary_validation_bytes_v1"));
    equations.emplace(
        "snapshot_storage_bytes",
        textJson(
            "validated_snapshot_inline+shared_owner_control+exact_clone_capacities_v1"));
    equations.emplace(
        "source_live_storage_bytes",
        textJson("2*graph_inline+all_source_vector_capacities_v1"));
    equations.emplace(
        "temporary_validation_bytes",
        textJson(
            "external_ids+2*edge_reference_flags+maximum_incident_intervals_v1"));

    dsm::v3::Json::Object root;
    root.emplace(
        "compiler_identity_sha256",
        textJson(DSM_V3_COMPILER_IDENTITY_SHA256));
    root.emplace(
        "excluded_ownership",
        stringArrayJson({
            "allocator_metadata",
            "executable_and_static_storage",
            "library_and_runtime_state",
            "stack_frames_and_thread_stacks"}));
    root.emplace(
        "included_storage_fields",
        stringArrayJson({
            "Graph.inline_object",
            "Graph.adj.outer_capacity<vector<Edge>>",
            "Graph.adj[*].capacity<Edge>",
            "Graph.external_ids.capacity<int>",
            "Graph.in_adj.outer_capacity<vector<Edge>>",
            "Graph.in_adj[*].capacity<Edge>",
            "Graph.incoming_neighbor_label_counts.capacity<array<int,kLabelCount>>",
            "Graph.neighbor_label_counts.capacity<array<int,kLabelCount>>",
            "Graph.temporal_edges.capacity<TemporalEdge>",
            "Graph.temporal_edges[*].activeIntervals.capacity<TimeInterval>",
            "Graph.vertex_active_durations.capacity<int>",
            "Graph.vertex_labels.capacity<Label>",
            "ValidatedGraphSnapshot.inline_object",
            "ValidatedGraphSnapshot.shared_owner_control_estimate"}));
    root.emplace(
        "included_temporary_fields",
        stringArrayJson({
            "external_ids.capacity<int>",
            "incident_intervals.capacity<TimeInterval>",
            "incoming_references.capacity<uint8>",
            "outgoing_references.capacity<uint8>"}));
    root.emplace("native_size_bytes", dsm::v3::Json::object(std::move(widths)));
    root.emplace(
        "ownership_convention",
        dsm::v3::Json::object(std::move(ownership)));
    root.emplace(
        "peak_equations", dsm::v3::Json::object(std::move(equations)));
    root.emplace("schema", textJson(kGraphAccountingModelSchema));
    root.emplace("schema_version", u64Json(kGraphAccountingModelVersion));
    root.emplace(
        "source_bundle_sha256", textJson(DSM_V3_SOURCE_BUNDLE_SHA256));
    return dsm::v3::Json::object(std::move(root));
}

void appendU64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & UINT64_C(0xff);
        hash *= kFnvPrime;
    }
}

void appendInt(std::uint64_t& hash, int value) noexcept {
    appendU64(
        hash,
        static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(value)));
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

bool checkedMultiply(
    std::size_t count,
    std::size_t width,
    std::uint64_t& bytes) noexcept {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (count > static_cast<std::size_t>(
                std::numeric_limits<std::uint64_t>::max()) ||
            width > static_cast<std::size_t>(
                std::numeric_limits<std::uint64_t>::max())) {
            return false;
        }
    }
    const std::uint64_t count_u64 = static_cast<std::uint64_t>(count);
    const std::uint64_t width_u64 = static_cast<std::uint64_t>(width);
    if (count_u64 != 0 &&
        width_u64 >
            std::numeric_limits<std::uint64_t>::max() / count_u64) {
        return false;
    }
    bytes = count_u64 * width_u64;
    return true;
}

bool graphValidationBudgetEqual(
    const GraphValidationBudget& lhs,
    const GraphValidationBudget& rhs) noexcept {
    return lhs.data_vertices == rhs.data_vertices &&
        lhs.query_vertices == rhs.query_vertices &&
        lhs.combined_outgoing_adjacency_entries ==
            rhs.combined_outgoing_adjacency_entries &&
        lhs.combined_incoming_adjacency_entries ==
            rhs.combined_incoming_adjacency_entries &&
        lhs.temporal_edges == rhs.temporal_edges &&
        lhs.interval_records == rhs.interval_records &&
        lhs.derived_incident_interval_visits ==
            rhs.derived_incident_interval_visits &&
        lhs.derived_neighbor_label_updates ==
            rhs.derived_neighbor_label_updates &&
        lhs.snapshot_storage_bytes == rhs.snapshot_storage_bytes &&
        lhs.temporary_validation_bytes ==
            rhs.temporary_validation_bytes &&
        lhs.seal_peak_live_bytes == rhs.seal_peak_live_bytes;
}

bool derivationInputsEqual(
    const GraphValidationDerivationInputsV1& lhs,
    const GraphValidationDerivationInputsV1& rhs) noexcept {
    return lhs.filtered_edges == rhs.filtered_edges &&
        lhs.filtered_vertices == rhs.filtered_vertices &&
        lhs.interval_records == rhs.interval_records &&
        lhs.query_edges == rhs.query_edges &&
        lhs.query_vertices == rhs.query_vertices;
}

bool traversalCountersEqual(
    const GraphValidationTraversalCountersV1& lhs,
    const GraphValidationTraversalCountersV1& rhs) noexcept {
    return lhs.storage_quote_graphs_visited ==
            rhs.storage_quote_graphs_visited &&
        lhs.storage_quote_vertices_visited ==
            rhs.storage_quote_vertices_visited &&
        lhs.storage_quote_outgoing_adjacency_entries_visited ==
            rhs.storage_quote_outgoing_adjacency_entries_visited &&
        lhs.storage_quote_incoming_adjacency_entries_visited ==
            rhs.storage_quote_incoming_adjacency_entries_visited &&
        lhs.storage_quote_incident_outgoing_entries_visited ==
            rhs.storage_quote_incident_outgoing_entries_visited &&
        lhs.storage_quote_incident_incoming_entries_visited ==
            rhs.storage_quote_incident_incoming_entries_visited &&
        lhs.storage_quote_temporal_edges_visited ==
            rhs.storage_quote_temporal_edges_visited &&
        lhs.storage_quote_interval_records_visited ==
            rhs.storage_quote_interval_records_visited;
}

bool graphQuoteEvidenceCoreEqual(
    const GraphValidationQuoteEvidenceV1& lhs,
    const GraphValidationQuoteEvidenceV1& rhs) noexcept {
    return lhs.schema == rhs.schema &&
        lhs.schema_version == rhs.schema_version &&
        lhs.complete == rhs.complete &&
        lhs.overflowed == rhs.overflowed &&
        lhs.status == rhs.status &&
        lhs.graph_accounting_model_sha256 ==
            rhs.graph_accounting_model_sha256 &&
        derivationInputsEqual(
            lhs.derivation_inputs, rhs.derivation_inputs) &&
        graphValidationBudgetEqual(lhs.exact_budget, rhs.exact_budget) &&
        lhs.source_live_storage_bytes ==
            rhs.source_live_storage_bytes &&
        lhs.snapshot_owner_control_bytes ==
            rhs.snapshot_owner_control_bytes &&
        lhs.maximum_incident_intervals ==
            rhs.maximum_incident_intervals &&
        traversalCountersEqual(
            lhs.traversal_counters, rhs.traversal_counters);
}

GraphValidationStatus validationStatusForQuoteStatus(
    GraphValidationQuoteStatus status) noexcept {
    switch (status) {
        case GraphValidationQuoteStatus::Complete:
            return GraphValidationStatus::Complete;
        case GraphValidationQuoteStatus::InvalidDataGraph:
            return GraphValidationStatus::InvalidDataGraph;
        case GraphValidationQuoteStatus::InvalidQueryGraph:
            return GraphValidationStatus::InvalidQueryGraph;
        case GraphValidationQuoteStatus::ArithmeticOverflow:
            return GraphValidationStatus::ArithmeticOverflow;
    }
    return GraphValidationStatus::QuoteEvidenceFailure;
}

void copyQuoteCountersToReceipt(
    const GraphValidationQuote& quote,
    GraphValidationReceipt& receipt) noexcept {
    receipt.storage_quote_passes = quote.storage_quote_passes;
    receipt.storage_quote_graphs_visited =
        quote.storage_quote_graphs_visited;
    receipt.storage_quote_vertices_visited =
        quote.storage_quote_vertices_visited;
    receipt.storage_quote_outgoing_adjacency_entries_visited =
        quote.storage_quote_outgoing_adjacency_entries_visited;
    receipt.storage_quote_incoming_adjacency_entries_visited =
        quote.storage_quote_incoming_adjacency_entries_visited;
    receipt.storage_quote_incident_outgoing_entries_visited =
        quote.storage_quote_incident_outgoing_entries_visited;
    receipt.storage_quote_incident_incoming_entries_visited =
        quote.storage_quote_incident_incoming_entries_visited;
    receipt.storage_quote_temporal_edges_visited =
        quote.storage_quote_temporal_edges_visited;
    receipt.storage_quote_interval_records_visited =
        quote.storage_quote_interval_records_visited;
    receipt.storage_accounting_passes =
        quote.storage_accounting_passes;
    receipt.storage_accounting_graphs_visited =
        quote.storage_accounting_graphs_visited;
    receipt.storage_accounting_vertices_visited =
        quote.storage_accounting_vertices_visited;
    receipt.storage_accounting_temporal_edges_visited =
        quote.storage_accounting_temporal_edges_visited;
    receipt.storage_accounting_interval_records_visited =
        quote.storage_accounting_interval_records_visited;
}

bool isLowercaseSha256(const std::string& value) noexcept {
    if (value.size() != 64U) return false;
    for (const char character : value) {
        const bool decimal = character >= '0' && character <= '9';
        const bool hex = character >= 'a' && character <= 'f';
        if (!decimal && !hex) return false;
    }
    return true;
}

GraphValidationQuoteEvidenceV1 makeGraphQuoteEvidence(
    const GraphValidationQuote& quote) {
    GraphValidationQuoteEvidenceV1 result;
    result.complete = quote.complete;
    result.overflowed = quote.overflowed;
    result.status = quote.status;
    result.graph_accounting_model_sha256 =
        graphValidationAccountingModelArtifactV1().sha256;
    result.derivation_inputs.filtered_edges =
        quote.exact_budget.temporal_edges;
    result.derivation_inputs.filtered_vertices =
        quote.exact_budget.data_vertices;
    result.derivation_inputs.interval_records =
        quote.exact_budget.interval_records;
    result.derivation_inputs.query_vertices =
        quote.exact_budget.query_vertices;
    if (quote.storage_quote_outgoing_adjacency_entries_visited >=
            quote.storage_quote_incident_outgoing_entries_visited) {
        result.derivation_inputs.query_edges =
            quote.storage_quote_outgoing_adjacency_entries_visited -
            quote.storage_quote_incident_outgoing_entries_visited;
    } else {
        result.complete = false;
        result.overflowed = true;
        result.status = GraphValidationQuoteStatus::ArithmeticOverflow;
    }
    result.exact_budget = quote.exact_budget;
    result.source_live_storage_bytes =
        quote.source_live_storage_bytes;
    result.snapshot_owner_control_bytes =
        quote.snapshot_owner_control_bytes;
    result.maximum_incident_intervals =
        quote.maximum_incident_intervals;
    result.traversal_counters.storage_quote_graphs_visited =
        quote.storage_quote_graphs_visited;
    result.traversal_counters.storage_quote_vertices_visited =
        quote.storage_quote_vertices_visited;
    result.traversal_counters
        .storage_quote_outgoing_adjacency_entries_visited =
        quote.storage_quote_outgoing_adjacency_entries_visited;
    result.traversal_counters
        .storage_quote_incoming_adjacency_entries_visited =
        quote.storage_quote_incoming_adjacency_entries_visited;
    result.traversal_counters
        .storage_quote_incident_outgoing_entries_visited =
        quote.storage_quote_incident_outgoing_entries_visited;
    result.traversal_counters
        .storage_quote_incident_incoming_entries_visited =
        quote.storage_quote_incident_incoming_entries_visited;
    result.traversal_counters.storage_quote_temporal_edges_visited =
        quote.storage_quote_temporal_edges_visited;
    result.traversal_counters.storage_quote_interval_records_visited =
        quote.storage_quote_interval_records_visited;
    result.quote_sha256 =
        graphValidationQuoteEvidenceSha256V1(result);
    return result;
}

bool addCapacityBytes(
    std::uint64_t& total,
    std::size_t count,
    std::size_t width) noexcept {
    std::uint64_t bytes = 0;
    return checkedMultiply(count, width, bytes) &&
        checkedAdd(total, bytes);
}

bool addGraphStorageBytes(
    const Graph& graph,
    std::uint64_t& total,
    GraphValidationReceipt* receipt = nullptr) noexcept {
    if (receipt != nullptr) {
        if (!checkedAdd(
                receipt->storage_accounting_graphs_visited, 1) ||
            !checkedAdd(
                receipt->storage_accounting_vertices_visited,
                static_cast<std::uint64_t>(graph.adj.size())) ||
            !checkedAdd(
                receipt->storage_accounting_temporal_edges_visited,
                static_cast<std::uint64_t>(
                    graph.temporal_edges.size()))) {
            return false;
        }
        for (const TemporalEdge& edge : graph.temporal_edges) {
            if (!checkedAdd(
                    receipt->storage_accounting_interval_records_visited,
                    static_cast<std::uint64_t>(
                        edge.activeIntervals().size()))) {
                return false;
            }
        }
    }
    if (!addCapacityBytes(total, graph.adj.capacity(),
                          sizeof(std::vector<Edge>)) ||
        !addCapacityBytes(total, graph.in_adj.capacity(),
                          sizeof(std::vector<Edge>)) ||
        !addCapacityBytes(total, graph.vertex_labels.capacity(),
                          sizeof(Label)) ||
        !addCapacityBytes(total, graph.external_ids.capacity(),
                          sizeof(int)) ||
        !addCapacityBytes(total,
                          graph.vertex_active_durations.capacity(),
                          sizeof(int)) ||
        !addCapacityBytes(total,
                          graph.neighbor_label_counts.capacity(),
                          sizeof(std::array<int, kLabelCount>)) ||
        !addCapacityBytes(
            total,
            graph.incoming_neighbor_label_counts.capacity(),
            sizeof(std::array<int, kLabelCount>)) ||
        !addCapacityBytes(total, graph.temporal_edges.capacity(),
                          sizeof(TemporalEdge))) {
        return false;
    }
    for (const auto& neighbors : graph.adj) {
        if (!addCapacityBytes(
                total, neighbors.capacity(), sizeof(Edge))) {
            return false;
        }
    }
    for (const auto& neighbors : graph.in_adj) {
        if (!addCapacityBytes(
                total, neighbors.capacity(), sizeof(Edge))) {
            return false;
        }
    }
    for (const TemporalEdge& edge : graph.temporal_edges) {
        if (!addCapacityBytes(
                total,
                edge.activeIntervals().capacity(),
                sizeof(TimeInterval))) {
            return false;
        }
    }
    return true;
}

struct SealStorageQuote {
    std::uint64_t source_live_bytes = 0;
    std::uint64_t snapshot_bytes = 0;
    std::uint64_t validation_temporary_bytes = 0;
    std::uint64_t peak_live_bytes = 0;
    std::uint64_t maximum_incident_intervals = 0;
};

bool addLogicalGraphCloneBytes(
    const Graph& graph,
    bool rebuild_data_metadata,
    std::uint64_t& total,
    std::uint64_t& maximum_incident_intervals,
    GraphValidationReceipt& receipt) noexcept {
    if (!checkedAdd(receipt.storage_quote_graphs_visited, 1) ||
        !checkedAdd(
            receipt.storage_quote_vertices_visited,
            static_cast<std::uint64_t>(graph.adj.size())) ||
        !checkedAdd(
            receipt.storage_quote_temporal_edges_visited,
            static_cast<std::uint64_t>(
                graph.temporal_edges.size())) ||
        !addCapacityBytes(
            total, graph.adj.size(), sizeof(std::vector<Edge>)) ||
        !addCapacityBytes(
            total, graph.in_adj.size(), sizeof(std::vector<Edge>)) ||
        !addCapacityBytes(
            total, graph.vertex_labels.size(), sizeof(Label)) ||
        !addCapacityBytes(
            total, graph.external_ids.size(), sizeof(int)) ||
        !addCapacityBytes(
            total, graph.temporal_edges.size(), sizeof(TemporalEdge))) {
        return false;
    }
    const std::size_t derived_vertex_count = rebuild_data_metadata &&
            graph.num_vertices >= 0
        ? static_cast<std::size_t>(graph.num_vertices)
        : graph.vertex_active_durations.size();
    const std::size_t duration_count = std::max(
        graph.vertex_active_durations.size(), derived_vertex_count);
    const std::size_t outgoing_label_count = std::max(
        graph.neighbor_label_counts.size(), derived_vertex_count);
    const std::size_t incoming_label_count = std::max(
        graph.incoming_neighbor_label_counts.size(),
        derived_vertex_count);
    if (!addCapacityBytes(total, duration_count, sizeof(int)) ||
        !addCapacityBytes(
            total, outgoing_label_count,
            sizeof(std::array<int, kLabelCount>)) ||
        !addCapacityBytes(
            total, incoming_label_count,
            sizeof(std::array<int, kLabelCount>))) {
        return false;
    }
    for (const auto& neighbors : graph.adj) {
        if (!checkedAdd(
                receipt.storage_quote_outgoing_adjacency_entries_visited,
                static_cast<std::uint64_t>(neighbors.size())) ||
            !addCapacityBytes(
                total, neighbors.size(), sizeof(Edge))) {
            return false;
        }
    }
    for (const auto& neighbors : graph.in_adj) {
        if (!checkedAdd(
                receipt.storage_quote_incoming_adjacency_entries_visited,
                static_cast<std::uint64_t>(neighbors.size())) ||
            !addCapacityBytes(
                total, neighbors.size(), sizeof(Edge))) {
            return false;
        }
    }
    for (const TemporalEdge& edge : graph.temporal_edges) {
        const std::uint64_t interval_count =
            static_cast<std::uint64_t>(
                edge.activeIntervals().size());
        if (!checkedAdd(
                receipt.storage_quote_interval_records_visited,
                interval_count) ||
            !addCapacityBytes(
                total, edge.activeIntervals().size(),
                sizeof(TimeInterval))) {
            return false;
        }
    }
    if (rebuild_data_metadata) {
        const std::size_t vertices = std::min(
            graph.adj.size(), graph.in_adj.size());
        for (std::size_t vertex = 0; vertex < vertices; ++vertex) {
            std::uint64_t incident_count = 0;
            for (const Edge& edge : graph.adj[vertex]) {
                if (!checkedAdd(
                        receipt
                            .storage_quote_incident_outgoing_entries_visited,
                        1)) {
                    return false;
                }
                if (edge.temporal_edge_id >= 0 &&
                    static_cast<std::size_t>(edge.temporal_edge_id) <
                        graph.temporal_edges.size()) {
                    if (!checkedAdd(
                            incident_count,
                            static_cast<std::uint64_t>(
                                graph.temporal_edges[
                                    static_cast<std::size_t>(
                                        edge.temporal_edge_id)]
                                .activeIntervals().size()))) {
                        return false;
                    }
                }
            }
            for (const Edge& edge : graph.in_adj[vertex]) {
                if (!checkedAdd(
                        receipt
                            .storage_quote_incident_incoming_entries_visited,
                        1)) {
                    return false;
                }
                if (edge.temporal_edge_id >= 0 &&
                    static_cast<std::size_t>(edge.temporal_edge_id) <
                        graph.temporal_edges.size()) {
                    if (!checkedAdd(
                            incident_count,
                            static_cast<std::uint64_t>(
                                graph.temporal_edges[
                                    static_cast<std::size_t>(
                                        edge.temporal_edge_id)]
                                .activeIntervals().size()))) {
                        return false;
                    }
                }
            }
            maximum_incident_intervals = std::max(
                maximum_incident_intervals, incident_count);
        }
    }
    return true;
}

bool quoteSealStorage(
    const Graph& data,
    const Graph& query,
    GraphValidationReceipt& receipt,
    SealStorageQuote& quote) noexcept {
    receipt.storage_quote_passes = 1;
    receipt.storage_accounting_passes = 1;
    quote.source_live_bytes = 2 * sizeof(Graph);
    if (!addGraphStorageBytes(
            data, quote.source_live_bytes, &receipt) ||
        !addGraphStorageBytes(
            query, quote.source_live_bytes, &receipt)) {
        return false;
    }
    quote.snapshot_bytes = sizeof(ValidatedGraphSnapshot);
    if (!checkedAdd(
            quote.snapshot_bytes, kSnapshotOwnerControlBytes)) {
        return false;
    }
    std::uint64_t maximum_incident_intervals = 0;
    if (!addLogicalGraphCloneBytes(
            data, true, quote.snapshot_bytes,
            maximum_incident_intervals, receipt) ||
        !addLogicalGraphCloneBytes(
            query, false, quote.snapshot_bytes,
            maximum_incident_intervals, receipt)) {
        return false;
    }
    quote.maximum_incident_intervals = maximum_incident_intervals;
    if (!addCapacityBytes(
            quote.validation_temporary_bytes,
            data.external_ids.size(), sizeof(int)) ||
        !addCapacityBytes(
            quote.validation_temporary_bytes,
            data.temporal_edges.size(), sizeof(std::uint8_t)) ||
        !addCapacityBytes(
            quote.validation_temporary_bytes,
            data.temporal_edges.size(), sizeof(std::uint8_t)) ||
        maximum_incident_intervals >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max()) ||
        !addCapacityBytes(
            quote.validation_temporary_bytes,
            static_cast<std::size_t>(maximum_incident_intervals),
            sizeof(TimeInterval))) {
        return false;
    }
    quote.peak_live_bytes = quote.source_live_bytes;
    return checkedAdd(
               quote.peak_live_bytes, quote.snapshot_bytes) &&
        checkedAdd(
               quote.peak_live_bytes,
               quote.validation_temporary_bytes);
}

template <typename T>
bool cloneVectorExact(
    const std::vector<T>& source,
    std::vector<T>& destination) {
    destination.reserve(source.size());
    if (destination.capacity() != source.size()) return false;
    destination.insert(
        destination.end(), source.begin(), source.end());
    return destination.capacity() == destination.size();
}

bool cloneNestedEdgesExact(
    const std::vector<std::vector<Edge>>& source,
    std::vector<std::vector<Edge>>& destination) {
    destination.reserve(source.size());
    if (destination.capacity() != source.size()) return false;
    destination.resize(source.size());
    for (std::size_t vertex = 0; vertex < source.size(); ++vertex) {
        if (!cloneVectorExact(
                source[vertex], destination[vertex])) {
            return false;
        }
    }
    return true;
}

bool cloneTemporalEdgesExact(
    const std::vector<TemporalEdge>& source,
    std::vector<TemporalEdge>& destination) {
    destination.reserve(source.size());
    if (destination.capacity() != source.size()) return false;
    for (const TemporalEdge& edge : source) {
        std::vector<TimeInterval> intervals;
        if (!cloneVectorExact(edge.activeIntervals(), intervals)) {
            return false;
        }
        destination.emplace_back(
            edge.u, edge.v, std::move(intervals),
            edge.active_snapshot_count);
    }
    return destination.capacity() == destination.size();
}

bool cloneGraphExact(
    const Graph& source,
    Graph& destination,
    bool rebuild_data_metadata) {
    destination.num_vertices = source.num_vertices;
    destination.input_occurrence_count = source.input_occurrence_count;
    destination.input_unique_edge_count = source.input_unique_edge_count;
    destination.filtered_edge_count = source.filtered_edge_count;
    if (!cloneNestedEdgesExact(source.adj, destination.adj) ||
        !cloneNestedEdgesExact(source.in_adj, destination.in_adj) ||
        !cloneVectorExact(
            source.vertex_labels, destination.vertex_labels) ||
        !cloneVectorExact(
            source.external_ids, destination.external_ids) ||
        !cloneTemporalEdgesExact(
            source.temporal_edges, destination.temporal_edges)) {
        return false;
    }
    const std::size_t derived_count = rebuild_data_metadata &&
            source.num_vertices >= 0
        ? static_cast<std::size_t>(source.num_vertices)
        : source.vertex_active_durations.size();
    const std::size_t duration_count = std::max(
        source.vertex_active_durations.size(), derived_count);
    const std::size_t outgoing_count = std::max(
        source.neighbor_label_counts.size(), derived_count);
    const std::size_t incoming_count = std::max(
        source.incoming_neighbor_label_counts.size(), derived_count);
    destination.vertex_active_durations.reserve(duration_count);
    destination.neighbor_label_counts.reserve(outgoing_count);
    destination.incoming_neighbor_label_counts.reserve(incoming_count);
    if (destination.vertex_active_durations.capacity() != duration_count ||
        destination.neighbor_label_counts.capacity() != outgoing_count ||
        destination.incoming_neighbor_label_counts.capacity() !=
            incoming_count) {
        return false;
    }
    if (!rebuild_data_metadata) {
        destination.vertex_active_durations.insert(
            destination.vertex_active_durations.end(),
            source.vertex_active_durations.begin(),
            source.vertex_active_durations.end());
        destination.neighbor_label_counts.insert(
            destination.neighbor_label_counts.end(),
            source.neighbor_label_counts.begin(),
            source.neighbor_label_counts.end());
        destination.incoming_neighbor_label_counts.insert(
            destination.incoming_neighbor_label_counts.end(),
            source.incoming_neighbor_label_counts.begin(),
            source.incoming_neighbor_label_counts.end());
    }
    return true;
}

bool incrementWithin(
    std::uint64_t& counter,
    std::uint64_t cap,
    std::uint64_t increment = 1) noexcept {
    return checkedAdd(counter, increment) && counter <= cap;
}

bool incrementWithinCombined(
    std::uint64_t& counter,
    std::uint64_t already_charged,
    std::uint64_t cap) noexcept {
    if (!checkedAdd(counter, 1)) return false;
    std::uint64_t combined = already_charged;
    return checkedAdd(combined, counter) && combined <= cap;
}

bool strictlySortedUnique(const std::vector<Edge>& edges) noexcept {
    for (std::size_t index = 1; index < edges.size(); ++index) {
        if (edges[index - 1].to >= edges[index].to) return false;
    }
    return true;
}

int unionDuration(std::vector<TimeInterval>& intervals) noexcept {
    if (intervals.empty()) return 0;
    std::sort(
        intervals.begin(), intervals.end(),
        [](const TimeInterval& lhs, const TimeInterval& rhs) {
            if (lhs.start != rhs.start) return lhs.start < rhs.start;
            return lhs.end < rhs.end;
        });
    std::int64_t total = 0;
    int current_start = intervals.front().start;
    int current_end = intervals.front().end;
    for (std::size_t index = 1;
         index < intervals.size(); ++index) {
        const TimeInterval& interval = intervals[index];
        if (interval.start <= current_end ||
            (current_end != std::numeric_limits<int>::max() &&
             interval.start == current_end + 1)) {
            current_end = std::max(current_end, interval.end);
        } else {
            const std::int64_t span =
                static_cast<std::int64_t>(current_end) -
                static_cast<std::int64_t>(current_start) + 1;
            if (span >= std::numeric_limits<int>::max() ||
                total >= std::numeric_limits<int>::max() - span) {
                return std::numeric_limits<int>::max();
            }
            total += span;
            current_start = interval.start;
            current_end = interval.end;
        }
    }
    const std::int64_t final_span =
        static_cast<std::int64_t>(current_end) -
        static_cast<std::int64_t>(current_start) + 1;
    if (final_span >= std::numeric_limits<int>::max() ||
        total >= std::numeric_limits<int>::max() - final_span) {
        return std::numeric_limits<int>::max();
    }
    total += final_span;
    return total > std::numeric_limits<int>::max()
        ? std::numeric_limits<int>::max()
        : static_cast<int>(total);
}

bool acquireSnapshotToken(std::uint64_t& token) noexcept {
    static std::mutex token_mutex;
    static std::uint64_t next_token = 1;
    const std::lock_guard<std::mutex> lock(token_mutex);
    if (next_token == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    token = next_token;
    ++next_token;
    return true;
}

GraphValidationStatus validateAndFingerprint(
    Graph& data,
    const Graph& query,
    const GraphValidationBudget& budget,
    GraphValidationReceipt& receipt,
    std::uint64_t maximum_incident_intervals) {
    receipt.graph_validation_calls = 1;

    if (data.num_vertices < 0) {
        return GraphValidationStatus::InvalidDataGraph;
    }
    const std::size_t data_vertex_count =
        static_cast<std::size_t>(data.num_vertices);
    if (data.external_ids.size() != data_vertex_count ||
        data.vertex_labels.size() != data_vertex_count ||
        data.adj.size() != data_vertex_count ||
        data.in_adj.size() != data_vertex_count) {
        return GraphValidationStatus::InvalidDataGraph;
    }

    std::uint64_t expected_temporary_bytes = 0;
    if (!addCapacityBytes(
            expected_temporary_bytes,
            data.external_ids.size(), sizeof(int)) ||
        !addCapacityBytes(
            expected_temporary_bytes,
            data.temporal_edges.size(), sizeof(std::uint8_t)) ||
        !addCapacityBytes(
            expected_temporary_bytes,
            data.temporal_edges.size(), sizeof(std::uint8_t))) {
        return GraphValidationStatus::ArithmeticOverflow;
    }
    if (expected_temporary_bytes >
        budget.temporary_validation_bytes) {
        return GraphValidationStatus::BudgetExceeded;
    }

    std::vector<int> external_ids;
    std::vector<std::uint8_t> outgoing_references;
    std::vector<std::uint8_t> incoming_references;
    external_ids.reserve(data.external_ids.size());
    external_ids.insert(
        external_ids.end(),
        data.external_ids.begin(), data.external_ids.end());
    outgoing_references.assign(data.temporal_edges.size(), 0);
    incoming_references.assign(data.temporal_edges.size(), 0);
    std::uint64_t temporary_capacity = 0;
    if (!addCapacityBytes(
            temporary_capacity, external_ids.capacity(), sizeof(int)) ||
        !addCapacityBytes(
            temporary_capacity,
            outgoing_references.capacity(), sizeof(std::uint8_t)) ||
        !addCapacityBytes(
            temporary_capacity,
            incoming_references.capacity(), sizeof(std::uint8_t))) {
        return GraphValidationStatus::ArithmeticOverflow;
    }
    receipt.temporary_validation_capacity_bytes = temporary_capacity;
    if (temporary_capacity > budget.temporary_validation_bytes) {
        return GraphValidationStatus::BudgetExceeded;
    }
    std::sort(external_ids.begin(), external_ids.end());
    if (std::adjacent_find(external_ids.begin(), external_ids.end()) !=
        external_ids.end()) {
        return GraphValidationStatus::InvalidDataGraph;
    }
    if (data.external_ids != external_ids ||
        std::any_of(
            data.external_ids.begin(), data.external_ids.end(),
            [](int external_id) { return external_id < 0; })) {
        return GraphValidationStatus::InvalidDataGraph;
    }

    std::uint64_t data_hash = kFnvOffset;
    appendU64(data_hash, UINT64_C(0x44534d2d47524150));
    appendU64(
        data_hash,
        static_cast<std::uint64_t>(data.external_ids.size()));
    for (std::size_t vertex = 0; vertex < data_vertex_count; ++vertex) {
        if (!incrementWithin(
                receipt.data_vertices_visited,
                budget.data_vertices)) {
            return GraphValidationStatus::BudgetExceeded;
        }
        if (data.vertex_labels[vertex] >= kLabelCount) {
            return GraphValidationStatus::InvalidDataGraph;
        }
        appendU64(
            data_hash,
            static_cast<std::uint32_t>(data.external_ids[vertex]));
        appendU64(
            data_hash,
            static_cast<std::uint64_t>(data.vertex_labels[vertex]));
    }
    appendU64(
        data_hash,
        static_cast<std::uint64_t>(data.temporal_edges.size()));
    for (std::size_t edge_id = 0;
         edge_id < data.temporal_edges.size(); ++edge_id) {
        if (!incrementWithin(
                receipt.temporal_edges_visited,
                budget.temporal_edges)) {
            return GraphValidationStatus::BudgetExceeded;
        }
        const TemporalEdge& edge = data.temporal_edges[edge_id];
        if (edge.u < 0 || edge.u >= data.num_vertices ||
            edge.v < 0 || edge.v >= data.num_vertices ||
            edge.active_snapshot_count < 0 ||
            !edge.hasCachedMaximumConsecutiveRun()) {
            return GraphValidationStatus::InvalidDataGraph;
        }
        appendU64(
            data_hash,
            static_cast<std::uint32_t>(
                data.external_ids[static_cast<std::size_t>(edge.u)]));
        appendU64(
            data_hash,
            static_cast<std::uint32_t>(
                data.external_ids[static_cast<std::size_t>(edge.v)]));
        appendU64(
            data_hash,
            static_cast<std::uint64_t>(edge.active_snapshot_count));
        appendU64(
            data_hash,
            static_cast<std::uint64_t>(edge.activeIntervals().size()));

        std::int64_t exact_count = 0;
        int maximum_run = 0;
        int previous_end = 0;
        bool have_previous = false;
        for (const TimeInterval& interval : edge.activeIntervals()) {
            if (!incrementWithin(
                    receipt.interval_records_visited,
                    budget.interval_records)) {
                return GraphValidationStatus::BudgetExceeded;
            }
            if (interval.end < interval.start ||
                (have_previous &&
                 (interval.start <= previous_end ||
                  (previous_end != std::numeric_limits<int>::max() &&
                   interval.start == previous_end + 1)))) {
                return GraphValidationStatus::InvalidDataGraph;
            }
            const std::int64_t span =
                static_cast<std::int64_t>(interval.end) -
                static_cast<std::int64_t>(interval.start) + 1;
            if (span > std::numeric_limits<std::int64_t>::max() -
                           exact_count) {
                return GraphValidationStatus::ArithmeticOverflow;
            }
            exact_count += span;
            maximum_run = std::max(maximum_run, interval.length());
            previous_end = interval.end;
            have_previous = true;
            appendU64(
                data_hash,
                static_cast<std::uint32_t>(interval.start));
            appendU64(
                data_hash,
                static_cast<std::uint32_t>(interval.end));
        }
        if (exact_count != edge.active_snapshot_count ||
            maximum_run != edge.maximumConsecutiveRun()) {
            return GraphValidationStatus::InvalidDataGraph;
        }
    }

    for (int vertex = 0; vertex < data.num_vertices; ++vertex) {
        const std::size_t vertex_index =
            static_cast<std::size_t>(vertex);
        const auto& outgoing = data.adj[vertex_index];
        const auto& incoming = data.in_adj[vertex_index];
        if (!strictlySortedUnique(outgoing) ||
            !strictlySortedUnique(incoming)) {
            return GraphValidationStatus::InvalidDataGraph;
        }
        for (const Edge& edge : outgoing) {
            if (!incrementWithin(
                    receipt.data_outgoing_adjacency_entries_visited,
                    budget.combined_outgoing_adjacency_entries)) {
                return GraphValidationStatus::BudgetExceeded;
            }
            if (edge.to < 0 || edge.to >= data.num_vertices ||
                edge.temporal_edge_id < 0 ||
                static_cast<std::size_t>(edge.temporal_edge_id) >=
                    data.temporal_edges.size()) {
                return GraphValidationStatus::InvalidDataGraph;
            }
            const TemporalEdge& temporal_edge =
                data.temporal_edges[
                    static_cast<std::size_t>(edge.temporal_edge_id)];
            if (temporal_edge.u != vertex ||
                temporal_edge.v != edge.to) {
                return GraphValidationStatus::InvalidDataGraph;
            }
            auto& reference_count = outgoing_references[
                static_cast<std::size_t>(edge.temporal_edge_id)];
            if (reference_count ==
                std::numeric_limits<std::uint8_t>::max()) {
                return GraphValidationStatus::InvalidDataGraph;
            }
            ++reference_count;
            const auto& reciprocal =
                data.in_adj[static_cast<std::size_t>(edge.to)];
            const auto found = std::lower_bound(
                reciprocal.begin(), reciprocal.end(), vertex,
                [](const Edge& candidate, int target) {
                    return candidate.to < target;
                });
            if (found == reciprocal.end() || found->to != vertex ||
                found->temporal_edge_id != edge.temporal_edge_id) {
                return GraphValidationStatus::InvalidDataGraph;
            }
        }
        for (const Edge& edge : incoming) {
            if (!incrementWithin(
                    receipt.data_incoming_adjacency_entries_visited,
                    budget.combined_incoming_adjacency_entries)) {
                return GraphValidationStatus::BudgetExceeded;
            }
            if (edge.to < 0 || edge.to >= data.num_vertices ||
                edge.temporal_edge_id < 0 ||
                static_cast<std::size_t>(edge.temporal_edge_id) >=
                    data.temporal_edges.size()) {
                return GraphValidationStatus::InvalidDataGraph;
            }
            const TemporalEdge& temporal_edge =
                data.temporal_edges[
                    static_cast<std::size_t>(edge.temporal_edge_id)];
            if (temporal_edge.u != edge.to ||
                temporal_edge.v != vertex) {
                return GraphValidationStatus::InvalidDataGraph;
            }
            auto& reference_count = incoming_references[
                static_cast<std::size_t>(edge.temporal_edge_id)];
            if (reference_count ==
                std::numeric_limits<std::uint8_t>::max()) {
                return GraphValidationStatus::InvalidDataGraph;
            }
            ++reference_count;
            const auto& reciprocal =
                data.adj[static_cast<std::size_t>(edge.to)];
            const auto found = std::lower_bound(
                reciprocal.begin(), reciprocal.end(), vertex,
                [](const Edge& candidate, int target) {
                    return candidate.to < target;
                });
            if (found == reciprocal.end() || found->to != vertex ||
                found->temporal_edge_id != edge.temporal_edge_id) {
                return GraphValidationStatus::InvalidDataGraph;
            }
        }
    }
    for (std::size_t edge_id = 0;
         edge_id < data.temporal_edges.size(); ++edge_id) {
        if (outgoing_references[edge_id] != 1 ||
            incoming_references[edge_id] != 1) {
            return GraphValidationStatus::InvalidDataGraph;
        }
    }

    // Incumbent construction consumes these denormalized arrays directly.
    // Rebuild them inside the exclusively-owned pre-publication snapshot so
    // malformed caller metadata cannot become V3 authority.
    data.vertex_active_durations.assign(data_vertex_count, 0);
    data.neighbor_label_counts.assign(data_vertex_count, {});
    data.incoming_neighbor_label_counts.assign(data_vertex_count, {});
    std::vector<TimeInterval> incident_intervals;
    const std::uint64_t base_temporary_capacity_bytes =
        receipt.temporary_validation_capacity_bytes;
    if (maximum_incident_intervals >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max())) {
        return GraphValidationStatus::ArithmeticOverflow;
    }
    incident_intervals.reserve(
        static_cast<std::size_t>(maximum_incident_intervals));
    if (incident_intervals.capacity() !=
        static_cast<std::size_t>(maximum_incident_intervals)) {
        return GraphValidationStatus::AllocationModelViolation;
    }
    std::uint64_t interval_capacity_bytes = 0;
    if (!checkedMultiply(
            incident_intervals.capacity(),
            sizeof(TimeInterval), interval_capacity_bytes)) {
        return GraphValidationStatus::ArithmeticOverflow;
    }
    std::uint64_t combined_temporary =
        base_temporary_capacity_bytes;
    if (!checkedAdd(combined_temporary, interval_capacity_bytes)) {
        return GraphValidationStatus::ArithmeticOverflow;
    }
    receipt.temporary_validation_capacity_bytes = combined_temporary;
    if (combined_temporary > budget.temporary_validation_bytes) {
        return GraphValidationStatus::BudgetExceeded;
    }
    for (std::size_t vertex = 0; vertex < data_vertex_count; ++vertex) {
        incident_intervals.clear();
        for (const Edge& edge : data.adj[vertex]) {
            const TemporalEdge& temporal_edge = data.temporal_edges[
                static_cast<std::size_t>(edge.temporal_edge_id)];
            const auto& intervals = temporal_edge.activeIntervals();
            incident_intervals.insert(
                incident_intervals.end(),
                intervals.begin(), intervals.end());
            if (!checkedAdd(
                    receipt.derived_incident_interval_visits,
                    static_cast<std::uint64_t>(intervals.size()))) {
                return GraphValidationStatus::ArithmeticOverflow;
            }
            if (receipt.derived_incident_interval_visits >
                budget.derived_incident_interval_visits) {
                return GraphValidationStatus::BudgetExceeded;
            }
            const Label neighbor_label =
                data.vertex_labels[static_cast<std::size_t>(edge.to)];
            int& count = data.neighbor_label_counts[vertex][neighbor_label];
            if (count == std::numeric_limits<int>::max() ||
                !checkedAdd(
                    receipt.derived_neighbor_label_updates, 1)) {
                return GraphValidationStatus::ArithmeticOverflow;
            }
            if (receipt.derived_neighbor_label_updates >
                budget.derived_neighbor_label_updates) {
                return GraphValidationStatus::BudgetExceeded;
            }
            ++count;
        }
        for (const Edge& edge : data.in_adj[vertex]) {
            const TemporalEdge& temporal_edge = data.temporal_edges[
                static_cast<std::size_t>(edge.temporal_edge_id)];
            const auto& intervals = temporal_edge.activeIntervals();
            incident_intervals.insert(
                incident_intervals.end(),
                intervals.begin(), intervals.end());
            if (!checkedAdd(
                    receipt.derived_incident_interval_visits,
                    static_cast<std::uint64_t>(intervals.size()))) {
                return GraphValidationStatus::ArithmeticOverflow;
            }
            if (receipt.derived_incident_interval_visits >
                budget.derived_incident_interval_visits) {
                return GraphValidationStatus::BudgetExceeded;
            }
            const Label neighbor_label =
                data.vertex_labels[static_cast<std::size_t>(edge.to)];
            int& count =
                data.incoming_neighbor_label_counts[vertex]
                                                   [neighbor_label];
            if (count == std::numeric_limits<int>::max() ||
                !checkedAdd(
                    receipt.derived_neighbor_label_updates, 1)) {
                return GraphValidationStatus::ArithmeticOverflow;
            }
            if (receipt.derived_neighbor_label_updates >
                budget.derived_neighbor_label_updates) {
                return GraphValidationStatus::BudgetExceeded;
            }
            ++count;
        }
        data.vertex_active_durations[vertex] =
            unionDuration(incident_intervals);
    }
    receipt.derived_metadata_rebuild_passes = 1;
    std::uint64_t storage_bytes = sizeof(ValidatedGraphSnapshot);
    if (!checkedAdd(storage_bytes, kSnapshotOwnerControlBytes) ||
        !checkedAdd(receipt.storage_accounting_passes, 1) ||
        !addGraphStorageBytes(data, storage_bytes, &receipt) ||
        !addGraphStorageBytes(query, storage_bytes, &receipt)) {
        return GraphValidationStatus::ArithmeticOverflow;
    }
    receipt.snapshot_storage_bytes = storage_bytes;
    if (storage_bytes > budget.snapshot_storage_bytes) {
        return GraphValidationStatus::BudgetExceeded;
    }
    receipt.data_fingerprint = data_hash;
    receipt.data_fingerprint_passes = 1;

    if (query.num_vertices <= 0) {
        return GraphValidationStatus::InvalidQueryGraph;
    }
    const std::size_t query_vertex_count =
        static_cast<std::size_t>(query.num_vertices);
    if (query.vertex_labels.size() != query_vertex_count ||
        query.adj.size() != query_vertex_count ||
        query.in_adj.size() != query_vertex_count) {
        return GraphValidationStatus::InvalidQueryGraph;
    }

    std::uint64_t query_arc_count = 0;
    for (std::size_t vertex = 0;
         vertex < query_vertex_count; ++vertex) {
        if (!incrementWithin(
                receipt.query_vertices_visited,
                budget.query_vertices)) {
            return GraphValidationStatus::BudgetExceeded;
        }
        if (query.vertex_labels[vertex] >= kLabelCount ||
            !strictlySortedUnique(query.adj[vertex]) ||
            !strictlySortedUnique(query.in_adj[vertex]) ||
            !checkedAdd(
                query_arc_count,
                static_cast<std::uint64_t>(query.adj[vertex].size()))) {
            return query.vertex_labels[vertex] >= kLabelCount ||
                    !strictlySortedUnique(query.adj[vertex]) ||
                    !strictlySortedUnique(query.in_adj[vertex])
                ? GraphValidationStatus::InvalidQueryGraph
                : GraphValidationStatus::ArithmeticOverflow;
        }
    }

    std::uint64_t query_hash = kFnvOffset;
    appendU64(query_hash, UINT64_C(0x51554552592d5633));
    appendInt(query_hash, query.num_vertices);
    appendU64(query_hash, static_cast<std::uint64_t>(query.num_vertices));
    for (int vertex = 0; vertex < query.num_vertices; ++vertex) {
        appendInt(query_hash, vertex);
        appendU64(
            query_hash,
            static_cast<std::uint64_t>(
                query.vertex_labels[static_cast<std::size_t>(vertex)]));
    }
    appendU64(query_hash, query_arc_count);
    for (int vertex = 0; vertex < query.num_vertices; ++vertex) {
        const auto& outgoing =
            query.adj[static_cast<std::size_t>(vertex)];
        const auto& incoming =
            query.in_adj[static_cast<std::size_t>(vertex)];
        for (const Edge& edge : outgoing) {
            if (!incrementWithinCombined(
                    receipt.query_outgoing_adjacency_entries_visited,
                    receipt.data_outgoing_adjacency_entries_visited,
                    budget.combined_outgoing_adjacency_entries)) {
                return GraphValidationStatus::BudgetExceeded;
            }
            if (edge.to < 0 || edge.to >= query.num_vertices ||
                !GraphUtils::hasEdge(query.in_adj, edge.to, vertex)) {
                return GraphValidationStatus::InvalidQueryGraph;
            }
            appendInt(query_hash, vertex);
            appendInt(query_hash, edge.to);
        }
        for (const Edge& edge : incoming) {
            if (!incrementWithinCombined(
                    receipt.query_incoming_adjacency_entries_visited,
                    receipt.data_incoming_adjacency_entries_visited,
                    budget.combined_incoming_adjacency_entries)) {
                return GraphValidationStatus::BudgetExceeded;
            }
            if (edge.to < 0 || edge.to >= query.num_vertices ||
                !GraphUtils::hasEdge(query.adj, edge.to, vertex)) {
                return GraphValidationStatus::InvalidQueryGraph;
            }
        }
    }
    receipt.query_fingerprint = query_hash;
    receipt.query_fingerprint_passes = 1;
    return GraphValidationStatus::Complete;
}

} // namespace

bool GraphValidationAccountingModelArtifactV1::complete() const noexcept {
    return !canonical_bytes.empty() && sha256.size() == 64U;
}

GraphValidationAccountingModelArtifactV1
graphValidationAccountingModelArtifactV1() {
    GraphValidationAccountingModelArtifactV1 result;
    result.canonical_bytes =
        dsm::v3::canonicalJsonBytes(graphAccountingModelJson());
    result.sha256 = dsm::v3::sha256Hex(result.canonical_bytes);
    return result;
}

std::string canonicalGraphValidationQuoteEvidenceCoreV1(
    const GraphValidationQuoteEvidenceV1& evidence) {
    return dsm::v3::canonicalJsonBytes(
        graphQuoteEvidenceCoreJson(evidence));
}

std::string graphValidationQuoteEvidenceSha256V1(
    const GraphValidationQuoteEvidenceV1& evidence) {
    return dsm::v3::sha256Hex(
        canonicalGraphValidationQuoteEvidenceCoreV1(evidence));
}

GraphValidationQuoteEvidenceV1 quoteGraphValidationEvidenceV1(
    const Graph& data,
    const Graph& query) {
    return makeGraphQuoteEvidence(
        quoteGraphValidation(data, query));
}

#ifdef DSM_TCC_V3_TESTING
bool graphValidationCheckedStorageAddForTesting(
    std::uint64_t initial_bytes,
    std::uint64_t count,
    std::uint64_t width,
    std::uint64_t& result_bytes) noexcept {
    if (count != 0 &&
        width > std::numeric_limits<std::uint64_t>::max() / count) {
        return false;
    }
    const std::uint64_t product = count * width;
    if (product >
        std::numeric_limits<std::uint64_t>::max() - initial_bytes) {
        return false;
    }
    result_bytes = initial_bytes + product;
    return true;
}
#endif

GraphValidationBudget GraphValidationBudget::unbounded() noexcept {
    const std::uint64_t maximum =
        std::numeric_limits<std::uint64_t>::max();
    GraphValidationBudget budget;
    budget.data_vertices = maximum;
    budget.query_vertices = maximum;
    budget.combined_outgoing_adjacency_entries = maximum;
    budget.combined_incoming_adjacency_entries = maximum;
    budget.temporal_edges = maximum;
    budget.interval_records = maximum;
    budget.derived_incident_interval_visits = maximum;
    budget.derived_neighbor_label_updates = maximum;
    budget.snapshot_storage_bytes = maximum;
    budget.temporary_validation_bytes = maximum;
    budget.seal_peak_live_bytes = maximum;
    return budget;
}

const char* graphValidationStatusName(
    GraphValidationStatus status) noexcept {
    switch (status) {
        case GraphValidationStatus::Complete:
            return "complete";
        case GraphValidationStatus::InvalidDataGraph:
            return "invalid-data-graph";
        case GraphValidationStatus::InvalidQueryGraph:
            return "invalid-query-graph";
        case GraphValidationStatus::BudgetExceeded:
            return "budget-exceeded";
        case GraphValidationStatus::ArithmeticOverflow:
            return "arithmetic-overflow";
        case GraphValidationStatus::AllocationFailure:
            return "allocation-failure";
        case GraphValidationStatus::AllocationModelViolation:
            return "allocation-model-violation";
        case GraphValidationStatus::SnapshotTokenExhausted:
            return "snapshot-token-exhausted";
        case GraphValidationStatus::InvalidExpectedQuote:
            return "invalid-expected-quote";
        case GraphValidationStatus::GraphAccountingModelMismatch:
            return "graph-accounting-model-mismatch";
        case GraphValidationStatus::ExpectedQuoteMismatch:
            return "expected-quote-mismatch";
        case GraphValidationStatus::QuoteEvidenceFailure:
            return "quote-evidence-failure";
    }
    return "unknown";
}

bool GraphValidationReceipt::complete() const noexcept {
    return status == GraphValidationStatus::Complete &&
        snapshot_token != 0 && graph_validation_calls == 1 &&
        data_fingerprint_passes == 1 &&
        query_fingerprint_passes == 1 &&
        derived_metadata_rebuild_passes == 1 &&
        (!production_expected_quote_binding ||
         (expected_quote_reverification_passes == 1 &&
          expected_quote_exact_equality &&
          isLowercaseSha256(graph_accounting_model_sha256) &&
          isLowercaseSha256(expected_quote_sha256) &&
          expected_quote_sha256 == observed_quote_sha256));
}

bool GraphSnapshotSealResult::complete() const noexcept {
    return snapshot != nullptr && receipt.complete();
}

const char* graphValidationQuoteStatusName(
    GraphValidationQuoteStatus status) noexcept {
    switch (status) {
        case GraphValidationQuoteStatus::Complete:
            return "complete";
        case GraphValidationQuoteStatus::InvalidDataGraph:
            return "invalid-data-graph";
        case GraphValidationQuoteStatus::InvalidQueryGraph:
            return "invalid-query-graph";
        case GraphValidationQuoteStatus::ArithmeticOverflow:
            return "arithmetic-overflow";
    }
    return "unknown";
}

GraphValidationQuote quoteGraphValidation(
    const Graph& data,
    const Graph& query) noexcept {
    GraphValidationQuote result;
    GraphValidationReceipt receipt;
    SealStorageQuote storage;
    if (data.num_vertices < 0 ||
        static_cast<std::uint64_t>(data.num_vertices) !=
            static_cast<std::uint64_t>(data.adj.size()) ||
        static_cast<std::uint64_t>(data.num_vertices) !=
            static_cast<std::uint64_t>(data.in_adj.size())) {
        result.status = GraphValidationQuoteStatus::InvalidDataGraph;
        return result;
    }
    if (query.num_vertices <= 0 || !query.temporal_edges.empty() ||
        static_cast<std::uint64_t>(query.num_vertices) !=
            static_cast<std::uint64_t>(query.adj.size()) ||
        static_cast<std::uint64_t>(query.num_vertices) !=
            static_cast<std::uint64_t>(query.in_adj.size())) {
        result.status = GraphValidationQuoteStatus::InvalidQueryGraph;
        return result;
    }
    if (!quoteSealStorage(data, query, receipt, storage)) {
        result.overflowed = true;
        result.status = GraphValidationQuoteStatus::ArithmeticOverflow;
        return result;
    }

    GraphValidationBudget budget;
    budget.data_vertices = static_cast<std::uint64_t>(data.num_vertices);
    budget.query_vertices = static_cast<std::uint64_t>(query.num_vertices);
    budget.combined_outgoing_adjacency_entries =
        receipt.storage_quote_outgoing_adjacency_entries_visited;
    budget.combined_incoming_adjacency_entries =
        receipt.storage_quote_incoming_adjacency_entries_visited;
    budget.temporal_edges =
        static_cast<std::uint64_t>(data.temporal_edges.size());
    for (const TemporalEdge& edge : data.temporal_edges) {
        if (!checkedAdd(
                budget.interval_records,
                static_cast<std::uint64_t>(
                    edge.activeIntervals().size()))) {
            result.overflowed = true;
            result.status = GraphValidationQuoteStatus::ArithmeticOverflow;
            return result;
        }
    }
    if (budget.interval_records >
            std::numeric_limits<std::uint64_t>::max() / 2U) {
        result.overflowed = true;
        result.status = GraphValidationQuoteStatus::ArithmeticOverflow;
        return result;
    }
    budget.derived_incident_interval_visits =
        budget.interval_records * 2U;
    if (
        !checkedAdd(
            budget.derived_neighbor_label_updates,
            receipt.storage_quote_incident_outgoing_entries_visited) ||
        !checkedAdd(
            budget.derived_neighbor_label_updates,
            receipt.storage_quote_incident_incoming_entries_visited)) {
        result.overflowed = true;
        result.status = GraphValidationQuoteStatus::ArithmeticOverflow;
        return result;
    }
    budget.snapshot_storage_bytes = storage.snapshot_bytes;
    budget.temporary_validation_bytes = storage.validation_temporary_bytes;
    budget.seal_peak_live_bytes = storage.peak_live_bytes;

    result.exact_budget = budget;
    result.source_live_storage_bytes = storage.source_live_bytes;
    result.snapshot_owner_control_bytes = kSnapshotOwnerControlBytes;
    result.maximum_incident_intervals = storage.maximum_incident_intervals;
    result.storage_quote_graphs_visited =
        receipt.storage_quote_graphs_visited;
    result.storage_quote_vertices_visited =
        receipt.storage_quote_vertices_visited;
    result.storage_quote_outgoing_adjacency_entries_visited =
        receipt.storage_quote_outgoing_adjacency_entries_visited;
    result.storage_quote_incoming_adjacency_entries_visited =
        receipt.storage_quote_incoming_adjacency_entries_visited;
    result.storage_quote_incident_outgoing_entries_visited =
        receipt.storage_quote_incident_outgoing_entries_visited;
    result.storage_quote_incident_incoming_entries_visited =
        receipt.storage_quote_incident_incoming_entries_visited;
    result.storage_quote_temporal_edges_visited =
        receipt.storage_quote_temporal_edges_visited;
    result.storage_quote_interval_records_visited =
        receipt.storage_quote_interval_records_visited;
    result.storage_quote_passes = receipt.storage_quote_passes;
    result.storage_accounting_passes =
        receipt.storage_accounting_passes;
    result.storage_accounting_graphs_visited =
        receipt.storage_accounting_graphs_visited;
    result.storage_accounting_vertices_visited =
        receipt.storage_accounting_vertices_visited;
    result.storage_accounting_temporal_edges_visited =
        receipt.storage_accounting_temporal_edges_visited;
    result.storage_accounting_interval_records_visited =
        receipt.storage_accounting_interval_records_visited;
    result.complete = true;
    result.status = GraphValidationQuoteStatus::Complete;
    return result;
}

GraphSnapshotSealResult ValidatedGraphSnapshot::seal(
    const Graph& data,
    const Graph& query,
    const GraphValidationBudget& budget) noexcept {
    const GraphValidationQuote observed_quote =
        quoteGraphValidation(data, query);
    GraphSnapshotSealResult result;
    copyQuoteCountersToReceipt(observed_quote, result.receipt);
    if (!observed_quote.complete) {
        result.receipt.status =
            validationStatusForQuoteStatus(observed_quote.status);
        return result;
    }
    return sealRequoted(
        data, query, budget, observed_quote,
        std::move(result.receipt));
}

GraphSnapshotSealResult ValidatedGraphSnapshot::seal(
    const Graph& data,
    const Graph& query,
    const GraphValidationQuoteEvidenceV1& expected_quote) noexcept {
    GraphSnapshotSealResult result;
    result.receipt.production_expected_quote_binding = true;
    try {
        const GraphValidationAccountingModelArtifactV1 model =
            graphValidationAccountingModelArtifactV1();
        result.receipt.graph_accounting_model_sha256 = model.sha256;
        result.receipt.expected_quote_sha256 =
            expected_quote.quote_sha256;
        if (expected_quote.schema != kGraphQuoteEvidenceSchema ||
            expected_quote.schema_version !=
                kGraphQuoteEvidenceVersion ||
            !expected_quote.complete || expected_quote.overflowed ||
            expected_quote.status !=
                GraphValidationQuoteStatus::Complete ||
            !isLowercaseSha256(
                expected_quote.graph_accounting_model_sha256) ||
            !isLowercaseSha256(expected_quote.quote_sha256) ||
            expected_quote.quote_sha256 !=
                graphValidationQuoteEvidenceSha256V1(
                    expected_quote)) {
            result.receipt.status =
                GraphValidationStatus::InvalidExpectedQuote;
            return result;
        }
        if (!model.complete() ||
            expected_quote.graph_accounting_model_sha256 !=
                model.sha256) {
            result.receipt.status =
                GraphValidationStatus::GraphAccountingModelMismatch;
            return result;
        }

        // This is the seal-verification scan.  The expected evidence was
        // created by a distinct caller-owned pre-materialization scan.
        const GraphValidationQuote observed_native_quote =
            quoteGraphValidation(data, query);
        copyQuoteCountersToReceipt(
            observed_native_quote, result.receipt);
        result.receipt.expected_quote_reverification_passes = 1;
        if (!observed_native_quote.complete) {
            result.receipt.status = validationStatusForQuoteStatus(
                observed_native_quote.status);
            return result;
        }
        const GraphValidationQuoteEvidenceV1 observed_quote =
            makeGraphQuoteEvidence(observed_native_quote);
        result.receipt.observed_quote_sha256 =
            observed_quote.quote_sha256;
        if (!graphQuoteEvidenceCoreEqual(
                expected_quote, observed_quote) ||
            expected_quote.quote_sha256 !=
                observed_quote.quote_sha256) {
            result.receipt.status =
                GraphValidationStatus::ExpectedQuoteMismatch;
            return result;
        }
        result.receipt.expected_quote_exact_equality = true;
        return sealRequoted(
            data, query, expected_quote.exact_budget,
            observed_native_quote, std::move(result.receipt));
    } catch (const std::bad_alloc&) {
        result.receipt.status =
            GraphValidationStatus::AllocationFailure;
    } catch (...) {
        result.receipt.status =
            GraphValidationStatus::QuoteEvidenceFailure;
    }
    return result;
}

GraphSnapshotSealResult ValidatedGraphSnapshot::sealRequoted(
    const Graph& data,
    const Graph& query,
    const GraphValidationBudget& budget,
    const GraphValidationQuote& observed_quote,
    GraphValidationReceipt receipt) noexcept {
    GraphSnapshotSealResult result;
    result.receipt = std::move(receipt);
    const auto started = std::chrono::steady_clock::now();
    if (!observed_quote.complete) {
        result.receipt.status = validationStatusForQuoteStatus(
            observed_quote.status);
    } else {
        result.receipt.source_live_storage_bytes =
            observed_quote.source_live_storage_bytes;
        result.receipt.quoted_snapshot_storage_bytes =
            observed_quote.exact_budget.snapshot_storage_bytes;
        result.receipt.snapshot_owner_control_bytes =
            observed_quote.snapshot_owner_control_bytes;
        result.receipt.quoted_validation_temporary_bytes =
            observed_quote.exact_budget.temporary_validation_bytes;
        result.receipt.quoted_maximum_incident_intervals =
            observed_quote.maximum_incident_intervals;
        result.receipt.quoted_seal_peak_live_bytes =
            observed_quote.exact_budget.seal_peak_live_bytes;
        if (observed_quote.exact_budget.snapshot_storage_bytes >
                budget.snapshot_storage_bytes ||
            observed_quote.exact_budget.temporary_validation_bytes >
                budget.temporary_validation_bytes ||
            observed_quote.exact_budget.seal_peak_live_bytes >
                budget.seal_peak_live_bytes) {
            result.receipt.status =
                GraphValidationStatus::BudgetExceeded;
        } else {
            result.receipt.status = GraphValidationStatus::Complete;
        }
    }
    if (result.receipt.status != GraphValidationStatus::Complete) {
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count();
        result.receipt.validation_nanoseconds = elapsed < 0
            ? 0
            : static_cast<std::uint64_t>(elapsed);
        return result;
    }
    try {
        std::shared_ptr<ValidatedGraphSnapshot> candidate(
            new ValidatedGraphSnapshot());
        if (!cloneGraphExact(data, candidate->data_, true) ||
            !cloneGraphExact(query, candidate->query_, false)) {
            result.receipt.status =
                GraphValidationStatus::AllocationModelViolation;
        } else {
        result.receipt.status = validateAndFingerprint(
            candidate->data_, candidate->query_, budget,
            result.receipt,
            observed_quote.maximum_incident_intervals);
        }
        std::uint64_t peak_bytes =
            result.receipt.source_live_storage_bytes;
        if (!checkedAdd(
                peak_bytes,
                result.receipt.snapshot_storage_bytes) ||
            !checkedAdd(
                peak_bytes,
                result.receipt.temporary_validation_capacity_bytes)) {
            result.receipt.status =
                GraphValidationStatus::ArithmeticOverflow;
        } else {
            result.receipt.seal_peak_live_bytes = peak_bytes;
            if (result.receipt.snapshot_storage_bytes >
                    result.receipt.quoted_snapshot_storage_bytes ||
                result.receipt.temporary_validation_capacity_bytes >
                    result.receipt.quoted_validation_temporary_bytes ||
                peak_bytes >
                    result.receipt.quoted_seal_peak_live_bytes) {
                result.receipt.status =
                    GraphValidationStatus::AllocationModelViolation;
            } else if (result.receipt.snapshot_storage_bytes >
                    budget.snapshot_storage_bytes ||
                result.receipt.temporary_validation_capacity_bytes >
                    budget.temporary_validation_bytes ||
                peak_bytes > budget.seal_peak_live_bytes) {
                result.receipt.status =
                    GraphValidationStatus::BudgetExceeded;
            }
        }
        if (result.receipt.status ==
            GraphValidationStatus::Complete) {
            if (!acquireSnapshotToken(
                    result.receipt.snapshot_token)) {
                result.receipt.status =
                    GraphValidationStatus::SnapshotTokenExhausted;
            }
        }
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count();
        result.receipt.validation_nanoseconds = elapsed < 0
            ? 0
            : static_cast<std::uint64_t>(elapsed);
        if (result.receipt.status ==
            GraphValidationStatus::Complete) {
            candidate->receipt_ = result.receipt;
            result.snapshot = std::move(candidate);
        }
    } catch (const std::bad_alloc&) {
        result.snapshot.reset();
        result.receipt.status =
            GraphValidationStatus::AllocationFailure;
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count();
        result.receipt.validation_nanoseconds = elapsed < 0
            ? 0
            : static_cast<std::uint64_t>(elapsed);
    } catch (...) {
        result.snapshot.reset();
        result.receipt.status =
            GraphValidationStatus::InvalidDataGraph;
    }
    return result;
}

const Graph& ValidatedGraphSnapshot::data() const noexcept {
    return data_;
}

const Graph& ValidatedGraphSnapshot::query() const noexcept {
    return query_;
}

std::uint64_t ValidatedGraphSnapshot::snapshotToken() const noexcept {
    return receipt_.snapshot_token;
}

std::uint64_t ValidatedGraphSnapshot::dataFingerprint() const noexcept {
    return receipt_.data_fingerprint;
}

std::uint64_t ValidatedGraphSnapshot::queryFingerprint() const noexcept {
    return receipt_.query_fingerprint;
}

const GraphValidationReceipt&
ValidatedGraphSnapshot::receipt() const noexcept {
    return receipt_;
}

bool sharesValidatedGraphSnapshot(
    const std::shared_ptr<const ValidatedGraphSnapshot>& lhs,
    const std::shared_ptr<const ValidatedGraphSnapshot>& rhs) noexcept {
    if (!lhs || !rhs) return false;
    return lhs.get() == rhs.get() &&
        !lhs.owner_before(rhs) && !rhs.owner_before(lhs);
}
