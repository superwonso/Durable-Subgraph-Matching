#include "../TDTree.h"
#include "../Utils.h"
#include "../query_decomposition.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Graph makeTriangleQuery() {
    Graph query;
    query.num_vertices = 3;
    query.vertex_labels = {
        labelFromString("A"), labelFromString("B"), labelFromString("C")};
    query.external_ids = {0, 1, 2};
    query.adj.resize(3);
    query.in_adj.resize(3);
    const std::array<std::pair<int, int>, 3> edges{{{0, 1}, {1, 2}, {2, 0}}};
    for (const auto& edge : edges) {
        query.adj[static_cast<std::size_t>(edge.first)].push_back({edge.second, -1});
        query.in_adj[static_cast<std::size_t>(edge.second)].push_back({edge.first, -1});
    }
    for (auto& neighbors : query.adj) {
        std::sort(neighbors.begin(), neighbors.end(), [](const Edge& lhs, const Edge& rhs) {
            return lhs.to < rhs.to;
        });
    }
    for (auto& neighbors : query.in_adj) {
        std::sort(neighbors.begin(), neighbors.end(), [](const Edge& lhs, const Edge& rhs) {
            return lhs.to < rhs.to;
        });
    }
    return query;
}

Graph makeTwoVertexQuery(bool reciprocal) {
    Graph query;
    query.num_vertices = 2;
    query.vertex_labels = {labelFromString("A"), labelFromString("B")};
    query.external_ids = {0, 1};
    query.adj.resize(2);
    query.in_adj.resize(2);
    query.adj[0].push_back({1, -1});
    query.in_adj[1].push_back({0, -1});
    if (reciprocal) {
        query.adj[1].push_back({0, -1});
        query.in_adj[0].push_back({1, -1});
    }
    return query;
}

void addTemporalEdge(
    Graph& graph,
    int u,
    int v,
    std::vector<TimeInterval> intervals) {
    int active_count = 0;
    for (const auto& interval : intervals) active_count += interval.length();
    const int edge_id = static_cast<int>(graph.temporal_edges.size());
    graph.temporal_edges.push_back({u, v, std::move(intervals), active_count});
    graph.adj[static_cast<std::size_t>(u)].push_back({v, edge_id});
    graph.in_adj[static_cast<std::size_t>(v)].push_back({u, edge_id});
}

std::vector<TimeInterval> intervalsFromMask(std::uint32_t mask) {
    std::vector<TimeInterval> intervals;
    for (int snapshot = 1; snapshot <= 6; ++snapshot) {
        if ((mask & (1U << (snapshot - 1))) == 0) continue;
        if (!intervals.empty() && intervals.back().end + 1 == snapshot) {
            intervals.back().end = snapshot;
        } else {
            intervals.push_back({snapshot, snapshot});
        }
    }
    return intervals;
}

void finalizeSyntheticGraph(Graph& graph) {
    graph.vertex_active_durations.assign(static_cast<std::size_t>(graph.num_vertices), 6);
    graph.neighbor_label_counts.assign(static_cast<std::size_t>(graph.num_vertices), {});
    graph.incoming_neighbor_label_counts.assign(
        static_cast<std::size_t>(graph.num_vertices), {});
    for (std::size_t vertex = 0; vertex < graph.adj.size(); ++vertex) {
        for (const auto& edge : graph.adj[vertex]) {
            ++graph.neighbor_label_counts[vertex]
                [graph.vertex_labels[static_cast<std::size_t>(edge.to)]];
        }
        std::sort(graph.adj[vertex].begin(), graph.adj[vertex].end(),
            [](const Edge& lhs, const Edge& rhs) { return lhs.to < rhs.to; });
        for (const auto& edge : graph.in_adj[vertex]) {
            ++graph.incoming_neighbor_label_counts[vertex]
                [graph.vertex_labels[static_cast<std::size_t>(edge.to)]];
        }
        std::sort(graph.in_adj[vertex].begin(), graph.in_adj[vertex].end(),
            [](const Edge& lhs, const Edge& rhs) { return lhs.to < rhs.to; });
    }
}

