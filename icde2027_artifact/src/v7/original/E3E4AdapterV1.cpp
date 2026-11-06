#include "E3E4AdapterV1.h"

#include "../benchmark/e3_e4_adapter_v1/E3E4RuntimeHooksV1.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace dsm::e3e4::adapter_v1::td_tree {

class AdapterProductionAccess final {
public:
    static TDTree construct(
        const Graph& temporal_graph,
        const Graph& query_graph,
        const QueryDecomposition& decomposition,
        int minimum_duration,
        RuntimeHookedOriginalNativePhaseObserver& phase_observer,
        const runtime_hooks_v1::MonotonicNowSource& clock) {
        return TDTree(
            temporal_graph, query_graph, decomposition, minimum_duration,
            phase_observer, clock, DurationMode::Consecutive,
            PathOrderMode::Sorted, UINT64_C(2022), true);
    }

    static MatchSummary countMatches(
        const TDTree& tree,
        std::size_t threads,
        RuntimeHookedOriginalNativePhaseObserver& phase_observer,
        const runtime_hooks_v1::MonotonicNowSource& clock,
        const runtime_hooks_v1::WorkerAffinityBinder& affinity_binder) {
        return tree.countMatchesAdapterOwnedWithRuntimeHooks(
            threads, phase_observer, clock, affinity_binder);
    }
};

