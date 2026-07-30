#include "../TDTree.h"
#include "../Utils.h"
#include "../query_decomposition.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
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

bool edgeActiveAt(const TemporalEdge& edge, int timestamp) {
    for (const auto& interval : edge.active_intervals) {
        if (timestamp < interval.start) return false;
        if (timestamp <= interval.end) return true;
    }
    return false;
}

bool bruteForceMappingIsDurable(
    const Graph& graph,
    const Graph& query,
    const std::vector<int>& mapping,
    int minimum_duration) {
    std::vector<const TemporalEdge*> mapped_edges;
    int first_snapshot = std::numeric_limits<int>::max();
    int last_snapshot = std::numeric_limits<int>::min();
    for (int source = 0; source < query.num_vertices; ++source) {
        for (const auto& query_edge :
             query.adj[static_cast<std::size_t>(source)]) {
            const TemporalEdge* data_edge = graph.findTemporalEdge(
                mapping[static_cast<std::size_t>(source)],
                mapping[static_cast<std::size_t>(query_edge.to)]);
            if (data_edge == nullptr || data_edge->active_intervals.empty()) {
                return false;
            }
            mapped_edges.push_back(data_edge);
            first_snapshot = std::min(
                first_snapshot, data_edge->active_intervals.front().start);
            last_snapshot = std::max(
                last_snapshot, data_edge->active_intervals.back().end);
        }
    }
    if (mapped_edges.empty()) return minimum_duration <= 0;

    int consecutive = 0;
    for (long long timestamp = first_snapshot;
         timestamp <= static_cast<long long>(last_snapshot);
         ++timestamp) {
        bool all_active = true;
        for (const TemporalEdge* edge : mapped_edges) {
            if (!edgeActiveAt(*edge, static_cast<int>(timestamp))) {
                all_active = false;
                break;
            }
        }
        consecutive = all_active ? consecutive + 1 : 0;
        if (consecutive >= minimum_duration) return true;
    }
    return false;
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

Graph makeParallelRootDataGraph(int root_count = 64) {
    Graph graph;
    graph.num_vertices = root_count * 2;
    graph.external_ids.resize(
        static_cast<std::size_t>(graph.num_vertices));
    graph.vertex_labels.resize(
        static_cast<std::size_t>(graph.num_vertices));
    graph.adj.resize(static_cast<std::size_t>(graph.num_vertices));
    graph.in_adj.resize(static_cast<std::size_t>(graph.num_vertices));
    for (int root = 0; root < root_count; ++root) {
        graph.external_ids[static_cast<std::size_t>(root)] =
            1000 + root;
        graph.external_ids[
            static_cast<std::size_t>(root_count + root)] =
            2000 + root;
        graph.vertex_labels[static_cast<std::size_t>(root)] =
            labelFromString("A");
        graph.vertex_labels[
            static_cast<std::size_t>(root_count + root)] =
            labelFromString("B");
        addTemporalEdge(
            graph,
            root,
            root_count + root,
            {{1, 3}});
    }
    finalizeSyntheticGraph(graph);
    return graph;
}

std::uint64_t bruteForceTriangleCount(const Graph& graph, int minimum_duration) {
    const Label label_a = labelFromString("A");
    const Label label_b = labelFromString("B");
    const Label label_c = labelFromString("C");
    const Graph query = makeTriangleQuery();
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

                const std::vector<int> mapping{a, b, c};
                if (bruteForceMappingIsDurable(
                        graph, query, mapping, minimum_duration)) {
                    ++count;
                }
            }
        }
    }
    return count;
}

