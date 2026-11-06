#include "../CandidateDag.h"
#include "../GopherMatcher.h"
#include "../TDTree.h"
#include "../Utils.h"
#include "../query_decomposition.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void addTemporalEdge(
    Graph& graph,
    int source,
    int target,
    std::vector<TimeInterval> intervals) {
    std::int64_t active = 0;
    for (const TimeInterval& interval : intervals) {
        active += interval.length();
    }
    const int edge_id =
        static_cast<int>(graph.temporal_edges.size());
    graph.temporal_edges.emplace_back(
        source, target, std::move(intervals), active);
    graph.adj[static_cast<std::size_t>(source)].push_back(
        {target, edge_id});
    graph.in_adj[static_cast<std::size_t>(target)].push_back(
        {source, edge_id});
}

void finalizeGraph(Graph& graph) {
    graph.external_ids.resize(
        static_cast<std::size_t>(graph.num_vertices));
    graph.vertex_active_durations.assign(
        static_cast<std::size_t>(graph.num_vertices), 8);
    graph.neighbor_label_counts.assign(
        static_cast<std::size_t>(graph.num_vertices), {});
    graph.incoming_neighbor_label_counts.assign(
        static_cast<std::size_t>(graph.num_vertices), {});
    for (int vertex = 0; vertex < graph.num_vertices; ++vertex) {
        graph.external_ids[static_cast<std::size_t>(vertex)] =
            1000 + vertex;
        auto& outgoing = graph.adj[static_cast<std::size_t>(vertex)];
        auto& incoming = graph.in_adj[static_cast<std::size_t>(vertex)];
        const auto by_target = [](const Edge& lhs, const Edge& rhs) {
            return lhs.to < rhs.to;
        };
        std::sort(outgoing.begin(), outgoing.end(), by_target);
        std::sort(incoming.begin(), incoming.end(), by_target);
        for (const Edge& edge : outgoing) {
            const Label label = graph.vertex_labels[
                static_cast<std::size_t>(edge.to)];
            ++graph.neighbor_label_counts[
                static_cast<std::size_t>(vertex)][label];
        }
        for (const Edge& edge : incoming) {
            const Label label = graph.vertex_labels[
                static_cast<std::size_t>(edge.to)];
            ++graph.incoming_neighbor_label_counts[
                static_cast<std::size_t>(vertex)][label];
        }
    }
}

Graph makeDataGraph() {
    Graph graph;
    graph.num_vertices = 6;
    graph.vertex_labels = {
        labelFromString("A"), labelFromString("A"),
        labelFromString("B"), labelFromString("B"),
        labelFromString("C"), labelFromString("C")};
    graph.adj.resize(6);
    graph.in_adj.resize(6);

    for (int source : {0, 1}) {
        for (int target : {2, 3}) {
            addTemporalEdge(graph, source, target, {{1, 6}});
        }
    }
    for (int source : {2, 3}) {
        for (int target : {4, 5}) {
            addTemporalEdge(graph, source, target, {{1, 6}});
        }
    }
    addTemporalEdge(graph, 0, 4, {{2, 5}});
    addTemporalEdge(graph, 0, 5, {{1, 2}});
    addTemporalEdge(graph, 1, 4, {{4, 6}});
    addTemporalEdge(graph, 1, 5, {{2, 6}});
    addTemporalEdge(graph, 0, 0, {{1, 4}});
    addTemporalEdge(graph, 1, 1, {{1, 2}});
    finalizeGraph(graph);
    return graph;
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
    for (std::size_t vertex = 0; vertex < labels.size(); ++vertex) {
        query.external_ids[vertex] = static_cast<int>(vertex);
    }
    for (const auto& arc : arcs) {
        query.adj[static_cast<std::size_t>(arc.first)].push_back(
            {arc.second, -1});
        query.in_adj[static_cast<std::size_t>(arc.second)].push_back(
            {arc.first, -1});
    }
    for (auto& edges : query.adj) {
        std::sort(edges.begin(), edges.end(),
                  [](const Edge& lhs, const Edge& rhs) {
                      return lhs.to < rhs.to;
                  });
    }
    for (auto& edges : query.in_adj) {
        std::sort(edges.begin(), edges.end(),
                  [](const Edge& lhs, const Edge& rhs) {
                      return lhs.to < rhs.to;
                  });
    }
    return query;
}

