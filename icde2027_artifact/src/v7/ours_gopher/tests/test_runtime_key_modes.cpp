#include "../BoundedMemo.h"
#include "../DagExecutor.h"
#include "../GopherMatcher.h"
#include "../TDTree.h"
#include "../Utils.h"
#include "../query_decomposition.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// This fixture compiles P0 only. The operation inventory now also includes
// three opt-in rewrite operations; they must stay absent, not be exercised,
// in this baseline/memo fixture. Accounting below still spans all ten slots.
constexpr std::size_t kP0OperationCount =
    static_cast<std::size_t>(DagOp::LongestConsecutiveRun) + 1;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

RuntimeKey staticNodeOnlyKey(const DagNode& node) {
    // Deliberately unsound and confined to this negative fixture. There is no
    // corresponding RuntimeKeyMode or production parser value.
    RuntimeKey key;
    key.static_fingerprint = node.static_fingerprint;
    key.node_id = node.id;
    return key;
}

void testModeContract() {
    RuntimeKeyMode parsed = RuntimeKeyMode::RequiredBindings;
    require(
        parseRuntimeKeyMode("full-prefix", parsed) &&
            parsed == RuntimeKeyMode::FullPrefix &&
            std::string(runtimeKeyModeName(parsed)) == "full-prefix",
        "full-prefix key mode must round-trip");
    require(
        parseRuntimeKeyMode("required-bindings", parsed) &&
            parsed == RuntimeKeyMode::RequiredBindings &&
            std::string(runtimeKeyModeName(parsed)) ==
                "required-bindings",
        "required-bindings key mode must round-trip");
    require(
        !parseRuntimeKeyMode("static-only", parsed),
        "static-only must not be a production key mode");

    RuntimeKeyDiagnosticsMode diagnostics =
        RuntimeKeyDiagnosticsMode::Off;
    require(
        parseRuntimeKeyDiagnosticsMode("exact", diagnostics) &&
            diagnostics == RuntimeKeyDiagnosticsMode::Exact &&
            std::string(runtimeKeyDiagnosticsModeName(diagnostics)) ==
                "exact",
        "exact diagnostic mode must round-trip");
    require(
        parseRuntimeKeyDiagnosticsMode("off", diagnostics) &&
            diagnostics == RuntimeKeyDiagnosticsMode::Off &&
            !parseRuntimeKeyDiagnosticsMode("approximate", diagnostics),
        "diagnostic mode must default/fail closed to off/exact only");
}

void testProjectionAndOverlay() {
    DagNode node;
    node.id = 17;
    node.static_fingerprint = 0x123456789abcdef0ULL;
    node.dependencies = {0, 2};

    SearchState first;
    first.mapping = {3, 4, -1};
    SearchState irrelevant_changed = first;
    irrelevant_changed.mapping[1] = 99;

    const RuntimeKey full_first = buildRuntimeKey(
        RuntimeKeyMode::FullPrefix, node, first, 2, 8);
    const RuntimeKey full_irrelevant_changed = buildRuntimeKey(
        RuntimeKeyMode::FullPrefix,
        node,
        irrelevant_changed,
        2,
        8);
    const RuntimeKey dep_first = buildRuntimeKey(
        RuntimeKeyMode::RequiredBindings, node, first, 2, 8);
    const RuntimeKey dep_irrelevant_changed = buildRuntimeKey(
        RuntimeKeyMode::RequiredBindings,
        node,
        irrelevant_changed,
        2,
        8);

    require(
        full_first.bindings == std::vector<int>({3, 4, 8}),
        "full-prefix key must encode every mapping position and overlay");
    require(
        dep_first.bindings == std::vector<int>({3, 8}),
        "dependency key must encode only compiled required bindings");
    require(
        !(full_first == full_irrelevant_changed),
        "full-prefix key must retain irrelevant bound positions");
    require(
        dep_first == dep_irrelevant_changed,
        "dependency key must project away irrelevant bound positions");

    const RuntimeKey full_other_candidate = buildRuntimeKey(
        RuntimeKeyMode::FullPrefix, node, first, 2, 9);
    const RuntimeKey dep_other_candidate = buildRuntimeKey(
        RuntimeKeyMode::RequiredBindings, node, first, 2, 9);
    require(
        !(full_first == full_other_candidate) &&
            !(dep_first == dep_other_candidate),
        "both valid keys must include a required candidate overlay");
    require(
        RuntimeKeyHash{}(dep_first) ==
            RuntimeKeyHash{}(dep_irrelevant_changed),
        "equal dependency keys must hash identically");
}

