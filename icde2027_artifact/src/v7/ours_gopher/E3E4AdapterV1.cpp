#include "E3E4AdapterV1.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace dsm::e3e4::gopher_adapter_v1 {

class AdapterProductionAccess final {
public:
    static GopherE3E4ObservedRun run(
        GopherMatcher& matcher,
        GopherE3E4NativeObserver& observer) {
        return matcher.runAdapterOwnedE3E4ClosedCountOnly(observer);
    }
};

namespace {

namespace common = dsm::e3e4::adapter_v1;
namespace hooks = dsm::e3e4::adapter_v1::runtime_hooks_v1;
namespace plans = dsm::e3e4::gopher_plan_v1;

[[noreturn]] void fail(
    common::ErrorCode code,
    const std::string& message) {
    throw common::AdapterError(
        code, "internal gopher E3/E4 adapter: " + message);
}

std::uint64_t authorityNow(const hooks::MonotonicNowSource& clock) {
    return clock.now().authorityNanoseconds();
}

std::uint64_t checkedElapsed(
    std::uint64_t begin,
    std::uint64_t end,
    const char* role) {
    if (end < begin) {
        fail(
            common::ErrorCode::kClockRegression,
            std::string(role) + " steady-clock regression");
    }
    return end - begin;
}

common::ArmId dispatchArm(int argc, const char* const argv[]) {
    if (argc < 5 || argv == nullptr || argv[0] == nullptr ||
        argv[1] == nullptr || argv[2] == nullptr ||
        argv[3] == nullptr || argv[4] == nullptr ||
        std::string(argv[1]) != "--engine" ||
        std::string(argv[2]) != "gopher" ||
        std::string(argv[3]) != "--variant") {
        fail(
            common::ErrorCode::kInvalidInvocation,
            "expected exact leading --engine gopher --variant selector");
    }
    const std::string variant(argv[4]);
    if (variant == "durable-dfs") {
        return common::ArmId::kPriorDirect;
    }
    if (variant == "dag-dep-key-memo") {
        return common::ArmId::kFinalDurableDag;
    }
    fail(
        common::ErrorCode::kInvalidInvocation,
        "variant must be exactly durable-dfs or dag-dep-key-memo");
}

struct MaterializedTemporalEdge final {
    std::int32_t source = 0;
    std::int32_t target = 0;
    std::vector<TimeInterval> active_intervals;
    std::int64_t active_snapshot_count = 0;
};

void checkedIncrement(int& value, const char* role) {
    if (value == std::numeric_limits<int>::max()) {
        fail(
            common::ErrorCode::kIntegerOverflow,
            std::string(role) + " multiplicity overflow");
    }
    ++value;
}

int compactId(
    const std::vector<int>& external_ids,
    std::int32_t external_id) {
    const int value = static_cast<int>(external_id);
    const auto found = std::lower_bound(
        external_ids.begin(), external_ids.end(), value);
    if (found == external_ids.end() || *found != value) {
        fail(
            common::ErrorCode::kInvalidGraph,
            "compact external vertex lookup failed");
    }
    return static_cast<int>(found - external_ids.begin());
}

int incidentUnionDuration(std::vector<TimeInterval> intervals) {
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
    for (std::size_t index = 1; index < intervals.size(); ++index) {
        const TimeInterval& interval = intervals[index];
        if (interval.start <= current_end ||
            (current_end != std::numeric_limits<int>::max() &&
             interval.start == current_end + 1)) {
            current_end = std::max(current_end, interval.end);
            continue;
        }
        total += static_cast<std::int64_t>(current_end) -
            static_cast<std::int64_t>(current_start) + 1;
        if (total >= std::numeric_limits<int>::max()) {
            return std::numeric_limits<int>::max();
        }
        current_start = interval.start;
        current_end = interval.end;
    }
    total += static_cast<std::int64_t>(current_end) -
        static_cast<std::int64_t>(current_start) + 1;
    return total >= std::numeric_limits<int>::max()
        ? std::numeric_limits<int>::max()
        : static_cast<int>(total);
}

Graph materializeTemporalGraph(const common::ValidatedInputs& inputs) {
    static_assert(
        std::numeric_limits<int>::min() <=
            std::numeric_limits<std::int32_t>::min() &&
        std::numeric_limits<int>::max() >=
            std::numeric_limits<std::int32_t>::max(),
        "Gopher native int must cover the common signed-int32 domain");
    if (inputs.raw_temporal_edges.empty()) {
        fail(common::ErrorCode::kInvalidGraph, "prepared graph is empty");
    }

    std::vector<common::RawTemporalEdge> sorted =
        inputs.raw_temporal_edges;
    for (const common::RawTemporalEdge& row : sorted) {
        if (row.source < 0 || row.target < 0) {
            fail(
                common::ErrorCode::kInvalidGraph,
                "prepared graph has a negative external vertex ID");
        }
    }
    std::sort(
        sorted.begin(), sorted.end(),
        [](const common::RawTemporalEdge& lhs,
           const common::RawTemporalEdge& rhs) {
            if (lhs.source != rhs.source) return lhs.source < rhs.source;
            if (lhs.target != rhs.target) return lhs.target < rhs.target;
            return lhs.timestamp < rhs.timestamp;
        });

    std::vector<MaterializedTemporalEdge> edges;
    std::vector<int> all_external_ids;
    std::vector<int> filtered_external_ids;
    std::size_t unique_edge_count = 0U;
    std::size_t position = 0;
    while (position < sorted.size()) {
        const std::int32_t source = sorted[position].source;
        const std::int32_t target = sorted[position].target;
        ++unique_edge_count;
        all_external_ids.push_back(static_cast<int>(source));
        all_external_ids.push_back(static_cast<int>(target));

        MaterializedTemporalEdge edge;
        edge.source = source;
        edge.target = target;
        bool have_timestamp = false;
        bool have_consecutive_pair = false;
        std::int32_t run_start = 0;
        std::int32_t previous = 0;
        while (position < sorted.size() &&
               sorted[position].source == source &&
               sorted[position].target == target) {
            const std::int32_t current = sorted[position].timestamp;
            ++position;
            if (have_timestamp && current == previous) continue;
            if (edge.active_snapshot_count ==
                std::numeric_limits<std::int64_t>::max()) {
                fail(
                    common::ErrorCode::kIntegerOverflow,
                    "temporal edge active-snapshot count overflow");
            }
            ++edge.active_snapshot_count;
            if (!have_timestamp) {
                have_timestamp = true;
                run_start = previous = current;
            } else if (
                previous != std::numeric_limits<std::int32_t>::max() &&
                current == previous + 1) {
                have_consecutive_pair = true;
                previous = current;
            } else {
                edge.active_intervals.push_back({
                    static_cast<int>(run_start),
                    static_cast<int>(previous),
                });
                run_start = previous = current;
            }
        }
        if (have_timestamp) {
            edge.active_intervals.push_back({
                static_cast<int>(run_start),
                static_cast<int>(previous),
            });
        }
        // Reconstruct the exact prepared-graph semantics from already-held,
        // already-parsed rows.  This is not a raw reader: Algorithm 3.1 keeps
        // the complete history iff the ordered pair has a consecutive pair.
        if (have_consecutive_pair) {
            filtered_external_ids.push_back(static_cast<int>(source));
            filtered_external_ids.push_back(static_cast<int>(target));
            edges.push_back(std::move(edge));
        }
    }
    std::vector<common::RawTemporalEdge>().swap(sorted);

    std::sort(all_external_ids.begin(), all_external_ids.end());
    all_external_ids.erase(
        std::unique(all_external_ids.begin(), all_external_ids.end()),
        all_external_ids.end());
    std::sort(filtered_external_ids.begin(), filtered_external_ids.end());
    filtered_external_ids.erase(
        std::unique(
            filtered_external_ids.begin(), filtered_external_ids.end()),
        filtered_external_ids.end());
    if (filtered_external_ids.empty()) {
        fail(
            common::ErrorCode::kInvalidGraph,
            "prepared graph has no edge surviving Algorithm 3.1");
    }
    if (filtered_external_ids.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        edges.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        fail(
            common::ErrorCode::kIntegerOverflow,
            "prepared graph cardinality exceeds native IDs");
    }
    if (inputs.runtime_labels_by_external_id.size() !=
        all_external_ids.size()) {
        fail(
            common::ErrorCode::kGraphMapCoverageMismatch,
            "runtime label coverage differs from prepared graph vertices");
    }
    for (std::size_t index = 0; index < all_external_ids.size(); ++index) {
        const common::RuntimeLabelRow& row =
            inputs.runtime_labels_by_external_id[index];
        if (row.external_vertex_id != all_external_ids[index] ||
            row.runtime_class_id >= common::kRuntimeLabelCount ||
            row.runtime_class_id >= kLabelCount) {
            fail(
                common::ErrorCode::kGraphMapCoverageMismatch,
                "runtime labels do not exactly cover the raw held vertex set");
        }
    }

    Graph graph;
    graph.input_occurrence_count = inputs.raw_temporal_edges.size();
    graph.input_unique_edge_count = unique_edge_count;
    graph.filtered_edge_count = edges.size();
    graph.num_vertices = static_cast<int>(filtered_external_ids.size());
    graph.external_ids = filtered_external_ids;
    graph.adj.resize(filtered_external_ids.size());
    graph.in_adj.resize(filtered_external_ids.size());
    graph.vertex_labels.reserve(filtered_external_ids.size());
    for (int external_id : filtered_external_ids) {
        const auto found = std::lower_bound(
            inputs.runtime_labels_by_external_id.begin(),
            inputs.runtime_labels_by_external_id.end(),
            external_id,
            [](const common::RuntimeLabelRow& row, int value) {
                return row.external_vertex_id < value;
            });
        if (found == inputs.runtime_labels_by_external_id.end() ||
            found->external_vertex_id != external_id) {
            fail(
                common::ErrorCode::kGraphMapCoverageMismatch,
                "filtered vertex lacks its held runtime label");
        }
        const common::RuntimeLabelRow& row = *found;
        if (row.runtime_class_id >= common::kRuntimeLabelCount ||
            row.runtime_class_id >= kLabelCount) {
            fail(
                common::ErrorCode::kCrossBindingMismatch,
                "validated runtime label is outside explicit A-through-E");
        }
        graph.vertex_labels.push_back(
            static_cast<Label>(row.runtime_class_id));
    }

    const std::vector<int>& external_ids = graph.external_ids;
    std::vector<std::size_t> out_degrees(external_ids.size(), 0U);
    std::vector<std::size_t> in_degrees(external_ids.size(), 0U);
    for (const MaterializedTemporalEdge& edge : edges) {
        ++out_degrees[static_cast<std::size_t>(
            compactId(external_ids, edge.source))];
        ++in_degrees[static_cast<std::size_t>(
            compactId(external_ids, edge.target))];
    }
    for (std::size_t vertex = 0; vertex < external_ids.size(); ++vertex) {
        graph.adj[vertex].reserve(out_degrees[vertex]);
        graph.in_adj[vertex].reserve(in_degrees[vertex]);
    }

    graph.temporal_edges.reserve(edges.size());
    for (MaterializedTemporalEdge& edge : edges) {
        const int source = compactId(external_ids, edge.source);
        const int target = compactId(external_ids, edge.target);
        const int edge_id = static_cast<int>(graph.temporal_edges.size());
        graph.temporal_edges.emplace_back(
            source,
            target,
            std::move(edge.active_intervals),
            edge.active_snapshot_count);
        graph.adj[static_cast<std::size_t>(source)].push_back(
            {target, edge_id});
        graph.in_adj[static_cast<std::size_t>(target)].push_back(
            {source, edge_id});
    }
    std::vector<MaterializedTemporalEdge>().swap(edges);

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
        std::vector<TimeInterval> incident_intervals;
        for (const Edge& reference : graph.adj[vertex]) {
            const TemporalEdge& edge = graph.temporal_edges[
                static_cast<std::size_t>(reference.temporal_edge_id)];
            incident_intervals.insert(
                incident_intervals.end(),
                edge.activeIntervals().begin(),
                edge.activeIntervals().end());
            const Label neighbor_label = graph.vertex_labels[
                static_cast<std::size_t>(reference.to)];
            if (neighbor_label >= kLabelCount) {
                fail(
                    common::ErrorCode::kCrossBindingMismatch,
                    "outgoing neighbor has a non-A-through-E label");
            }
            checkedIncrement(
                graph.neighbor_label_counts[vertex][neighbor_label],
                "outgoing neighbor-label");
        }
        for (const Edge& reference : graph.in_adj[vertex]) {
            const TemporalEdge& edge = graph.temporal_edges[
                static_cast<std::size_t>(reference.temporal_edge_id)];
            incident_intervals.insert(
                incident_intervals.end(),
                edge.activeIntervals().begin(),
                edge.activeIntervals().end());
            const Label neighbor_label = graph.vertex_labels[
                static_cast<std::size_t>(reference.to)];
            if (neighbor_label >= kLabelCount) {
                fail(
                    common::ErrorCode::kCrossBindingMismatch,
                    "incoming neighbor has a non-A-through-E label");
            }
            checkedIncrement(
                graph.incoming_neighbor_label_counts[vertex][neighbor_label],
                "incoming neighbor-label");
        }
        graph.vertex_active_durations[vertex] =
            incidentUnionDuration(std::move(incident_intervals));
    }
    return graph;
}