QueryDecomposition decompose(
    const Graph& graph,
    const Graph& query) {
    std::array<std::size_t, kLabelCount> counts{};
    std::array<double, kLabelCount> lifespans{};
    for (Label label : graph.vertex_labels) ++counts[label];
    for (std::size_t label = 0; label < kLabelCount; ++label) {
        if (counts[label] != 0) lifespans[label] = 8.0;
    }
    return decomposeQuery(
        query, counts, lifespans, SelectivityMode::Paper);
}

using MappingSet = std::set<std::vector<int>>;

std::vector<std::vector<int>> bruteForceMappings(
    const Graph& graph,
    const Graph& query,
    int minimum_duration) {
    std::vector<int> mapping(
        static_cast<std::size_t>(query.num_vertices), -1);
    std::vector<std::uint8_t> used(
        static_cast<std::size_t>(graph.num_vertices), 0);
    std::vector<std::vector<int>> matches;
    std::function<void(int)> enumerate = [&](int query_vertex) {
        if (query_vertex < query.num_vertices) {
            const Label required = query.vertex_labels[
                static_cast<std::size_t>(query_vertex)];
            for (int data_vertex = 0;
                 data_vertex < graph.num_vertices;
                 ++data_vertex) {
                if (used[static_cast<std::size_t>(data_vertex)] != 0 ||
                    graph.vertex_labels[
                        static_cast<std::size_t>(data_vertex)] != required) {
                    continue;
                }
                mapping[static_cast<std::size_t>(query_vertex)] = data_vertex;
                used[static_cast<std::size_t>(data_vertex)] = 1;
                enumerate(query_vertex + 1);
                used[static_cast<std::size_t>(data_vertex)] = 0;
                mapping[static_cast<std::size_t>(query_vertex)] = -1;
            }
            return;
        }

        std::vector<TimeInterval> active;
        bool initialized = false;
        for (int source = 0; source < query.num_vertices; ++source) {
            for (const Edge& query_edge :
                 query.adj[static_cast<std::size_t>(source)]) {
                const TemporalEdge* data_edge = graph.findTemporalEdge(
                    mapping[static_cast<std::size_t>(source)],
                    mapping[static_cast<std::size_t>(query_edge.to)]);
                if (data_edge == nullptr) return;
                if (!initialized) {
                    for (const TimeInterval& interval :
                         data_edge->activeIntervals()) {
                        if (interval.length() >= minimum_duration) {
                            active.push_back(interval);
                        }
                    }
                    initialized = true;
                } else {
                    active = intersectTimeIntervals(
                        active,
                        data_edge->activeIntervals(),
                        minimum_duration);
                }
                if (active.empty()) return;
            }
        }
        if (initialized && !active.empty()) matches.push_back(mapping);
    };
    enumerate(0);
    return matches;
}

std::uint64_t bruteForceCount(
    const Graph& graph,
    const Graph& query,
    int minimum_duration) {
    return static_cast<std::uint64_t>(
        bruteForceMappings(graph, query, minimum_duration).size());
}

MappingSet bruteForceExternalMappingSet(
    const Graph& graph,
    const Graph& query,
    int minimum_duration) {
    MappingSet result;
    for (std::vector<int> mapping :
         bruteForceMappings(graph, query, minimum_duration)) {
        for (int& data_vertex : mapping) {
            data_vertex = graph.externalId(data_vertex);
        }
        const bool inserted = result.insert(std::move(mapping)).second;
        require(inserted, "independent oracle emitted a duplicate mapping");
    }
    return result;
}