void testStaticNodeOnlyAliasesIncorrectly() {
    DagNode node;
    node.id = 23;
    node.static_fingerprint = 0x0fedcba987654321ULL;
    node.dependencies = {0};

    SearchState first;
    first.mapping = {11, 5};
    SearchState second;
    second.mapping = {12, 5};

    const RuntimeKey first_dep = buildRuntimeKey(
        RuntimeKeyMode::RequiredBindings, node, first);
    const RuntimeKey second_dep = buildRuntimeKey(
        RuntimeKeyMode::RequiredBindings, node, second);
    require(
        !(first_dep == second_dep),
        "sound dependency identity must distinguish required bindings");

    const RuntimeKey first_static = staticNodeOnlyKey(node);
    const RuntimeKey second_static = staticNodeOnlyKey(node);
    require(
        first_static == second_static,
        "negative fixture requires the static-only alias");

    MemoOptions options;
    options.budget_bytes = 1ULL << 20;
    options.max_entry_bytes = 1ULL << 20;
    options.shard_count = 1;
    options.cost_aware_admission = false;
    BoundedMemo<RuntimeKey, int, RuntimeKeyHash> memo(options);

    memo.insertOrGet(
        first_static,
        std::make_shared<const int>(first.mapping[0]),
        0,
        1);
    const std::shared_ptr<const int> aliased = memo.find(second_static);
    require(
        aliased && *aliased == first.mapping[0] &&
            *aliased != second.mapping[0],
        "static-node-only key must deterministically demonstrate a wrong hit");
}

void addTemporalEdge(Graph& graph, int source, int target) {
    const int edge_id = static_cast<int>(graph.temporal_edges.size());
    graph.temporal_edges.emplace_back(
        source,
        target,
        std::vector<TimeInterval>{{1, 5}},
        5);
    graph.adj[static_cast<std::size_t>(source)].push_back(
        {target, edge_id});
    graph.in_adj[static_cast<std::size_t>(target)].push_back(
        {source, edge_id});
}

void addQueryEdge(Graph& query, int source, int target) {
    query.adj[static_cast<std::size_t>(source)].push_back({target, -1});
    query.in_adj[static_cast<std::size_t>(target)].push_back({source, -1});
}

void finishDataGraph(Graph& graph) {
    graph.vertex_active_durations.assign(
        static_cast<std::size_t>(graph.num_vertices), 5);
    graph.neighbor_label_counts.assign(
        static_cast<std::size_t>(graph.num_vertices), {});
    graph.incoming_neighbor_label_counts.assign(
        static_cast<std::size_t>(graph.num_vertices), {});
    for (int vertex = 0; vertex < graph.num_vertices; ++vertex) {
        auto& outgoing = graph.adj[static_cast<std::size_t>(vertex)];
        auto& incoming = graph.in_adj[static_cast<std::size_t>(vertex)];
        std::sort(
            outgoing.begin(), outgoing.end(),
            [](const Edge& lhs, const Edge& rhs) {
                return lhs.to < rhs.to;
            });
        std::sort(
            incoming.begin(), incoming.end(),
            [](const Edge& lhs, const Edge& rhs) {
                return lhs.to < rhs.to;
            });
        for (const Edge& edge : outgoing) {
            ++graph.neighbor_label_counts[
                static_cast<std::size_t>(vertex)]
                [graph.vertex_labels[
                    static_cast<std::size_t>(edge.to)]];
        }
        for (const Edge& edge : incoming) {
            ++graph.incoming_neighbor_label_counts[
                static_cast<std::size_t>(vertex)]
                [graph.vertex_labels[
                    static_cast<std::size_t>(edge.to)]];
        }
    }
}