namespace {

[[noreturn]] void fail(ErrorCode code, const std::string& message) {
    throw AdapterError(code, "td-tree source adapter: " + message);
}

std::uint64_t checkedElapsed(
    std::uint64_t begin,
    std::uint64_t end,
    const char* role) {
    if (end < begin) {
        fail(
            ErrorCode::kInvalidChildReceipt,
            std::string(role) + " steady-clock regression");
    }
    return end - begin;
}

struct ExternalTemporalEdge {
    std::int32_t source = 0;
    std::int32_t target = 0;
    std::vector<TimeInterval> active_intervals;
    std::int64_t active_snapshot_count = 0;
};

void checkedIncrement(int& value, const std::string& context) {
    if (value == std::numeric_limits<int>::max()) {
        fail(ErrorCode::kIntegerOverflow, context + " multiplicity overflow");
    }
    ++value;
}

Graph buildQueryGraph(const ParsedQuery& parsed_query) {
    if (parsed_query.vertices.empty() ||
        parsed_query.vertices.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        fail(ErrorCode::kInvalidQuery, "invalid query vertex count");
    }

    Graph query_graph;
    query_graph.num_vertices = static_cast<int>(parsed_query.vertices.size());
    query_graph.adj.resize(parsed_query.vertices.size());
    query_graph.in_adj.resize(parsed_query.vertices.size());
    query_graph.vertex_labels.reserve(parsed_query.vertices.size());
    query_graph.external_ids.reserve(parsed_query.vertices.size());
    for (std::size_t index = 0; index < parsed_query.vertices.size(); ++index) {
        const std::uint8_t label =
            parsed_query.vertices[index].runtime_class_id;
        if (label >= kRuntimeLabelCount || label >= kLabelCount) {
            fail(ErrorCode::kInvalidQuery, "query runtime label is outside A--E");
        }
        query_graph.vertex_labels.push_back(static_cast<Label>(label));
        query_graph.external_ids.push_back(static_cast<int>(index));
    }

    std::set<std::pair<std::uint32_t, std::uint32_t>> directed_edges;
    for (const QueryDirectedEdge& edge : parsed_query.directed_edges) {
        if (edge.source_index >= parsed_query.vertices.size() ||
            edge.target_index >= parsed_query.vertices.size() ||
            !directed_edges.emplace(
                 edge.source_index, edge.target_index).second) {
            fail(ErrorCode::kInvalidQuery, "invalid or duplicate directed query edge");
        }
        query_graph.adj[edge.source_index].push_back(
            {static_cast<int>(edge.target_index), -1});
        query_graph.in_adj[edge.target_index].push_back(
            {static_cast<int>(edge.source_index), -1});
    }
    if (directed_edges.empty()) {
        fail(ErrorCode::kInvalidQuery, "query has no directed edge");
    }
    for (auto& neighbors : query_graph.adj) {
        std::sort(
            neighbors.begin(), neighbors.end(),
            [](const Edge& lhs, const Edge& rhs) {
                return lhs.to < rhs.to;
            });
    }
    for (auto& neighbors : query_graph.in_adj) {
        std::sort(
            neighbors.begin(), neighbors.end(),
            [](const Edge& lhs, const Edge& rhs) {
                return lhs.to < rhs.to;
            });
    }
    return query_graph;
}

InputIdentitySet captureInputIdentities(
    const ValidatedInputs& inputs,
    std::uint32_t minimum_duration_k) {
    InputIdentitySet identities;
    identities.prepared_graph = inputs.prepared_graph.identity();
    identities.explicit_label_map = inputs.explicit_label_map.identity();
    identities.query_projection = inputs.query_projection.identity();
    identities.query = inputs.query.identity();
    identities.minimum_duration_k = minimum_duration_k;
    identities.typed_plan = std::nullopt;
    return identities;
}

namespace runtime = runtime_hooks_v1;

class TdTreePhaseBridge final
    : public RuntimeHookedOriginalNativePhaseObserver {
public:
    TdTreePhaseBridge(
        PhaseLedger& ledger,
        std::uint64_t boundary_ns,
        const runtime::MonotonicNowSource& clock)
        : RuntimeHookedOriginalNativePhaseObserver(clock),
          ledger_(ledger),
          boundary_ns_(boundary_ns) {}

    void beginPlanning() {
        if (started_) {
            fail(ErrorCode::kInvalidPhaseLedger, "planning started twice");
        }
        ledger_.start(
            CommonPhase::kPlanning,
            "explicit-runtime-label-graph-and-tdtree-decomposition",
            true,
            boundary_ns_);
        started_ = true;
    }

    void onRuntimeHookedOriginalNativePhase(
        OriginalNativePhase phase,
        const runtime::MonotonicClockReading& reading) override {
        if (!started_) {
            fail(ErrorCode::kInvalidPhaseLedger, "native phase before boundary");
        }
        const std::uint64_t now_ns = reading.authorityNanoseconds();
        switch (phase) {
            case OriginalNativePhase::Planning:
                if (!planning_seen_) {
                    if (td_construction_seen_ || order_planning_seen_ ||
                        enumeration_seen_ || output_seen_) {
                        fail(
                            ErrorCode::kInvalidPhaseLedger,
                            "initial native planning notification out of order");
                    }
                    planning_seen_ = true;
                    return;
                }
                if (!td_construction_seen_ || order_planning_seen_ ||
                    enumeration_seen_ || output_seen_) {
                    fail(
                        ErrorCode::kInvalidPhaseLedger,
                        "matching-order planning notification out of order");
                }
                ledger_.transition(
                    CommonPhase::kPlanning,
                    "tdtree-matching-order-and-query-paths",
                    true,
                    now_ns);
                order_planning_seen_ = true;
                return;
            case OriginalNativePhase::TDConstruction:
                if (!planning_seen_ || td_construction_seen_ ||
                    enumeration_seen_ || output_seen_) {
                    fail(
                        ErrorCode::kInvalidPhaseLedger,
                        "native TD construction notification out of order");
                }
                ledger_.transition(
                    CommonPhase::kTdConstruction,
                    "tdtree-build",
                    true,
                    now_ns);
                td_construction_seen_ = true;
                return;
            case OriginalNativePhase::Enumeration: {
                if (!td_construction_seen_ || !order_planning_seen_ ||
                    enumeration_seen_ ||
                    output_seen_) {
                    fail(
                        ErrorCode::kInvalidPhaseLedger,
                        "native enumeration notification out of order");
                }
                // External arms must carry an explicit zero-duration DAG
                // interval.  Both transitions share the one captured instant.
                ledger_.transition(
                    CommonPhase::kDagCompilation,
                    "not-applicable",
                    false,
                    now_ns);
                ledger_.transition(
                    CommonPhase::kEnumeration,
                    "tdtree-count-only-enumeration",
                    true,
                    now_ns);
                enumeration_seen_ = true;
                return;
            }
            case OriginalNativePhase::OutputFinalization:
                if (!enumeration_seen_ || output_seen_) {
                    fail(
                        ErrorCode::kInvalidPhaseLedger,
                        "native output notification out of order");
                }
                ledger_.transition(
                    CommonPhase::kOutputFinalization,
                    "tdtree-count-reduction",
                    true,
                    now_ns);
                output_seen_ = true;
                return;
        }
        fail(ErrorCode::kInvalidPhaseLedger, "unknown native phase");
    }

    PhaseLedgerSnapshot finish(std::uint64_t now_ns) {
        if (!planning_seen_ || !td_construction_seen_ ||
            !order_planning_seen_ || !enumeration_seen_ || !output_seen_) {
            fail(ErrorCode::kInvalidPhaseLedger, "native phase sequence incomplete");
        }
        return ledger_.finish(now_ns);
    }

private:
    PhaseLedger& ledger_;
    std::uint64_t boundary_ns_ = 0;
    bool started_ = false;
    bool planning_seen_ = false;
    bool td_construction_seen_ = false;
    bool order_planning_seen_ = false;
    bool enumeration_seen_ = false;
    bool output_seen_ = false;
};

}  // namespace