Graph materializeQueryGraph(const common::ParsedQuery& parsed) {
    if (parsed.vertices.empty() ||
        parsed.vertices.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        fail(common::ErrorCode::kInvalidQuery, "invalid query vertex count");
    }
    Graph query;
    query.num_vertices = static_cast<int>(parsed.vertices.size());
    query.adj.resize(parsed.vertices.size());
    query.in_adj.resize(parsed.vertices.size());
    query.vertex_labels.reserve(parsed.vertices.size());
    query.external_ids.reserve(parsed.vertices.size());
    for (std::size_t index = 0; index < parsed.vertices.size(); ++index) {
        const std::uint8_t label = parsed.vertices[index].runtime_class_id;
        if (label >= common::kRuntimeLabelCount || label >= kLabelCount) {
            fail(
                common::ErrorCode::kInvalidQuery,
                "query runtime label is outside explicit A-through-E");
        }
        query.vertex_labels.push_back(static_cast<Label>(label));
        query.external_ids.push_back(static_cast<int>(index));
    }

    std::set<std::pair<std::uint32_t, std::uint32_t>> directed_edges;
    for (const common::QueryDirectedEdge& edge : parsed.directed_edges) {
        if (edge.source_index >= parsed.vertices.size() ||
            edge.target_index >= parsed.vertices.size() ||
            !directed_edges.emplace(
                 edge.source_index, edge.target_index).second) {
            fail(
                common::ErrorCode::kInvalidQuery,
                "query contains an invalid or duplicate directed edge");
        }
        query.adj[edge.source_index].push_back({
            static_cast<int>(edge.target_index), -1,
        });
        query.in_adj[edge.target_index].push_back({
            static_cast<int>(edge.source_index), -1,
        });
    }
    if (directed_edges.empty()) {
        fail(common::ErrorCode::kInvalidQuery, "query has no directed edge");
    }
    for (auto& neighbors : query.adj) {
        std::sort(
            neighbors.begin(), neighbors.end(),
            [](const Edge& lhs, const Edge& rhs) {
                return lhs.to < rhs.to;
            });
    }
    for (auto& neighbors : query.in_adj) {
        std::sort(
            neighbors.begin(), neighbors.end(),
            [](const Edge& lhs, const Edge& rhs) {
                return lhs.to < rhs.to;
            });
    }
    return query;
}