Graph makeParityDataGraph() {
    Graph graph;
    graph.num_vertices = 6;
    graph.vertex_labels = {
        labelFromString("A"),
        labelFromString("B"),
        labelFromString("B"),
        labelFromString("C"),
        labelFromString("C"),
        labelFromString("C")};
    graph.external_ids = {100, 101, 102, 103, 104, 105};
    graph.adj.resize(6);
    graph.in_adj.resize(6);

    addTemporalEdge(graph, 0, 1);
    addTemporalEdge(graph, 0, 2);
    for (int candidate = 3; candidate <= 5; ++candidate) {
        addTemporalEdge(graph, 0, candidate);
        addTemporalEdge(graph, candidate, 0);
    }
    addTemporalEdge(graph, 1, 3);
    addTemporalEdge(graph, 1, 4);
    addTemporalEdge(graph, 2, 4);
    addTemporalEdge(graph, 2, 5);
    finishDataGraph(graph);
    return graph;
}

Graph makeParityQueryGraph() {
    Graph query;
    query.num_vertices = 3;
    query.vertex_labels = {
        labelFromString("A"),
        labelFromString("B"),
        labelFromString("C")};
    query.external_ids = {0, 1, 2};
    query.adj.resize(3);
    query.in_adj.resize(3);
    addQueryEdge(query, 0, 1);
    addQueryEdge(query, 0, 2);
    addQueryEdge(query, 2, 0);
    addQueryEdge(query, 1, 2);
    return query;
}

QueryDecomposition makeParityDecomposition(
    const Graph& graph,
    const Graph& query) {
    std::array<std::size_t, kLabelCount> counts{};
    std::array<double, kLabelCount> lifespans{};
    for (Label label : graph.vertex_labels) ++counts[label];
    for (std::size_t label = 0; label < kLabelCount; ++label) {
        if (counts[label] != 0) lifespans[label] = 5.0;
    }
    return decomposeQuery(
        query,
        counts,
        lifespans,
        SelectivityMode::Paper);
}

struct ParityRun {
    GopherMatchSummary summary;
    std::string rows;
};

ParityRun runParityArm(
    const Graph& graph,
    const Graph& query,
    const QueryPlan& plan,
    const CandidateDag& dag,
    const TDTree& tree,
    const DagExecutorOptions& executor_options) {
    GopherMatcherOptions options;
    options.threads = 1;
    options.parallel_mode = ParallelMode::Roots;
    options.order_mode = OrderMode::Paper;
    options.early_prune = true;
    options.result_digest = true;
    options.executor = executor_options;

    GopherMatcher matcher(
        graph,
        query,
        plan,
        dag,
        tree,
        3,
        options);
    std::ostringstream rows;
    ParityRun run;
    run.summary = matcher.run(MatchOutputMode::Full, &rows);
    run.rows = rows.str();
    return run;
}

DagExecutorOptions makeMemoArm(RuntimeKeyMode key_mode) {
    DagExecutorOptions options;
    options.memo_mode = MemoMode::Dag;
    options.memo_policy = MemoPolicy::Legacy;
    options.runtime_key_mode = key_mode;
    options.verify_dag = true;
    options.collect_statistics = true;
    options.memo_options.budget_bytes = 1ULL << 20;
    options.memo_options.max_entry_bytes = 1ULL << 18;
    options.memo_options.shard_count = 4;
    options.memo_options.minimum_estimated_work = 0;
    options.memo_options.cost_aware_admission = false;
    return options;
}

DagExecutorOptions makeExactKeyArm(
    MemoMode memo_mode,
    RuntimeKeyMode key_mode,
    std::uint64_t cap = 1000000) {
    DagExecutorOptions options = makeMemoArm(key_mode);
    options.memo_mode = memo_mode;
    options.runtime_key_diagnostics = RuntimeKeyDiagnosticsMode::Exact;
    options.exact_runtime_key_cap = cap;
    return options;
}

void requireCommonMemoControls(
    const DagExecutorOptions& full,
    const DagExecutorOptions& dependencies) {
    require(
        full.memo_mode == MemoMode::Dag &&
            dependencies.memo_mode == MemoMode::Dag &&
            full.memo_policy == MemoPolicy::Legacy &&
            dependencies.memo_policy == MemoPolicy::Legacy,
        "both memo identities must share all-operator legacy DAG coverage");
    require(
        !full.memo_options.cost_aware_admission &&
            !dependencies.memo_options.cost_aware_admission &&
            full.memo_options.minimum_estimated_work == 0 &&
            dependencies.memo_options.minimum_estimated_work == 0,
        "both memo identities must share eager non-cost-aware admission");
    require(
        full.memo_options.budget_bytes ==
                dependencies.memo_options.budget_bytes &&
            full.memo_options.max_entry_bytes ==
                dependencies.memo_options.max_entry_bytes &&
            full.memo_options.shard_count ==
                dependencies.memo_options.shard_count,
        "both memo identities must share hard limits and sharding");
}

