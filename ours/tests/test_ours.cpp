#include "../TDTree.h"
#include "../Utils.h"
#include "../query_decomposition.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    std::array<double, kLabelCount> lifespans{};
    counts[0] = counts[1] = counts[2] = 2;
    lifespans[0] = lifespans[1] = lifespans[2] = 5.0;
    return decomposeQuery(query, counts, lifespans);
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

void testFilteringAndDenseIds(const std::filesystem::path& directory) {
    const auto first_path = directory / "ours_filter_order_1.dat";
    const auto second_path = directory / "ours_filter_order_2.dat";
    const auto different_survival_path = directory / "ours_filter_survival.dat";
    {
        std::ofstream output(first_path);
        output << "1000000 5 4\n8 9 3\n5 1000000 2\n8 9 1\n5 1000000 3\n";
    }
    {
        std::ofstream output(second_path);
        output << "5 1000000 3\n8 9 1\n5 1000000 2\n8 9 3\n1000000 5 4\n";
    }
    {
        std::ofstream output(different_survival_path);
        output << "1000000 5 4\n8 9 2\n5 1000000 2\n8 9 1\n5 1000000 3\n";
    }

    Graph first;
    Graph second;
    Graph alternate_seed;
    Graph different_survival;
    require(readTemporalGraph(first_path.string(), first), "read first temporal fixture");
    require(readTemporalGraph(second_path.string(), second), "read reordered temporal fixture");
    require(readTemporalGraph(first_path.string(), alternate_seed, 43U),
            "read temporal fixture with alternate label seed");
    require(readTemporalGraph(different_survival_path.string(), different_survival),
            "read temporal fixture with a different filtered vertex set");
    require(first.input_unique_edge_count == 3, "directed unique edge count before filter");
    require(first.filtered_edge_count == 1, "transient edge removed");
    require(first.num_vertices == 2, "only active sparse IDs are compacted");
    require(first.external_ids == std::vector<int>({5, 1000000}), "external ID restoration");
    require(first.temporal_edges[0].active_intervals.size() == 1, "compressed interval count");
    require(first.temporal_edges[0].active_intervals[0].start == 2 &&
            first.temporal_edges[0].active_intervals[0].end == 3,
            "compressed interval value");
    require(first.externalId(first.temporal_edges[0].u) == 5 &&
            first.externalId(first.temporal_edges[0].v) == 1000000,
            "directed edge orientation is retained");
    require(first.findTemporalEdge(0, 1) != nullptr &&
            first.findTemporalEdge(1, 0) == nullptr,
            "reciprocal edge histories are not merged");
    require(first.external_ids == second.external_ids &&
            first.vertex_labels == second.vertex_labels,
            "seeded random labels are stable across input ordering");
    require(first.vertex_labels != alternate_seed.vertex_labels,
            "changing the random label seed changes the assignment");
    auto label_for_external_id = [](const Graph& graph, int external_id) {
        const auto found = std::lower_bound(
            graph.external_ids.begin(), graph.external_ids.end(), external_id);
        require(found != graph.external_ids.end() && *found == external_id,
                "external ID exists in label fixture");
        return graph.vertex_labels[static_cast<std::size_t>(
            found - graph.external_ids.begin())];
    };
    require(label_for_external_id(first, 5) ==
                label_for_external_id(different_survival, 5) &&
            label_for_external_id(first, 1000000) ==
                label_for_external_id(different_survival, 1000000),
            "random labels are assigned before temporal filtering");

    std::filesystem::remove(first_path);
    std::filesystem::remove(second_path);
    std::filesystem::remove(different_survival_path);
}

