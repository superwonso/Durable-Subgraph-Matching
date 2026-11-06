#include "../CandidateDag.h"
#include "../BoundedMemo.h"
#include "../GopherMatcher.h"
#include "../TDTree.h"
#include "../Utils.h"
#include "../query_decomposition.h"
#include "../temporal_cost_planner.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<TimeInterval> intervalsFromMask(
    std::uint32_t mask,
    int horizon = 8) {
    std::vector<TimeInterval> intervals;
    for (int snapshot = 1; snapshot <= horizon; ++snapshot) {
        if ((mask & (1U << (snapshot - 1))) == 0) continue;
        if (!intervals.empty() &&
            intervals.back().end + 1 == snapshot) {
            intervals.back().end = snapshot;
        } else {
            intervals.push_back({snapshot, snapshot});
        }
    }
    return intervals;
}

void addTemporalEdge(
    Graph& graph,
    int source,
    int target,
    std::vector<TimeInterval> intervals) {
    int active_count = 0;
    for (const auto& interval : intervals) {
        active_count += interval.length();
    }
    const int edge_id =
        static_cast<int>(graph.temporal_edges.size());
    graph.temporal_edges.push_back(
        {source, target, std::move(intervals), active_count});
    graph.adj[static_cast<std::size_t>(source)].push_back(
        {target, edge_id});
    graph.in_adj[static_cast<std::size_t>(target)].push_back(
        {source, edge_id});
}

void finalizeGraph(Graph& graph, int horizon = 8) {
    graph.external_ids.resize(
        static_cast<std::size_t>(graph.num_vertices));
    for (int vertex = 0;
         vertex < graph.num_vertices;
         ++vertex) {
        graph.external_ids[static_cast<std::size_t>(vertex)] =
            100 + vertex * 17;
    }
    graph.vertex_active_durations.assign(
        static_cast<std::size_t>(graph.num_vertices), horizon);
    graph.neighbor_label_counts.assign(
        static_cast<std::size_t>(graph.num_vertices), {});
    graph.incoming_neighbor_label_counts.assign(
        static_cast<std::size_t>(graph.num_vertices), {});
    for (int vertex = 0;
         vertex < graph.num_vertices;
         ++vertex) {
        auto& outgoing =
            graph.adj[static_cast<std::size_t>(vertex)];
        auto& incoming =
            graph.in_adj[static_cast<std::size_t>(vertex)];
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
        for (const auto& edge : outgoing) {
            const Label label =
                graph.vertex_labels[
                    static_cast<std::size_t>(edge.to)];
            ++graph.neighbor_label_counts[
                static_cast<std::size_t>(vertex)][label];
        }
        for (const auto& edge : incoming) {
            const Label label =
                graph.vertex_labels[
                    static_cast<std::size_t>(edge.to)];
            ++graph.incoming_neighbor_label_counts[
                static_cast<std::size_t>(vertex)][label];
        }
    }
}

Graph makeQuery(
    const std::vector<Label>& labels,
    const std::vector<std::pair<int, int>>& arcs) {
    Graph query;
    query.num_vertices = static_cast<int>(labels.size());
    query.vertex_labels = labels;
    query.external_ids.resize(labels.size());
    query.adj.resize(labels.size());
    query.in_adj.resize(labels.size());
    for (std::size_t vertex = 0;
         vertex < labels.size();
         ++vertex) {
        query.external_ids[vertex] =
            static_cast<int>(vertex);
    }
    std::vector<std::pair<int, int>> normalized = arcs;
    std::sort(normalized.begin(), normalized.end());
    normalized.erase(
        std::unique(normalized.begin(), normalized.end()),
        normalized.end());
    for (const auto& arc : normalized) {
        query.adj[static_cast<std::size_t>(arc.first)]
            .push_back({arc.second, -1});
        query.in_adj[static_cast<std::size_t>(arc.second)]
            .push_back({arc.first, -1});
    }
    return query;
}

bool activeAt(const TemporalEdge& edge, int timestamp) {
    for (const auto& interval : edge.activeIntervals()) {
        if (timestamp < interval.start) return false;
        if (timestamp <= interval.end) return true;
    }
    return false;
}

bool durableMappingBySnapshotScan(
    const Graph& graph,
    const Graph& query,
    const std::vector<int>& mapping,
    int minimum_duration) {
    std::vector<const TemporalEdge*> edges;
    int first = 0;
    int last = -1;
    bool initialized = false;
    for (int source = 0;
         source < query.num_vertices;
         ++source) {
        for (const auto& query_edge :
             query.adj[static_cast<std::size_t>(source)]) {
            const TemporalEdge* edge = graph.findTemporalEdge(
                mapping[static_cast<std::size_t>(source)],
                mapping[static_cast<std::size_t>(query_edge.to)]);
            if (edge == nullptr || edge->activeIntervals().empty()) {
                return false;
            }
            edges.push_back(edge);
            const int edge_first =
                edge->activeIntervals().front().start;
            const int edge_last =
                edge->activeIntervals().back().end;
            if (!initialized) {
                first = edge_first;
                last = edge_last;
                initialized = true;
            } else {
                first = std::min(first, edge_first);
                last = std::max(last, edge_last);
            }
        }
    }
    if (edges.empty()) return false;

    int run = 0;
    for (long long timestamp = first;
         timestamp <= static_cast<long long>(last);
         ++timestamp) {
        bool all_active = true;
        for (const TemporalEdge* edge : edges) {
            if (!activeAt(*edge, static_cast<int>(timestamp))) {
                all_active = false;
                break;
            }
        }
        run = all_active ? run + 1 : 0;
        if (run >= minimum_duration) return true;
    }
    return false;
}

std::uint64_t bruteForceCount(
    const Graph& graph,
    const Graph& query,
    int minimum_duration) {
    std::vector<int> mapping(
        static_cast<std::size_t>(query.num_vertices), -1);
    std::vector<std::uint8_t> used(
        static_cast<std::size_t>(graph.num_vertices), 0);
    std::uint64_t count = 0;
    std::function<void(int)> enumerate =
        [&](int query_vertex) {
            if (query_vertex == query.num_vertices) {
                if (durableMappingBySnapshotScan(
                        graph,
                        query,
                        mapping,
                        minimum_duration)) {
                    ++count;
                }
                return;
            }
            const Label required =
                query.vertex_labels[
                    static_cast<std::size_t>(query_vertex)];
            for (int data_vertex = 0;
                 data_vertex < graph.num_vertices;
                 ++data_vertex) {
                if (used[static_cast<std::size_t>(data_vertex)] ||
                    graph.vertex_labels[
                        static_cast<std::size_t>(data_vertex)] !=
                        required) {
                    continue;
                }
                mapping[static_cast<std::size_t>(query_vertex)] =
                    data_vertex;
                used[static_cast<std::size_t>(data_vertex)] = 1;
                enumerate(query_vertex + 1);
                used[static_cast<std::size_t>(data_vertex)] = 0;
                mapping[static_cast<std::size_t>(query_vertex)] = -1;
            }
        };
    enumerate(0);
    return count;
}

std::string mappingKey(
    const Graph& graph,
    const std::vector<int>& mapping) {
    std::string key;
    for (std::size_t query_vertex = 0;
         query_vertex < mapping.size();
         ++query_vertex) {
        if (query_vertex > 0) key.push_back(',');
        key += std::to_string(
            graph.externalId(mapping[query_vertex]));
    }
    return key;
}

std::vector<std::string> bruteForceMappingKeys(
    const Graph& graph,
    const Graph& query,
    int minimum_duration) {
    std::vector<int> mapping(
        static_cast<std::size_t>(query.num_vertices), -1);
    std::vector<std::uint8_t> used(
        static_cast<std::size_t>(graph.num_vertices), 0);
    std::vector<std::string> matches;
    std::function<void(int)> enumerate =
        [&](int query_vertex) {
            if (query_vertex == query.num_vertices) {
                if (durableMappingBySnapshotScan(
                        graph,
                        query,
                        mapping,
                        minimum_duration)) {
                    matches.push_back(mappingKey(graph, mapping));
                }
                return;
            }
            const Label required =
                query.vertex_labels[
                    static_cast<std::size_t>(query_vertex)];
            for (int data_vertex = 0;
                 data_vertex < graph.num_vertices;
                 ++data_vertex) {
                if (used[static_cast<std::size_t>(data_vertex)] ||
                    graph.vertex_labels[
                        static_cast<std::size_t>(data_vertex)] !=
                        required) {
                    continue;
                }
                mapping[static_cast<std::size_t>(query_vertex)] =
                    data_vertex;
                used[static_cast<std::size_t>(data_vertex)] = 1;
                enumerate(query_vertex + 1);
                used[static_cast<std::size_t>(data_vertex)] = 0;
                mapping[static_cast<std::size_t>(query_vertex)] = -1;
            }
        };
    enumerate(0);
    std::sort(matches.begin(), matches.end());
    return matches;
}