std::uint64_t bruteForceCount(
    const Graph& graph,
    const Graph& query,
    int minimum_duration) {
    std::vector<int> mapping(static_cast<std::size_t>(query.num_vertices), -1);
    std::vector<std::uint8_t> used(static_cast<std::size_t>(graph.num_vertices), 0);
    std::uint64_t count = 0;

    std::function<void(int)> enumerate = [&](int query_vertex) {
        if (query_vertex == query.num_vertices) {
            if (bruteForceMappingIsDurable(
                    graph, query, mapping, minimum_duration)) {
                ++count;
            }
            return;
        }

        const Label required_label =
            query.vertex_labels[static_cast<std::size_t>(query_vertex)];
        for (int data_vertex = 0; data_vertex < graph.num_vertices; ++data_vertex) {
            if (used[static_cast<std::size_t>(data_vertex)] != 0 ||
                graph.vertex_labels[static_cast<std::size_t>(data_vertex)] !=
                    required_label) {
                continue;
            }
            mapping[static_cast<std::size_t>(query_vertex)] = data_vertex;
            used[static_cast<std::size_t>(data_vertex)] = 1;
            enumerate(query_vertex + 1);
            used[static_cast<std::size_t>(data_vertex)] = 0;
            mapping[static_cast<std::size_t>(query_vertex)] = -1;
        }
    };
    enumerate(0);
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
    const TimeInterval extreme{
        std::numeric_limits<int>::min(), std::numeric_limits<int>::max()};
    require(extreme.length() == std::numeric_limits<int>::max(),
            "extreme timestamp span saturates without signed overflow");
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
            "overflow leaves a saturated count and explicit state");
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
    Graph unfiltered;
    require(readTemporalGraph(first_path.string(), first), "read first temporal fixture");
    require(readTemporalGraph(second_path.string(), second), "read reordered temporal fixture");
    require(readTemporalGraph(first_path.string(), alternate_seed, 43U),
            "read temporal fixture with alternate label seed");
    require(readTemporalGraph(different_survival_path.string(), different_survival),
            "read temporal fixture with a different filtered vertex set");
    require(
        readTemporalGraph(
            first_path.string(),
            unfiltered,
            kDefaultLabelSeed,
            nullptr,
            false),
        "read temporal fixture with Algorithm 3.1 prefilter disabled");
    require(first.input_unique_edge_count == 3, "directed unique edge count before filter");
    require(first.filtered_edge_count == 1, "transient edge removed");
    require(
        unfiltered.filtered_edge_count == 3 &&
            unfiltered.num_vertices == 4,
        "prefilter-off retains every unique directed edge and vertex");
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
    require(
        label_for_external_id(first, 5) ==
                label_for_external_id(unfiltered, 5) &&
            label_for_external_id(first, 1000000) ==
                label_for_external_id(unfiltered, 1000000),
        "prefilter on/off uses the same raw-vertex label assignment");

    std::filesystem::remove(first_path);
    std::filesystem::remove(second_path);
    std::filesystem::remove(different_survival_path);
}

void testStableIdLabelsAcrossVertexSubsets(
    const std::filesystem::path& directory) {
    const auto full_path =
        directory / "ours_stable_id_full.dat";
    const auto subset_path =
        directory / "ours_stable_id_subset.dat";
    {
        std::ofstream output(full_path);
        output
            << "10 20 1\n10 20 2\n"
            << "20 30 1\n20 30 2\n"
            << "30 40 1\n30 40 2\n";
    }
    {
        std::ofstream output(subset_path);
        output << "20 30 1\n20 30 2\n";
    }

    Graph full;
    Graph subset;
    require(
        readTemporalGraph(
            full_path.string(),
            full,
            42U,
            nullptr,
            true,
            LabelMode::StableId),
        "read full stable-id fixture");
    require(
        readTemporalGraph(
            subset_path.string(),
            subset,
            42U,
            nullptr,
            true,
            LabelMode::StableId),
        "read subset stable-id fixture");

    auto label_for_external_id = [](const Graph& graph, int external_id) {
        const auto found = std::lower_bound(
            graph.external_ids.begin(),
            graph.external_ids.end(),
            external_id);
        require(
            found != graph.external_ids.end() &&
                *found == external_id,
            "stable-id fixture contains external ID");
        return graph.vertex_labels[
            static_cast<std::size_t>(
                found - graph.external_ids.begin())];
    };
    for (int external_id : {20, 30}) {
        require(
            label_for_external_id(full, external_id) ==
                label_for_external_id(subset, external_id),
            "stable-id labels are invariant to observed vertex subsets");
        require(
            label_for_external_id(full, external_id) ==
                stableIdLabel(external_id, 42U),
            "graph loading uses the public stable-id mixer");
    }
    require(
        stableIdLabel(0, 42U) == labelFromString("A") &&
            stableIdLabel(1, 42U) == labelFromString("B") &&
            stableIdLabel(20, 42U) == labelFromString("D") &&
            stableIdLabel(30, 42U) == labelFromString("C") &&
            stableIdLabel(40, 42U) == labelFromString("E"),
        "stable-id mixer golden labels");
    require(
        std::string(labelModeName(LabelMode::SequentialRng)) ==
                "sequential-rng" &&
            std::string(labelModeName(LabelMode::StableId)) ==
                "stable-id",
        "label mode names");

    std::filesystem::remove(full_path);
    std::filesystem::remove(subset_path);
}