MappingSet parseFullOutputMappings(
    const std::string& text,
    int query_vertex_count) {
    MappingSet result;
    std::istringstream input(text);
    std::string row;
    while (std::getline(input, row)) {
        if (row.rfind("Match ", 0) != 0) continue;
        std::vector<int> mapping;
        mapping.reserve(static_cast<std::size_t>(query_vertex_count));
        std::size_t cursor = 0;
        for (int query_vertex = 0;
             query_vertex < query_vertex_count;
             ++query_vertex) {
            const std::string marker =
                "q" + std::to_string(query_vertex) + "(";
            const std::size_t query_position = row.find(marker, cursor);
            require(query_position != std::string::npos,
                    "full-output row is missing an ordered query vertex");
            const std::size_t arrow = row.find(")->", query_position);
            require(arrow != std::string::npos,
                    "full-output row is missing a mapping arrow");
            const std::size_t value_start = arrow + 3;
            const std::size_t value_end =
                row.find_first_of(", ", value_start);
            require(value_end != std::string::npos &&
                        value_end > value_start,
                    "full-output row is missing a mapped data vertex");
            std::size_t consumed = 0;
            const std::string value_text =
                row.substr(value_start, value_end - value_start);
            const int external_vertex = std::stoi(value_text, &consumed);
            require(consumed == value_text.size(),
                    "full-output row contains a malformed data vertex");
            mapping.push_back(external_vertex);
            cursor = value_end;
        }
        const bool inserted = result.insert(std::move(mapping)).second;
        require(inserted, "full output contains a duplicate mapping");
    }
    return result;
}

struct ComparedRuns {
    GopherMatchSummary direct;
    GopherMatchSummary dag;
};

ComparedRuns compareCountRuns(
    const Graph& graph,
    const Graph& query,
    const QueryDecomposition& decomposition,
    const TDTree& tree,
    int minimum_duration,
    std::size_t threads,
    bool early_prune) {
    const QueryPlan plan = makeQueryPlan(
        query, decomposition, tree, OrderMode::Paper);
    CandidateDagCompiler compiler;
    const CandidateDag dag = compiler.compile(
        query, plan, decomposition);

    GopherMatcherOptions options;
    options.threads = threads;
    options.parallel_mode = ParallelMode::Roots;
    options.order_mode = OrderMode::Paper;
    options.early_prune = early_prune;
    options.result_digest = true;
    options.executor.memo_mode = MemoMode::None;
    options.executor.verify_dag = true;
    options.executor.collect_statistics = true;

    GopherMatcher direct(
        graph, query, plan, tree, minimum_duration, options);
    GopherMatcher candidate_dag(
        graph, query, plan, dag, tree, minimum_duration, options);
    return {
        direct.run(MatchOutputMode::CountOnly),
        candidate_dag.run(MatchOutputMode::CountOnly)};
}