QueryDecomposition decompose(
    const Graph& graph,
    const Graph& query,
    SelectivityMode selectivity_mode = SelectivityMode::Paper) {
    std::array<std::size_t, kLabelCount> counts{};
    std::array<double, kLabelCount> lifespans{};
    for (Label label : graph.vertex_labels) ++counts[label];
    for (std::size_t label = 0;
         label < kLabelCount;
         ++label) {
        if (counts[label] != 0) lifespans[label] = 8.0;
    }
    return decomposeQuery(
        query, counts, lifespans, selectivity_mode);
}

std::uint64_t legacyCount(
    const TDTree& tree,
    const std::filesystem::path& directory,
    const std::string& suffix) {
    const auto output = directory /
        ("legacy_" + suffix + ".txt");
    const MatchSummary summary =
        tree.save_res(
            output.string(), MatchOutputMode::CountOnly);
    require(summary.output_written, "legacy result write");
    require(!summary.count_overflow, "legacy count overflow");
    std::filesystem::remove(output);
    return summary.match_count;
}

struct GopherRun {
    GopherMatchSummary summary;
    QueryPlan plan;
    CandidateDag dag;
};

bool candidateDagCanonicalExpressionsUnique(const CandidateDag& dag) {
    std::unordered_set<std::string> expressions;
    for (const DagNode& node : dag.nodes) {
        if (!expressions.insert(node.canonical_expression).second) {
            return false;
        }
    }
    return true;
}

GopherRun runGopher(
    const Graph& graph,
    const Graph& query,
    const QueryDecomposition& decomposition,
    const TDTree& tree,
    int minimum_duration,
    OrderMode order,
    MemoMode memo,
    std::size_t threads,
    bool early_prune,
    bool verify_dag = true,
    std::size_t cache_budget = 4ULL << 20,
    ParallelMode parallel_mode = ParallelMode::Roots,
    std::size_t prefix_depth = 0,
    std::size_t tasks_per_thread = 8,
    MemoPolicy memo_policy = MemoPolicy::Legacy,
    CandidateDagInternMode intern_mode =
        CandidateDagInternMode::Intern) {
    GopherRun run;
    run.plan = makeQueryPlan(
        query, decomposition, tree, order);
    CandidateDagCompiler compiler;
    run.dag = compiler.compile(
        query, run.plan, decomposition, intern_mode);

    for (const DagNode& node : run.dag.nodes) {
        require(
            std::is_sorted(
                node.dependencies.begin(),
                node.dependencies.end()),
            "DAG dependencies are sorted");
        require(
            std::adjacent_find(
                node.dependencies.begin(),
                node.dependencies.end()) ==
                node.dependencies.end(),
            "DAG dependencies are minimal/unique");
    }
    if (intern_mode == CandidateDagInternMode::Intern) {
        require(
            candidateDagCanonicalExpressionsUnique(run.dag),
            "hash-consed DAG has no duplicate canonical node");
    }

    GopherMatcherOptions options;
    options.threads = threads;
    options.parallel_mode = parallel_mode;
    options.prefix_depth = prefix_depth;
    options.tasks_per_thread = tasks_per_thread;
    options.order_mode = order;
    options.early_prune = early_prune;
    options.result_digest = true;
    options.executor.memo_mode = memo;
    options.executor.memo_policy = memo_policy;
    options.executor.verify_dag = verify_dag;
    options.executor.memo_options.budget_bytes = cache_budget;
    options.executor.memo_options.max_entry_bytes = 1ULL << 20;
    options.executor.memo_options.shard_count = 8;
    options.executor.memo_options.minimum_estimated_work = 0;
    options.executor.memo_options.cost_aware_admission = false;

    GopherMatcher matcher(
        graph,
        query,
        run.plan,
        run.dag,
        tree,
        minimum_duration,
        options);
    run.summary = matcher.run(MatchOutputMode::CountOnly);
    return run;
}

std::vector<std::string> fullGopherMappingKeys(
    const Graph& graph,
    const Graph& query,
    const QueryDecomposition& decomposition,
    const TDTree& tree,
    int minimum_duration,
    const std::filesystem::path& output,
    MemoPolicy memo_policy = MemoPolicy::Legacy) {
    const QueryPlan plan = makeQueryPlan(
        query,
        decomposition,
        tree,
        OrderMode::ReuseAware);
    CandidateDagCompiler compiler;
    const CandidateDag dag =
        compiler.compile(query, plan, decomposition);
    GopherMatcherOptions options;
    options.threads = 4;
    options.order_mode = OrderMode::ReuseAware;
    options.executor.memo_mode = MemoMode::Dag;
    options.executor.memo_policy = memo_policy;
    options.executor.memo_options.minimum_estimated_work = 0;
    options.executor.memo_options.cost_aware_admission = false;
    options.executor.verify_dag = true;
    GopherMatcher matcher(
        graph,
        query,
        plan,
        dag,
        tree,
        minimum_duration,
        options);
    const GopherMatchSummary summary =
        matcher.save_res(
            output.string(), MatchOutputMode::Full);
    require(summary.output_written, "write exact mapping fixture");

    std::ifstream input(output);
    std::string line;
    std::vector<std::string> mappings;
    while (std::getline(input, line)) {
        if (line.rfind("Match ", 0) != 0) continue;
        std::vector<int> external_mapping;
        std::size_t position = 0;
        for (int query_vertex = 0;
             query_vertex < query.num_vertices;
             ++query_vertex) {
            const std::size_t marker =
                line.find(")->", position);
            require(
                marker != std::string::npos,
                "parse Gopher full mapping marker");
            const std::size_t value_start = marker + 3;
            const std::size_t value_end =
                line.find_first_of(", |", value_start);
            external_mapping.push_back(std::stoi(
                line.substr(
                    value_start,
                    value_end - value_start)));
            position = value_end;
        }
        std::string key;
        for (std::size_t index = 0;
             index < external_mapping.size();
             ++index) {
            if (index > 0) key.push_back(',');
            key += std::to_string(external_mapping[index]);
        }
        mappings.push_back(std::move(key));
    }
    std::sort(mappings.begin(), mappings.end());
    require(
        mappings.size() == summary.match_count,
        "full mapping parse count");
    std::filesystem::remove(output);
    return mappings;
}

Graph makeLayeredGraph() {
    Graph graph;
    graph.num_vertices = 8;
    graph.vertex_labels = {
        labelFromString("A"),
        labelFromString("A"),
        labelFromString("A"),
        labelFromString("A"),
        labelFromString("B"),
        labelFromString("B"),
        labelFromString("C"),
        labelFromString("C")};
    graph.adj.resize(8);
    graph.in_adj.resize(8);
    for (int a = 0; a < 4; ++a) {
        for (int b = 4; b < 6; ++b) {
            addTemporalEdge(graph, a, b, {{1, 6}});
        }
    }
    for (int b = 4; b < 6; ++b) {
        for (int c = 6; c < 8; ++c) {
            addTemporalEdge(graph, b, c, {{2, 7}});
        }
    }
    finalizeGraph(graph);
    return graph;
}

void testTemporalCostV2Handoff(
    const std::filesystem::path& directory) {
    const Graph graph = makeLayeredGraph();
    const Graph query = makeQuery(
        {labelFromString("A"),
         labelFromString("B"),
         labelFromString("C")},
        {{0, 1}, {1, 2}});
    std::array<std::size_t, kLabelCount> counts{};
    std::array<double, kLabelCount> lifespans{};
    for (Label label : graph.vertex_labels) ++counts[label];
    for (std::size_t label = 0; label < kLabelCount; ++label) {
        if (counts[label] != 0) lifespans[label] = 8.0;
    }

    TemporalCostPlannerOptions options;
    options.max_state_expansions = 0;
    TemporalPlanDiagnostics diagnostics;
    TemporalCostPlanResult result = planTemporalCostV2(
        graph,
        query,
        3,
        counts,
        lifespans,
        options,
        &diagnostics);
    require(
        result.selected_tree != nullptr &&
            result.decomposition.selectivity_mode ==
                SelectivityMode::TemporalCostV2 &&
            diagnostics.v2_kci_incumbent_selected &&
            diagnostics.v2_fallback_reason ==
                "state-expansion-budget" &&
            diagnostics.selected_tree_handoff,
        "Gopher v2 returns a complete KCI handoff at zero state budget");

    const std::uint64_t oracle = bruteForceCount(graph, query, 3);
    require(
        legacyCount(*result.selected_tree, directory, "v2_handoff") ==
            oracle,
        "Gopher TD-tree v2 handoff preserves exact count");
    const GopherRun run = runGopher(
        graph,
        query,
        result.decomposition,
        *result.selected_tree,
        3,
        OrderMode::ReuseAware,
        MemoMode::Dag,
        2,
        true);
    require(
        run.summary.match_count == oracle,
        "Gopher executor consumes the v2 handed-off tree exactly");
}