common::InputIdentitySet captureInputIdentities(
    const common::ValidatedInputs& inputs,
    const common::ClosedInvocation& invocation,
    const plans::ConsumedTypedPlan& consumed_plan) {
    common::InputIdentitySet identities;
    identities.prepared_graph = inputs.prepared_graph.identity();
    identities.explicit_label_map = inputs.explicit_label_map.identity();
    identities.query_projection = inputs.query_projection.identity();
    identities.query = inputs.query.identity();
    identities.minimum_duration_k = invocation.minimum_duration_k;
    identities.typed_plan = consumed_plan.identity();
    return identities;
}

InternalPlanConsumptionEvidence initialConsumptionEvidence(
    common::ArmId arm,
    const plans::ConsumedTypedPlan& consumed_plan) {
    const plans::TypedPlanConsumptionRecord& record =
        consumed_plan.record();
    const common::FileIdentity& identity = consumed_plan.identity();
    if (record.plan_consumed ||
        record.lexical_path != identity.path ||
        record.device != identity.device ||
        record.inode != identity.inode ||
        record.file_sha256 != identity.sha256 ||
        record.bytes != identity.bytes ||
        record.mode != identity.mode ||
        record.nlink != identity.nlink ||
        record.file_type != identity.file_type ||
        !identity.object_sha256.has_value() ||
        *identity.object_sha256 != record.runtime_object_sha256 ||
        record.payload_object_sha256 !=
            consumed_plan.typedValue().plan_payload_sha256) {
        fail(
            common::ErrorCode::kInvalidChildReceipt,
            "typed-plan loader identity/negative-consumption record drift");
    }
    InternalPlanConsumptionEvidence evidence;
    evidence.backend_arm = arm;
    evidence.loader_record_plan_consumed = record.plan_consumed;
    evidence.lexical_path = record.lexical_path;
    evidence.device = record.device;
    evidence.inode = record.inode;
    evidence.file_sha256 = record.file_sha256;
    evidence.bytes = record.bytes;
    evidence.mode = record.mode;
    evidence.nlink = record.nlink;
    evidence.file_type = record.file_type;
    evidence.payload_object_sha256 = record.payload_object_sha256;
    evidence.runtime_object_sha256 = record.runtime_object_sha256;
    return evidence;
}