void testTriangleParity() {
    const Graph graph = makeDataGraph();
    const Graph query = makeQuery(
        {labelFromString("A"),
         labelFromString("B"),
         labelFromString("C")},
        {{0, 1}, {1, 2}, {0, 2}});
    const int k = 3;
    const QueryDecomposition decomposition = decompose(graph, query);
    require(decomposition.connected, "triangle decomposition");
    const TDTree tree(graph, query, decomposition, k);
    require(bruteForceCount(graph, query, k) == 6,
            "triangle independent oracle count");

    for (std::size_t threads : {std::size_t{1}, std::size_t{4}}) {
        for (bool early_prune : {false, true}) {
            const ComparedRuns runs = compareCountRuns(
                graph,
                query,
                decomposition,
                tree,
                k,
                threads,
                early_prune);
            require(runs.direct.match_count == 6,
                    "direct triangle known count");
            require(runs.direct.match_count == runs.dag.match_count,
                    "direct/DAG triangle count parity");
            require(runs.direct.result_digest == runs.dag.result_digest,
                    "direct/DAG triangle digest parity");
            require(!runs.direct.count_overflow &&
                        !runs.dag.count_overflow,
                    "triangle count does not overflow");
            require(runs.direct.cache_memory_bytes == 0,
                    "direct path has no cache");
            require(runs.direct.statistics.dag_nodes_attempted == 0 &&
                        runs.direct.statistics.dag_nodes_materialized == 0 &&
                        runs.direct.statistics.dag_nodes_merged == 0,
                    "direct path has no Candidate-DAG");
            require(runs.direct.parallel_mode_effective == "roots",
                    "direct path uses requested root scheduler");
            require(runs.dag.parallel_mode_effective == "roots",
                    "DAG path uses requested root scheduler");
            require(runs.direct.threads_used == runs.dag.threads_used &&
                        runs.direct.parallel_statistics.root_tasks_claimed ==
                            runs.dag.parallel_statistics.root_tasks_claimed &&
                        runs.direct.parallel_statistics.queue_claims ==
                            runs.dag.parallel_statistics.queue_claims,
                    "direct/DAG root scheduler metadata parity");
        }
    }

    const QueryPlan plan = makeQueryPlan(
        query, decomposition, tree, OrderMode::Paper);
    CandidateDagCompiler compiler;
    const CandidateDag dag = compiler.compile(
        query, plan, decomposition);
    GopherMatcherOptions options;
    options.threads = 4;
    options.parallel_mode = ParallelMode::Roots;
    options.order_mode = OrderMode::Paper;
    options.early_prune = true;
    options.result_digest = true;
    options.executor.memo_mode = MemoMode::None;
    options.executor.verify_dag = true;
    GopherMatcher direct(
        graph, query, plan, tree, k, options);
    GopherMatcher candidate_dag(
        graph, query, plan, dag, tree, k, options);
    std::ostringstream direct_rows;
    std::ostringstream dag_rows;
    const GopherMatchSummary direct_full =
        direct.run(MatchOutputMode::Full, &direct_rows);
    const GopherMatchSummary dag_full =
        candidate_dag.run(MatchOutputMode::Full, &dag_rows);
    require(direct_full.match_count == dag_full.match_count &&
                direct_full.result_digest == dag_full.result_digest &&
                direct_rows.str() == dag_rows.str(),
            "direct/DAG full-output parity");
    const MappingSet oracle_mappings =
        bruteForceExternalMappingSet(graph, query, k);
    require(oracle_mappings.size() == 6,
            "triangle independent oracle mapping-set size");
    require(parseFullOutputMappings(
                direct_rows.str(), query.num_vertices) == oracle_mappings,
            "direct full output equals independent oracle mapping set");
    require(parseFullOutputMappings(
                dag_rows.str(), query.num_vertices) == oracle_mappings,
            "DAG full output equals independent oracle mapping set");
    require(direct_full.threads_used == 1 &&
                direct_full.parallel_mode_effective == "serial-output",
            "direct full output preserves deterministic serial semantics");

    GopherMatcherOptions quiet_options = options;
    quiet_options.executor.collect_statistics = false;
    quiet_options.executor.verify_dag = false;
    GopherMatcher quiet_direct(
        graph, query, plan, tree, k, quiet_options);
    GopherMatcher quiet_dag(
        graph, query, plan, dag, tree, k, quiet_options);
    const GopherMatchSummary quiet_direct_summary =
        quiet_direct.run(MatchOutputMode::CountOnly);
    const GopherMatchSummary quiet_dag_summary =
        quiet_dag.run(MatchOutputMode::CountOnly);
    require(quiet_direct_summary.match_count == oracle_mappings.size() &&
                quiet_direct_summary.match_count ==
                    quiet_dag_summary.match_count &&
                quiet_direct_summary.result_digest ==
                    quiet_dag_summary.result_digest,
            "statistics-off direct/DAG/oracle parity");
    require(quiet_direct_summary.statistics.search_nanoseconds == 0 &&
                quiet_direct_summary.statistics.longest_compute_nanoseconds ==
                    0 &&
                quiet_dag_summary.statistics.search_nanoseconds == 0 &&
                quiet_dag_summary.statistics.longest_compute_nanoseconds == 0,
            "statistics-off runs do not collect timing counters");
    require(quiet_direct_summary.parallel_mode_effective == "roots" &&
                quiet_dag_summary.parallel_mode_effective == "roots" &&
                quiet_direct_summary.threads_used ==
                    quiet_dag_summary.threads_used &&
                quiet_direct_summary.parallel_statistics
                        .root_tasks_claimed ==
                    quiet_dag_summary.parallel_statistics.root_tasks_claimed,
            "statistics-off root scheduler metadata parity");

    SearchStatisticsCollector disabled_statistics(false);
    DirectEvaluator quiet_evaluator(
        graph, query, plan, tree, k, disabled_statistics);
    SearchState bound_state;
    bound_state.mapping = {0, -1, 4};
    const TemporalSupport quiet_support =
        quiet_evaluator.temporalSupport(1, 2, bound_state);
    require(quiet_support.has_constraints &&
                !quiet_support.intervals->empty() &&
                maximumConsecutiveRun(*quiet_support.intervals) > k &&
                quiet_support.longest_run == k,
            "statistics-off direct evaluator uses the k sentinel");
}