void testLayeredCacheAndAblations(
    const std::filesystem::path& directory) {
    const Graph graph = makeLayeredGraph();
    const Graph query = makeQuery(
        {labelFromString("A"),
         labelFromString("B"),
         labelFromString("C")},
        {{0, 1}, {1, 2}});
    const int k = 3;
    const QueryDecomposition decomposition =
        decompose(graph, query);
    require(decomposition.connected, "layered decomposition");
    require(
        decomposition.root == 1,
        "corrected durability-aware score selects the layered center");
    TDTree tree(graph, query, decomposition, k);

    const std::uint64_t oracle =
        bruteForceCount(graph, query, k);
    require(oracle == 16, "layered brute-force count");
    require(
        legacyCount(tree, directory, "layered") == oracle,
        "layered legacy count");

    std::string reference_digest;
    for (OrderMode order :
         {OrderMode::Paper, OrderMode::ReuseAware}) {
        for (MemoMode memo :
             {MemoMode::None,
              MemoMode::Structural,
              MemoMode::Temporal,
              MemoMode::Dag}) {
            const GopherRun run = runGopher(
                graph,
                query,
                decomposition,
                tree,
                k,
                order,
                memo,
                4,
                true);
            require(
                run.summary.match_count == oracle,
                "layered Gopher count parity");
            if (reference_digest.empty()) {
                reference_digest = run.summary.result_digest;
            } else {
                require(
                    run.summary.result_digest ==
                        reference_digest,
                    "memo/order/thread digest parity");
            }
            require(
                run.summary.threads_used == 2,
                "root-parallel execution is bounded by the corrected root domain");
            require(
                run.summary.statistics
                        .raw_extension_candidates >=
                    run.summary.statistics
                        .generated_candidates &&
                    run.summary.statistics
                            .generated_candidates >=
                        run.summary.statistics
                            .accepted_extensions &&
                    run.summary.statistics
                            .accepted_extensions ==
                        run.summary.statistics
                            .accepted_candidates &&
                    run.summary.statistics
                            .completed_matches ==
                        run.summary.match_count,
                "common logical-extension counters preserve the "
                "raw/post-structural/accepted invariant");
            if (memo == MemoMode::Dag) {
                require(
                    run.summary.statistics
                            .structural_cache_hits >
                        0,
                    "structural runtime-key reuse occurs");
                require(
                    run.summary.statistics
                            .temporal_cache_hits >
                        0,
                    "temporal runtime-key reuse occurs");
                require(
                    run.summary.cache_memory_bytes <=
                        (4ULL << 20),
                    "combined cache respects total budget");
                require(
                    run.summary.statistics.structural_cache_hits ==
                        run.summary.statistics.td_block_cache_hits +
                        run.summary.statistics.neighbor_lookup_cache_hits +
                        run.summary.statistics
                            .structural_intersection_cache_hits &&
                    run.summary.statistics.temporal_cache_hits ==
                        run.summary.statistics.edge_time_cache_hits +
                        run.summary.statistics
                            .temporal_intersection_cache_hits &&
                    run.summary.statistics.longest_cache_hits ==
                        run.summary.statistics.longest_run_cache_hits,
                    "per-operation hits partition aggregate cache hits");
            }
        }
    }

    const GopherRun prefix_parallel = runGopher(
        graph,
        query,
        decomposition,
        tree,
        k,
        OrderMode::ReuseAware,
        MemoMode::Dag,
        8,
        true,
        true,
        4ULL << 20,
        ParallelMode::Prefix,
        0,
        2);
    require(
        prefix_parallel.summary.match_count == oracle &&
            prefix_parallel.summary.result_digest ==
                reference_digest,
        "dynamic prefix scheduler preserves exact count/digest");
    require(
        prefix_parallel.summary.threads_used == 8 &&
            prefix_parallel.summary.parallel_mode_effective ==
                "prefix",
        "prefix scheduler can use more workers than root tasks");
    require(
        prefix_parallel.summary.parallel_statistics
                .prefix_tasks_spawned > 0 &&
            prefix_parallel.summary.parallel_statistics.queue_peak > 0 &&
            prefix_parallel.summary.parallel_statistics
                .worker_tasks_total ==
                prefix_parallel.summary.parallel_statistics
                    .queue_claims,
        "prefix scheduler exposes and accounts bounded subtrees");
    require(
        prefix_parallel.summary.parallel_statistics
                .worker_dfs_states_total ==
            prefix_parallel.summary.statistics.dfs_states,
        "always-on worker DFS accounting agrees with search statistics");

    const GopherRun prefix_structural_v2 = runGopher(
        graph,
        query,
        decomposition,
        tree,
        k,
        OrderMode::ReuseAware,
        MemoMode::Dag,
        8,
        true,
        true,
        4ULL << 20,
        ParallelMode::Prefix,
        0,
        2,
        MemoPolicy::StructuralV2);
    require(
        prefix_structural_v2.summary.match_count == oracle &&
            prefix_structural_v2.summary.result_digest ==
                reference_digest &&
            prefix_structural_v2.summary.parallel_mode_effective ==
                "prefix",
        "structural-v2 is exact under the dynamic prefix scheduler");

    const GopherRun prefix_depth_one = runGopher(
        graph,
        query,
        decomposition,
        tree,
        k,
        OrderMode::ReuseAware,
        MemoMode::Dag,
        8,
        true,
        true,
        4ULL << 20,
        ParallelMode::Prefix,
        1,
        2);
    require(
        prefix_depth_one.summary.match_count == oracle &&
            prefix_depth_one.summary.result_digest ==
                reference_digest &&
            prefix_depth_one.summary.parallel_statistics
                    .prefix_tasks_spawned == 0,
        "prefix depth one is exact and disables below-root splitting");

    const GopherRun no_early = runGopher(
        graph,
        query,
        decomposition,
        tree,
        k,
        OrderMode::Paper,
        MemoMode::Dag,
        2,
        false);
    require(
        no_early.summary.match_count == oracle &&
            no_early.summary.result_digest == reference_digest,
        "early-prune ablation preserves exact results");

    const GopherRun cache_zero = runGopher(
        graph,
        query,
        decomposition,
        tree,
        k,
        OrderMode::ReuseAware,
        MemoMode::Dag,
        1,
        true,
        true,
        0);
    require(
        cache_zero.summary.match_count == oracle &&
            cache_zero.summary.cache_memory_bytes == 0 &&
            cache_zero.summary.statistics.structural_cache_hits == 0 &&
            cache_zero.summary.statistics.structural_cache_misses == 0 &&
            cache_zero.summary.statistics.structural_cache_bypassed == 0 &&
            cache_zero.summary.statistics.temporal_cache_hits == 0 &&
            cache_zero.summary.statistics.temporal_cache_misses == 0 &&
            cache_zero.summary.statistics.temporal_cache_bypassed == 0,
        "zero cache budget is exact memo-none behavior");

    const QueryDecomposition intended_decomposition =
        decompose(
            graph, query, SelectivityMode::Intended);
    require(
        intended_decomposition.root == 1 &&
            intended_decomposition.selectivity_mode ==
                SelectivityMode::Intended,
        "intended alias preserves the corrected layered root");
    TDTree intended_tree(
        graph, query, intended_decomposition, k);
    const GopherRun intended = runGopher(
        graph,
        query,
        intended_decomposition,
        intended_tree,
        k,
        OrderMode::ReuseAware,
        MemoMode::Dag,
        1,
        true);
    require(
        intended.summary.match_count == oracle &&
            intended.summary.result_digest == reference_digest &&
            intended.summary.threads_used == 1 &&
            intended.plan.selectivity_mode ==
                SelectivityMode::Intended,
        "intended selectivity and single-thread Gopher preserve exact results");
}

Graph makeRandomGraph(std::uint32_t seed) {
    std::mt19937 generator(seed);
    Graph graph;
    graph.num_vertices = 7;
    graph.vertex_labels = {
        labelFromString("A"),
        labelFromString("A"),
        labelFromString("A"),
        labelFromString("B"),
        labelFromString("B"),
        labelFromString("C"),
        labelFromString("C")};
    graph.adj.resize(7);
    graph.in_adj.resize(7);
    std::uniform_int_distribution<std::uint32_t> mask(1, 255);
    std::bernoulli_distribution include(0.43);
    for (int source = 0; source < graph.num_vertices; ++source) {
        for (int target = 0; target < graph.num_vertices; ++target) {
            if (!include(generator)) continue;
            addTemporalEdge(
                graph,
                source,
                target,
                intervalsFromMask(mask(generator)));
        }
    }
    finalizeGraph(graph);
    return graph;
}

Graph randomQuery(std::uint32_t seed) {
    std::vector<std::pair<int, int>> arcs{
        {0, 1}, {1, 2}, {2, 0}};
    if (seed % 2 == 0) arcs.push_back({1, 0});
    if (seed % 3 == 0) arcs.push_back({1, 1});
    if (seed % 5 == 0) arcs.push_back({0, 2});
    std::vector<Label> labels{
        labelFromString("A"),
        labelFromString("B"),
        seed % 4 == 0
            ? labelFromString("A")
            : labelFromString("C")};
    return makeQuery(labels, arcs);
}