void requireMemoOperatorAccounting(
    const SearchStatistics& statistics);

void testEndToEndArmParity() {
    const Graph graph = makeParityDataGraph();
    const Graph query = makeParityQueryGraph();
    const QueryDecomposition decomposition =
        makeParityDecomposition(graph, query);
    require(
        decomposition.connected && decomposition.root == 0,
        "parity fixture must freeze q0 as the paper-order root");
    const TDTree tree(graph, query, decomposition, 3);
    const QueryPlan plan = makeQueryPlan(
        query, decomposition, tree, OrderMode::Paper);
    CandidateDagCompiler compiler;
    const CandidateDag dag = compiler.compile(
        query, plan, decomposition);
    const CandidateDagTopology topology = dag.topologySummary();
    for (std::size_t operation = 0;
         operation < topology.op_node_counts.size();
         ++operation) {
        require(
            operation < kP0OperationCount
                ? topology.op_node_counts[operation] > 0
                : topology.op_node_counts[operation] == 0,
            "P0 parity fixture must exercise the original seven operations and no rewrite operations");
    }

    const DagExecutorOptions full_options = makeExactKeyArm(
        MemoMode::Dag, RuntimeKeyMode::FullPrefix);
    const DagExecutorOptions dependency_options = makeExactKeyArm(
        MemoMode::Dag, RuntimeKeyMode::RequiredBindings);
    requireCommonMemoControls(full_options, dependency_options);
    DagExecutorOptions no_memo_options = dependency_options;
    no_memo_options.memo_mode = MemoMode::None;

    const ParityRun no_memo = runParityArm(
        graph, query, plan, dag, tree, no_memo_options);
    const ParityRun full = runParityArm(
        graph, query, plan, dag, tree, full_options);
    const ParityRun dependencies = runParityArm(
        graph, query, plan, dag, tree, dependency_options);

    require(
        no_memo.summary.match_count == 4 &&
            full.summary.match_count == no_memo.summary.match_count &&
            dependencies.summary.match_count ==
                no_memo.summary.match_count,
        "NoMemo, FullPrefix, and DepKey counts must match the oracle");
    require(
        !no_memo.summary.count_overflow &&
            !full.summary.count_overflow &&
            !dependencies.summary.count_overflow,
        "parity fixture counts must not overflow");
    require(
        full.summary.result_digest == no_memo.summary.result_digest &&
            dependencies.summary.result_digest ==
                no_memo.summary.result_digest &&
            no_memo.summary.result_digest != "disabled",
        "NoMemo, FullPrefix, and DepKey embedding digests must match");
    require(
        full.rows == no_memo.rows &&
            dependencies.rows == no_memo.rows &&
            !no_memo.rows.empty(),
        "NoMemo, FullPrefix, and DepKey full embedding rows must match");

    auto require_all_cache_classes = [](const ParityRun& run) {
        require(
            run.summary.statistics.structural_cache_misses > 0 &&
                run.summary.statistics.temporal_cache_misses > 0 &&
                run.summary.statistics.longest_cache_misses > 0,
            "memo arm must probe structural, temporal, and longest caches");
    };
    require_all_cache_classes(full);
    require_all_cache_classes(dependencies);
    require(
        full.summary.statistics.exact_runtime_key_diagnostics &&
            dependencies.summary.statistics
                .exact_runtime_key_diagnostics &&
            full.summary.statistics.cache_lookup_nanoseconds > 0 &&
            dependencies.summary.statistics.cache_lookup_nanoseconds > 0,
        "FullPrefix and DepKey memo arms must expose separate exact-key "
        "and cache-lookup timing diagnostics");
    auto require_lookup_sum = [](const ParityRun& run) {
        std::uint64_t lookup_sum = 0;
        for (std::uint64_t nanoseconds :
             run.summary.statistics.cache_lookup_nanoseconds_by_op) {
            lookup_sum += nanoseconds;
        }
        require(
            lookup_sum ==
                run.summary.statistics.cache_lookup_nanoseconds,
            "per-op cache lookup ns must sum to the separate overall value");
    };
    require_lookup_sum(full);
    require_lookup_sum(dependencies);
    requireMemoOperatorAccounting(full.summary.statistics);
    requireMemoOperatorAccounting(dependencies.summary.statistics);
}