common::InternalPlanConsumptionObservation receiptConsumptionObservation(
    const InternalPlanConsumptionEvidence& evidence) {
    common::InternalPlanConsumptionObservation observation;
    observation.backend_arm = evidence.backend_arm;
    observation.plan_consumed = evidence.plan_consumed;
    observation.loader_record_plan_consumed =
        evidence.loader_record_plan_consumed;
    observation.same_executable_plan_object =
        evidence.same_executable_plan_object;
    observation.exact_executable_decomposition_passed_to_td_tree =
        evidence.exact_executable_decomposition_passed_to_td_tree;
    observation.payload_object_sha256 = evidence.payload_object_sha256;
    observation.runtime_object_sha256 = evidence.runtime_object_sha256;
    return observation;
}

class InternalNativePhaseBridge final : public GopherE3E4NativeObserver {
public:
    InternalNativePhaseBridge(
        common::PhaseLedger& ledger,
        common::ArmId arm,
        const QueryPlan& expected_plan,
        InternalPlanConsumptionEvidence& consumption)
        : ledger_(ledger),
          arm_(arm),
          expected_plan_(expected_plan),
          consumption_(consumption) {}

    void onGopherE3E4NativePhase(
        GopherE3E4NativePhase phase,
        const hooks::MonotonicClockReading& reading,
        const QueryPlan& executable_plan,
        bool dag_backend) override {
        const bool expected_dag =
            arm_ == common::ArmId::kFinalDurableDag;
        if (&executable_plan != &expected_plan_ ||
            dag_backend != expected_dag ||
            consumption_.backend_arm != arm_ ||
            consumption_.loader_record_plan_consumed) {
            fail(
                common::ErrorCode::kInvalidChildReceipt,
                "selected backend did not use the held executable plan object");
        }
        const std::uint64_t now_ns = reading.authorityNanoseconds();
        if (phase == GopherE3E4NativePhase::Enumeration) {
            if (state_ != State::Ready || consumption_.plan_consumed ||
                consumption_.same_executable_plan_object) {
                fail(
                    common::ErrorCode::kInvalidPhaseLedger,
                    "native enumeration boundary repeated or out of order");
            }
            // This is the first positive consumption point: the selected
            // native backend has entered execution with the exact reference
            // retained from ConsumedTypedPlan::executablePlan().
            consumption_.same_executable_plan_object = true;
            consumption_.plan_consumed = true;
            if (!expected_dag) {
                ledger_.transition(
                    common::CommonPhase::kDagCompilation,
                    "gopher.prior-direct.candidate-dag-not-applicable",
                    false,
                    now_ns);
            }
            ledger_.transition(
                common::CommonPhase::kEnumeration,
                expected_dag
                    ? "gopher.final-durable-dag.enumeration"
                    : "gopher.prior-direct.enumeration",
                true,
                now_ns);
            state_ = State::Enumerating;
            return;
        }
        if (state_ != State::Enumerating ||
            !consumption_.plan_consumed ||
            !consumption_.same_executable_plan_object) {
            fail(
                common::ErrorCode::kInvalidPhaseLedger,
                "native count-reduction boundary is out of order");
        }
        ledger_.transition(
            common::CommonPhase::kOutputFinalization,
            expected_dag
                ? "gopher.final-durable-dag.count-reduction"
                : "gopher.prior-direct.count-reduction",
            true,
            now_ns);
        state_ = State::Reducing;
    }