std::uint64_t bruteForceTriangleCount(const Graph& graph, int minimum_duration) {
    const Label label_a = labelFromString("A");
    const Label label_b = labelFromString("B");
    const Label label_c = labelFromString("C");
    std::uint64_t count = 0;

    for (int a = 0; a < graph.num_vertices; ++a) {
        if (graph.vertex_labels[static_cast<std::size_t>(a)] != label_a) continue;
        for (int b = 0; b < graph.num_vertices; ++b) {
            if (a == b || graph.vertex_labels[static_cast<std::size_t>(b)] != label_b) continue;
            const TemporalEdge* edge_ab = graph.findTemporalEdge(a, b);
            if (edge_ab == nullptr) continue;
            for (int c = 0; c < graph.num_vertices; ++c) {
                if (c == a || c == b ||
                    graph.vertex_labels[static_cast<std::size_t>(c)] != label_c) {
                    continue;
                }
                const TemporalEdge* edge_ca = graph.findTemporalEdge(c, a);
                const TemporalEdge* edge_bc = graph.findTemporalEdge(b, c);
                if (edge_ca == nullptr || edge_bc == nullptr) continue;

                auto common = intersectTimeIntervals(
                    edge_ab->active_intervals, edge_bc->active_intervals, minimum_duration);
                common = intersectTimeIntervals(
                    common, edge_ca->active_intervals, minimum_duration);
                if (!common.empty()) ++count;
            }
        }
    }
    return count;
}

Graph makeMatchingDataGraph() {
    Graph graph;
    graph.num_vertices = 6;
    graph.external_ids = {10, 20, 30, 40, 50, 60};
    graph.vertex_labels = {
        labelFromString("A"), labelFromString("B"), labelFromString("C"),
        labelFromString("A"), labelFromString("B"), labelFromString("C")};
    graph.adj.resize(6);
    graph.in_adj.resize(6);

    // Exactly one durable triangle: common interval is [3,5].
    addTemporalEdge(graph, 0, 1, {{1, 5}});
    addTemporalEdge(graph, 1, 2, {{2, 6}});
    addTemporalEdge(graph, 2, 0, {{3, 7}});

    // Structurally valid and each edge lasts for three snapshots, but the
    // three-way common interval has length one and must not be reported.
    addTemporalEdge(graph, 3, 4, {{10, 12}});
    addTemporalEdge(graph, 4, 5, {{11, 13}});
    addTemporalEdge(graph, 5, 3, {{12, 14}});
    finalizeSyntheticGraph(graph);
    return graph;
}

QueryDecomposition makeDecomposition(const Graph& query) {
    std::array<std::size_t, kLabelCount> counts{};
    counts[0] = counts[1] = counts[2] = 2;
    return decomposeQuery(query, counts);
}

void testIntervals() {
    const std::vector<TimeInterval> lhs{{1, 5}, {10, 20}};
    const std::vector<TimeInterval> rhs{{3, 8}, {12, 14}, {18, 22}};
    const auto intersection = intersectTimeIntervals(lhs, rhs, 3);
    require(intersection.size() == 3, "interval intersection count");
    require(intersection[0].start == 3 && intersection[0].end == 5, "first interval");
    require(intersection[1].start == 12 && intersection[1].end == 14, "second interval");
    require(intersection[2].start == 18 && intersection[2].end == 20, "third interval");
    require(hasMinimumConsecutiveDuration(intersection, 3), "minimum run accepted");
    require(!hasMinimumConsecutiveDuration(intersection, 4), "minimum run rejected");
}

void testCheckedMatchCounterOverflow() {
    CheckedMatchCounter counter;
    counter.value = std::numeric_limits<std::uint64_t>::max() - 1;
    require(counter.increment(), "last representable match count increment succeeds");
    require(counter.value == std::numeric_limits<std::uint64_t>::max() &&
            !counter.overflowed,
            "counter reaches uint64 max without wrapping");
    require(!counter.increment(), "overflowing match count increment is rejected");
    require(counter.value == std::numeric_limits<std::uint64_t>::max() &&
            counter.overflowed,
            "overflow leaves the saturated count and records explicit state");
}