void requireExactAllOperatorAccounting(
    const SearchStatistics& statistics) {
    require(
        statistics.exact_runtime_key_diagnostics &&
            statistics.exact_runtime_key_cap == 1000000,
        "exact-key diagnostic contract must be reported");
    std::uint64_t invocation_sum = 0;
    std::uint64_t distinct_sum = 0;
    std::uint64_t construction_sum = 0;
    std::uint64_t structural_compute_sum = 0;
    std::uint64_t temporal_compute_sum = 0;
    for (std::size_t operation = 0;
         operation < kDagOpCount;
         ++operation) {
        require(
            operation < kP0OperationCount
                ? statistics.exact_runtime_key_invocations_by_op[operation] > 0
                : statistics.exact_runtime_key_invocations_by_op[operation] == 0,
            "P0 exact diagnostics must exercise seven baseline ops and zero rewrite ops");
        require(
            (operation < kP0OperationCount
                ? statistics.exact_runtime_key_distinct_by_op[operation] > 0
                : statistics.exact_runtime_key_distinct_by_op[operation] == 0) &&
                statistics.exact_runtime_key_distinct_by_op[operation] <=
                    statistics
                        .exact_runtime_key_invocations_by_op[operation],
            "baseline ops need nonempty distinct keys; absent rewrite ops need zero keys");
        invocation_sum +=
            statistics.exact_runtime_key_invocations_by_op[operation];
        distinct_sum +=
            statistics.exact_runtime_key_distinct_by_op[operation];
        construction_sum +=
            statistics
                .runtime_key_construction_nanoseconds_by_op[operation];
        require(
            statistics.dag_operator_invocations_by_op[operation] ==
                    statistics
                        .exact_runtime_key_invocations_by_op[operation] &&
                statistics
                        .dag_operator_result_observations_by_op[operation] ==
                    statistics.dag_operator_invocations_by_op[operation] &&
                statistics.dag_operator_cache_hits_by_op[operation] == 0 &&
                statistics.dag_operator_cache_misses_by_op[operation] == 0 &&
                statistics
                        .dag_operator_exclusive_miss_compute_nanoseconds_by_op[
                            operation] == 0 &&
                statistics
                        .dag_operator_cache_admissions_by_op[operation] == 0 &&
                statistics
                        .dag_operator_cache_admitted_bytes_by_op[operation] ==
                    0,
            "NoMemo must account every successful return without inventing "
            "cache probes, misses, or admissions");
        if (operation < dagOpIndex(DagOp::EdgeTimeLookup)) {
            structural_compute_sum +=
                statistics
                    .dag_operator_exclusive_compute_nanoseconds_by_op[
                        operation];
        } else if (
            operation < dagOpIndex(DagOp::LongestConsecutiveRun)) {
            temporal_compute_sum +=
                statistics
                    .dag_operator_exclusive_compute_nanoseconds_by_op[
                        operation];
        }
    }
    require(
        invocation_sum == statistics.exact_runtime_key_invocations &&
            distinct_sum == statistics.exact_runtime_key_distinct,
        "overall I and U must equal the exact per-op sums");
    require(
        construction_sum ==
            statistics.runtime_key_construction_nanoseconds,
        "overall key-construction time must equal the per-op sum");
    require(
        statistics.dag_operator_reconciliation_passed &&
            structural_compute_sum ==
                statistics.structural_compute_nanoseconds &&
            temporal_compute_sum ==
                statistics.temporal_compute_nanoseconds &&
            statistics
                    .dag_operator_exclusive_compute_nanoseconds_by_op[
                        dagOpIndex(DagOp::LongestConsecutiveRun)] ==
                statistics.longest_compute_nanoseconds &&
            statistics.longest_run_returned_length_observations ==
                statistics.dag_operator_invocations_by_op[
                    dagOpIndex(DagOp::LongestConsecutiveRun)] &&
            statistics.dag_operator_result_units_sum_by_op[
                dagOpIndex(DagOp::LongestConsecutiveRun)] ==
                statistics.dag_operator_result_observations_by_op[
                    dagOpIndex(DagOp::LongestConsecutiveRun)],
        "NoMemo seven-op compute/result totals must reconcile exactly");
}