    void proveFinished() const {
        if (state_ != State::Reducing ||
            !consumption_.plan_consumed ||
            !consumption_.same_executable_plan_object) {
            fail(
                common::ErrorCode::kInvalidPhaseLedger,
                "native Gopher phase sequence ended before count reduction");
        }
    }

private:
    enum class State : std::uint8_t {
        Ready,
        Enumerating,
        Reducing,
    };

    common::PhaseLedger& ledger_;
    common::ArmId arm_;
    const QueryPlan& expected_plan_;
    InternalPlanConsumptionEvidence& consumption_;
    State state_ = State::Ready;
};

GopherMatcherOptions closedMatcherOptions(common::ArmId arm) {
    GopherMatcherOptions options;
    options.threads = common::kRequiredThreads;
    options.parallel_mode = ParallelMode::Roots;
    options.prefix_depth = 0U;
    options.tasks_per_thread = 8U;
    options.order_mode = OrderMode::Paper;
    options.early_prune = true;
    options.result_digest = false;
    options.executor.memo_mode =
        arm == common::ArmId::kFinalDurableDag
            ? MemoMode::Dag
            : MemoMode::None;
    options.executor.memo_policy = MemoPolicy::Legacy;
    options.executor.runtime_key_mode = RuntimeKeyMode::RequiredBindings;
    options.executor.runtime_key_diagnostics =
        RuntimeKeyDiagnosticsMode::Off;
    options.executor.exact_runtime_key_cap = 0U;
    options.executor.memo_options.budget_bytes = 256ULL << 20;
    options.executor.memo_options.max_entry_bytes = 8ULL << 20;
    options.executor.memo_options.shard_count = 64U;
    options.executor.memo_options.minimum_estimated_work = 0U;
    options.executor.memo_options.cost_aware_admission = false;
    options.executor.memo_options.collect_statistics = false;
    options.executor.verify_dag = false;
    options.executor.collect_statistics = false;
    return options;
}