void testRootSelfLoopParity() {
    const Graph graph = makeDataGraph();
    const Graph query = makeQuery(
        {labelFromString("A")}, {{0, 0}});
    const int k = 3;
    const QueryDecomposition decomposition = decompose(graph, query);
    require(decomposition.connected, "self-loop decomposition");
    const TDTree tree(graph, query, decomposition, k);
    const ComparedRuns runs = compareCountRuns(
        graph, query, decomposition, tree, k, 4, true);
    require(runs.direct.match_count == 1,
            "direct self-loop known count");
    require(bruteForceCount(graph, query, k) == 1,
            "self-loop independent oracle count");
    require(runs.direct.match_count == runs.dag.match_count &&
                runs.direct.result_digest == runs.dag.result_digest,
            "direct/DAG root self-loop parity");
}

void testTimestampGapFixture() {
    Graph graph;
    graph.num_vertices = 3;
    graph.vertex_labels = {
        labelFromString("A"),
        labelFromString("B"),
        labelFromString("C")};
    graph.adj.resize(3);
    graph.in_adj.resize(3);
    // Each edge is individually 3-durable, but their common support is split
    // into two length-2 runs. Aggregate overlap must not pass k=3.
    addTemporalEdge(graph, 0, 1, {{1, 3}, {5, 7}});
    addTemporalEdge(graph, 1, 2, {{2, 6}});
    finalizeGraph(graph);
    const Graph query = makeQuery(
        {labelFromString("A"),
         labelFromString("B"),
         labelFromString("C")},
        {{0, 1}, {1, 2}});
    const int k = 3;
    const QueryDecomposition decomposition = decompose(graph, query);
    const TDTree tree(graph, query, decomposition, k);
    const ComparedRuns runs = compareCountRuns(
        graph, query, decomposition, tree, k, 2, true);
    require(bruteForceCount(graph, query, k) == 0,
            "timestamp-gap independent oracle count");
    require(runs.direct.match_count == 0 &&
                runs.direct.match_count == runs.dag.match_count &&
                runs.direct.result_digest == runs.dag.result_digest,
            "timestamp-gap direct/DAG/oracle parity");
}

void testReciprocalArcsFixture() {
    Graph graph;
    graph.num_vertices = 2;
    graph.vertex_labels = {
        labelFromString("A"), labelFromString("B")};
    graph.adj.resize(2);
    graph.in_adj.resize(2);
    addTemporalEdge(graph, 0, 1, {{1, 5}});
    addTemporalEdge(graph, 1, 0, {{3, 6}});
    finalizeGraph(graph);
    const Graph query = makeQuery(
        {labelFromString("A"), labelFromString("B")},
        {{0, 1}, {1, 0}});
    const int k = 3;
    const QueryDecomposition decomposition = decompose(graph, query);
    const TDTree tree(graph, query, decomposition, k);
    const ComparedRuns runs = compareCountRuns(
        graph, query, decomposition, tree, k, 2, true);
    require(bruteForceCount(graph, query, k) == 1,
            "reciprocal-arcs independent oracle count");
    require(runs.direct.match_count == 1 &&
                runs.direct.match_count == runs.dag.match_count &&
                runs.direct.result_digest == runs.dag.result_digest,
            "reciprocal-arcs direct/DAG/oracle parity");
}