void requireMemoOperatorAccounting(
    const SearchStatistics& statistics) {
    require(
        statistics.dag_operator_reconciliation_passed,
        "memo operator statistics must pass fail-closed reconciliation");
    std::uint64_t admissions = 0;
    std::uint64_t admitted_bytes = 0;
    for (std::size_t operation = 0;
         operation < kDagOpCount;
         ++operation) {
        require(
            statistics
                        .dag_operator_result_observations_by_op[operation] ==
                    statistics.dag_operator_invocations_by_op[operation],
            "memo result observations must equal invocations across all operation slots");
        if (operation < kP0OperationCount) {
            require(
                statistics.dag_operator_invocations_by_op[operation] > 0 &&
                    statistics.dag_operator_cache_misses_by_op[operation] > 0 &&
                    statistics.dag_operator_cache_admissions_by_op[operation] > 0 &&
                    statistics.dag_operator_cache_admitted_bytes_by_op[operation] > 0,
                "P0 memo fixture must exercise misses, results, and admissions for seven baseline ops");
        } else {
            require(
                statistics.dag_operator_invocations_by_op[operation] == 0 &&
                    statistics.dag_operator_cache_hits_by_op[operation] == 0 &&
                    statistics.dag_operator_cache_misses_by_op[operation] == 0 &&
                    statistics.dag_operator_cache_admissions_by_op[operation] == 0 &&
                    statistics.dag_operator_cache_admitted_bytes_by_op[operation] == 0 &&
                    statistics.dag_operator_exclusive_compute_nanoseconds_by_op[operation] == 0,
                "P0 memo fixture must not execute or cache opt-in rewrite operations");
        }
        require(
            statistics
                    .dag_operator_exclusive_miss_compute_nanoseconds_by_op[
                        operation] <=
                statistics
                    .dag_operator_exclusive_compute_nanoseconds_by_op[
                        operation],
            "actual-miss compute must be a subset of all physical compute");
        admissions +=
            statistics.dag_operator_cache_admissions_by_op[operation];
        admitted_bytes +=
            statistics
                .dag_operator_cache_admitted_bytes_by_op[operation];
    }
    require(
        admissions ==
                statistics.structural_cache_admitted +
                    statistics.temporal_cache_admitted &&
            admitted_bytes ==
                statistics.structural_cache_bytes +
                    statistics.temporal_cache_bytes,
        "per-op admissions and charged bytes must equal the three-store "
        "aggregate (temporal aggregate includes longest-run)");
    require(
        statistics.dag_operator_cache_hits_by_op[
                dagOpIndex(DagOp::OutNeighborLookup)] +
                    statistics.dag_operator_cache_hits_by_op[
                        dagOpIndex(DagOp::InNeighborLookup)] ==
            statistics.neighbor_lookup_cache_hits,
        "out- and in-neighbor hits must remain separately visible");
}