Graph buildExplicitRuntimeLabeledTemporalGraph(
    const std::vector<RawTemporalEdge>& raw_temporal_edges,
    const std::vector<RuntimeLabelRow>& runtime_labels_by_external_id) {
    if (raw_temporal_edges.empty()) {
        fail(ErrorCode::kInvalidGraph, "temporal graph is empty");
    }

    std::vector<RawTemporalEdge> sorted_edges = raw_temporal_edges;
    for (const RawTemporalEdge& edge : sorted_edges) {
        if (edge.source < 0 || edge.target < 0) {
            fail(ErrorCode::kInvalidGraph, "negative external vertex ID");
        }
    }
    std::sort(
        sorted_edges.begin(), sorted_edges.end(),
        [](const RawTemporalEdge& lhs, const RawTemporalEdge& rhs) {
            if (lhs.source != rhs.source) return lhs.source < rhs.source;
            if (lhs.target != rhs.target) return lhs.target < rhs.target;
            return lhs.timestamp < rhs.timestamp;
        });

    std::vector<ExternalTemporalEdge> retained_edges;
    std::vector<int> external_ids;
    std::size_t index = 0;
    while (index < sorted_edges.size()) {
        const std::int32_t source = sorted_edges[index].source;
        const std::int32_t target = sorted_edges[index].target;
        external_ids.push_back(static_cast<int>(source));
        external_ids.push_back(static_cast<int>(target));

        ExternalTemporalEdge retained;
        retained.source = source;
        retained.target = target;
        bool have_timestamp = false;
        std::int32_t run_start = 0;
        std::int32_t previous = 0;
        while (index < sorted_edges.size() &&
               sorted_edges[index].source == source &&
               sorted_edges[index].target == target) {
            const std::int32_t current = sorted_edges[index].timestamp;
            ++index;
            if (have_timestamp && current == previous) continue;
            if (retained.active_snapshot_count ==
                std::numeric_limits<std::int64_t>::max()) {
                fail(ErrorCode::kIntegerOverflow, "edge active count overflow");
            }
            ++retained.active_snapshot_count;
            if (!have_timestamp) {
                have_timestamp = true;
                run_start = previous = current;
            } else if (
                previous != std::numeric_limits<std::int32_t>::max() &&
                current == previous + 1) {
                previous = current;
            } else {
                retained.active_intervals.push_back(
                    {static_cast<int>(run_start), static_cast<int>(previous)});
                run_start = previous = current;
            }
        }
        if (have_timestamp) {
            retained.active_intervals.push_back(
                {static_cast<int>(run_start), static_cast<int>(previous)});
        }
        retained_edges.push_back(std::move(retained));
    }
    std::vector<RawTemporalEdge>().swap(sorted_edges);

    std::sort(external_ids.begin(), external_ids.end());
    external_ids.erase(
        std::unique(external_ids.begin(), external_ids.end()),
        external_ids.end());
    if (external_ids.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        retained_edges.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        fail(ErrorCode::kIntegerOverflow, "graph cardinality exceeds native IDs");
    }
    if (runtime_labels_by_external_id.size() != external_ids.size()) {
        fail(ErrorCode::kGraphMapCoverageMismatch, "runtime label count mismatch");
    }

    Graph graph;
    graph.input_occurrence_count = raw_temporal_edges.size();
    graph.input_unique_edge_count = retained_edges.size();
    graph.retained_edge_count = retained_edges.size();
    graph.external_ids = external_ids;
    graph.num_vertices = static_cast<int>(external_ids.size());
    graph.adj.resize(external_ids.size());
    graph.in_adj.resize(external_ids.size());
    graph.vertex_labels.reserve(external_ids.size());
    for (std::size_t label_index = 0;
         label_index < external_ids.size();
         ++label_index) {
        const RuntimeLabelRow& row =
            runtime_labels_by_external_id[label_index];
        if (row.external_vertex_id != external_ids[label_index]) {
            fail(
                ErrorCode::kGraphMapCoverageMismatch,
                "runtime label external ID mismatch");
        }
        if (row.runtime_class_id >= kRuntimeLabelCount ||
            row.runtime_class_id >= kLabelCount) {
            fail(
                ErrorCode::kCrossBindingMismatch,
                "runtime label is outside A--E");
        }
        graph.vertex_labels.push_back(
            static_cast<Label>(row.runtime_class_id));
    }

    std::vector<std::size_t> out_degrees(external_ids.size(), 0);
    std::vector<std::size_t> in_degrees(external_ids.size(), 0);
    for (const ExternalTemporalEdge& edge : retained_edges) {
        const auto source_it = std::lower_bound(
            external_ids.begin(), external_ids.end(), edge.source);
        const auto target_it = std::lower_bound(
            external_ids.begin(), external_ids.end(), edge.target);
        if (source_it == external_ids.end() || *source_it != edge.source ||
            target_it == external_ids.end() || *target_it != edge.target) {
            fail(ErrorCode::kInvalidGraph, "compact vertex lookup failed");
        }
        ++out_degrees[static_cast<std::size_t>(
            source_it - external_ids.begin())];
        ++in_degrees[static_cast<std::size_t>(
            target_it - external_ids.begin())];
    }
    for (std::size_t vertex = 0; vertex < external_ids.size(); ++vertex) {
        graph.adj[vertex].reserve(out_degrees[vertex]);
        graph.in_adj[vertex].reserve(in_degrees[vertex]);
    }

    graph.temporal_edges.reserve(retained_edges.size());
    for (ExternalTemporalEdge& edge : retained_edges) {
        const int compact_source = static_cast<int>(
            std::lower_bound(
                external_ids.begin(), external_ids.end(), edge.source) -
            external_ids.begin());
        const int compact_target = static_cast<int>(
            std::lower_bound(
                external_ids.begin(), external_ids.end(), edge.target) -
            external_ids.begin());
        const int temporal_edge_id =
            static_cast<int>(graph.temporal_edges.size());
        graph.temporal_edges.push_back({
            compact_source,
            compact_target,
            std::move(edge.active_intervals),
            edge.active_snapshot_count});
        graph.adj[static_cast<std::size_t>(compact_source)].push_back(
            {compact_target, temporal_edge_id});
        graph.in_adj[static_cast<std::size_t>(compact_target)].push_back(
            {compact_source, temporal_edge_id});
    }
    std::vector<ExternalTemporalEdge>().swap(retained_edges);

    for (auto& neighbors : graph.adj) {
        std::sort(
            neighbors.begin(), neighbors.end(),
            [](const Edge& lhs, const Edge& rhs) {
                return lhs.to < rhs.to;
            });
    }
    for (auto& neighbors : graph.in_adj) {
        std::sort(
            neighbors.begin(), neighbors.end(),
            [](const Edge& lhs, const Edge& rhs) {
                return lhs.to < rhs.to;
            });
    }

    graph.vertex_active_durations.assign(external_ids.size(), 0);
    graph.neighbor_label_counts.assign(external_ids.size(), {});
    graph.incoming_neighbor_label_counts.assign(external_ids.size(), {});
    for (std::size_t vertex = 0; vertex < external_ids.size(); ++vertex) {
        std::vector<TimeInterval> active_intervals;
        for (const Edge& edge_ref : graph.adj[vertex]) {
            const TemporalEdge& temporal_edge = graph.temporal_edges[
                static_cast<std::size_t>(edge_ref.temporal_edge_id)];
            active_intervals = unionTimeIntervals(
                active_intervals, temporal_edge.active_intervals);
            const Label neighbor_label = graph.vertex_labels[
                static_cast<std::size_t>(edge_ref.to)];
            checkedIncrement(
                graph.neighbor_label_counts[vertex][neighbor_label],
                "outgoing neighbor label");
        }
        for (const Edge& edge_ref : graph.in_adj[vertex]) {
            const TemporalEdge& temporal_edge = graph.temporal_edges[
                static_cast<std::size_t>(edge_ref.temporal_edge_id)];
            active_intervals = unionTimeIntervals(
                active_intervals, temporal_edge.active_intervals);
            const Label neighbor_label = graph.vertex_labels[
                static_cast<std::size_t>(edge_ref.to)];
            checkedIncrement(
                graph.incoming_neighbor_label_counts[vertex][neighbor_label],
                "incoming neighbor label");
        }
        graph.vertex_active_durations[vertex] =
            timeInstanceCardinality(active_intervals);
    }
    return graph;
}