void testSelfLoopFilteringAndStrictParsing(
    const std::filesystem::path& directory) {
    const auto self_loop_path = directory / "ours_self_loop_filter.dat";
    const auto malformed_temporal = directory / "ours_malformed_rows.dat";
    const auto commented_temporal = directory / "ours_commented_rows.dat";
    const auto malformed_query = directory / "ours_malformed_query.qry";
    const auto commented_query = directory / "ours_commented_query.qry";
    {
        std::ofstream output(self_loop_path);
        output << "7 7 1\n7 7 2\n8 8 3\n";
    }
    {
        std::ofstream output(malformed_temporal);
        output << "1 2\n3 4 5 6\n";
    }
    {
        std::ofstream output(commented_temporal);
        output << "  # comment\n\n1 2 3 # occurrence\n1 2 4\n";
    }
    {
        std::ofstream output(malformed_query);
        output << "A B EXTRA\n";
    }
    {
        std::ofstream output(commented_query);
        output << "   # comment\n\nloop:A loop:A # directed self-loop\n";
    }

    Graph graph;
    Graph query;
    require(readTemporalGraph(self_loop_path.string(), graph),
            "read temporal self-loop fixture");
    require(graph.input_unique_edge_count == 2 && graph.filtered_edge_count == 1,
            "Algorithm 3.1 keeps a consecutive self-loop and removes a transient one");
    require(graph.num_vertices == 1 && graph.findTemporalEdge(0, 0) != nullptr,
            "durable self-loop survives compact graph construction");
    require(!readTemporalGraph(malformed_temporal.string(), graph),
            "temporal rows cannot borrow columns from a following line");
    require(readTemporalGraph(commented_temporal.string(), graph),
            "blank and commented temporal rows are accepted");
    require(!readQueryGraph(malformed_query.string(), query),
            "query rows reject trailing tokens");
    require(readQueryGraph(commented_query.string(), query) &&
            query.num_vertices == 1 &&
            GraphUtils::hasEdge(query.adj, 0, 0),
            "query parser accepts a directed self-loop and inline comments");

    std::filesystem::remove(self_loop_path);
    std::filesystem::remove(malformed_temporal);
    std::filesystem::remove(commented_temporal);
    std::filesystem::remove(malformed_query);
    std::filesystem::remove(commented_query);
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

void testEndToEndFilteredGraphAgainstBruteForce(
    const std::filesystem::path& directory) {
    const auto temporal_path = directory / "ours_end_to_end_temporal.dat";
    {
        std::ofstream output(temporal_path);
        output << "10 20 1\n10 20 2\n10 20 3\n10 20 3\n"
                  "20 30 1\n20 30 2\n20 30 3\n"
                  "30 10 1\n30 10 2\n30 10 3\n"
                  "40 10 2\n"; // transient decoy removed by Algorithm 3.1
    }

    Graph data;
    require(readTemporalGraph(temporal_path.string(), data, 42U),
            "read end-to-end temporal fixture");
    require(data.filtered_edge_count == 3 && data.num_vertices == 3,
            "end-to-end fixture applies consecutive filtering and compaction");

    Graph query;
    query.num_vertices = 3;
    query.external_ids = {0, 1, 2};
    query.vertex_labels = data.vertex_labels;
    query.adj.resize(3);
    query.in_adj.resize(3);
    const std::array<std::pair<int, int>, 3> edges{{{0, 1}, {1, 2}, {2, 0}}};
    for (const auto& edge : edges) {
        query.adj[static_cast<std::size_t>(edge.first)].push_back(
            {edge.second, -1});
        query.in_adj[static_cast<std::size_t>(edge.second)].push_back(
            {edge.first, -1});
    }

    std::array<std::size_t, kLabelCount> counts{};
    std::array<long long, kLabelCount> duration_sums{};
    for (std::size_t vertex = 0; vertex < data.vertex_labels.size(); ++vertex) {
        const Label label = data.vertex_labels[vertex];
        ++counts[label];
        duration_sums[label] += data.vertex_active_durations[vertex];
    }
    std::array<double, kLabelCount> average_lifespans{};
    for (std::size_t label = 0; label < kLabelCount; ++label) {
        if (counts[label] != 0) {
            average_lifespans[label] =
                static_cast<double>(duration_sums[label]) /
                static_cast<double>(counts[label]);
        }
    }
    const QueryDecomposition decomposition =
        decomposeQuery(query, counts, average_lifespans);
    require(decomposition.connected, "end-to-end query decomposition");

    const std::uint64_t expected = bruteForceCount(data, query, 3);
    require(expected > 0, "end-to-end oracle fixture contains a durable match");
    const auto result_path = directory / "ours_end_to_end_result.dat";
    TDTree tree(data, query, decomposition, 3);
    const MatchSummary actual = tree.save_res(
        result_path.string(), MatchOutputMode::CountOnly);
    require(actual.output_written && actual.match_count == expected,
            "parsed-and-filtered TD-tree result matches brute-force oracle");

    std::filesystem::remove(temporal_path);
    std::filesystem::remove(result_path);
}

void testPdfSelectivityAndRecursiveDfs() {
    const Graph query = makeTriangleQuery();
    std::array<std::size_t, kLabelCount> counts{};
    std::array<double, kLabelCount> lifespans{};
    counts[0] = 1;
    counts[1] = 2;
    counts[2] = 4;
    lifespans[0] = lifespans[1] = lifespans[2] = 1.0;

    const auto paper =
        decomposeQuery(query, counts, lifespans, SelectivityMode::Paper);
    require(paper.root == 2, "printed Eq. (3.2) argmin root");
    require(paper.temporal_selectivity[2] == 0.5,
            "paper mode uses degree over candidate count and lifespan");
    require(paper.selectivity_mode == SelectivityMode::Paper,
            "paper selectivity mode is retained in the decomposition");
    require(paper.connected, "connected paper decomposition");
    require(paper.dfs_order == std::vector<int>({2, 1, 0}),
            "recursive DFS produces a deep triangle tree");
    require(paper.non_tree_edges == std::vector<std::pair<int, int>>({{2, 0}}),
            "directed tree and non-tree edges are disjoint and complete");

    const auto intended =
        decomposeQuery(query, counts, lifespans, SelectivityMode::Intended);
    require(intended.root == 0,
            "prose-consistent selectivity restores candidate-count numerator");
    require(intended.temporal_selectivity[0] == 0.5,
            "intended mode uses candidate count over degree and lifespan");
    require(intended.selectivity_mode == SelectivityMode::Intended,
            "intended selectivity mode is retained in the decomposition");
    require(intended.connected &&
            intended.dfs_order == std::vector<int>({0, 1, 2}),
            "intended mode deterministically changes the DFS root/order");

    const auto tdtree =
        decomposeQuery(
            query,
            counts,
            lifespans,
            SelectivityMode::TDTree);
    require(
        tdtree.root == 0 &&
            tdtree.temporal_selectivity[0] == 0.5 &&
            tdtree.dfs_order == std::vector<int>({0, 1, 2}),
        "TD-Tree mode uses |V_L|/degree and deterministic argmin DFS");

    const std::array<std::size_t, kLabelCount> empty_counts{};
    const std::array<double, kLabelCount> empty_lifespans{};
    const auto no_candidate_decomposition =
        decomposeQuery(
            query,
            empty_counts,
            empty_lifespans,
            SelectivityMode::Intended);
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
    require(contents.find("mode: full") != std::string::npos,
            "default result mode is recorded");
    require(contents.find("Match 0:") != std::string::npos,
            "full mode writes individual match rows");
    require(contents.find("active=[3-5]") != std::string::npos,
            "exact common interval is reported");
    result.close();

    const auto count_only_path = directory / "ours_exact_count_only.dat";
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
            "count-only mode records the exact count");
    require(count_only_contents.find("Match ") == std::string::npos,
            "count-only mode suppresses individual match rows");
    count_only_result.close();

    std::array<std::size_t, kLabelCount> intended_counts{};
    std::array<double, kLabelCount> intended_lifespans{};
    intended_counts[0] = intended_counts[1] = intended_counts[2] = 2;
    intended_lifespans[0] =
        intended_lifespans[1] =
        intended_lifespans[2] = 5.0;
    const QueryDecomposition intended_decomposition = decomposeQuery(
        query,
        intended_counts,
        intended_lifespans,
        SelectivityMode::Intended);
    TDTree intended_tree(
        data, query, intended_decomposition, 3);
    const auto intended_path =
        directory / "ours_exact_intended_selectivity_result.dat";
    const MatchSummary intended_summary = intended_tree.save_res(
        intended_path.string(), MatchOutputMode::CountOnly);
    require(
        intended_summary.output_written &&
            intended_summary.match_count == summary.match_count,
        "selectivity ablation changes order but preserves exact matches");
    std::ifstream intended_result(intended_path, std::ios::binary);
    const std::string intended_contents(
        (std::istreambuf_iterator<char>(intended_result)),
        std::istreambuf_iterator<char>());
    require(
        intended_contents.find("selectivity_mode: intended") !=
            std::string::npos,
        "result file records intended selectivity mode");
    intended_result.close();

    TDTree stricter_tree(data, query, decomposition, 4);
    const auto strict_path = directory / "ours_exact_matching_strict_result.dat";
    const MatchSummary strict_summary = stricter_tree.save_res(strict_path.string());
    require(strict_summary.match_count == 0,
            "individual long edges without a common k-run are rejected");

    std::filesystem::remove(result_path);
    std::filesystem::remove(count_only_path);
    std::filesystem::remove(intended_path);
    std::filesystem::remove(strict_path);
}