void testExactNoMemoIdentityDiagnostics() {
    const Graph graph = makeParityDataGraph();
    const Graph query = makeParityQueryGraph();
    const QueryDecomposition decomposition =
        makeParityDecomposition(graph, query);
    const TDTree tree(graph, query, decomposition, 3);
    const QueryPlan plan = makeQueryPlan(
        query, decomposition, tree, OrderMode::Paper);
    CandidateDagCompiler compiler;
    const CandidateDag dag = compiler.compile(
        query, plan, decomposition);

    const ParityRun full = runParityArm(
        graph,
        query,
        plan,
        dag,
        tree,
        makeExactKeyArm(
            MemoMode::None,
            RuntimeKeyMode::FullPrefix));
    const ParityRun dependencies = runParityArm(
        graph,
        query,
        plan,
        dag,
        tree,
        makeExactKeyArm(
            MemoMode::None,
            RuntimeKeyMode::RequiredBindings));

    require(
        full.summary.match_count == dependencies.summary.match_count &&
            full.summary.result_digest ==
                dependencies.summary.result_digest &&
            full.rows == dependencies.rows,
        "exact diagnostics must not change NoMemo results");
    requireExactAllOperatorAccounting(full.summary.statistics);
    requireExactAllOperatorAccounting(dependencies.summary.statistics);

    const std::uint64_t invocation_count =
        dependencies.summary.statistics
            .exact_runtime_key_invocations;
    const std::uint64_t distinct_count =
        dependencies.summary.statistics
            .exact_runtime_key_distinct;
    require(
        invocation_count ==
                full.summary.statistics
                    .exact_runtime_key_invocations &&
            invocation_count > distinct_count,
        "NoMemo DepKey must expose I and a positive I-U reuse opportunity");
    const std::uint64_t reuse_opportunities =
        invocation_count - distinct_count;
    const double reuse_fraction =
        static_cast<double>(reuse_opportunities) /
        static_cast<double>(invocation_count);
    require(
        reuse_opportunities > 0 &&
            reuse_fraction > 0.0 && reuse_fraction <= 1.0,
        "NoMemo DepKey quotient must be exactly (I-U)/I");
    require(
        distinct_count <
            full.summary.statistics.exact_runtime_key_distinct,
        "DepKey must project at least one irrelevant FullPrefix binding in "
        "the reuse fixture");
    require(
        dependencies.summary.statistics.cache_lookup_nanoseconds == 0,
        "NoMemo key construction must remain separate from cache lookup");
}

void testDiagnosticsOffParityAndCapFailure() {
    const Graph graph = makeParityDataGraph();
    const Graph query = makeParityQueryGraph();
    const QueryDecomposition decomposition =
        makeParityDecomposition(graph, query);
    const TDTree tree(graph, query, decomposition, 3);
    const QueryPlan plan = makeQueryPlan(
        query, decomposition, tree, OrderMode::Paper);
    CandidateDagCompiler compiler;
    const CandidateDag dag = compiler.compile(
        query, plan, decomposition);

    DagExecutorOptions statistics_on =
        makeMemoArm(RuntimeKeyMode::RequiredBindings);
    statistics_on.memo_mode = MemoMode::None;
    DagExecutorOptions statistics_off = statistics_on;
    statistics_off.collect_statistics = false;

    const ParityRun enabled = runParityArm(
        graph, query, plan, dag, tree, statistics_on);
    const ParityRun disabled = runParityArm(
        graph, query, plan, dag, tree, statistics_off);
    DagExecutorOptions exact_statistics_off = makeExactKeyArm(
        MemoMode::None,
        RuntimeKeyMode::RequiredBindings);
    exact_statistics_off.collect_statistics = false;
    const ParityRun exact_disabled = runParityArm(
        graph,
        query,
        plan,
        dag,
        tree,
        exact_statistics_off);
    require(
        enabled.summary.match_count == disabled.summary.match_count &&
            enabled.summary.result_digest == disabled.summary.result_digest &&
            enabled.rows == disabled.rows &&
            enabled.summary.match_count ==
                exact_disabled.summary.match_count &&
            enabled.summary.result_digest ==
                exact_disabled.summary.result_digest &&
            enabled.rows == exact_disabled.rows,
        "statistics OFF and exact diagnostics OFF must preserve results");
    require(
        !disabled.summary.statistics.exact_runtime_key_diagnostics &&
            disabled.summary.statistics.exact_runtime_key_invocations == 0 &&
            disabled.summary.statistics.exact_runtime_key_distinct == 0 &&
            disabled.summary.statistics
                    .runtime_key_construction_nanoseconds == 0 &&
            disabled.summary.statistics.logical_longest_run == 0 &&
            !disabled.summary.statistics
                 .dag_operator_reconciliation_passed &&
            std::all_of(
                disabled.summary.statistics
                    .dag_operator_invocations_by_op.begin(),
                disabled.summary.statistics
                    .dag_operator_invocations_by_op.end(),
                [](std::uint64_t value) { return value == 0; }) &&
            std::all_of(
                disabled.summary.statistics
                    .dag_operator_result_observations_by_op.begin(),
                disabled.summary.statistics
                    .dag_operator_result_observations_by_op.end(),
                [](std::uint64_t value) { return value == 0; }),
        "OFF mode must not construct or retain diagnostic keys");
    require(
        exact_disabled.summary.statistics
                .exact_runtime_key_diagnostics &&
            exact_disabled.summary.statistics
                    .exact_runtime_key_invocations > 0 &&
            exact_disabled.summary.statistics.logical_td_block_lookups == 0 &&
            exact_disabled.summary.statistics.logical_longest_run == 0,
        "explicit exact-key collection must remain independent from ordinary "
        "search-statistics atomics");

    bool cap_failed = false;
    try {
        (void)runParityArm(
            graph,
            query,
            plan,
            dag,
            tree,
            makeExactKeyArm(
                MemoMode::None,
                RuntimeKeyMode::RequiredBindings,
                1));
    } catch (const std::runtime_error& error) {
        cap_failed = std::string(error.what()).find(
            "distinct-key cap exceeded") != std::string::npos;
    }
    require(
        cap_failed,
        "exact diagnostics must fail closed instead of approximating at cap");
}

