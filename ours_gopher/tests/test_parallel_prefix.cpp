#include "../CandidateDag.h"
#include "../GopherMatcher.h"
#include "../TDTree.h"
#include "../Utils.h"
#include "../query_decomposition.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void addEdge(Graph& graph, int source, int target) {
    const int edge_id =
        static_cast<int>(graph.temporal_edges.size());
    graph.temporal_edges.push_back(
        {source, target, {{1, 3}}, 3});
    graph.adj[static_cast<std::size_t>(source)].push_back(
        {target, edge_id});
    graph.in_adj[static_cast<std::size_t>(target)].push_back(
        {source, edge_id});
}

Graph makeSkewedData(int width) {
    Graph graph;
    graph.num_vertices = 1 + 4 * width;
    graph.vertex_labels.resize(
        static_cast<std::size_t>(graph.num_vertices));
    graph.vertex_labels[0] = labelFromString("B");
    graph.adj.resize(static_cast<std::size_t>(graph.num_vertices));
    graph.in_adj.resize(static_cast<std::size_t>(graph.num_vertices));

    const std::array<Label, 4> leaf_labels = {
        labelFromString("A"),
        labelFromString("C"),
        labelFromString("D"),
        labelFromString("E")};
    for (std::size_t group = 0;
         group < leaf_labels.size();
         ++group) {
        for (int offset = 0; offset < width; ++offset) {
            const int vertex =
                1 + static_cast<int>(group) * width + offset;
            graph.vertex_labels[
                static_cast<std::size_t>(vertex)] =
                leaf_labels[group];
            addEdge(graph, 0, vertex);
        }
    }

    graph.external_ids.resize(
        static_cast<std::size_t>(graph.num_vertices));
    graph.vertex_active_durations.assign(
        static_cast<std::size_t>(graph.num_vertices), 3);
    graph.neighbor_label_counts.assign(
        static_cast<std::size_t>(graph.num_vertices), {});
    graph.incoming_neighbor_label_counts.assign(
        static_cast<std::size_t>(graph.num_vertices), {});
    for (int vertex = 0;
         vertex < graph.num_vertices;
         ++vertex) {
        graph.external_ids[static_cast<std::size_t>(vertex)] =
            1000 + vertex;
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
    return graph;
}

Graph makeStarQuery() {
    Graph query;
    query.num_vertices = 5;
    query.vertex_labels = {
        labelFromString("B"),
        labelFromString("A"),
        labelFromString("C"),
        labelFromString("D"),
        labelFromString("E")};
    query.external_ids = {0, 1, 2, 3, 4};
    query.adj.resize(5);
    query.in_adj.resize(5);
    for (int leaf = 1; leaf < 5; ++leaf) {
        query.adj[0].push_back({leaf, -1});
        query.in_adj[static_cast<std::size_t>(leaf)]
            .push_back({0, -1});
    }
    return query;
}

QueryDecomposition decompose(
    const Graph& graph,
    const Graph& query) {
    std::array<std::size_t, kLabelCount> counts{};
    std::array<double, kLabelCount> lifespans{};
    for (Label label : graph.vertex_labels) ++counts[label];
    for (std::size_t label = 0;
         label < kLabelCount;
         ++label) {
        if (counts[label] != 0) lifespans[label] = 3.0;
    }
    return decomposeQuery(
        query,
        counts,
        lifespans,
        SelectivityMode::Intended);
}

GopherMatchSummary run(
    const Graph& graph,
    const Graph& query,
    const QueryDecomposition& decomposition,
    const TDTree& tree,
    ParallelMode parallel_mode,
    std::size_t threads,
    std::size_t prefix_depth,
    bool collect_statistics) {
    const QueryPlan plan = makeQueryPlan(
        query,
        decomposition,
        tree,
        OrderMode::ReuseAware);
    CandidateDagCompiler compiler;
    const CandidateDag dag =
        compiler.compile(query, plan, decomposition);

    GopherMatcherOptions options;
    options.threads = threads;
    options.parallel_mode = parallel_mode;
    options.prefix_depth = prefix_depth;
    options.tasks_per_thread = 2;
    options.order_mode = OrderMode::ReuseAware;
    options.result_digest = true;
    options.executor.memo_mode = MemoMode::None;
    options.executor.collect_statistics = collect_statistics;

    GopherMatcher matcher(
        graph,
        query,
        plan,
        dag,
        tree,
        3,
        options);
    return matcher.run(MatchOutputMode::CountOnly);
}

} // namespace

int main() {
    try {
        constexpr int width = 16;
        constexpr std::uint64_t expected = 65'536;
        const Graph graph = makeSkewedData(width);
        const Graph query = makeStarQuery();
        const QueryDecomposition decomposition =
            decompose(graph, query);
        require(
            decomposition.connected && decomposition.root == 0,
            "controlled query must have one center root");
        const TDTree tree(graph, query, decomposition, 3);
        require(
            tree.rootCandidates().size() == 1,
            "controlled data must expose exactly one root task");

        const GopherMatchSummary roots = run(
            graph,
            query,
            decomposition,
            tree,
            ParallelMode::Roots,
            8,
            0,
            true);
        const GopherMatchSummary prefix = run(
            graph,
            query,
            decomposition,
            tree,
            ParallelMode::Prefix,
            8,
            0,
            true);
        require(
            roots.match_count == expected &&
                prefix.match_count == expected &&
                roots.result_digest == prefix.result_digest,
            "root and prefix schedulers must be exactly equivalent");
        require(
            roots.threads_used == 1 &&
                prefix.threads_used == 8,
            "prefix scheduler must expose a single root to all workers");
        require(
            prefix.parallel_statistics.prefix_tasks_spawned > 0 &&
                prefix.parallel_statistics.queue_peak <= 16 &&
                prefix.parallel_statistics
                        .queue_state_bytes_peak > 0 &&
                prefix.parallel_statistics.worker_tasks_total ==
                    prefix.parallel_statistics.queue_claims,
            "prefix task queue must be active, bounded, and accounted");
        require(
            prefix.parallel_statistics.worker_dfs_states_total ==
                prefix.statistics.dfs_states,
            "always-on DFS accounting must agree with enabled counters");

        const GopherMatchSummary no_counters = run(
            graph,
            query,
            decomposition,
            tree,
            ParallelMode::Prefix,
            4,
            0,
            false);
        require(
            no_counters.match_count == expected &&
                no_counters.result_digest == roots.result_digest &&
                no_counters.statistics.dfs_states == 0 &&
                no_counters.parallel_statistics
                        .worker_dfs_states_total > 0,
            "load-balance metrics must remain valid with counters off");

        const GopherMatchSummary depth_one = run(
            graph,
            query,
            decomposition,
            tree,
            ParallelMode::Prefix,
            8,
            1,
            true);
        require(
            depth_one.match_count == expected &&
                depth_one.result_digest == roots.result_digest &&
                depth_one.parallel_statistics
                        .prefix_tasks_spawned == 0,
            "depth-one prefix mode must reduce exactly to root tasks");

        const GopherMatchSummary prefix_single = run(
            graph,
            query,
            decomposition,
            tree,
            ParallelMode::Prefix,
            1,
            0,
            true);
        require(
            prefix_single.match_count == expected &&
                prefix_single.result_digest == roots.result_digest &&
                prefix_single.parallel_mode_effective ==
                    "roots-single-worker" &&
                prefix_single.parallel_statistics
                        .prefix_tasks_spawned == 0,
            "single-thread prefix request must avoid queue overhead");

        std::cout << "Dynamic prefix scheduler tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