GopherE3E4ObservedRun runSelectedBackend(
    common::ArmId arm,
    const Graph& temporal_graph,
    const Graph& query_graph,
    const plans::ConsumedTypedPlan& consumed_plan,
    const TDTree& td_tree,
    int minimum_duration,
    common::PhaseLedger& ledger,
    std::uint64_t td_end_ns,
    InternalPlanConsumptionEvidence& consumption) {
    const QueryPlan& executable_plan = consumed_plan.executablePlan();
    const QueryDecomposition& executable_decomposition =
        consumed_plan.executableDecomposition();
    InternalNativePhaseBridge observer(
        ledger, arm, executable_plan, consumption);
    GopherE3E4ObservedRun observed;
    if (arm == common::ArmId::kPriorDirect) {
        // No CandidateDag object is constructed in this lexical branch.
        ledger.transition(
            common::CommonPhase::kPlanning,
            "gopher.prior-direct.backend-setup",
            true,
            td_end_ns);
        GopherMatcher matcher(
            temporal_graph,
            query_graph,
            executable_plan,
            td_tree,
            minimum_duration,
            closedMatcherOptions(arm));
        observed = AdapterProductionAccess::run(matcher, observer);
    } else if (arm == common::ArmId::kFinalDurableDag) {
        ledger.transition(
            common::CommonPhase::kDagCompilation,
            "gopher.final-durable-dag.compile-and-backend-setup",
            true,
            td_end_ns);
        CandidateDagCompiler compiler;
        CandidateDag dag = compiler.compile(
            query_graph,
            executable_plan,
            executable_decomposition,
            CandidateDagInternMode::Intern,
            nullptr);
        GopherMatcher matcher(
            temporal_graph,
            query_graph,
            executable_plan,
            dag,
            td_tree,
            minimum_duration,
            closedMatcherOptions(arm));
        observed = AdapterProductionAccess::run(matcher, observer);
    } else {
        fail(
            common::ErrorCode::kInvalidInvocation,
            "non-internal arm reached Gopher backend dispatch");
    }
    observer.proveFinished();
    return observed;
}

}  // namespace

