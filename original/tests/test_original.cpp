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
    std::int64_t active_count = 0;
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

std::uint64_t bruteForceTriangleCount(
    const Graph& graph,
    int minimum_duration,
    DurationMode duration_mode = DurationMode::Cardinality) {
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

                auto active_at = [](const TemporalEdge& edge, int snapshot) {
                    return std::any_of(
                        edge.active_intervals.begin(),
                        edge.active_intervals.end(),
                        [snapshot](const TimeInterval& interval) {
                            return interval.start <= snapshot &&
                                snapshot <= interval.end;
                        });
                };
                int common_cardinality = 0;
                int longest_run = 0;
                int current_run = 0;
                for (int snapshot = 1; snapshot <= 6; ++snapshot) {
                    if (active_at(*edge_ab, snapshot) &&
                        active_at(*edge_bc, snapshot) &&
                        active_at(*edge_ca, snapshot)) {
                        ++common_cardinality;
                        longest_run = std::max(longest_run, ++current_run);
                    } else {
                        current_run = 0;
                    }
                }
                const bool durable = duration_mode == DurationMode::Cardinality
                    ? common_cardinality >= minimum_duration
                    : longest_run >= minimum_duration;
                if (durable) {
                    ++count;
                }
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
    const auto intersection = intersectTimeIntervals(lhs, rhs);
    require(intersection.size() == 3, "interval intersection count");
    require(intersection[0].start == 3 && intersection[0].end == 5, "first interval");
    require(intersection[1].start == 12 && intersection[1].end == 14, "second interval");
    require(intersection[2].start == 18 && intersection[2].end == 20, "third interval");
    require(
        timeInstanceCardinality(intersection) == 9,
        "intersection cardinality");
    require(
        hasMinimumDuration(intersection, 9, DurationMode::Cardinality),
        "minimum cardinality accepted");
    require(
        !hasMinimumDuration(intersection, 10, DurationMode::Cardinality),
        "minimum cardinality rejected");
    require(
        hasMinimumDuration(intersection, 3, DurationMode::Consecutive),
        "minimum consecutive run accepted");
    require(
        !hasMinimumDuration(intersection, 4, DurationMode::Consecutive),
        "minimum consecutive run rejected");

    const std::vector<TimeInterval> separated{{1, 1}, {3, 3}, {5, 5}};
    require(
        hasMinimumDuration(separated, 3, DurationMode::Cardinality),
        "paper cardinality sums nonconsecutive time instances");
    require(
        !hasMinimumDuration(separated, 3, DurationMode::Consecutive),
        "compatibility mode requires one consecutive run");

    const std::vector<TimeInterval> compact_union{{1, 5}};
    const std::vector<TimeInterval> original_set{{1, 2}, {4, 5}};
    const auto delta = subtractTimeIntervals(compact_union, original_set);
    require(
        delta.size() == 1 && delta[0].start == 3 && delta[0].end == 3,
        "leaf compaction stores union minus original set");
    const auto reconstructed = subtractTimeIntervals(compact_union, delta);
    require(
        reconstructed.size() == 2 &&
            reconstructed[0].start == 1 && reconstructed[0].end == 2 &&
            reconstructed[1].start == 4 && reconstructed[1].end == 5,
        "leaf time set reconstructs exactly from compact representation");

    const auto merged = unionTimeIntervals(
        std::vector<TimeInterval>({{1, 3}, {7, 7}}),
        std::vector<TimeInterval>({{3, 6}}));
    require(
        merged.size() == 1 && merged[0].start == 1 && merged[0].end == 7,
        "time-set union merges overlap and adjacent intervals");
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

void testStableIdLabelsAcrossVertexSubsets(
    const std::filesystem::path& directory) {
    const auto full_path =
        directory / "original_stable_id_full.dat";
    const auto subset_path =
        directory / "original_stable_id_subset.dat";
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
            LabelMode::StableId),
        "read full stable-id fixture");
    require(
        readTemporalGraph(
            subset_path.string(),
            subset,
            42U,
            nullptr,
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
    require(
        decomposition.tree_edges ==
            std::vector<std::pair<int, int>>({{0, 1}, {1, 2}}),
        "DFS tree keeps the original direction of each selected query arc");
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
    require(
        summary.match_count == 1,
        "only the triangle with common cardinality at least k is reported");

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
            "individual long edges without k common time instances are rejected");

    std::filesystem::remove(result_path);
    std::filesystem::remove(count_only_path);
    std::filesystem::remove(strict_path);
}