void testCountOnlyRootTaskParallelism(
    const std::filesystem::path& directory) {
    constexpr int root_count = 64;
    const Graph data = makeParallelRootDataGraph(root_count);
    const Graph query = makeTwoVertexQuery(false);

    std::array<std::size_t, kLabelCount> counts{};
    std::array<double, kLabelCount> lifespans{};
    counts[labelFromString("A")] =
        static_cast<std::size_t>(root_count);
    counts[labelFromString("B")] =
        static_cast<std::size_t>(root_count);
    lifespans[labelFromString("A")] = 3.0;
    lifespans[labelFromString("B")] = 3.0;

    auto match_rows = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        std::vector<std::string> rows;
        std::string line;
        while (std::getline(input, line)) {
            if (line.rfind("Match ", 0) == 0) {
                rows.push_back(line);
            }
        }
        return rows;
    };

    const QueryDecomposition paper = decomposeQuery(
        query, counts, lifespans, SelectivityMode::Paper);
    const auto serial_path =
        directory / "ours_parallel_serial.dat";
    const auto parallel_path =
        directory / "ours_parallel_32.dat";
    const auto full_serial_path =
        directory / "ours_parallel_full_serial.dat";
    const auto full_requested_path =
        directory / "ours_parallel_full_requested.dat";

    TDTree tree(data, query, paper, 3);
    const MatchSummary serial = tree.save_res(
        serial_path.string(), MatchOutputMode::CountOnly, 1);
    const MatchSummary parallel = tree.save_res(
        parallel_path.string(), MatchOutputMode::CountOnly, 32);
    require(
        serial.match_count ==
                static_cast<std::uint64_t>(root_count) &&
            parallel.match_count == serial.match_count &&
            !parallel.count_overflow,
        "parallel paper-selectivity count matches serial enumeration");
    require(
        parallel.threads_requested == 32 &&
            parallel.threads_used == 32 &&
            parallel.workers_with_tasks == 32 &&
            parallel.root_tasks_claimed ==
                static_cast<std::size_t>(root_count) &&
            parallel.worker_root_tasks_min >= 1,
        "paper-selectivity count-only search executes all 32 root workers");
    require(
        parallel.dfs_states == serial.dfs_states &&
            parallel.generated_candidates ==
                serial.generated_candidates &&
            parallel.accepted_candidates ==
                serial.accepted_candidates &&
            parallel.completed_matches ==
                serial.completed_matches &&
            parallel.completed_matches ==
                parallel.match_count &&
            parallel.accepted_candidates <=
                parallel.generated_candidates,
        "worker-local candidate counters are exact and thread-invariant");

    const QueryDecomposition intended = decomposeQuery(
        query,
        counts,
        lifespans,
        SelectivityMode::Intended);
    TDTree intended_tree(data, query, intended, 3);
    const auto intended_path =
        directory / "ours_parallel_intended.dat";
    const MatchSummary intended_parallel =
        intended_tree.save_res(
            intended_path.string(),
            MatchOutputMode::CountOnly,
            32);
    require(
        intended_parallel.match_count == serial.match_count &&
            intended_parallel.threads_used == 32 &&
            intended_parallel.workers_with_tasks == 32,
        "parallel intended-selectivity mode preserves exact results");

    const MatchSummary full_serial = tree.save_res(
        full_serial_path.string(), MatchOutputMode::Full, 1);
    const MatchSummary full_requested = tree.save_res(
        full_requested_path.string(), MatchOutputMode::Full, 32);
    require(
        full_requested.match_count == full_serial.match_count &&
            full_requested.threads_requested == 32 &&
            full_requested.threads_used == 1 &&
            full_requested.workers_with_tasks == 1 &&
            match_rows(full_requested_path) ==
                match_rows(full_serial_path),
        "full output remains serial and preserves deterministic row order");

    std::filesystem::remove(serial_path);
    std::filesystem::remove(parallel_path);
    std::filesystem::remove(intended_path);
    std::filesystem::remove(full_serial_path);
    std::filesystem::remove(full_requested_path);
}