void testRandomGraphsAgainstIndependentOracle(
    const std::filesystem::path& directory) {
    for (std::uint32_t seed = 1; seed <= 24; ++seed) {
        const Graph graph =
            makeRandomGraph(0x12340000U + seed);
        const Graph query = randomQuery(seed);
        const int k = 2 + static_cast<int>(seed % 3);
        const QueryDecomposition decomposition =
            decompose(graph, query);
        require(
            decomposition.connected,
            "random query decomposition");
        TDTree tree(graph, query, decomposition, k);
        const std::vector<std::string> oracle_mappings =
            bruteForceMappingKeys(graph, query, k);
        const std::uint64_t oracle =
            oracle_mappings.size();
        require(
            legacyCount(
                tree,
                directory,
                "random_" + std::to_string(seed)) ==
                oracle,
            "random legacy/oracle parity at seed " +
                std::to_string(seed));

        std::string digest;
        for (OrderMode order :
             {OrderMode::Paper, OrderMode::ReuseAware}) {
            for (MemoMode memo :
                 {MemoMode::None,
                  MemoMode::Structural,
                  MemoMode::Temporal,
                  MemoMode::Dag}) {
                const GopherRun run = runGopher(
                    graph,
                    query,
                    decomposition,
                    tree,
                    k,
                    order,
                    memo,
                    seed % 2 == 0 ? 1 : 4,
                    true,
                    seed <= 8);
                require(
                    run.summary.match_count == oracle,
                    "random Gopher/oracle parity at seed " +
                        std::to_string(seed));
                require(
                    !run.summary.count_overflow,
                    "random Gopher count does not overflow");
                if (digest.empty()) {
                    digest = run.summary.result_digest;
                } else {
                    require(
                        run.summary.result_digest == digest,
                        "random digest parity at seed " +
                            std::to_string(seed));
                }
            }
        }

        const GopherRun no_early = runGopher(
            graph,
            query,
            decomposition,
            tree,
            k,
            OrderMode::ReuseAware,
            MemoMode::Dag,
            3,
            false,
            seed <= 4);
        require(
            no_early.summary.match_count == oracle &&
                no_early.summary.result_digest == digest,
            "random early-prune parity at seed " +
                std::to_string(seed));
        const GopherRun structural_v2 = runGopher(
            graph,
            query,
            decomposition,
            tree,
            k,
            OrderMode::ReuseAware,
            MemoMode::Dag,
            seed % 2 == 0 ? 1 : 4,
            true,
            seed <= 8,
            4ULL << 20,
            ParallelMode::Roots,
            0,
            8,
            MemoPolicy::StructuralV2);
        require(
            structural_v2.summary.match_count == oracle &&
                structural_v2.summary.result_digest == digest,
            "random structural-v2/oracle parity at seed " +
                std::to_string(seed));
        require(
            structural_v2.summary.statistics.temporal_cache_hits == 0 &&
                structural_v2.summary.statistics.temporal_cache_misses == 0 &&
                structural_v2.summary.statistics.longest_cache_hits == 0 &&
                structural_v2.summary.statistics.longest_cache_misses == 0,
            "structural-v2 bypasses temporal and longest-run caches");
        const std::vector<std::string> gopher_mappings =
            fullGopherMappingKeys(
                graph,
                query,
                decomposition,
                tree,
                k,
                directory /
                    ("gopher_exact_" +
                     std::to_string(seed) + ".txt"),
                MemoPolicy::StructuralV2);
        require(
            gopher_mappings == oracle_mappings,
            "random exact mapping-set parity at seed " +
                std::to_string(seed));
    }
}

void testFullAndCountOnlyParity(
    const std::filesystem::path& directory) {
    const Graph graph = makeLayeredGraph();
    const Graph query = makeQuery(
        {labelFromString("A"),
         labelFromString("B"),
         labelFromString("C")},
        {{0, 1}, {1, 2}});
    const QueryDecomposition decomposition =
        decompose(graph, query);
    TDTree tree(graph, query, decomposition, 3);
    const QueryPlan plan = makeQueryPlan(
        query,
        decomposition,
        tree,
        OrderMode::ReuseAware);
    CandidateDagCompiler compiler;
    const CandidateDag dag =
        compiler.compile(query, plan, decomposition);

    GopherMatcherOptions options;
    options.threads = 4;
    options.order_mode = OrderMode::ReuseAware;
    options.result_digest = true;
    options.executor.memo_mode = MemoMode::Dag;
    options.executor.memo_options.minimum_estimated_work = 0;
    options.executor.memo_options.cost_aware_admission = false;

    GopherMatcher full_matcher(
        graph, query, plan, dag, tree, 3, options);
    const auto full_path = directory / "gopher_full.txt";
    const GopherMatchSummary full =
        full_matcher.save_res(
            full_path.string(), MatchOutputMode::Full);
    require(
        full.output_written && full.threads_used == 1,
        "full output is deterministic and written");

    GopherMatcher count_matcher(
        graph, query, plan, dag, tree, 3, options);
    const GopherMatchSummary count =
        count_matcher.run(MatchOutputMode::CountOnly);
    require(
        full.match_count == count.match_count &&
            full.result_digest == count.result_digest,
        "full/count-only parity");

    std::ifstream input(full_path);
    std::string line;
    std::size_t match_rows = 0;
    while (std::getline(input, line)) {
        if (line.rfind("Match ", 0) == 0) ++match_rows;
    }
    require(
        match_rows == full.match_count,
        "full output contains one row per match");
    std::filesystem::remove(full_path);
}

void testReciprocalSelfLoopExactMappings(
    const std::filesystem::path& directory) {
    Graph graph;
    graph.num_vertices = 4;
    graph.vertex_labels = {
        labelFromString("A"),
        labelFromString("A"),
        labelFromString("B"),
        labelFromString("B")};
    graph.adj.resize(4);
    graph.in_adj.resize(4);
    addTemporalEdge(graph, 0, 0, {{1, 6}});
    addTemporalEdge(graph, 0, 2, {{1, 5}});
    addTemporalEdge(graph, 2, 0, {{2, 6}});
    addTemporalEdge(graph, 1, 1, {{1, 6}});
    addTemporalEdge(graph, 1, 3, {{1, 2}, {5, 6}});
    addTemporalEdge(graph, 3, 1, {{2, 5}});
    finalizeGraph(graph);

    const Graph query = makeQuery(
        {labelFromString("A"), labelFromString("B")},
        {{0, 0}, {0, 1}, {1, 0}});
    const QueryDecomposition decomposition =
        decompose(graph, query);
    require(
        decomposition.connected,
        "reciprocal/self-loop decomposition");
    TDTree tree(graph, query, decomposition, 3);
    const std::vector<std::string> oracle =
        bruteForceMappingKeys(graph, query, 3);
    require(
        oracle == std::vector<std::string>{"100,134"},
        "reciprocal/self-loop independent oracle fixture");
    require(
        legacyCount(tree, directory, "reciprocal_loop") ==
            oracle.size(),
        "reciprocal/self-loop legacy count");
    const std::vector<std::string> actual =
        fullGopherMappingKeys(
            graph,
            query,
            decomposition,
            tree,
            3,
            directory / "reciprocal_loop_gopher.txt");
    require(
        actual == oracle,
        "reciprocal arcs and self-loop exact mapping parity");
}