void testCountOnlyRootTaskParallelism(
    const std::filesystem::path& directory) {
    constexpr int root_count = 64;
    const Graph data = makeParallelRootDataGraph(root_count);
    const Graph query = makeTwoVertexQuery(false);
    const QueryDecomposition decomposition =
        makeDecomposition(query);

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

    const auto serial_path =
        directory / "original_parallel_serial.dat";
    const auto parallel_path =
        directory / "original_parallel_32.dat";
    const auto full_serial_path =
        directory / "original_parallel_full_serial.dat";
    const auto full_requested_path =
        directory / "original_parallel_full_requested.dat";

    TDTree tree(
        data,
        query,
        decomposition,
        3,
        DurationMode::Cardinality);
    const MatchSummary serial = tree.save_res(
        serial_path.string(), MatchOutputMode::CountOnly, 1);
    const MatchSummary parallel = tree.save_res(
        parallel_path.string(), MatchOutputMode::CountOnly, 32);
    require(
        serial.match_count ==
                static_cast<std::uint64_t>(root_count) &&
            parallel.match_count == serial.match_count &&
            !parallel.count_overflow,
        "parallel cardinality count matches the serial root enumeration");
    require(
        parallel.threads_requested == 32 &&
            parallel.threads_used == 32 &&
            parallel.workers_with_tasks == 32 &&
            parallel.root_tasks_claimed ==
                static_cast<std::size_t>(root_count) &&
            parallel.worker_root_tasks_min >= 1,
        "cardinality count-only search executes all 32 root workers");
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
        "TD-Tree worker-local candidate counters are exact "
        "and thread-invariant");

    TDTree consecutive_tree(
        data,
        query,
        decomposition,
        3,
        DurationMode::Consecutive);
    const auto consecutive_path =
        directory / "original_parallel_consecutive.dat";
    const MatchSummary consecutive = consecutive_tree.save_res(
        consecutive_path.string(),
        MatchOutputMode::CountOnly,
        32);
    require(
        consecutive.match_count == serial.match_count &&
            consecutive.threads_used == 32 &&
            consecutive.workers_with_tasks == 32,
        "parallel consecutive compatibility mode preserves exact results");

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
    std::filesystem::remove(consecutive_path);
    std::filesystem::remove(full_serial_path);
    std::filesystem::remove(full_requested_path);
}

void testPaperCardinalityAndConsecutiveCompatibility(
    const std::filesystem::path& directory) {
    const Graph query = makeTwoVertexQuery(false);
    const QueryDecomposition decomposition = makeDecomposition(query);

    Graph data;
    data.num_vertices = 2;
    data.external_ids = {10, 20};
    data.vertex_labels = {labelFromString("A"), labelFromString("B")};
    data.adj.resize(2);
    data.in_adj.resize(2);
    addTemporalEdge(data, 0, 1, {{1, 1}, {3, 3}, {5, 5}});
    finalizeSyntheticGraph(data);

    const auto cardinality_path =
        directory / "original_nonconsecutive_cardinality.dat";
    TDTree paper_tree(
        data, query, decomposition, 3, DurationMode::Cardinality);
    const MatchSummary paper_summary =
        paper_tree.save_res(cardinality_path.string());
    require(
        paper_summary.match_count == 1,
        "paper mode accepts k common nonconsecutive time instances");
    std::ifstream cardinality_result(cardinality_path, std::ios::binary);
    const std::string cardinality_contents(
        (std::istreambuf_iterator<char>(cardinality_result)),
        std::istreambuf_iterator<char>());
    require(
        cardinality_contents.find("duration_mode: cardinality") !=
            std::string::npos,
        "paper duration mode is recorded");
    require(
        cardinality_contents.find("active=[1, 3, 5]") != std::string::npos,
        "nonconsecutive common time-instance set is reported exactly");
    cardinality_result.close();

    const auto consecutive_path =
        directory / "original_nonconsecutive_compatibility.dat";
    TDTree compatibility_tree(
        data, query, decomposition, 3, DurationMode::Consecutive);
    const MatchSummary compatibility_summary =
        compatibility_tree.save_res(consecutive_path.string());
    require(
        compatibility_summary.match_count == 0,
        "consecutive compatibility mode rejects separated singleton runs");
    std::ifstream consecutive_result(consecutive_path, std::ios::binary);
    const std::string consecutive_contents(
        (std::istreambuf_iterator<char>(consecutive_result)),
        std::istreambuf_iterator<char>());
    require(
        consecutive_contents.find("duration_mode: consecutive") !=
            std::string::npos,
        "consecutive duration mode is recorded");
    consecutive_result.close();

    std::filesystem::remove(cardinality_path);
    std::filesystem::remove(consecutive_path);
}