void testEmptyResultFixture() {
    Graph graph;
    graph.num_vertices = 3;
    graph.vertex_labels = {
        labelFromString("A"),
        labelFromString("B"),
        labelFromString("B")};
    graph.adj.resize(3);
    graph.in_adj.resize(3);
    addTemporalEdge(graph, 0, 1, {{1, 5}});
    finalizeGraph(graph);
    const Graph query = makeQuery(
        {labelFromString("A"), labelFromString("B")},
        {{0, 1}});
    const int k = 3;
    const QueryDecomposition decomposition = decompose(graph, query);
    const TDTree tree(graph, query, decomposition, k);
    const QueryPlan plan = makeQueryPlan(
        query, decomposition, tree, OrderMode::Paper);
    SearchStatisticsCollector statistics(true);
    DirectEvaluator evaluator(
        graph, query, plan, tree, k, statistics);

    const int extension = plan.order[1];
    const int parent = plan.parent[static_cast<std::size_t>(extension)];
    SearchState missing_block;
    missing_block.mapping.assign(
        static_cast<std::size_t>(query.num_vertices), -1);
    // Vertex 2 has the wrong label for the bound parent and therefore has no
    // TD relation block for this extension.
    missing_block.mapping[static_cast<std::size_t>(parent)] = 2;
    const auto empty_candidates = evaluator.structuralCandidates(
        extension, missing_block);
    require(empty_candidates->empty(),
            "direct structural missing-block result is empty");

    SearchState missing_edge;
    missing_edge.mapping.assign(
        static_cast<std::size_t>(query.num_vertices), -1);
    if (extension == 1) {
        missing_edge.mapping[0] = 0;
        const TemporalSupport support = evaluator.temporalSupport(
            1, 2, missing_edge);
        require(support.has_constraints && support.intervals->empty(),
                "direct temporal missing-edge result is empty");
    } else {
        missing_edge.mapping[1] = 2;
        const TemporalSupport support = evaluator.temporalSupport(
            0, 0, missing_edge);
        require(support.has_constraints && support.intervals->empty(),
                "direct temporal missing-edge result is empty");
    }
}

void testInjectivityRejectionFixture() {
    Graph graph;
    graph.num_vertices = 2;
    graph.vertex_labels = {
        labelFromString("A"), labelFromString("A")};
    graph.adj.resize(2);
    graph.in_adj.resize(2);
    addTemporalEdge(graph, 0, 1, {{1, 5}});
    addTemporalEdge(graph, 1, 0, {{1, 5}});
    finalizeGraph(graph);
    const Graph query = makeQuery(
        {labelFromString("A"),
         labelFromString("A"),
         labelFromString("A")},
        {{0, 1}, {1, 2}});
    const int k = 3;
    const QueryDecomposition decomposition = decompose(graph, query);
    const TDTree tree(graph, query, decomposition, k);
    const ComparedRuns runs = compareCountRuns(
        graph, query, decomposition, tree, k, 2, true);
    require(bruteForceCount(graph, query, k) == 0,
            "injectivity independent oracle count");
    require(runs.direct.match_count == 0 &&
                runs.direct.match_count == runs.dag.match_count &&
                runs.direct.result_digest == runs.dag.result_digest,
            "injectivity direct/DAG/oracle parity");
    require(runs.direct.statistics.pruned_injectivity > 0 &&
                runs.dag.statistics.pruned_injectivity > 0,
            "both paths explicitly reject reused data vertices");
}

} // namespace

int main() {
    try {
        testTriangleParity();
        testRootSelfLoopParity();
        testTimestampGapFixture();
        testReciprocalArcsFixture();
        testEmptyResultFixture();
        testInjectivityRejectionFixture();
        std::cout << "DirectEvaluator focused tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DirectEvaluator focused test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