void testNoPrefilterAndDenseIds(const std::filesystem::path& directory) {
    const auto first_path = directory / "original_history_order_1.dat";
    const auto second_path = directory / "original_history_order_2.dat";
    {
        std::ofstream output(first_path);
        output << "1000000 5 4\n8 9 3\n5 1000000 2\n8 9 1\n"
                  "5 1000000 3\n9 9 7\n9 9 7\n";
    }
    {
        std::ofstream output(second_path);
        output << "9 9 7\n5 1000000 3\n8 9 1\n5 1000000 2\n"
                  "8 9 3\n1000000 5 4\n9 9 7\n";
    }

    Graph first;
    Graph second;
    Graph alternate_seed;
    require(readTemporalGraph(first_path.string(), first), "read first temporal fixture");
    require(readTemporalGraph(second_path.string(), second), "read reordered temporal fixture");
    require(readTemporalGraph(first_path.string(), alternate_seed, 43U),
            "read temporal fixture with alternate label seed");
    require(first.input_occurrence_count == 7, "all input occurrences counted");
    require(first.input_unique_edge_count == 4, "directed unique edge count");
    require(first.retained_edge_count == 4, "Original retains every edge history");
    require(first.num_vertices == 4, "sparse IDs are compacted without phantom vertices");
    require(first.external_ids == std::vector<int>({5, 8, 9, 1000000}),
            "external ID restoration");

    const TemporalEdge* consecutive = first.findTemporalEdge(0, 3);
    require(consecutive != nullptr && consecutive->active_intervals.size() == 1,
            "consecutive directed history retained");
    require(consecutive->active_intervals[0].start == 2 &&
            consecutive->active_intervals[0].end == 3,
            "compressed interval value");
    const TemporalEdge* nonconsecutive = first.findTemporalEdge(1, 2);
    require(nonconsecutive != nullptr && nonconsecutive->active_intervals.size() == 2,
            "nonconsecutive history is not prefiltered");
    require(first.findTemporalEdge(3, 0) != nullptr,
            "reverse directed history is retained separately");
    const TemporalEdge* self_loop = first.findTemporalEdge(2, 2);
    require(self_loop != nullptr && self_loop->active_snapshot_count == 1,
            "self-loop is retained and duplicate timestamps are deduplicated");
    require(first.external_ids == second.external_ids &&
            first.vertex_labels == second.vertex_labels,
            "seeded random labels are stable across input ordering");
    require(first.vertex_labels != alternate_seed.vertex_labels,
            "changing the random label seed changes the assignment");

    std::filesystem::remove(first_path);
    std::filesystem::remove(second_path);
}

void testStrictLineParsing(const std::filesystem::path& directory) {
    const auto malformed_temporal = directory / "original_malformed_rows.dat";
    const auto commented_temporal = directory / "original_commented_rows.dat";
    const auto malformed_query = directory / "original_malformed_query.qry";
    const auto commented_query = directory / "original_commented_query.qry";
    {
        std::ofstream output(malformed_temporal);
        output << "1 2\n3 4 5 6\n";
    }
    {
        std::ofstream output(commented_temporal);
        output << "  # comment\n\n1 2 3 # occurrence\n";
    }
    {
        std::ofstream output(malformed_query);
        output << "A B EXTRA\n";
    }
    {
        std::ofstream output(commented_query);
        output << "   # comment\n\nA B # directed arc\n";
    }

    Graph graph;
    Graph query;
    require(!readTemporalGraph(malformed_temporal.string(), graph),
            "temporal rows cannot borrow columns from the next line");
    require(readTemporalGraph(commented_temporal.string(), graph),
            "blank and commented temporal rows are accepted");
    require(!readQueryGraph(malformed_query.string(), query),
            "query rows reject trailing tokens");
    require(readQueryGraph(commented_query.string(), query),
            "blank, leading-space, and inline query comments are accepted");

    std::filesystem::remove(malformed_temporal);
    std::filesystem::remove(commented_temporal);
    std::filesystem::remove(malformed_query);
    std::filesystem::remove(commented_query);
}

void testRepeatedLabelQueryParsing(const std::filesystem::path& directory) {
    const auto query_path = directory / "original_repeated_label_query.qry";
    {
        std::ofstream output(query_path);
        output << "left:A center:B\ncenter:B right:A\n";
    }

    Graph query;
    require(readQueryGraph(query_path.string(), query), "read ID-qualified query labels");
    require(query.num_vertices == 3, "same-label query vertices remain distinct");
    require(query.vertex_labels == std::vector<Label>({
        labelFromString("A"), labelFromString("B"), labelFromString("A")}),
        "ID-qualified query labels are preserved");
    require(query.adj[1].size() == 1 && query.in_adj[1].size() == 1,
            "directed repeated-label path topology");
    require(GraphUtils::hasEdge(query.adj, 0, 1) &&
            GraphUtils::hasEdge(query.adj, 1, 2) &&
            !GraphUtils::hasEdge(query.adj, 1, 0),
            "query arc directions are retained");
    std::filesystem::remove(query_path);
}