void testMultipleLeafTimeSetJoin(const std::filesystem::path& directory) {
    Graph query;
    query.num_vertices = 3;
    query.external_ids = {0, 1, 2};
    query.vertex_labels = {
        labelFromString("A"), labelFromString("B"), labelFromString("C")};
    query.adj.resize(3);
    query.in_adj.resize(3);
    query.adj[0].push_back({1, -1});
    query.adj[0].push_back({2, -1});
    query.in_adj[1].push_back({0, -1});
    query.in_adj[2].push_back({0, -1});
    const QueryDecomposition decomposition = makeDecomposition(query);
    require(
        decomposition.root == 0 &&
            decomposition.spanning_tree_adj[0].size() == 2,
        "star fixture produces two root-to-leaf query paths");

    Graph data;
    data.num_vertices = 3;
    data.external_ids = {10, 20, 30};
    data.vertex_labels = {
        labelFromString("A"), labelFromString("B"), labelFromString("C")};
    data.adj.resize(3);
    data.in_adj.resize(3);
    addTemporalEdge(data, 0, 1, {{1, 1}, {3, 3}, {5, 5}});
    addTemporalEdge(data, 0, 2, {{1, 1}, {4, 4}, {5, 5}});
    finalizeSyntheticGraph(data);

    const auto accepted_path = directory / "original_leaf_join_k2.dat";
    TDTree accepted_tree(
        data, query, decomposition, 2, DurationMode::Cardinality);
    require(
        accepted_tree.save_res(accepted_path.string()).match_count == 1,
        "Eq. (1) intersects leaf path sets and counts both common snapshots");

    const auto rejected_path = directory / "original_leaf_join_k3.dat";
    TDTree rejected_tree(
        data, query, decomposition, 3, DurationMode::Cardinality);
    require(
        rejected_tree.save_res(rejected_path.string()).match_count == 0,
        "multiple leaf path sets with total common cardinality below k are rejected");

    const auto consecutive_path =
        directory / "original_leaf_join_consecutive.dat";
    TDTree consecutive_tree(
        data, query, decomposition, 2, DurationMode::Consecutive);
    require(
        consecutive_tree.save_res(consecutive_path.string()).match_count == 0,
        "leaf-path compatibility join also enforces a common consecutive run");

    std::filesystem::remove(accepted_path);
    std::filesystem::remove(rejected_path);
    std::filesystem::remove(consecutive_path);
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
        require(
            decomposition.tree_edges.size() == 1 &&
                decomposition.non_tree_edges.size() == 1 &&
                decomposition.tree_edges[0] != decomposition.non_tree_edges[0],
            "one reciprocal arc is a tree edge and the reverse remains non-tree");

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
            "self-loop with cardinality three is durable");

    const auto cardinality_two_path =
        directory / "original_self_loop_cardinality_two.dat";
    TDTree cardinality_two_tree(
        data, query, decomposition, 2, DurationMode::Cardinality);
    require(
        cardinality_two_tree.save_res(cardinality_two_path.string()).match_count == 2,
        "nonconsecutive self-loop snapshots count in paper mode");

    const auto consecutive_two_path =
        directory / "original_self_loop_consecutive_two.dat";
    TDTree consecutive_two_tree(
        data, query, decomposition, 2, DurationMode::Consecutive);
    require(
        consecutive_two_tree.save_res(consecutive_two_path.string()).match_count == 1,
        "consecutive self-loop mode rejects separated snapshots");
    std::filesystem::remove(result_path);
    std::filesystem::remove(cardinality_two_path);
    std::filesystem::remove(consecutive_two_path);
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
    bool saw_mode_difference = false;
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
        const std::uint64_t expected = bruteForceTriangleCount(
            graph, minimum_duration, DurationMode::Cardinality);
        const std::uint64_t expected_consecutive = bruteForceTriangleCount(
            graph, minimum_duration, DurationMode::Consecutive);
        saw_match = saw_match || expected > 0;
        saw_zero_match_graph = saw_zero_match_graph || expected == 0;
        saw_mode_difference =
            saw_mode_difference || expected != expected_consecutive;
        const auto result_path = directory /
            ("original_random_oracle_" + std::to_string(round) + ".dat");
        TDTree tree(graph, query, decomposition, minimum_duration);
        const MatchSummary actual = tree.save_res(result_path.string());
        require(actual.output_written, "random oracle result file written");
        require(actual.match_count == expected,
                "TD-tree result differs from brute-force oracle in round " +
                    std::to_string(round));

        const auto consecutive_result_path = directory /
            ("original_random_consecutive_oracle_" +
             std::to_string(round) + ".dat");
        TDTree consecutive_tree(
            graph,
            query,
            decomposition,
            minimum_duration,
            DurationMode::Consecutive);
        const MatchSummary actual_consecutive =
            consecutive_tree.save_res(consecutive_result_path.string());
        require(
            actual_consecutive.match_count == expected_consecutive,
            "consecutive TD-tree result differs from brute-force oracle in round " +
                std::to_string(round));
        std::filesystem::remove(result_path);
        std::filesystem::remove(consecutive_result_path);
    }
    require(saw_match && saw_zero_match_graph,
            "random oracle fixtures must include positive and zero-match graphs");
    require(
        saw_mode_difference,
        "random oracle fixtures distinguish cardinality and consecutive semantics");
}

} // namespace

int main() {
    try {
        const auto temp_directory = std::filesystem::temp_directory_path();
        testIntervals();
        testCheckedMatchCounterOverflow();
        testNoPrefilterAndDenseIds(temp_directory);
        testStableIdLabelsAcrossVertexSubsets(temp_directory);
        testStrictLineParsing(temp_directory);
        testRepeatedLabelQueryParsing(temp_directory);
        testTopologicalSelectivityAndRecursiveDfs();
        testExactDurableMatching(temp_directory);
        testCountOnlyRootTaskParallelism(temp_directory);
        testPaperCardinalityAndConsecutiveCompatibility(temp_directory);
        testMultipleLeafTimeSetJoin(temp_directory);
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