void testPlanFingerprintAndDagTopology() {
    const Graph graph = makeLayeredGraph();
    const Graph query = makeQuery(
        {labelFromString("A"),
         labelFromString("B"),
         labelFromString("C")},
        {{0, 1}, {0, 2}, {1, 2}});
    const QueryDecomposition tree_decomposition =
        decompose(graph, query);
    TDTree tree(graph, query, tree_decomposition, 3);

    QueryDecomposition base = tree_decomposition;
    base.root = 0;
    base.connected = true;
    base.dfs_order = {0, 1, 2};
    base.parent = {-1, 0, 1};
    base.temporal_selectivity.assign(3, 1.0);

    const QueryPlan first = makeQueryPlan(
        query, base, tree, OrderMode::Paper);
    const QueryPlan repeated = makeQueryPlan(
        query, base, tree, OrderMode::Paper);
    require(
        first.query_fingerprint == repeated.query_fingerprint &&
            first.plan_id == repeated.plan_id,
        "query and plan fingerprints are deterministic");

    QueryDecomposition changed_root = base;
    changed_root.root = 1;
    const QueryPlan root_plan = makeQueryPlan(
        query, changed_root, tree, OrderMode::Paper);
    require(
        root_plan.query_fingerprint == first.query_fingerprint &&
            root_plan.plan_id != first.plan_id,
        "plan fingerprint includes the root");

    QueryDecomposition changed_order = base;
    changed_order.dfs_order = {0, 2, 1};
    const QueryPlan order_plan = makeQueryPlan(
        query, changed_order, tree, OrderMode::Paper);
    require(
        order_plan.plan_id != first.plan_id,
        "plan fingerprint includes the full order");

    QueryDecomposition changed_parent = base;
    changed_parent.parent = {-1, 0, 0};
    const QueryPlan parent_plan = makeQueryPlan(
        query, changed_parent, tree, OrderMode::Paper);
    require(
        parent_plan.plan_id != first.plan_id,
        "plan fingerprint includes the full parent vector");

    const Graph changed_query = makeQuery(
        {labelFromString("A"),
         labelFromString("B"),
         labelFromString("C")},
        {{0, 1}, {0, 2}, {2, 1}});
    const QueryPlan query_plan = makeQueryPlan(
        changed_query, base, tree, OrderMode::Paper);
    require(
        query_plan.query_fingerprint != first.query_fingerprint &&
            query_plan.plan_id != first.plan_id,
        "plan fingerprint includes normalized query semantics");

    const Graph star = makeQuery(
        {labelFromString("B"),
         labelFromString("A"),
         labelFromString("C"),
         labelFromString("D"),
         labelFromString("E")},
        {{0, 1}, {0, 2}, {0, 3}, {0, 4}});
    QueryPlan star_plan;
    star_plan.root = 1;
    star_plan.order = {1, 0, 2, 3, 4};
    star_plan.parent = {1, -1, 0, 0, 0};
    CandidateDagCompiler compiler;
    const CandidateDag dag = compiler.compile(
        star, star_plan, QueryDecomposition{});
    const CandidateDagTopology topology = dag.topologySummary();
    require(
        topology.nodes_attempted == 20 &&
            topology.nodes_materialized == 18 &&
            topology.nodes_merged == 2,
        "DAG topology reports hash-consing totals");
    require(
        topology.total_parent_edges == 12 &&
            topology.shared_nodes == 1 &&
            topology.max_fanout == 3 &&
            topology.max_depth == 2,
        "DAG topology reports edges, sharing, fanout, and depth");
    require(
        topology.op_node_counts[
            dagOpIndex(DagOp::TdBlockLookup)] == 4 &&
            topology.op_node_counts[
                dagOpIndex(DagOp::OutNeighborLookup)] == 1 &&
            topology.op_node_counts[
                dagOpIndex(DagOp::InNeighborLookup)] == 1 &&
            topology.op_node_counts[
                dagOpIndex(DagOp::StructuralIntersection)] == 4 &&
            topology.op_node_counts[
                dagOpIndex(DagOp::EdgeTimeLookup)] == 4 &&
            topology.op_node_counts[
                dagOpIndex(DagOp::TemporalIntersection)] == 0 &&
            topology.op_node_counts[
                dagOpIndex(DagOp::LongestConsecutiveRun)] == 4,
        "DAG topology reports per-operation materialized nodes");
}

std::string candidateDagNodeDenotation(
    const CandidateDag& dag,
    const DagNode& node) {
    std::string text = std::string(dagOpMetricName(node.op)) + '|' +
        dagResultKindName(node.result_kind) + "|P=";
    for (DagNodeId parent : node.parents) {
        text += dag.node(parent).canonical_expression;
        text.push_back(';');
    }
    text += "|D=";
    for (int dependency : node.dependencies) {
        text += std::to_string(dependency) + ',';
    }
    text += "|Q=" +
        std::to_string(node.parameter.first_query_vertex) + ',' +
        std::to_string(node.parameter.second_query_vertex) +
        "|E=" + node.canonical_expression +
        "|Z=" + std::to_string(node.depth);
    return text;
}

std::map<std::string, std::string> collapsedCandidateDag(
    const CandidateDag& dag) {
    std::map<std::string, std::string> result;
    for (const DagNode& node : dag.nodes) {
        const std::string denotation = candidateDagNodeDenotation(dag, node);
        const auto inserted = result.emplace(
            node.canonical_expression, denotation);
        require(
            inserted.second || inserted.first->second == denotation,
            "duplicate canonical expressions have different denotations");
    }
    return result;
}

std::string attemptDenotation(const CandidateDagAttemptRecord& record) {
    std::string text = std::to_string(record.attempt_index) + '|' +
        dagOpMetricName(record.op) + '|' +
        dagResultKindName(record.result_kind) + "|P=";
    for (const std::string& parent : record.parent_expressions) {
        text += parent;
        text.push_back(';');
    }
    text += "|D=";
    for (int dependency : record.dependencies) {
        text += std::to_string(dependency) + ',';
    }
    text += "|Q=" +
        std::to_string(record.parameter.first_query_vertex) + ',' +
        std::to_string(record.parameter.second_query_vertex) +
        "|E=" + record.canonical_expression +
        "|F=" + std::to_string(record.static_fingerprint);
    return text;
}

std::uint64_t constantCandidateDagFingerprint(const std::string&) {
    return 7;
}

