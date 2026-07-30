#include "../CandidateDag.h"
#include "../BoundedMemo.h"
#include "../GopherMatcher.h"
#include "../TDTree.h"
#include "../Utils.h"
#include "../query_decomposition.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
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
    for (const auto& interval : edge.active_intervals) {
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
            if (edge == nullptr || edge->active_intervals.empty()) {
                return false;
            }
            edges.push_back(edge);
            const int edge_first =
                edge->active_intervals.front().start;
            const int edge_last =
                edge->active_intervals.back().end;
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
    std::size_t tasks_per_thread = 8) {
    GopherRun run;
    run.plan = makeQueryPlan(
        query, decomposition, tree, order);
    CandidateDagCompiler compiler;
    run.dag = compiler.compile(
        query, run.plan, decomposition);

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
    std::unordered_set<std::string> expressions;
    for (const DagNode& node : run.dag.nodes) {
        require(
            expressions.insert(
                node.canonical_expression).second,
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
    const std::filesystem::path& output) {
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
    require(decomposition.root == 0, "layered root enables reuse");
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
                run.summary.threads_used == 4,
                "layered count-only uses requested threads");
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
        "intended selectivity changes the layered root");
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
        const std::vector<std::string> gopher_mappings =
            fullGopherMappingKeys(
                graph,
                query,
                decomposition,
                tree,
                k,
                directory /
                    ("gopher_exact_" +
                     std::to_string(seed) + ".txt"));
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
        plan.order == std::vector<int>({0, 1, 2}),
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
    rejection_memo.insertOrGet(
        1,
        large_value,
        large_value->size() * sizeof(int),
        100,
        0,
        large_value->size());
    const MemoSnapshot rejected = rejection_memo.snapshot();
    require(
        rejected.bytes == 0 &&
            rejected.rejected_entry_size == 1,
        "oversized memo entry is rejected by reason");

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
    std::vector<std::thread> workers;
    for (int worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&]() {
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
                value->size());
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
    require(
        values_valid.load(std::memory_order_relaxed) &&
            hit && snapshot.entries == 1 &&
            snapshot.duplicate_inserts ==
                static_cast<std::uint64_t>(worker_count - 1) &&
            snapshot.bytes <= stress_options.budget_bytes &&
            snapshot.hit_value_elements == 64 &&
            snapshot.hit_saved_work == 1000,
        "sharded bounded memo is race-safe and reports reuse work");
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
            testRandomGraphsAgainstIndependentOracle(directory);
            testFullAndCountOnlyParity(directory);
            testReciprocalSelfLoopExactMappings(directory);
            testMinimalRuntimeBindings();
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