void testSelfLoopDurability(const std::filesystem::path& directory) {
    {
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

        const auto result_path = directory / "ours_root_self_loop_matching.dat";
        TDTree tree(data, query, decomposition, 3);
        require(tree.save_res(
                    result_path.string(), MatchOutputMode::CountOnly).match_count == 1,
                "root self-loop must itself contain the common k-run");
        std::filesystem::remove(result_path);
    }

    {
        Graph query = makeTwoVertexQuery(false);
        query.adj[1].push_back({1, -1});
        query.in_adj[1].push_back({1, -1});
        const QueryDecomposition decomposition = makeDecomposition(query);
        require(decomposition.root == 0 && decomposition.parent[1] == 0,
                "self-loop fixture places the loop on a non-root query vertex");

        Graph data;
        data.num_vertices = 4;
        data.external_ids = {10, 20, 30, 40};
        data.vertex_labels = {
            labelFromString("A"), labelFromString("B"),
            labelFromString("A"), labelFromString("B")};
        data.adj.resize(4);
        data.in_adj.resize(4);
        addTemporalEdge(data, 0, 1, {{1, 4}});
        addTemporalEdge(data, 1, 1, {{2, 4}}); // common run [2,4]
        addTemporalEdge(data, 2, 3, {{10, 12}});
        addTemporalEdge(data, 3, 3, {{12, 14}}); // common run has length one
        finalizeSyntheticGraph(data);

        const auto result_path = directory / "ours_non_root_self_loop_matching.dat";
        TDTree tree(data, query, decomposition, 3);
        require(tree.save_res(
                    result_path.string(), MatchOutputMode::CountOnly).match_count == 1,
                "non-root self-loop participates in the shared temporal intersection");
        std::filesystem::remove(result_path);
    }
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

void testRandomGeneralQueriesAgainstBruteForce(
    const std::filesystem::path& directory) {
    constexpr int minimum_duration = 2;
    std::uint32_t state = 0xa511e9b3U;
    auto next_random = [&]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };

    for (int round = 0; round < 48; ++round) {
        Graph graph;
        graph.num_vertices = 6;
        graph.external_ids = {10, 20, 30, 40, 50, 60};
        graph.vertex_labels = {
            labelFromString("A"), labelFromString("B"), labelFromString("C"),
            labelFromString("A"), labelFromString("B"), labelFromString("C")};
        graph.adj.resize(6);
        graph.in_adj.resize(6);
        for (int u = 0; u < graph.num_vertices; ++u) {
            for (int v = 0; v < graph.num_vertices; ++v) {
                const std::uint32_t mask = next_random() & 0x3fU;
                if (mask == 0 || (next_random() & 3U) == 0) continue;
                addTemporalEdge(graph, u, v, intervalsFromMask(mask));
            }
        }
        finalizeSyntheticGraph(graph);

        Graph query;
        query.num_vertices = 4;
        query.external_ids = {0, 1, 2, 3};
        query.vertex_labels = {
            labelFromString("A"), labelFromString("B"),
            labelFromString("C"), labelFromString("A")};
        query.adj.resize(4);
        query.in_adj.resize(4);
        auto add_query_edge = [&](int u, int v) {
            query.adj[static_cast<std::size_t>(u)].push_back({v, -1});
            query.in_adj[static_cast<std::size_t>(v)].push_back({u, -1});
        };

        // A weakly connected directed backbone, randomized in orientation.
        for (int vertex = 0; vertex < 3; ++vertex) {
            if ((next_random() & 1U) == 0) {
                add_query_edge(vertex, vertex + 1);
            } else {
                add_query_edge(vertex + 1, vertex);
            }
        }
        for (int u = 0; u < query.num_vertices; ++u) {
            for (int v = 0; v < query.num_vertices; ++v) {
                if (u == v || (u + 1 == v) || (v + 1 == u)) continue;
                if ((next_random() & 7U) == 0) add_query_edge(u, v);
            }
            if ((next_random() & 7U) == 0) add_query_edge(u, u);
        }
        for (auto& neighbors : query.adj) {
            std::sort(neighbors.begin(), neighbors.end(),
                [](const Edge& lhs, const Edge& rhs) { return lhs.to < rhs.to; });
            neighbors.erase(
                std::unique(neighbors.begin(), neighbors.end(),
                    [](const Edge& lhs, const Edge& rhs) {
                        return lhs.to == rhs.to;
                    }),
                neighbors.end());
        }
        for (auto& neighbors : query.in_adj) {
            std::sort(neighbors.begin(), neighbors.end(),
                [](const Edge& lhs, const Edge& rhs) { return lhs.to < rhs.to; });
            neighbors.erase(
                std::unique(neighbors.begin(), neighbors.end(),
                    [](const Edge& lhs, const Edge& rhs) {
                        return lhs.to == rhs.to;
                    }),
                neighbors.end());
        }

        std::array<std::size_t, kLabelCount> counts{};
        std::array<double, kLabelCount> lifespans{};
        counts[0] = counts[1] = counts[2] = 2;
        lifespans[0] = lifespans[1] = lifespans[2] = 6.0;
        const QueryDecomposition decomposition =
            decomposeQuery(query, counts, lifespans);
        require(decomposition.connected, "random general query decomposition");

        const std::uint64_t expected =
            bruteForceCount(graph, query, minimum_duration);
        const auto result_path = directory /
            ("ours_general_oracle_" + std::to_string(round) + ".dat");
        TDTree tree(graph, query, decomposition, minimum_duration);
        const MatchSummary actual = tree.save_res(
            result_path.string(), MatchOutputMode::CountOnly);
        require(actual.output_written && !actual.count_overflow,
                "general oracle result written without overflow");
        require(actual.match_count == expected,
                "general TD-tree result differs from brute-force oracle in round " +
                    std::to_string(round));
        std::filesystem::remove(result_path);
    }
}

} // namespace

int main() {
    try {
        const auto temp_directory = std::filesystem::temp_directory_path();
        testIntervals();
        testCheckedMatchCounterOverflow();
        testFilteringAndDenseIds(temp_directory);
        testStableIdLabelsAcrossVertexSubsets(temp_directory);
        testSelfLoopFilteringAndStrictParsing(temp_directory);
        testRepeatedLabelQueryParsing(temp_directory);
        testEndToEndFilteredGraphAgainstBruteForce(temp_directory);
        testPdfSelectivityAndRecursiveDfs();
        testExactDurableMatching(temp_directory);
        testCountOnlyRootTaskParallelism(temp_directory);
        testSelfLoopDurability(temp_directory);
        testDirectedTreeOrientationAndReciprocalArcs(temp_directory);
        testRandomGraphsAgainstBruteForce(temp_directory);
        testRandomGeneralQueriesAgainstBruteForce(temp_directory);
        std::cout << "All ours tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