void testCandidateDagNoInternIsolation() {
    Graph graph;
    graph.num_vertices = 10;
    graph.vertex_labels = {
        labelFromString("A"), labelFromString("A"),
        labelFromString("B"), labelFromString("B"),
        labelFromString("C"), labelFromString("C"),
        labelFromString("D"), labelFromString("D"),
        labelFromString("E"), labelFromString("E")};
    graph.adj.resize(10);
    graph.in_adj.resize(10);
    for (int source = 0; source < graph.num_vertices; ++source) {
        for (int target = 0; target < graph.num_vertices; ++target) {
            if (source == target) continue;
            addTemporalEdge(graph, source, target, {{1, 8}});
        }
    }
    finalizeGraph(graph);

    const Graph query = makeQuery(
        {labelFromString("B"),
         labelFromString("A"),
         labelFromString("C"),
         labelFromString("D"),
         labelFromString("E")},
        {{0, 1}, {0, 2}, {0, 3}, {0, 4}});
    const QueryDecomposition decomposition = decompose(graph, query);
    TDTree tree(graph, query, decomposition, 3);
    const QueryPlan plan = makeQueryPlan(
        query, decomposition, tree, OrderMode::Paper);

    CandidateDagCompilationAudit intern_audit;
    CandidateDagCompilationAudit no_intern_audit;
    CandidateDagCompiler compiler;
    const CandidateDag intern = compiler.compile(
        query,
        plan,
        decomposition,
        CandidateDagInternMode::Intern,
        &intern_audit);
    const CandidateDag no_intern = compiler.compile(
        query,
        plan,
        decomposition,
        CandidateDagInternMode::NoIntern,
        &no_intern_audit);
    const CandidateDagTopology intern_topology = intern.topologySummary();
    const CandidateDagTopology no_intern_topology =
        no_intern.topologySummary();

    require(
        candidateDagCanonicalExpressionsUnique(intern),
        "Intern fixture has unique canonical materialized nodes");
    require(
        intern_topology.nodes_merged > 0 &&
            intern_topology.nodes_materialized +
                    intern_topology.nodes_merged ==
                intern_topology.nodes_attempted,
        "Intern fixture exercises exact hash-cons merging");
    require(
        no_intern_topology.nodes_merged == 0 &&
            no_intern_topology.nodes_materialized ==
                no_intern_topology.nodes_attempted,
        "NoIntern materializes every attempted canonical node");
    require(
        !candidateDagCanonicalExpressionsUnique(no_intern),
        "NoIntern fixture preserves duplicate canonical materializations");
    require(
        intern_topology.nodes_attempted ==
                no_intern_topology.nodes_attempted &&
            intern_topology.op_node_attempt_counts ==
                no_intern_topology.op_node_attempt_counts,
        "Interning cannot alter attempt counts or per-op construction");
    require(
        intern_audit.attempts.size() == no_intern_audit.attempts.size(),
        "Interning cannot alter the attempted expression stream length");
    for (std::size_t index = 0;
         index < intern_audit.attempts.size();
         ++index) {
        require(
            attemptDenotation(intern_audit.attempts[index]) ==
                attemptDenotation(no_intern_audit.attempts[index]),
            "Interning cannot alter an attempted expression denotation");
    }
    require(
        intern_audit.structural_root_expressions ==
                no_intern_audit.structural_root_expressions &&
            intern_audit.temporal_root_expressions ==
                no_intern_audit.temporal_root_expressions &&
            intern_audit.longest_run_root_expressions ==
                no_intern_audit.longest_run_root_expressions,
        "Interning cannot alter semantic root expressions");
    require(
        collapsedCandidateDag(intern) == collapsedCandidateDag(no_intern),
        "collapsing NoIntern by full denotation reproduces Intern exactly");

    for (const DagNode& node : no_intern.nodes) {
        std::set<std::string> parents;
        for (DagNodeId parent : node.parents) {
            require(
                parents.insert(
                    no_intern.node(parent).canonical_expression).second,
                "commutative canonical parents contain a duplicate denotation");
        }
    }
    std::vector<std::uint8_t> reachable(no_intern.nodes.size(), 0);
    std::vector<DagNodeId> pending;
    pending.insert(
        pending.end(),
        no_intern.structural_roots.begin(),
        no_intern.structural_roots.end());
    pending.insert(
        pending.end(),
        no_intern.temporal_roots.begin(),
        no_intern.temporal_roots.end());
    pending.insert(
        pending.end(),
        no_intern.longest_run_roots.begin(),
        no_intern.longest_run_roots.end());
    while (!pending.empty()) {
        const DagNodeId id = pending.back();
        pending.pop_back();
        if (id == kInvalidDagNode ||
            reachable[static_cast<std::size_t>(id)] != 0) {
            continue;
        }
        reachable[static_cast<std::size_t>(id)] = 1;
        const DagNode& node = no_intern.node(id);
        pending.insert(
            pending.end(), node.parents.begin(), node.parents.end());
    }
    std::uint64_t unreachable = 0;
    for (std::size_t index = 0; index < reachable.size(); ++index) {
        if (reachable[index] != 0) continue;
        ++unreachable;
    }
    require(
        unreachable == no_intern_topology.unreachable_materialized_nodes &&
            no_intern_topology.reachable_nodes + unreachable ==
                no_intern_topology.nodes_materialized,
        "reachable and arena topology counts reconcile without assigning "
        "flatten-discarded intermediates to canonical-parent deduplication");

    std::vector<std::pair<int, int>> dense_arcs;
    for (int source = 0; source < 5; ++source) {
        for (int target = source + 1; target < 5; ++target) {
            dense_arcs.emplace_back(source, target);
        }
    }
    const Graph dense_query = makeQuery(
        {labelFromString("A"),
         labelFromString("B"),
         labelFromString("C"),
         labelFromString("D"),
         labelFromString("E")},
        dense_arcs);
    const QueryDecomposition dense_decomposition =
        decompose(graph, dense_query);
    TDTree dense_tree(graph, dense_query, dense_decomposition, 3);
    const QueryPlan dense_plan = makeQueryPlan(
        dense_query,
        dense_decomposition,
        dense_tree,
        OrderMode::Paper);
    require(
        std::any_of(
            dense_plan.earlier_neighbors.begin(),
            dense_plan.earlier_neighbors.end(),
            [](const std::vector<int>& neighbors) {
                return neighbors.size() >= 2;
            }),
        "complete-DAG query plan has a vertex with two earlier neighbors");
    CandidateDagCompilationAudit dense_audit;
    const CandidateDag dense_no_intern = compiler.compile(
        dense_query,
        dense_plan,
        dense_decomposition,
        CandidateDagInternMode::NoIntern,
        &dense_audit);
    CandidateDagCompilationAudit dense_intern_audit;
    const CandidateDag dense_intern = compiler.compile(
        dense_query,
        dense_plan,
        dense_decomposition,
        CandidateDagInternMode::Intern,
        &dense_intern_audit);
    require(
        std::any_of(
            dense_audit.attempts.begin(),
            dense_audit.attempts.end(),
            [](const CandidateDagAttemptRecord& record) {
                return (record.op == DagOp::StructuralIntersection ||
                        record.op == DagOp::TemporalIntersection) &&
                    record.parent_expressions.size() >= 3;
            }),
        "complete-DAG fixture exercises multi-parent flattening");
    require(
        dense_no_intern.topologySummary().unreachable_materialized_nodes > 0,
        "complete-DAG NoIntern fixture retains flattened intermediates");
    require(
        dense_intern_audit.attempts.size() == dense_audit.attempts.size(),
        "complete-DAG fixture preserves the attempted stream length");
    for (std::size_t index = 0; index < dense_audit.attempts.size(); ++index) {
        require(
            attemptDenotation(dense_intern_audit.attempts[index]) ==
                attemptDenotation(dense_audit.attempts[index]),
            "complete-DAG interning changes a multi-depth attempt");
    }
    require(
        dense_intern_audit.structural_root_expressions ==
                dense_audit.structural_root_expressions &&
            dense_intern_audit.temporal_root_expressions ==
                dense_audit.temporal_root_expressions &&
            dense_intern_audit.longest_run_root_expressions ==
                dense_audit.longest_run_root_expressions &&
            collapsedCandidateDag(dense_intern) ==
                collapsedCandidateDag(dense_no_intern),
        "complete-DAG multi-depth roots and full-denotation quotient drifted");

    CandidateDagCompiler collision_compiler(
        constantCandidateDagFingerprint);
    const CandidateDag collided = collision_compiler.compile(
        query,
        plan,
        decomposition,
        CandidateDagInternMode::Intern);
    require(
        collided.nodes_merged == intern.nodes_merged &&
            collapsedCandidateDag(collided) ==
                collapsedCandidateDag(intern),
        "forced fingerprint collisions retain full-expression equality");

    const GopherRun intern_run = runGopher(
        graph,
        query,
        decomposition,
        tree,
        3,
        OrderMode::Paper,
        MemoMode::None,
        1,
        true,
        true,
        4ULL << 20,
        ParallelMode::Roots,
        0,
        8,
        MemoPolicy::Legacy,
        CandidateDagInternMode::Intern);
    const GopherRun no_intern_run = runGopher(
        graph,
        query,
        decomposition,
        tree,
        3,
        OrderMode::Paper,
        MemoMode::None,
        1,
        true,
        true,
        4ULL << 20,
        ParallelMode::Roots,
        0,
        8,
        MemoPolicy::Legacy,
        CandidateDagInternMode::NoIntern);
    require(
        intern_run.summary.match_count == no_intern_run.summary.match_count &&
            intern_run.summary.result_digest ==
                no_intern_run.summary.result_digest &&
            !intern_run.summary.count_overflow &&
            !no_intern_run.summary.count_overflow,
        "NoIntern preserves exact count and embedding digest");
}

void testMinimalRuntimeBindings() {
    const Graph graph = makeLayeredGraph();
    const Graph query = makeQuery(
        {labelFromString("A"),
         labelFromString("B"),
         labelFromString("C")},
        {{0, 1}, {1, 2}});
    const QueryDecomposition decomposition =
        decompose(graph, query);
    TDTree tree(graph, query, decomposition, 3);
    const QueryPlan plan = makeQueryPlan(
        query,
        decomposition,
        tree,
        OrderMode::Paper);
    require(
        plan.order == std::vector<int>({1, 2, 0}),
        "runtime-key fixture plan");
    CandidateDagCompiler compiler;
    const CandidateDag dag =
        compiler.compile(query, plan, decomposition);

    for (const DagNode& node : dag.nodes) {
        if (node.op == DagOp::TdBlockLookup) {
            require(
                node.dependencies ==
                    std::vector<int>{
                        node.parameter.second_query_vertex},
                "TD block key binds only its parent");
        } else if (
            node.op == DagOp::OutNeighborLookup ||
            node.op == DagOp::InNeighborLookup) {
            require(
                node.dependencies ==
                    std::vector<int>{
                        node.parameter.first_query_vertex},
                "neighbor key binds only its anchor");
        } else if (node.op == DagOp::EdgeTimeLookup) {
            std::vector<int> expected{
                node.parameter.first_query_vertex,
                node.parameter.second_query_vertex};
            std::sort(expected.begin(), expected.end());
            expected.erase(
                std::unique(expected.begin(), expected.end()),
                expected.end());
            require(
                node.dependencies == expected,
                "edge-time key binds exactly its endpoints");
        }
    }

    SearchStatisticsCollector statistics;
    DagExecutorOptions options;
    options.memo_mode = MemoMode::Dag;
    options.memo_options.budget_bytes = 1ULL << 20;
    options.memo_options.max_entry_bytes = 1ULL << 20;
    options.memo_options.minimum_estimated_work = 0;
    options.memo_options.cost_aware_admission = false;
    DagExecutor executor(
        graph,
        query,
        plan,
        dag,
        tree,
        3,
        options,
        statistics);
    SearchState state;
    state.mapping.assign(3, -1);
    state.mapping[0] = 0;
    state.mapping[1] = 4;
    executor.structuralCandidates(2, state);
    SearchStatistics first = statistics.snapshot();
    executor.augmentStatistics(first);

    state.mapping[0] = 1; // irrelevant to q2's structural expression
    executor.structuralCandidates(2, state);
    SearchStatistics irrelevant = statistics.snapshot();
    executor.augmentStatistics(irrelevant);
    require(
        irrelevant.structural_cache_hits >
            first.structural_cache_hits,
        "irrelevant branch binding reuses structural result");

    state.mapping[1] = 5; // relevant parent binding
    executor.structuralCandidates(2, state);
    SearchStatistics relevant = statistics.snapshot();
    executor.augmentStatistics(relevant);
    require(
        relevant.structural_cache_misses >
            irrelevant.structural_cache_misses,
        "relevant structural binding produces a new key");

    state.mapping[0] = 0;
    state.mapping[1] = 4;
    executor.temporalSupport(2, 6, state);
    SearchStatistics temporal_first = statistics.snapshot();
    executor.augmentStatistics(temporal_first);
    state.mapping[0] = 1;
    executor.temporalSupport(2, 6, state);
    SearchStatistics temporal_irrelevant = statistics.snapshot();
    executor.augmentStatistics(temporal_irrelevant);
    require(
        temporal_irrelevant.temporal_cache_hits >
            temporal_first.temporal_cache_hits,
        "irrelevant branch binding reuses temporal result");
    state.mapping[1] = 5;
    executor.temporalSupport(2, 6, state);
    SearchStatistics temporal_relevant = statistics.snapshot();
    executor.augmentStatistics(temporal_relevant);
    require(
        temporal_relevant.temporal_cache_misses >
            temporal_irrelevant.temporal_cache_misses,
        "relevant temporal endpoint produces a new key");
}