static TdTreeSourceOnlyResult runTdTreeSourceOnlyImpl(
    const RepositoryAnchor& repository,
    std::uint64_t child_start_ns,
    const runtime::MonotonicNowSource& clock,
    const runtime::WorkerAffinityBinder& affinity_binder,
    int argc,
    const char* const argv[]) {
    ClosedInvocation invocation = parseClosedInvocation(
        argc, argv, ArmId::kTdTree);
    if (invocation.arm != ArmId::kTdTree || !invocation.count_only ||
        invocation.threads != kRequiredThreads ||
        invocation.typed_plan_path.has_value()) {
        fail(ErrorCode::kInvalidInvocation, "closed TD-tree invocation drift");
    }

    ValidatedInputs inputs = loadValidatedInputs(repository, invocation);
    const std::uint64_t boundary_ns =
        clock.now().authorityNanoseconds();
    PhaseLedger ledger(ArmId::kTdTree, boundary_ns);
    TdTreePhaseBridge phase_bridge(ledger, boundary_ns, clock);
    phase_bridge.beginPlanning();

    Graph temporal_graph = buildExplicitRuntimeLabeledTemporalGraph(
        inputs.raw_temporal_edges,
        inputs.runtime_labels_by_external_id);
    Graph query_graph = buildQueryGraph(inputs.parsed_query);
    QueryDecomposition decomposition =
        decomposeQueryWithExplicitRuntimeLabels(
            query_graph,
            temporal_graph,
            phase_bridge,
            QueryTreeMode::Dfs);
    if (!decomposition.connected || decomposition.root < 0) {
        fail(ErrorCode::kInvalidQuery, "query decomposition is disconnected");
    }

    TDTree tree = AdapterProductionAccess::construct(
        temporal_graph,
        query_graph,
        decomposition,
        static_cast<int>(invocation.minimum_duration_k),
        phase_bridge,
        clock);
    MatchSummary match_summary =
        AdapterProductionAccess::countMatches(
            tree,
            invocation.threads,
            phase_bridge,
            clock,
            affinity_binder);
    if (match_summary.output_written ||
        match_summary.threads_requested != invocation.threads ||
        match_summary.threads_used != invocation.threads) {
        fail(ErrorCode::kInvalidChildReceipt, "native count-only thread observation drift");
    }
    runtime::ExactWorkerAffinityProof affinity_proof =
        runtime::aggregateExact16WorkerAffinity(
            match_summary.worker_affinity_observations);
    ThreadObservation threads =
        affinity_proof.toAuthorityThreadObservation();

    // Identity replay is part of the active in-memory output-finalization
    // interval. Canonical receipt bytes are constructed only after it closes;
    // no filesystem receipt is emitted here.
    inputs.proveStable();
    repository.proveStable();
    InputIdentitySet input_identities = captureInputIdentities(
        inputs, invocation.minimum_duration_k);
    const std::uint64_t primary_end_ns =
        clock.now().authorityNanoseconds();
    PhaseLedgerSnapshot phase_ledger = phase_bridge.finish(primary_end_ns);

    ChildReceiptValue receipt_value;
    receipt_value.governance_id = std::string(kGovernanceId);
    receipt_value.arm = ArmId::kTdTree;
    receipt_value.input_identities = input_identities;
    receipt_value.loads = inputs.loads;
    receipt_value.ledger = phase_ledger;
    receipt_value.match_count = match_summary.match_count;
    receipt_value.count_overflow = match_summary.count_overflow;
    receipt_value.threads = threads;
    receipt_value.child_wall_segments.clock = "steady_clock";
    receipt_value.child_wall_segments.unit = "ns";
    receipt_value.child_wall_segments.preboundary_ns = checkedElapsed(
        child_start_ns, boundary_ns, "preboundary");
    receipt_value.child_wall_segments.primary_total_ns =
        phase_ledger.totals.prepared_query_total_ns;
    const std::uint64_t postboundary_end_ns =
        clock.now().authorityNanoseconds();
    receipt_value.child_wall_segments.postboundary_ns = checkedElapsed(
        primary_end_ns, postboundary_end_ns, "postboundary");

    TdTreeSourceOnlyResult result;
    result.invocation = std::move(invocation);
    result.input_identities = std::move(input_identities);
    result.loads = inputs.loads;
    result.phase_ledger = std::move(phase_ledger);
    result.match_summary = std::move(match_summary);
    result.threads = std::move(threads);
    result.canonical_child_receipt =
        serializeCanonicalChildReceipt(receipt_value);
    return result;
}

TdTreeSourceOnlyResult runTdTreeClosedCountOnly(
    int argc,
    const char* const argv[]) {
    const runtime::MonotonicNowSource& clock =
        runtime::productionMonotonicNowSource();
    const runtime::WorkerAffinityBinder& affinity_binder =
        runtime::productionWorkerAffinityBinder();
    const std::uint64_t child_start_ns =
        clock.now().authorityNanoseconds();
    RepositoryAnchor repository = RepositoryAnchor::openAbsolute(
        std::string(kCanonicalRepositoryRoot));
    return runTdTreeSourceOnlyImpl(
        repository,
        child_start_ns,
        clock,
        affinity_binder,
        argc,
        argv);
}

}  // namespace dsm::e3e4::adapter_v1::td_tree