InternalSourceOnlyResult runInternalClosedCountOnly(
    int argc,
    const char* const argv[]) {
    const hooks::MonotonicNowSource& clock =
        hooks::productionMonotonicNowSource();
    const std::uint64_t child_start_ns = authorityNow(clock);
    common::RepositoryAnchor repository =
        common::RepositoryAnchor::openAbsolute(
            std::string(common::kCanonicalRepositoryRoot));

    const common::ArmId arm = dispatchArm(argc, argv);
    common::ClosedInvocation invocation =
        common::parseClosedInvocation(argc, argv, arm);
    if (!common::isInternalArm(arm) ||
        !invocation.count_only ||
        invocation.threads != common::kRequiredThreads ||
        !invocation.typed_plan_path.has_value()) {
        fail(
            common::ErrorCode::kInvalidInvocation,
            "closed internal count-only invocation contract drift");
    }

    // These are the sole four measured-excluded input loads.  No typed-plan
    // access and no graph/query materialization occurs before the boundary.
    common::ValidatedInputs inputs =
        common::loadValidatedInputs(repository, invocation);
    const std::uint64_t boundary_ns = authorityNow(clock);
    common::PhaseLedger ledger(arm, boundary_ns);
    ledger.start(
        common::CommonPhase::kPlanning,
        "gopher.held-input-materialization-and-fresh-paper-planning",
        true,
        boundary_ns);

    Graph temporal_graph = materializeTemporalGraph(inputs);
    Graph query_graph = materializeQueryGraph(inputs.parsed_query);
    plans::FreshPaperPlanning fresh_planning =
        plans::buildFreshPaperPlanAfterBoundary(
            inputs,
            temporal_graph,
            query_graph,
            boundary_ns);
    plans::ConsumedTypedPlan consumed_plan =
        plans::consumeTypedPlanAfterBoundary(
            repository,
            invocation,
            inputs,
            query_graph,
            fresh_planning);
    InternalPlanConsumptionEvidence consumption =
        initialConsumptionEvidence(arm, consumed_plan);

    const QueryDecomposition& executable_decomposition =
        consumed_plan.executableDecomposition();
    const std::uint64_t td_start_ns = authorityNow(clock);
    ledger.transition(
        common::CommonPhase::kTdConstruction,
        "gopher.td-tree-construction-from-held-plan",
        true,
        td_start_ns);
    TDTree td_tree(
        temporal_graph,
        query_graph,
        executable_decomposition,
        static_cast<int>(invocation.minimum_duration_k));
    consumption.exact_executable_decomposition_passed_to_td_tree = true;
    const std::uint64_t td_end_ns = authorityNow(clock);

    GopherE3E4ObservedRun observed = runSelectedBackend(
        arm,
        temporal_graph,
        query_graph,
        consumed_plan,
        td_tree,
        static_cast<int>(invocation.minimum_duration_k),
        ledger,
        td_end_ns,
        consumption);
    if (observed.executable_plan_address !=
            &consumed_plan.executablePlan() ||
        observed.dag_backend !=
            (arm == common::ArmId::kFinalDurableDag) ||
        !consumption.plan_consumed ||
        !consumption.same_executable_plan_object ||
        !consumption.exact_executable_decomposition_passed_to_td_tree ||
        consumed_plan.record().plan_consumed ||
        observed.summary.output_written ||
        (observed.summary.count_overflow &&
         observed.summary.match_count !=
             std::numeric_limits<std::uint64_t>::max()) ||
        observed.summary.threads_used != common::kRequiredThreads ||
        observed.summary.parallel_mode_requested != ParallelMode::Roots ||
        observed.summary.parallel_mode_effective != "roots" ||
        observed.summary.result_digest != "disabled") {
        fail(
            common::ErrorCode::kInvalidChildReceipt,
            "closed native Gopher result/plan-consumption contract drift");
    }

    const hooks::ExactWorkerAffinityProof affinity_proof =
        hooks::aggregateExact16WorkerAffinity(observed.worker_affinity);
    if (!affinity_proof.exact() ||
        !affinity_proof.authorityEligible()) {
        fail(
            common::ErrorCode::kInvalidChildReceipt,
            affinity_proof.failureReason().empty()
                ? "native Gopher affinity proof is not authoritative"
                : affinity_proof.failureReason());
    }
    common::ThreadObservation threads =
        affinity_proof.toAuthorityThreadObservation();

    // Final descriptor and lexical reattachment proofs remain inside the
    // output/count-finalization interval opened at the native reduction call.
    inputs.proveStable();
    consumed_plan.proveStable();
    repository.proveStable();
    const std::uint64_t primary_end_ns = authorityNow(clock);
    common::PhaseLedgerSnapshot phase_ledger =
        ledger.finish(primary_end_ns);

    common::InputIdentitySet input_identities =
        captureInputIdentities(inputs, invocation, consumed_plan);
    common::ChildReceiptValue receipt_value;
    receipt_value.governance_id = std::string(common::kGovernanceId);
    receipt_value.arm = arm;
    receipt_value.input_identities = input_identities;
    receipt_value.loads = inputs.loads;
    receipt_value.ledger = phase_ledger;
    receipt_value.match_count = observed.summary.match_count;
    receipt_value.count_overflow = observed.summary.count_overflow;
    receipt_value.threads = threads;
    receipt_value.plan_consumption =
        receiptConsumptionObservation(consumption);
    receipt_value.child_wall_segments.clock = "steady_clock";
    receipt_value.child_wall_segments.unit = "ns";
    receipt_value.child_wall_segments.preboundary_ns = checkedElapsed(
        child_start_ns, boundary_ns, "preboundary");
    receipt_value.child_wall_segments.primary_total_ns =
        phase_ledger.totals.prepared_query_total_ns;
    const std::uint64_t postboundary_end_ns = authorityNow(clock);
    receipt_value.child_wall_segments.postboundary_ns = checkedElapsed(
        primary_end_ns, postboundary_end_ns, "postboundary");

    InternalSourceOnlyResult result;
    result.invocation = std::move(invocation);
    result.input_identities = std::move(input_identities);
    result.loads = inputs.loads;
    result.phase_ledger = std::move(phase_ledger);
    result.match_summary = std::move(observed.summary);
    result.threads = std::move(threads);
    result.plan_consumption = std::move(consumption);
    result.canonical_child_receipt =
        common::serializeCanonicalChildReceipt(receipt_value);
    return result;
}

}  // namespace dsm::e3e4::gopher_adapter_v1