void testStructuralV2ProbeBeforeParents() {
    MemoPolicy parsed = MemoPolicy::Legacy;
    require(
        DagExecutorOptions{}.memo_policy == MemoPolicy::Legacy &&
            parseMemoPolicy("structural-v2", parsed) &&
            parsed == MemoPolicy::StructuralV2 &&
            std::string(memoPolicyName(parsed)) == "structural-v2" &&
            !parseMemoPolicy("adaptive", parsed),
        "memo policy parser accepts only documented policy names");

    const Graph graph = makeLayeredGraph();
    const Graph query = makeQuery(
        {labelFromString("A"),
         labelFromString("B"),
         labelFromString("C")},
        {{0, 1}, {1, 2}});
    // Keep q2 behind both q0 and q1 so this test isolates projected-key
    // memoization from the canonical root-selection fixture above.
    std::array<std::size_t, kLabelCount> counts{};
    std::array<double, kLabelCount> lifespans{};
    counts[labelFromString("A")] = 1;
    counts[labelFromString("B")] = 4;
    counts[labelFromString("C")] = 4;
    lifespans[labelFromString("A")] = 1.0;
    lifespans[labelFromString("B")] = 1.0;
    lifespans[labelFromString("C")] = 1.0;
    const QueryDecomposition decomposition = decomposeQuery(
        query, counts, lifespans, SelectivityMode::Paper);
    TDTree tree(graph, query, decomposition, 3);
    const QueryPlan plan = makeQueryPlan(
        query, decomposition, tree, OrderMode::Paper);
    CandidateDagCompiler compiler;
    const CandidateDag dag = compiler.compile(
        query, plan, decomposition);
    const DagNodeId root = dag.structural_roots[2];
    require(
        root != kInvalidDagNode &&
            dag.node(root).op == DagOp::StructuralIntersection &&
            !dag.node(root).parents.empty(),
        "structural-v2 fixture has an internal structural root");

    DagExecutorOptions options;
    options.memo_mode = MemoMode::Dag;
    options.memo_policy = MemoPolicy::StructuralV2;
    options.memo_options.budget_bytes = 1ULL << 20;
    options.memo_options.max_entry_bytes = 1ULL << 20;
    options.memo_options.minimum_estimated_work = 1;
    options.memo_options.cost_aware_admission = true;
    SearchStatisticsCollector statistics;
    DagExecutor executor(
        graph, query, plan, dag, tree, 3, options, statistics);

    SearchState state;
    state.mapping.assign(3, -1);
    state.mapping[0] = 0;
    state.mapping[1] = 4;
    const auto first_result = executor.structuralCandidates(2, state);
    SearchStatistics first = statistics.snapshot();
    executor.augmentStatistics(first);
    require(
        first.structural_cache_misses > 0 &&
            first.structural_cache_entries > 0 &&
            first.td_block_cache_hits == 0 &&
            first.neighbor_lookup_cache_hits == 0 &&
            first.temporal_cache_entries == 0 &&
            first.structural_v2_eligible_nodes > 0 &&
            first.structural_v2_no_projection_reuse_nodes > 0,
        "structural-v2 admits only structural-intersection results");

    const std::uint64_t first_parent_logical =
        first.logical_td_block_lookups +
        first.logical_neighbor_lookups;
    const std::uint64_t first_parent_physical =
        first.physical_td_block_lookups +
        first.physical_neighbor_lookups;
    state.mapping[0] = 1; // not in q2's exact dependency projection
    const auto reused_result = executor.structuralCandidates(2, state);
    SearchStatistics reused = statistics.snapshot();
    executor.augmentStatistics(reused);
    require(
        *reused_result == *first_result &&
            reused.structural_cache_hits ==
                first.structural_cache_hits + 1 &&
            reused.structural_intersection_cache_hits ==
                first.structural_intersection_cache_hits + 1,
        "structural-v2 hits the exact projected key");
    require(
        reused.structural_parent_subtrees_skipped ==
                first.structural_parent_subtrees_skipped +
                    dag.node(root).parents.size() &&
            reused.logical_td_block_lookups +
                    reused.logical_neighbor_lookups ==
                first_parent_logical &&
            reused.physical_td_block_lookups +
                    reused.physical_neighbor_lookups ==
                first_parent_physical,
        "structural-v2 hit skips every immediate parent subtree");

    state.mapping[1] = 5; // part of q2's exact dependency projection
    executor.structuralCandidates(2, state);
    SearchStatistics changed = statistics.snapshot();
    executor.augmentStatistics(changed);
    require(
        changed.structural_cache_misses >
            reused.structural_cache_misses,
        "a relevant binding cannot alias a structural-v2 cache key");

    // Legacy remains explicitly selectable and retains parent-before-probe
    // under cost-aware admission.
    options.memo_policy = MemoPolicy::Legacy;
    SearchStatisticsCollector legacy_statistics;
    DagExecutor legacy_executor(
        graph,
        query,
        plan,
        dag,
        tree,
        3,
        options,
        legacy_statistics);
    state.mapping[0] = 0;
    state.mapping[1] = 4;
    legacy_executor.structuralCandidates(2, state);
    SearchStatistics legacy_first = legacy_statistics.snapshot();
    legacy_executor.augmentStatistics(legacy_first);
    state.mapping[0] = 1;
    legacy_executor.structuralCandidates(2, state);
    SearchStatistics legacy_second = legacy_statistics.snapshot();
    legacy_executor.augmentStatistics(legacy_second);
    require(
        legacy_second.logical_td_block_lookups +
                legacy_second.logical_neighbor_lookups >
            legacy_first.logical_td_block_lookups +
                legacy_first.logical_neighbor_lookups &&
            legacy_second.structural_parent_subtrees_skipped == 0,
        "legacy policy retains parent-before-probe semantics");

    // A new executor owns a fresh one-run cache.
    options.memo_policy = MemoPolicy::StructuralV2;
    SearchStatisticsCollector fresh_statistics;
    DagExecutor fresh_executor(
        graph,
        query,
        plan,
        dag,
        tree,
        3,
        options,
        fresh_statistics);
    state.mapping[0] = 0;
    state.mapping[1] = 4;
    fresh_executor.structuralCandidates(2, state);
    SearchStatistics fresh = fresh_statistics.snapshot();
    fresh_executor.augmentStatistics(fresh);
    require(
        fresh.structural_cache_hits == 0 &&
            fresh.structural_cache_misses > 0 &&
            fresh.structural_cache_entries > 0,
        "structural-v2 cache scope ends with its executor");

    // Memo-none is unaffected by the policy selection.
    options.memo_mode = MemoMode::None;
    SearchStatisticsCollector none_statistics;
    DagExecutor none_executor(
        graph,
        query,
        plan,
        dag,
        tree,
        3,
        options,
        none_statistics);
    state.mapping[0] = 0;
    state.mapping[1] = 4;
    const auto none_result = none_executor.structuralCandidates(2, state);
    SearchStatistics none = none_statistics.snapshot();
    none_executor.augmentStatistics(none);
    require(
        *none_result == *first_result &&
            none.structural_cache_hits == 0 &&
            none.structural_cache_misses == 0 &&
            none.structural_cache_entries == 0 &&
            none.structural_parent_subtrees_skipped == 0,
        "structural-v2 leaves memo-none behavior unchanged");

    // The v2 pre-probe and the existing post-miss work gate are independent:
    // a rejected miss is never admitted and therefore cannot become a hit.
    options.memo_mode = MemoMode::Dag;
    options.memo_options.minimum_estimated_work = 1ULL << 20;
    SearchStatisticsCollector gated_statistics;
    DagExecutor gated_executor(
        graph,
        query,
        plan,
        dag,
        tree,
        3,
        options,
        gated_statistics);
    gated_executor.structuralCandidates(2, state);
    gated_executor.structuralCandidates(2, state);
    SearchStatistics gated = gated_statistics.snapshot();
    gated_executor.augmentStatistics(gated);
    require(
        gated.structural_cache_hits == 0 &&
            gated.structural_cache_misses >= 2 &&
            gated.structural_cache_entries == 0 &&
            gated.structural_cache_rejected_cost >= 2 &&
            gated.structural_parent_subtrees_skipped == 0,
        "structural-v2 preserves post-miss minimum-work admission");
}