void testTopologicalSelectivityAndRecursiveDfs() {
    const Graph query = makeTriangleQuery();
    std::array<std::size_t, kLabelCount> counts{};
    counts[0] = 1;
    counts[1] = 2;
    counts[2] = 4;

    const auto decomposition = decomposeQuery(query, counts);
    require(decomposition.root == 0, "S_topo argmin root");
    require(decomposition.topological_selectivity[0] == 0.5,
            "S_topo uses label count over directed total degree");
    require(decomposition.connected, "connected decomposition");
    require(decomposition.dfs_order == std::vector<int>({0, 1, 2}),
            "recursive DFS produces a deep triangle tree");
    require(decomposition.non_tree_edges == std::vector<std::pair<int, int>>({{2, 0}}),
            "directed tree and non-tree edges are disjoint and complete");

    const std::array<std::size_t, kLabelCount> empty_counts{};
    const auto no_candidate_decomposition =
        decomposeQuery(query, empty_counts);
    require(no_candidate_decomposition.root == 0 && no_candidate_decomposition.connected,
            "no-candidate queries still decompose and can report zero matches");
}

void testExactDurableMatching(const std::filesystem::path& directory) {
    const Graph data = makeMatchingDataGraph();
    const Graph query = makeTriangleQuery();
    const QueryDecomposition decomposition = makeDecomposition(query);
    require(decomposition.connected, "matching query decomposition");

    const auto result_path = directory / "original_exact_matching_result.dat";
    TDTree tree(data, query, decomposition, 3);
    const MatchSummary summary = tree.save_res(result_path.string());
    require(summary.output_written, "matching result file written");
    require(summary.match_count == 1, "only the common consecutive triangle is reported");

    std::ifstream result(result_path, std::ios::binary);
    const std::string contents(
        (std::istreambuf_iterator<char>(result)), std::istreambuf_iterator<char>());
    require(contents.find("Count:                    1") != std::string::npos,
            "match count header is intact");
    require(contents.find("mode: full") != std::string::npos,
            "default result mode is recorded");
    require(contents.find("Match 0:") != std::string::npos,
            "full mode writes individual match rows");
    require(contents.find("active=[3-5]") != std::string::npos,
            "exact common interval is reported");
    result.close();

    const auto count_only_path = directory / "original_exact_count_only.dat";
    const MatchSummary count_only_summary = tree.save_res(
        count_only_path.string(), MatchOutputMode::CountOnly);
    require(count_only_summary.output_written &&
            count_only_summary.match_count == summary.match_count &&
            !count_only_summary.count_overflow,
            "count-only mode performs the same exact enumeration");
    std::ifstream count_only_result(count_only_path, std::ios::binary);
    const std::string count_only_contents(
        (std::istreambuf_iterator<char>(count_only_result)),
        std::istreambuf_iterator<char>());
    require(count_only_contents.find("mode: count-only") != std::string::npos,
            "count-only result mode is recorded");
    require(count_only_contents.find("Count:                    1") != std::string::npos,
            "count-only result records the exact count");
    require(count_only_contents.find("Match ") == std::string::npos,
            "count-only result suppresses individual match rows");
    count_only_result.close();

    TDTree stricter_tree(data, query, decomposition, 4);
    const auto strict_path = directory / "original_exact_matching_strict_result.dat";
    const MatchSummary strict_summary = stricter_tree.save_res(strict_path.string());
    require(strict_summary.match_count == 0,
            "individual long edges without a common k-run are rejected");

    std::filesystem::remove(result_path);
    std::filesystem::remove(count_only_path);
    std::filesystem::remove(strict_path);
}