void testFailClosedOperatorReconciliation() {
    const Graph graph = makeParityDataGraph();
    const Graph query = makeParityQueryGraph();
    const QueryDecomposition decomposition =
        makeParityDecomposition(graph, query);
    const TDTree tree(graph, query, decomposition, 3);
    const QueryPlan plan = makeQueryPlan(
        query, decomposition, tree, OrderMode::Paper);
    CandidateDagCompiler compiler;
    const CandidateDag dag = compiler.compile(
        query, plan, decomposition);

    SearchStatisticsCollector collector(true);
    const DagExecutorOptions options = makeMemoArm(
        RuntimeKeyMode::RequiredBindings);
    DagExecutor executor(
        graph,
        query,
        plan,
        dag,
        tree,
        3,
        options,
        collector);
    SearchState state;
    state.mapping.assign(
        static_cast<std::size_t>(query.num_vertices), -1);
    require(
        !tree.rootCandidates().empty() && plan.order.size() > 1,
        "reconciliation fixture needs a root and one extension");
    state.mapping[static_cast<std::size_t>(plan.root)] =
        tree.rootCandidates().front();
    const int query_vertex = plan.order[1];
    const auto candidates = executor.structuralCandidates(
        query_vertex, state);
    if (!candidates->empty()) {
        (void)executor.temporalSupport(
            query_vertex,
            candidates->front(),
            state);
    }

    SearchStatistics valid = collector.snapshot();
    executor.augmentStatistics(valid);
    require(
        valid.dag_operator_reconciliation_passed,
        "uncorrupted operator summary must reconcile");

    SearchStatistics corrupted = collector.snapshot();
    ++corrupted.dag_operator_result_observations_by_op[
        dagOpIndex(DagOp::TdBlockLookup)];
    bool rejected = false;
    try {
        executor.augmentStatistics(corrupted);
    } catch (const std::logic_error& error) {
        rejected = std::string(error.what()).find(
            "operator-statistics reconciliation failed") !=
            std::string::npos;
    }
    require(
        rejected,
        "a hostile per-op/aggregate mismatch must fail closed");

    SearchStatistics corrupted_longest = collector.snapshot();
    ++corrupted_longest.dag_operator_result_units_sum_by_op[
        dagOpIndex(DagOp::LongestConsecutiveRun)];
    rejected = false;
    try {
        executor.augmentStatistics(corrupted_longest);
    } catch (const std::logic_error& error) {
        rejected = std::string(error.what()).find(
            "operator-statistics reconciliation failed") !=
            std::string::npos;
    }
    require(
        rejected,
        "a hostile longest scalar-unit mismatch must fail closed");
}

} // namespace

int main() {
    try {
        testModeContract();
        testProjectionAndOverlay();
        testStaticNodeOnlyAliasesIncorrectly();
        testEndToEndArmParity();
        testExactNoMemoIdentityDiagnostics();
        testDiagnosticsOffParityAndCapFailure();
        testFailClosedOperatorReconciliation();
        std::cout << "Runtime-key mode tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Runtime-key mode test failure: "
                  << error.what() << '\n';
        return 1;
    }
}