void testStructuralV2StaticNoReuseGate() {
    Graph graph;
    graph.num_vertices = 6;
    graph.vertex_labels = {
        labelFromString("A"), labelFromString("A"),
        labelFromString("B"), labelFromString("B"),
        labelFromString("C"), labelFromString("C")};
    graph.adj.resize(6);
    graph.in_adj.resize(6);
    for (int source = 0; source < graph.num_vertices; ++source) {
        for (int target = 0; target < graph.num_vertices; ++target) {
            if (source == target) continue;
            addTemporalEdge(graph, source, target, {{1, 8}});
        }
    }
    finalizeGraph(graph);
    std::vector<std::pair<int, int>> arcs;
    for (int source = 0; source < 3; ++source) {
        for (int target = 0; target < 3; ++target) {
            if (source != target) arcs.emplace_back(source, target);
        }
    }
    const Graph query = makeQuery(
        {labelFromString("A"),
         labelFromString("B"),
         labelFromString("C")},
        arcs);
    const QueryDecomposition decomposition = decompose(graph, query);
    TDTree tree(graph, query, decomposition, 3);
    const std::uint64_t oracle = bruteForceCount(graph, query, 3);
    const GopherRun run = runGopher(
        graph,
        query,
        decomposition,
        tree,
        3,
        OrderMode::Paper,
        MemoMode::Dag,
        1,
        true,
        true,
        4ULL << 20,
        ParallelMode::Roots,
        0,
        8,
        MemoPolicy::StructuralV2);
    require(
        run.summary.match_count == oracle && oracle > 0,
        "no-reuse static-gate fixture preserves exact matches");
    require(
        run.summary.statistics.structural_v2_intersection_nodes > 0 &&
            run.summary.statistics.structural_v2_eligible_nodes == 0 &&
            run.summary.statistics
                    .structural_v2_no_projection_reuse_nodes ==
                run.summary.statistics
                    .structural_v2_intersection_nodes &&
            run.summary.statistics.structural_v2_projection_bypasses > 0 &&
            run.summary.statistics.structural_v2_eligible_probes == 0 &&
            run.summary.statistics.structural_cache_hits == 0 &&
            run.summary.statistics.structural_cache_misses == 0 &&
            run.summary.cache_memory_bytes == 0,
        "static projection gate avoids every lookup on a full-prefix query");
}

struct ConstantRuntimeKeyHash {
    std::size_t operator()(const RuntimeKey&) const noexcept { return 0; }
};

void testRuntimeKeyCollisionUsesFullEquality() {
    MemoOptions options;
    options.budget_bytes = 1ULL << 20;
    options.max_entry_bytes = 1ULL << 20;
    options.minimum_estimated_work = 0;
    options.cost_aware_admission = false;
    options.shard_count = 1;
    BoundedMemo<
        RuntimeKey,
        std::vector<int>,
        ConstantRuntimeKeyHash> memo(options);
    RuntimeKey first;
    first.static_fingerprint = 77;
    first.node_id = 9;
    first.bindings = {3, 5};
    RuntimeKey second = first;
    second.bindings = {3, 6};
    memo.insertOrGet(
        first,
        std::make_shared<const std::vector<int>>(
            std::vector<int>{11}),
        sizeof(int),
        10,
        first.bindings.capacity() * sizeof(int),
        1);
    require(
        !memo.find(second),
        "hash collision cannot match a different full binding vector");
    memo.insertOrGet(
        second,
        std::make_shared<const std::vector<int>>(
            std::vector<int>{12}),
        sizeof(int),
        10,
        second.bindings.capacity() * sizeof(int),
        1);
    const auto first_value = memo.find(first);
    const auto second_value = memo.find(second);
    require(
        first_value && second_value &&
            first_value->front() == 11 &&
            second_value->front() == 12 &&
            memo.snapshot().entries == 2,
        "full RuntimeKey equality disambiguates forced hash collisions");
}

void testBoundedMemoBudgetAndConcurrency() {
    MemoOptions rejection_options;
    rejection_options.budget_bytes = 4096;
    rejection_options.max_entry_bytes = 16;
    rejection_options.minimum_estimated_work = 0;
    rejection_options.cost_aware_admission = false;
    rejection_options.shard_count = 2;
    BoundedMemo<
        int,
        std::vector<int>,
        std::hash<int>> rejection_memo(rejection_options);
    auto large_value =
        std::make_shared<const std::vector<int>>(100, 7);
    MemoInsertOutcome rejected_outcome;
    rejection_memo.insertOrGet(
        1,
        large_value,
        large_value->size() * sizeof(int),
        100,
        0,
        large_value->size(),
        &rejected_outcome);
    const MemoSnapshot rejected = rejection_memo.snapshot();
    require(
        rejected.bytes == 0 &&
            rejected.rejected_entry_size == 1 &&
            rejected_outcome.disposition ==
                MemoInsertDisposition::RejectedEntrySize &&
            rejected_outcome.admitted_bytes == 0,
        "oversized memo entry is rejected by an exact per-call reason");

    MemoOptions zero_options = rejection_options;
    zero_options.budget_bytes = 0;
    zero_options.max_entry_bytes = 0;
    BoundedMemo<
        int,
        std::vector<int>,
        std::hash<int>> zero_memo(zero_options);
    MemoInsertOutcome budget_outcome;
    const auto budget_value = zero_memo.insertOrGet(
        2,
        std::make_shared<const std::vector<int>>(1, 3),
        sizeof(int),
        1,
        0,
        1,
        &budget_outcome);
    require(
        budget_value && budget_outcome.disposition ==
                            MemoInsertDisposition::RejectedBudget &&
            budget_outcome.admitted_bytes == 0 &&
            zero_memo.snapshot().bypassed == 1,
        "zero-budget bypass is visible without being an admission");

    MemoOptions stress_options;
    stress_options.budget_bytes = 1ULL << 20;
    stress_options.max_entry_bytes = 1ULL << 20;
    stress_options.minimum_estimated_work = 0;
    stress_options.cost_aware_admission = false;
    stress_options.shard_count = 8;
    BoundedMemo<
        int,
        std::vector<int>,
        std::hash<int>> memo(stress_options);
    constexpr int worker_count = 8;
    std::atomic<int> ready{0};
    std::atomic<int> looked_up{0};
    std::atomic<bool> start{false};
    std::atomic<bool> insert{false};
    std::atomic<bool> values_valid{true};
    std::array<MemoInsertOutcome, worker_count> insertion_outcomes{};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&, worker]() {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (memo.find(7)) {
                values_valid.store(false, std::memory_order_relaxed);
            }
            looked_up.fetch_add(1, std::memory_order_release);
            while (!insert.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            auto value =
                std::make_shared<const std::vector<int>>(64, 9);
            const auto returned = memo.insertOrGet(
                7,
                value,
                value->size() * sizeof(int),
                1000,
                0,
                value->size(),
                &insertion_outcomes[static_cast<std::size_t>(worker)]);
            if (!returned ||
                returned->size() != 64 ||
                returned->front() != 9) {
                values_valid.store(false, std::memory_order_relaxed);
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != worker_count) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    while (looked_up.load(std::memory_order_acquire) != worker_count) {
        std::this_thread::yield();
    }
    insert.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    const auto hit = memo.find(7);
    const MemoSnapshot snapshot = memo.snapshot();
    std::uint64_t admitted_outcomes = 0;
    std::uint64_t duplicate_outcomes = 0;
    std::uint64_t admitted_bytes = 0;
    for (const MemoInsertOutcome& outcome : insertion_outcomes) {
        if (outcome.disposition == MemoInsertDisposition::Admitted) {
            ++admitted_outcomes;
            admitted_bytes += outcome.admitted_bytes;
        } else if (
            outcome.disposition == MemoInsertDisposition::Duplicate) {
            ++duplicate_outcomes;
        }
    }
    require(
        values_valid.load(std::memory_order_relaxed) &&
            hit && snapshot.entries == 1 &&
            snapshot.duplicate_inserts ==
                static_cast<std::uint64_t>(worker_count - 1) &&
            snapshot.bytes <= stress_options.budget_bytes &&
            snapshot.hit_value_elements == 64 &&
            snapshot.hit_saved_work == 1000 &&
            admitted_outcomes == 1 &&
            duplicate_outcomes ==
                static_cast<std::uint64_t>(worker_count - 1) &&
            admitted_bytes == snapshot.bytes,
        "sharded bounded memo reports the one true admission and exact "
        "charged bytes under a duplicate-insert race");
}

} // namespace

int main() {
    try {
        const std::filesystem::path directory =
            std::filesystem::temp_directory_path() /
            ("ours_gopher_tests_" +
             std::to_string(
                 static_cast<unsigned long long>(
                     std::random_device{}())));
        std::filesystem::create_directories(directory);
        try {
            testLayeredCacheAndAblations(directory);
            testTemporalCostV2Handoff(directory);
            testRandomGraphsAgainstIndependentOracle(directory);
            testFullAndCountOnlyParity(directory);
            testReciprocalSelfLoopExactMappings(directory);
            testPlanFingerprintAndDagTopology();
            testCandidateDagNoInternIsolation();
            testMinimalRuntimeBindings();
            testStructuralV2ProbeBeforeParents();
            testStructuralV2StaticNoReuseGate();
            testRuntimeKeyCollisionUsesFullEquality();
            testBoundedMemoBudgetAndConcurrency();
        } catch (...) {
            std::filesystem::remove_all(directory);
            throw;
        }
        std::filesystem::remove_all(directory);
        std::cout << "All ours_gopher tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