void testDirectedTreeOrientationAndReciprocalArcs(
    const std::filesystem::path& directory) {
    {
        const Graph query = makeTwoVertexQuery(false);
        std::array<std::size_t, kLabelCount> counts{};
        counts[labelFromString("A")] = 10;
        counts[labelFromString("B")] = 1;
        const QueryDecomposition decomposition =
            decomposeQuery(query, counts);
        require(decomposition.root == 1,
                "directed sink can be selected as the weak DFS root");
        require(decomposition.parent[0] == 1,
                "tree parent direction may oppose the query arc");

        Graph data;
        data.num_vertices = 4;
        data.external_ids = {10, 20, 30, 40};
        data.vertex_labels = {
            labelFromString("A"), labelFromString("B"),
            labelFromString("A"), labelFromString("B")};
        data.adj.resize(4);
        data.in_adj.resize(4);
        addTemporalEdge(data, 0, 1, {{1, 3}}); // valid A -> B
        addTemporalEdge(data, 3, 2, {{1, 3}}); // reverse-only decoy B -> A
        finalizeSyntheticGraph(data);

        const auto result_path = directory / "original_directed_sink_root.dat";
        TDTree tree(data, query, decomposition, 3);
        const MatchSummary summary = tree.save_res(result_path.string());
        require(summary.match_count == 1,
                "incoming expansion from a sink root preserves arc direction");
        std::filesystem::remove(result_path);
    }

    {
        const Graph query = makeTwoVertexQuery(true);
        std::array<std::size_t, kLabelCount> counts{};
        counts[labelFromString("A")] = counts[labelFromString("B")] = 2;
        const QueryDecomposition decomposition =
            decomposeQuery(query, counts);

        Graph data;
        data.num_vertices = 4;
        data.external_ids = {11, 22, 33, 44};
        data.vertex_labels = {
            labelFromString("A"), labelFromString("B"),
            labelFromString("A"), labelFromString("B")};
        data.adj.resize(4);
        data.in_adj.resize(4);
        addTemporalEdge(data, 0, 1, {{1, 4}});
        addTemporalEdge(data, 1, 0, {{2, 4}}); // common run [2,4]
        addTemporalEdge(data, 2, 3, {{10, 12}});
        addTemporalEdge(data, 3, 2, {{12, 14}}); // individual k, common length one
        finalizeSyntheticGraph(data);

        const auto result_path = directory / "original_directed_reciprocal.dat";
        TDTree tree(data, query, decomposition, 3);
        const MatchSummary summary = tree.save_res(result_path.string());
        require(summary.match_count == 1,
                "both reciprocal query arcs share one durable interval");
        std::ifstream result(result_path, std::ios::binary);
        const std::string contents(
            (std::istreambuf_iterator<char>(result)), std::istreambuf_iterator<char>());
        require(contents.find("active=[2-4]") != std::string::npos,
                "reciprocal arc histories are intersected, not merged");
        result.close();
        std::filesystem::remove(result_path);
    }
}

void testDirectedAndNonTreeRejections(const std::filesystem::path& directory) {
    {
        const Graph query = makeTwoVertexQuery(false);
        const QueryDecomposition decomposition = makeDecomposition(query);

        Graph data;
        data.num_vertices = 2;
        data.external_ids = {10, 20};
        data.vertex_labels = {labelFromString("A"), labelFromString("B")};
        data.adj.resize(2);
        data.in_adj.resize(2);
        addTemporalEdge(data, 1, 0, {{1, 4}}); // only B -> A
        finalizeSyntheticGraph(data);

        const auto result_path = directory / "original_reverse_only.dat";
        TDTree tree(data, query, decomposition, 3);
        require(tree.save_res(result_path.string()).match_count == 0,
                "reverse-only data arc must not satisfy A -> B");
        std::filesystem::remove(result_path);
    }

    {
        const Graph query = makeTriangleQuery();
        const QueryDecomposition decomposition = makeDecomposition(query);

        Graph data;
        data.num_vertices = 3;
        data.external_ids = {10, 20, 30};
        data.vertex_labels = {
            labelFromString("A"), labelFromString("B"), labelFromString("C")};
        data.adj.resize(3);
        data.in_adj.resize(3);
        addTemporalEdge(data, 0, 1, {{1, 4}});
        addTemporalEdge(data, 1, 2, {{1, 4}});
        // Missing non-tree query arc C -> A.
        finalizeSyntheticGraph(data);

        const auto result_path = directory / "original_missing_non_tree.dat";
        TDTree tree(data, query, decomposition, 3);
        require(tree.save_res(result_path.string()).match_count == 0,
                "a missing directed non-tree arc must reject the embedding");
        std::filesystem::remove(result_path);
    }
}