void testRepeatedLabelQueryParsing(const std::filesystem::path& directory) {
    const auto query_path = directory / "ours_repeated_label_query.qry";
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

void testPdfSelectivityAndRecursiveDfs() {
    const Graph query = makeTriangleQuery();
    std::array<std::size_t, kLabelCount> counts{};
    std::array<double, kLabelCount> lifespans{};
    counts[0] = 1;
    counts[1] = 2;
    counts[2] = 4;
    lifespans[0] = lifespans[1] = lifespans[2] = 1.0;

    const auto decomposition = decomposeQuery(query, counts, lifespans);
    require(decomposition.root == 2, "Eq. (3.2) argmin root");
    require(decomposition.connected, "connected decomposition");
    require(decomposition.dfs_order == std::vector<int>({2, 1, 0}),
            "recursive DFS produces a deep triangle tree");
    require(decomposition.non_tree_edges == std::vector<std::pair<int, int>>({{2, 0}}),
            "directed tree and non-tree edges are disjoint and complete");

    const std::array<std::size_t, kLabelCount> empty_counts{};
    const std::array<double, kLabelCount> empty_lifespans{};
    const auto no_candidate_decomposition =
        decomposeQuery(query, empty_counts, empty_lifespans);
    require(no_candidate_decomposition.root == 0 && no_candidate_decomposition.connected,
            "no-candidate queries still decompose and can report zero matches");
}

void testExactDurableMatching(const std::filesystem::path& directory) {
    const Graph data = makeMatchingDataGraph();
    const Graph query = makeTriangleQuery();
    const QueryDecomposition decomposition = makeDecomposition(query);
    require(decomposition.connected, "matching query decomposition");

    const auto result_path = directory / "ours_exact_matching_result.dat";
    TDTree tree(data, query, decomposition, 3);
    const MatchSummary summary = tree.save_res(result_path.string());
    require(summary.output_written, "matching result file written");
    require(summary.match_count == 1, "only the common consecutive triangle is reported");

    std::ifstream result(result_path, std::ios::binary);
    const std::string contents(
        (std::istreambuf_iterator<char>(result)), std::istreambuf_iterator<char>());
    require(contents.find("Count:                    1") != std::string::npos,
            "match count header is intact");
    require(contents.find("active=[3-5]") != std::string::npos,
            "exact common interval is reported");
    result.close();

    TDTree stricter_tree(data, query, decomposition, 4);
    const auto strict_path = directory / "ours_exact_matching_strict_result.dat";
    const MatchSummary strict_summary = stricter_tree.save_res(strict_path.string());
    require(strict_summary.match_count == 0,
            "individual long edges without a common k-run are rejected");

    std::filesystem::remove(result_path);
    std::filesystem::remove(strict_path);
}

void testDirectedTreeOrientationAndReciprocalArcs(
    const std::filesystem::path& directory) {
    {
        const Graph query = makeTwoVertexQuery(false);
        std::array<std::size_t, kLabelCount> counts{};
        std::array<double, kLabelCount> lifespans{};
        counts[labelFromString("A")] = 1;
        counts[labelFromString("B")] = 10;
        lifespans[labelFromString("A")] = 1.0;
        lifespans[labelFromString("B")] = 1.0;
        const QueryDecomposition decomposition =
            decomposeQuery(query, counts, lifespans);
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

        const auto result_path = directory / "ours_directed_sink_root.dat";
        TDTree tree(data, query, decomposition, 3);
        const MatchSummary summary = tree.save_res(result_path.string());
        require(summary.match_count == 1,
                "incoming expansion from a sink root preserves arc direction");
        std::filesystem::remove(result_path);
    }

    {
        const Graph query = makeTwoVertexQuery(true);
        std::array<std::size_t, kLabelCount> counts{};
        std::array<double, kLabelCount> lifespans{};
        counts[labelFromString("A")] = counts[labelFromString("B")] = 2;
        lifespans[labelFromString("A")] = lifespans[labelFromString("B")] = 6.0;
        const QueryDecomposition decomposition =
            decomposeQuery(query, counts, lifespans);

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

        const auto result_path = directory / "ours_directed_reciprocal.dat";
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
            ("ours_random_oracle_" + std::to_string(round) + ".dat");
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
        testFilteringAndDenseIds(temp_directory);
        testRepeatedLabelQueryParsing(temp_directory);
        testPdfSelectivityAndRecursiveDfs();
        testExactDurableMatching(temp_directory);
        testDirectedTreeOrientationAndReciprocalArcs(temp_directory);
        testRandomGraphsAgainstBruteForce(temp_directory);
        std::cout << "All ours tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