void testSelfLoopDurability(const std::filesystem::path& directory) {
    Graph query;
    query.num_vertices = 1;
    query.external_ids = {0};
    query.vertex_labels = {labelFromString("A")};
    query.adj.resize(1);
    query.in_adj.resize(1);
    query.adj[0].push_back({0, -1});
    query.in_adj[0].push_back({0, -1});
    const QueryDecomposition decomposition = makeDecomposition(query);

    Graph data;
    data.num_vertices = 2;
    data.external_ids = {10, 20};
    data.vertex_labels = {labelFromString("A"), labelFromString("A")};
    data.adj.resize(2);
    data.in_adj.resize(2);
    addTemporalEdge(data, 0, 0, {{1, 3}});
    addTemporalEdge(data, 1, 1, {{1, 1}, {3, 3}});
    finalizeSyntheticGraph(data);

    const auto result_path = directory / "original_self_loop.dat";
    TDTree tree(data, query, decomposition, 3);
    require(tree.save_res(result_path.string()).match_count == 1,
            "self-loop must itself contain the common k-run");
    std::filesystem::remove(result_path);
}

void testInjectiveMapping(const std::filesystem::path& directory) {
    Graph query;
    query.num_vertices = 2;
    query.external_ids = {0, 1};
    query.vertex_labels = {labelFromString("A"), labelFromString("A")};
    query.adj.resize(2);
    query.in_adj.resize(2);
    query.adj[0].push_back({1, -1});
    query.in_adj[1].push_back({0, -1});
    const QueryDecomposition decomposition = makeDecomposition(query);

    Graph data;
    data.num_vertices = 1;
    data.external_ids = {10};
    data.vertex_labels = {labelFromString("A")};
    data.adj.resize(1);
    data.in_adj.resize(1);
    addTemporalEdge(data, 0, 0, {{1, 3}});
    finalizeSyntheticGraph(data);

    const auto result_path = directory / "original_injective.dat";
    TDTree tree(data, query, decomposition, 3);
    require(tree.save_res(result_path.string()).match_count == 0,
            "two query vertices cannot reuse one data vertex");
    std::filesystem::remove(result_path);
}

void testRandomGraphsAgainstBruteForce(const std::filesystem::path& directory) {
    constexpr int minimum_duration = 2;
    bool saw_match = false;
    bool saw_zero_match_graph = false;
    std::uint32_t state = 0x6d2b79f5U;
    auto next_random = [&]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };

    for (int round = 0; round < 32; ++round) {
        Graph graph;
        graph.num_vertices = 6;
        graph.external_ids = {101, 205, 309, 413, 517, 621};
        graph.vertex_labels = {
            labelFromString("A"), labelFromString("B"), labelFromString("C"),
            labelFromString("A"), labelFromString("B"), labelFromString("C")};
        graph.adj.resize(6);
        graph.in_adj.resize(6);

        for (int u = 0; u < graph.num_vertices; ++u) {
            for (int v = 0; v < graph.num_vertices; ++v) {
                if (u == v) continue;
                const std::uint32_t mask = next_random() & 0x3fU;
                if (mask == 0 || (next_random() & 3U) == 0) continue;
                addTemporalEdge(graph, u, v, intervalsFromMask(mask));
            }
        }
        finalizeSyntheticGraph(graph);

        const Graph query = makeTriangleQuery();
        const QueryDecomposition decomposition = makeDecomposition(query);
        const std::uint64_t expected =
            bruteForceTriangleCount(graph, minimum_duration);
        saw_match = saw_match || expected > 0;
        saw_zero_match_graph = saw_zero_match_graph || expected == 0;
        const auto result_path = directory /
            ("original_random_oracle_" + std::to_string(round) + ".dat");
        TDTree tree(graph, query, decomposition, minimum_duration);
        const MatchSummary actual = tree.save_res(result_path.string());
        require(actual.output_written, "random oracle result file written");
        require(actual.match_count == expected,
                "TD-tree result differs from brute-force oracle in round " +
                    std::to_string(round));
        std::filesystem::remove(result_path);
    }
    require(saw_match && saw_zero_match_graph,
            "random oracle fixtures must include positive and zero-match graphs");
}

} // namespace

int main() {
    try {
        const auto temp_directory = std::filesystem::temp_directory_path();
        testIntervals();
        testCheckedMatchCounterOverflow();
        testNoPrefilterAndDenseIds(temp_directory);
        testStrictLineParsing(temp_directory);
        testRepeatedLabelQueryParsing(temp_directory);
        testTopologicalSelectivityAndRecursiveDfs();
        testExactDurableMatching(temp_directory);
        testDirectedTreeOrientationAndReciprocalArcs(temp_directory);
        testDirectedAndNonTreeRejections(temp_directory);
        testSelfLoopDurability(temp_directory);
        testInjectiveMapping(temp_directory);
        testRandomGraphsAgainstBruteForce(temp_directory);
        std::cout << "All original tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
