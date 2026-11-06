#include "../TDTree.h"
#include "../Utils.h"
#include "../query_decomposition.h"
#include "../temporal_cost_planner.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void requireLegalCompletePlan(
    const Graph& query,
    const QueryDecomposition& decomposition,
    const std::string& context) {
    require(decomposition.connected, context + ": connected plan");
    require(
        decomposition.dfs_order.size() ==
                static_cast<std::size_t>(query.num_vertices) &&
            decomposition.parent.size() ==
                static_cast<std::size_t>(query.num_vertices),
        context + ": full plan vectors");
    require(
        !decomposition.dfs_order.empty() &&
            decomposition.dfs_order.front() == decomposition.root,
        context + ": root is first");
    std::vector<std::uint8_t> seen(
        static_cast<std::size_t>(query.num_vertices), 0);
    for (std::size_t depth = 0;
         depth < decomposition.dfs_order.size();
         ++depth) {
        const int vertex = decomposition.dfs_order[depth];
        require(
            vertex >= 0 && vertex < query.num_vertices &&
                !seen[static_cast<std::size_t>(vertex)],
            context + ": unique in-range DFS vertex");
        if (depth == 0) {
            require(
                decomposition.parent[static_cast<std::size_t>(vertex)] == -1,
                context + ": root parent sentinel");
        } else {
            const int parent =
                decomposition.parent[static_cast<std::size_t>(vertex)];
            require(
                parent >= 0 && parent < query.num_vertices &&
                    seen[static_cast<std::size_t>(parent)],
                context + ": parent precedes child");
            require(
                GraphUtils::hasEdge(query.adj, parent, vertex) ||
                    GraphUtils::hasEdge(query.adj, vertex, parent),
                context + ": parent-child query adjacency");
        }
        seen[static_cast<std::size_t>(vertex)] = 1;
    }
}

void requireTreePlanMetadata(
    const std::filesystem::path& result_path,
    const QueryDecomposition& decomposition,
    const std::string& context) {
    std::ifstream input(result_path, std::ios::binary);
    const std::string contents(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    std::ostringstream order;
    order << "plan_order:";
    for (int vertex : decomposition.dfs_order) order << ' ' << vertex;
    std::ostringstream parent;
    parent << "plan_parent:";
    for (int value : decomposition.parent) parent << ' ' << value;
    require(
        contents.find(order.str()) != std::string::npos &&
            contents.find(parent.str()) != std::string::npos,
        context + ": handed-off tree owns the selected plan");
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
    for (const auto& interval : edge.activeIntervals()) {
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
            if (data_edge == nullptr || data_edge->activeIntervals().empty()) {
                return false;
            }
            mapped_edges.push_back(data_edge);
            first_snapshot = std::min(
                first_snapshot, data_edge->activeIntervals().front().start);
            last_snapshot = std::max(
                last_snapshot, data_edge->activeIntervals().back().end);
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

// Test-only exhaustive oracle for small queries.  This deliberately does not
// call either temporal-cost planner to produce candidate plans: every
// parent-before-child weakly connected order and every legal earlier parent is
// generated independently here.
struct ExhaustivePlanMeasurement {
    QueryDecomposition decomposition;
    std::uint64_t answer_count = 0;
    std::uint64_t root_plus_raw_extensions = 0;
};

struct ExhaustivePlanOracleResult {
    std::vector<ExhaustivePlanMeasurement> plans;
    std::uint64_t optimum_work = 0;
};

bool weaklyAdjacent(const Graph& query, int lhs, int rhs) {
    return GraphUtils::hasEdge(query.adj, lhs, rhs) ||
        GraphUtils::hasEdge(query.adj, rhs, lhs);
}

QueryDecomposition makeIndependentOracleDecomposition(
    const Graph& query,
    const std::vector<int>& order,
    const std::vector<int>& parent) {
    QueryDecomposition result;
    result.vertex_labels = query.vertex_labels;
    result.root = order.empty() ? -1 : order.front();
    result.dfs_order = order;
    result.parent = parent;
    result.connected =
        order.size() == static_cast<std::size_t>(query.num_vertices);
    result.selectivity_mode = SelectivityMode::TemporalCostV2;
    result.temporal_selectivity.assign(
        static_cast<std::size_t>(query.num_vertices), 0.0);
    result.spanning_tree_adj.assign(
        static_cast<std::size_t>(query.num_vertices), {});

    std::vector<std::vector<std::uint8_t>> is_tree_pair(
        static_cast<std::size_t>(query.num_vertices),
        std::vector<std::uint8_t>(
            static_cast<std::size_t>(query.num_vertices), 0));
    for (int child = 0; child < query.num_vertices; ++child) {
        const int tree_parent = parent[static_cast<std::size_t>(child)];
        if (tree_parent < 0) continue;
        result.spanning_tree_adj[static_cast<std::size_t>(tree_parent)]
            .push_back(child);
        is_tree_pair[static_cast<std::size_t>(tree_parent)]
                    [static_cast<std::size_t>(child)] = 1;
        is_tree_pair[static_cast<std::size_t>(child)]
                    [static_cast<std::size_t>(tree_parent)] = 1;
    }
    for (int source = 0; source < query.num_vertices; ++source) {
        for (const Edge& edge :
             query.adj[static_cast<std::size_t>(source)]) {
            if (is_tree_pair[static_cast<std::size_t>(source)]
                            [static_cast<std::size_t>(edge.to)] == 0) {
                result.non_tree_edges.emplace_back(source, edge.to);
            }
        }
    }
    return result;
}

std::vector<QueryDecomposition> independentlyEnumerateAllLegalPlans(
    const Graph& query) {
    require(
        query.num_vertices > 0 && query.num_vertices <= 5,
        "exhaustive plan oracle is restricted to 1..5 query vertices");
    std::vector<QueryDecomposition> plans;
    std::vector<int> order;
    std::vector<int> parent(
        static_cast<std::size_t>(query.num_vertices), -1);
    std::vector<std::uint8_t> seen(
        static_cast<std::size_t>(query.num_vertices), 0);

    std::function<void()> extend = [&]() {
        if (order.size() == static_cast<std::size_t>(query.num_vertices)) {
            plans.push_back(makeIndependentOracleDecomposition(
                query, order, parent));
            return;
        }
        for (int next = 0; next < query.num_vertices; ++next) {
            if (seen[static_cast<std::size_t>(next)] != 0) continue;
            if (order.empty()) {
                seen[static_cast<std::size_t>(next)] = 1;
                order.push_back(next);
                extend();
                order.pop_back();
                seen[static_cast<std::size_t>(next)] = 0;
                continue;
            }
            // Choosing each earlier weak neighbor independently enumerates all
            // legal parent assignments for this exact order.
            // Copy the prefix because recursive push_back/pop_back operations
            // may reallocate `order` and invalidate a range-for iterator.
            const std::vector<int> earlier_vertices = order;
            for (int earlier : earlier_vertices) {
                if (!weaklyAdjacent(query, earlier, next)) continue;
                parent[static_cast<std::size_t>(next)] = earlier;
                seen[static_cast<std::size_t>(next)] = 1;
                order.push_back(next);
                extend();
                order.pop_back();
                seen[static_cast<std::size_t>(next)] = 0;
                parent[static_cast<std::size_t>(next)] = -1;
            }
        }
    };
    extend();
    return plans;
}

std::string oraclePlanIdentity(const QueryDecomposition& plan) {
    std::ostringstream output;
    output << plan.root << '|';
    for (int vertex : plan.dfs_order) output << vertex << ',';
    output << '|';
    for (int tree_parent : plan.parent) output << tree_parent << ',';
    return output.str();
}

ExhaustivePlanOracleResult measureAllLegalPlans(
    const Graph& data,
    const Graph& query,
    int minimum_duration,
    std::uint64_t expected_answer_count,
    const std::filesystem::path& scratch_path) {
    ExhaustivePlanOracleResult result;
    const std::vector<QueryDecomposition> plans =
        independentlyEnumerateAllLegalPlans(query);
    require(!plans.empty(), "exhaustive oracle found at least one legal plan");
    std::vector<std::string> identities;
    identities.reserve(plans.size());
    for (const QueryDecomposition& plan : plans) {
        identities.push_back(oraclePlanIdentity(plan));
    }
    std::sort(identities.begin(), identities.end());
    require(
        std::adjacent_find(identities.begin(), identities.end()) ==
            identities.end(),
        "exhaustive oracle emits every legal plan exactly once");
    result.optimum_work = std::numeric_limits<std::uint64_t>::max();
    result.plans.reserve(plans.size());
    for (const QueryDecomposition& decomposition : plans) {
        requireLegalCompletePlan(query, decomposition, "exhaustive oracle plan");
        TDTree tree(data, query, decomposition, minimum_duration);
        const MatchSummary summary = tree.save_res(
            scratch_path.string(), MatchOutputMode::CountOnly, 1);
        require(
            summary.output_written && !summary.count_overflow &&
                summary.threads_requested == 1 &&
                summary.threads_used == 1,
            "exhaustive oracle evaluates every plan count-only on one thread");
        require(
            summary.match_count == expected_answer_count,
            "all legal TD-tree plans have the brute-force answer count");
        require(
            summary.generated_candidates <=
                std::numeric_limits<std::uint64_t>::max() -
                    static_cast<std::uint64_t>(
                        summary.root_candidate_count),
            "oracle logical-work addition does not overflow");
        const std::uint64_t logical_work =
            static_cast<std::uint64_t>(summary.root_candidate_count) +
            summary.generated_candidates;
        result.optimum_work = std::min(result.optimum_work, logical_work);
        result.plans.push_back(
            {decomposition, summary.match_count, logical_work});
    }
    std::filesystem::remove(scratch_path);
    return result;
}

const ExhaustivePlanMeasurement* findOraclePlan(
    const ExhaustivePlanOracleResult& oracle,
    const QueryDecomposition& selected,
    const std::string& context) {
    const std::string identity = oraclePlanIdentity(selected);
    const auto found = std::find_if(
        oracle.plans.begin(), oracle.plans.end(),
        [&](const ExhaustivePlanMeasurement& measurement) {
            return oraclePlanIdentity(measurement.decomposition) == identity;
        });
    require(
        found != oracle.plans.end(),
        context + ": selected plan occurs in independent legal-plan oracle");
    return &*found;
}

double oracleRegretRatio(
    std::uint64_t selected_work,
    std::uint64_t optimum_work) {
    if (optimum_work == 0) {
        return selected_work == 0
            ? 1.0
            : std::numeric_limits<double>::infinity();
    }
    return static_cast<double>(selected_work) /
        static_cast<double>(optimum_work);
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
    std::array<double, kLabelCount> mean_active_snapshots{};
    counts[0] = counts[1] = counts[2] = 2;
    mean_active_snapshots[0] = mean_active_snapshots[1] =
        mean_active_snapshots[2] = 5.0;
    return decomposeQuery(query, counts, mean_active_snapshots);
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
    require(first.temporal_edges[0].activeIntervals().size() == 1, "compressed interval count");
    require(first.temporal_edges[0].activeIntervals()[0].start == 2 &&
            first.temporal_edges[0].activeIntervals()[0].end == 3,
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
    std::array<double, kLabelCount> mean_vertex_active_snapshots{};
    for (std::size_t label = 0; label < kLabelCount; ++label) {
        if (counts[label] != 0) {
            mean_vertex_active_snapshots[label] =
                static_cast<double>(duration_sums[label]) /
                static_cast<double>(counts[label]);
        }
    }
    const QueryDecomposition decomposition =
        decomposeQuery(query, counts, mean_vertex_active_snapshots);
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
    std::array<double, kLabelCount> mean_active_snapshots{};
    counts[0] = 1;
    counts[1] = 2;
    counts[2] = 4;
    // Deliberately asymmetric: with argmin these values select roots 0, 1,
    // and 2 for c/(d*A), d/(c*A), and d*A/c, respectively.  This prevents a
    // numerator/denominator regression from hiding behind equal activity.
    mean_active_snapshots[0] = 2.0;
    mean_active_snapshots[1] = 3.0;
    mean_active_snapshots[2] = 1.0;

    const auto paper =
        decomposeQuery(
            query, counts, mean_active_snapshots, SelectivityMode::Paper);
    require(paper.root == 0, "corrected durability-aware argmin root");
    require(
        std::abs(paper.temporal_selectivity[0] - 0.25) < 1e-15 &&
            std::abs(paper.temporal_selectivity[1] - (1.0 / 3.0)) < 1e-15 &&
            std::abs(paper.temporal_selectivity[2] - 2.0) < 1e-15,
        "paper mode uses candidate count over degree-times-mean-activity");
    require(paper.selectivity_mode == SelectivityMode::Paper,
            "paper selectivity mode is retained in the decomposition");
    require(paper.connected, "connected paper decomposition");
    require(paper.dfs_order == std::vector<int>({0, 1, 2}),
            "recursive DFS produces a deep triangle tree");
    require(paper.non_tree_edges == std::vector<std::pair<int, int>>({{2, 0}}),
            "directed tree and non-tree edges are disjoint and complete");

    const auto legacy_reciprocal = decomposeQuery(
        query, counts, mean_active_snapshots,
        SelectivityMode::LegacyReciprocal);
    require(
        legacy_reciprocal.root == 1 &&
            std::abs(legacy_reciprocal.temporal_selectivity[0] - 1.0) < 1e-15 &&
            std::abs(legacy_reciprocal.temporal_selectivity[1] - (1.0 / 3.0)) < 1e-15 &&
            std::abs(legacy_reciprocal.temporal_selectivity[2] - 0.5) < 1e-15,
        "legacy reciprocal mode preserves degree over candidate-times-mean-activity");

    const auto legacy_extracted_product = decomposeQuery(
        query, counts, mean_active_snapshots,
        SelectivityMode::LegacyExtractedProduct);
    require(
        legacy_extracted_product.root == 2 &&
            std::abs(legacy_extracted_product.temporal_selectivity[0] - 4.0) < 1e-15 &&
            std::abs(legacy_extracted_product.temporal_selectivity[1] - 3.0) < 1e-15 &&
            std::abs(legacy_extracted_product.temporal_selectivity[2] - 0.5) < 1e-15,
        "legacy extracted-product preserves degree-times-mean-activity over candidates");

    const auto intended =
        decomposeQuery(
            query, counts, mean_active_snapshots, SelectivityMode::Intended);
    require(intended.root == 0,
            "intended alias retains the corrected durability-aware root");
    require(
        intended.temporal_selectivity == paper.temporal_selectivity,
        "intended is an arithmetic alias for paper mode");
    require(intended.selectivity_mode == SelectivityMode::Intended,
            "intended selectivity mode is retained in the decomposition");
    require(intended.connected &&
            intended.dfs_order == std::vector<int>({0, 1, 2}),
            "intended alias preserves the corrected DFS root/order");

    const auto tdtree =
        decomposeQuery(
            query,
            counts,
            mean_active_snapshots,
            SelectivityMode::TDTree);
    require(
        tdtree.root == 0 &&
            tdtree.temporal_selectivity[0] == 0.5 &&
            tdtree.temporal_selectivity[1] == 1.0 &&
            tdtree.temporal_selectivity[2] == 2.0 &&
            tdtree.dfs_order == std::vector<int>({0, 1, 2}),
        "TD-Tree mode uses |V_L|/degree and deterministic argmin DFS");

    require(
        std::string(selectivityFormulaId(SelectivityMode::TDTree)) ==
                "label_population_over_query_degree" &&
            std::string(selectivityFormulaId(SelectivityMode::Paper)) ==
                "label_population_over_product_query_degree_mean_vertex_activity" &&
            std::string(selectivityFormulaId(SelectivityMode::Intended)) ==
                "label_population_over_product_query_degree_mean_vertex_activity" &&
            std::string(selectivityFormulaId(SelectivityMode::LegacyReciprocal)) ==
                "query_degree_over_product_label_population_mean_vertex_activity" &&
            std::string(selectivityFormulaId(SelectivityMode::LegacyExtractedProduct)) ==
                "query_degree_times_mean_vertex_activity_over_label_population",
        "formula IDs unambiguously identify every scalar formula");
    SelectivityMode parsed = SelectivityMode::TDTree;
    require(
        parseSelectivityMode("paper", parsed) &&
            parsed == SelectivityMode::Paper &&
            parseSelectivityMode("intended", parsed) &&
            parsed == SelectivityMode::Intended &&
            parseSelectivityMode("legacy-reciprocal", parsed) &&
            parsed == SelectivityMode::LegacyReciprocal &&
            parseSelectivityMode("legacy-extracted-product", parsed) &&
            parsed == SelectivityMode::LegacyExtractedProduct &&
            parseSelectivityMode("tdtree", parsed) &&
            parsed == SelectivityMode::TDTree,
        "all scalar selectivity names parse to their explicit modes");

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

    std::array<std::size_t, kLabelCount> zero_activity_counts{};
    zero_activity_counts[0] = 9;
    zero_activity_counts[1] = 2;
    zero_activity_counts[2] = 5;
    const std::array<double, kLabelCount> zero_activity{};
    const auto infinite_tie = decomposeQuery(
        query,
        zero_activity_counts,
        zero_activity,
        SelectivityMode::Paper);
    require(
        infinite_tie.root == 1 &&
            std::isinf(infinite_tie.temporal_selectivity[0]) &&
            std::isinf(infinite_tie.temporal_selectivity[1]) &&
            std::isinf(infinite_tie.temporal_selectivity[2]),
        "equal infinite Paper scores retain candidate-count/query-ID tie breaks");

    std::array<std::size_t, kLabelCount> near_counts{};
    std::array<double, kLabelCount> near_activity{};
    near_counts[0] = 2;
    near_counts[1] = 1;
    near_counts[2] = 100;
    near_activity[0] = 1.0;
    near_activity[1] = std::nextafter(0.5, 0.0);
    near_activity[2] = 1.0;
    const auto strict_argmin = decomposeQuery(
        query,
        near_counts,
        near_activity,
        SelectivityMode::Paper);
    require(
        strict_argmin.temporal_selectivity[0] == 1.0 &&
            strict_argmin.temporal_selectivity[1] > 1.0 &&
            strict_argmin.temporal_selectivity[1] - 1.0 < 1e-15 &&
            near_counts[1] < near_counts[0] &&
            strict_argmin.root == 0,
        "Paper root is the strict numeric argmin, not an epsilon-tied smaller domain");

    SelectivityMode parsed_v2 = SelectivityMode::Paper;
    require(
        parseSelectivityMode("temporal-cost-v2", parsed_v2) &&
            parsed_v2 == SelectivityMode::TemporalCostV2 &&
            std::string(selectivityModeName(parsed_v2)) ==
                "temporal-cost-v2",
        "temporal-cost-v2 is an explicit opt-in selectivity mode");
}

void testSurvivalSelectivity() {
    Graph data;
    data.num_vertices = 4;
    data.external_ids = {10, 11, 20, 21};
    data.vertex_labels = {
        labelFromString("A"), labelFromString("A"),
        labelFromString("B"), labelFromString("B")};
    data.adj.resize(4);
    data.in_adj.resize(4);
    addTemporalEdge(data, 0, 2, {{1, 3}});
    addTemporalEdge(data, 1, 3, {{1, 2}});
    finalizeSyntheticGraph(data);

    const Graph query = makeTwoVertexQuery(false);
    const SurvivalSelectivityStatistics statistics =
        computeSurvivalSelectivityStatistics(data, query, 3);
    require(
        statistics.structural_candidate_counts ==
            std::vector<std::size_t>({2, 2}),
        "survival statistics use the exact local candidate predicate");
    require(
        statistics.compatible_edge_counts ==
                std::vector<std::size_t>({2, 2}) &&
            statistics.durable_compatible_edge_counts ==
                std::vector<std::size_t>({1, 1}),
        "survival statistics preserve directed label-pair edge counts");
    require(
        std::abs(statistics.durability_survival_rates[0] - 0.5) <
                1e-15 &&
            std::abs(statistics.durability_survival_rates[1] - 0.5) <
                1e-15,
        "survival rate uses consecutive-duration edge histories");

    std::array<std::size_t, kLabelCount> label_counts{};
    std::array<double, kLabelCount> mean_vertex_active_snapshots{};
    label_counts[labelFromString("A")] = 2;
    label_counts[labelFromString("B")] = 2;
    mean_vertex_active_snapshots[labelFromString("A")] = 6.0;
    mean_vertex_active_snapshots[labelFromString("B")] = 6.0;

    const QueryDecomposition structural = decomposeQuery(
        query,
        label_counts,
        mean_vertex_active_snapshots,
        SelectivityMode::Structural,
        &statistics);
    const QueryDecomposition survival = decomposeQuery(
        query,
        label_counts,
        mean_vertex_active_snapshots,
        SelectivityMode::Survival,
        &statistics);
    require(
        structural.root == 0 &&
            std::abs(structural.temporal_selectivity[0] - 2.0) <
                1e-15,
        "structural control score is C_struct over query degree");
    require(
        survival.root == 0 &&
            std::abs(survival.temporal_selectivity[0] - 1.0) <
                1e-15,
        "survival score multiplies C_struct over degree by rho_k");
    require(
        survival.structural_candidate_counts ==
                statistics.structural_candidate_counts &&
            survival.durability_survival_rates ==
                statistics.durability_survival_rates,
        "survival diagnostics are retained in the query plan");

    bool rejected_missing_statistics = false;
    try {
        (void)decomposeQuery(
            query,
            label_counts,
            mean_vertex_active_snapshots,
            SelectivityMode::Survival);
    } catch (const std::invalid_argument&) {
        rejected_missing_statistics = true;
    }
    require(
        rejected_missing_statistics,
        "survival mode rejects a plan without k-aware statistics");

    TDTree tree(data, query, survival, 3);
    const auto result_path =
        std::filesystem::temp_directory_path() /
        "ours_survival_selectivity_result.dat";
    const MatchSummary summary = tree.save_res(
        result_path.string(), MatchOutputMode::CountOnly);
    require(
        summary.output_written && summary.match_count == 1,
        "survival ordering preserves the exact durable match count");
    std::ifstream result(result_path, std::ios::binary);
    const std::string contents(
        (std::istreambuf_iterator<char>(result)),
        std::istreambuf_iterator<char>());
    require(
        contents.find("selectivity_mode: survival") !=
                std::string::npos &&
            contents.find("structural_candidate_count=2") !=
                std::string::npos &&
            contents.find("durability_survival_rate=0.5") !=
                std::string::npos,
        "survival result records auditable per-vertex statistics");
    result.close();
    std::filesystem::remove(result_path);
}

void testTemporalConditionalCostPlanner(
    const std::filesystem::path& directory) {
    Graph data;
    data.num_vertices = 6;
    data.external_ids = {10, 20, 30, 40, 50, 60};
    data.vertex_labels = {
        labelFromString("A"), labelFromString("B"),
        labelFromString("C"), labelFromString("A"),
        labelFromString("B"), labelFromString("C")};
    data.adj.resize(6);
    data.in_adj.resize(6);
    // Every edge has an individual k-run. The first A-B-C path has no
    // common k-window, while the second path is a durable match.
    addTemporalEdge(data, 0, 1, {{1, 3}});
    addTemporalEdge(data, 1, 2, {{10, 12}});
    addTemporalEdge(data, 3, 4, {{5, 7}});
    addTemporalEdge(data, 4, 5, {{5, 7}});
    finalizeSyntheticGraph(data);

    Graph query;
    query.num_vertices = 3;
    query.external_ids = {0, 1, 2};
    query.vertex_labels = {
        labelFromString("A"), labelFromString("B"),
        labelFromString("C")};
    query.adj.resize(3);
    query.in_adj.resize(3);
    query.adj[0].push_back({1, -1});
    query.in_adj[1].push_back({0, -1});
    query.adj[1].push_back({2, -1});
    query.in_adj[2].push_back({1, -1});

    std::array<std::size_t, kLabelCount> counts{};
    std::array<double, kLabelCount> lifespans{};
    for (Label label : data.vertex_labels) {
        ++counts[label];
    }
    lifespans.fill(6.0);
    TemporalCostPlannerOptions options;
    options.beam_width = 64;
    options.sample_cap = 64;
    TemporalPlanDiagnostics diagnostics;
    const QueryDecomposition decomposition =
        planTemporalCostQuery(
            data,
            query,
            3,
            counts,
            lifespans,
            options,
            &diagnostics);
    require(
        decomposition.connected &&
            decomposition.selectivity_mode ==
                SelectivityMode::TemporalCost &&
            decomposition.dfs_order.size() == 3,
        "temporal-cost planner returns a connected full plan");
    std::vector<int> position(3, -1);
    for (std::size_t index = 0;
         index < decomposition.dfs_order.size();
         ++index) {
        position[static_cast<std::size_t>(
            decomposition.dfs_order[index])] =
            static_cast<int>(index);
    }
    for (int query_vertex = 0;
         query_vertex < query.num_vertices;
         ++query_vertex) {
        const int parent =
            decomposition.parent[
                static_cast<std::size_t>(query_vertex)];
        if (parent < 0) continue;
        require(
            position[static_cast<std::size_t>(parent)] <
                    position[
                        static_cast<std::size_t>(query_vertex)] &&
                (GraphUtils::hasEdge(
                     query.adj, parent, query_vertex) ||
                 GraphUtils::hasEdge(
                     query.adj, query_vertex, parent)),
            "temporal-cost parent is an earlier weak neighbor");
    }
    bool observed_common_window_pruning = false;
    for (const auto& root_plan : diagnostics.root_plans) {
        for (const auto& transition :
             root_plan.transitions) {
            if (transition.sampled_raw_extensions >
                transition.sampled_accepted_extensions) {
                observed_common_window_pruning = true;
            }
        }
    }
    require(
        observed_common_window_pruning,
        "temporal-cost samples reject individually durable edges "
        "without a common k-window");
    require(
        std::all_of(
            diagnostics.root_plans.begin(),
            diagnostics.root_plans.end(),
            [](const TemporalRootPlanEstimate& plan) {
                return plan.estimated_total_exact &&
                    std::all_of(
                        plan.transitions.begin(),
                        plan.transitions.end(),
                        [](const TemporalTransitionEstimate&
                               transition) {
                            return transition
                                       .input_prefix_population_exact &&
                                transition
                                    .output_prefix_population_exact;
                        });
            }),
        "temporal-cost diagnostics mark uncapped fixture estimates exact");
    require(
        diagnostics.sampled_baseline_estimate_exact,
        "temporal-cost diagnostics mark the uncapped baseline cost exact");
    require(
        std::abs(
            diagnostics.selected_estimated_total_candidates - 6.0) <
            1e-12,
        "temporal-cost objective equals root scans plus attempted TD-block "
        "extensions on the exact fixture");

    TemporalPlanDiagnostics repeated_diagnostics;
    const QueryDecomposition repeated_decomposition =
        planTemporalCostQuery(
            data,
            query,
            3,
            counts,
            lifespans,
            options,
            &repeated_diagnostics);
    require(
        repeated_decomposition.root == decomposition.root &&
            repeated_decomposition.dfs_order ==
                decomposition.dfs_order &&
            repeated_decomposition.parent == decomposition.parent &&
            repeated_diagnostics
                    .selected_estimated_total_candidates ==
                diagnostics.selected_estimated_total_candidates,
        "temporal-cost root/order/parent and estimated cost are deterministic");

    const auto result_path =
        directory / "ours_temporal_cost_result.dat";
    TDTree tree(data, query, decomposition, 3);
    const MatchSummary summary = tree.save_res(
        result_path.string(), MatchOutputMode::CountOnly);
    require(
        summary.output_written &&
            !summary.count_overflow &&
            summary.match_count == 1,
        "temporal-cost plan preserves the exact durable result");

    TemporalCostPlannerOptions sampled_options = options;
    sampled_options.sample_cap = 1;
    TemporalPlanDiagnostics sampled_diagnostics;
    const QueryDecomposition sampled_decomposition =
        planTemporalCostQuery(
            data,
            query,
            3,
            counts,
            lifespans,
            sampled_options,
            &sampled_diagnostics);
    require(
        sampled_decomposition.connected &&
            std::any_of(
                sampled_diagnostics.root_plans.begin(),
                sampled_diagnostics.root_plans.end(),
                [](const TemporalRootPlanEstimate& plan) {
                    return !plan.estimated_total_exact;
                }),
        "temporal-cost diagnostics expose capped estimates as sampled");
    require(
        !sampled_diagnostics
             .sampled_baseline_estimate_exact,
        "temporal-cost diagnostics expose a capped baseline cost as sampled");
    const auto sampled_result_path =
        directory / "ours_temporal_cost_sampled_result.dat";
    TDTree sampled_tree(
        data,
        query,
        sampled_decomposition,
        3);
    const MatchSummary sampled_summary =
        sampled_tree.save_res(
            sampled_result_path.string(),
            MatchOutputMode::CountOnly);
    require(
        sampled_summary.output_written &&
            sampled_summary.match_count ==
                summary.match_count,
        "sampled temporal-cost planning preserves exact match semantics");

    const QueryDecomposition kci_decomposition = decomposeQuery(
        query,
        counts,
        lifespans,
        SelectivityMode::Paper);
    TemporalCostPlannerOptions v2_budget_options = options;
    v2_budget_options.max_state_expansions = 0;
    TemporalPlanDiagnostics v2_budget_diagnostics;
    TemporalCostPlanResult v2_budget_result = planTemporalCostV2(
        data,
        query,
        3,
        counts,
        lifespans,
        v2_budget_options,
        &v2_budget_diagnostics);
    require(
        v2_budget_result.selected_tree != nullptr &&
            v2_budget_result.decomposition.selectivity_mode ==
                SelectivityMode::TemporalCostV2 &&
            v2_budget_result.decomposition.root ==
                kci_decomposition.root &&
            v2_budget_result.decomposition.dfs_order ==
                kci_decomposition.dfs_order &&
            v2_budget_result.decomposition.parent ==
                kci_decomposition.parent &&
            v2_budget_diagnostics.v2_budget_exhausted &&
            v2_budget_diagnostics.v2_fallback_reason ==
                "state-expansion-budget" &&
            v2_budget_diagnostics.state_expansions == 0 &&
            v2_budget_diagnostics.final_planner_spent_cost > 0.0 &&
            !v2_budget_diagnostics.final_cost_comparison_available,
        "v2 zero state budget returns the byte-identical complete KCI plan/tree");
    const auto v2_result_path =
        directory / "ours_temporal_cost_v2_result.dat";
    const MatchSummary v2_summary =
        v2_budget_result.selected_tree->save_res(
            v2_result_path.string(),
            MatchOutputMode::CountOnly);
    require(
        v2_summary.output_written &&
            v2_summary.match_count == summary.match_count,
        "v2 KCI fallback preserves the exact durable match count");

    TemporalCostPlannerOptions interval_options = options;
    interval_options.cost_gate_enabled = false;
    interval_options.opportunity_margin_fraction = 0.0;
    interval_options.catalog_probe_cost_weight = 0.0;
    interval_options.max_interval_intersections = 0;
    TemporalPlanDiagnostics zero_interval_diagnostics;
    TemporalCostPlanResult zero_interval_result = planTemporalCostV2(
        data,
        query,
        3,
        counts,
        lifespans,
        interval_options,
        &zero_interval_diagnostics);
    require(
        zero_interval_result.selected_tree != nullptr &&
            zero_interval_diagnostics.v2_budget_exhausted &&
            zero_interval_diagnostics.v2_fallback_reason ==
                "interval-intersection-budget" &&
            zero_interval_diagnostics.interval_intersections == 0 &&
            zero_interval_diagnostics.temporal_edge_applications > 0,
        "v2 zero interval-intersection budget returns the complete KCI tree");

    TemporalCostPlannerOptions measured_interval_options = interval_options;
    measured_interval_options.max_interval_intersections = 1000000;
    TemporalPlanDiagnostics measured_interval_diagnostics;
    TemporalCostPlanResult measured_interval_result = planTemporalCostV2(
        data,
        query,
        3,
        counts,
        lifespans,
        measured_interval_options,
        &measured_interval_diagnostics);
    require(
        measured_interval_result.selected_tree != nullptr &&
            measured_interval_diagnostics.interval_intersections > 0 &&
            !measured_interval_diagnostics.v2_budget_exhausted,
        "v2 fixture measures a positive deterministic interval budget");
    TemporalCostPlannerOptions exact_interval_options = interval_options;
    exact_interval_options.max_interval_intersections =
        measured_interval_diagnostics.interval_intersections;
    TemporalPlanDiagnostics exact_interval_diagnostics;
    TemporalCostPlanResult exact_interval_result = planTemporalCostV2(
        data,
        query,
        3,
        counts,
        lifespans,
        exact_interval_options,
        &exact_interval_diagnostics);
    require(
        exact_interval_result.selected_tree != nullptr &&
            !exact_interval_diagnostics.v2_budget_exhausted &&
            exact_interval_diagnostics.interval_intersections ==
                measured_interval_diagnostics.interval_intersections &&
            exact_interval_diagnostics.temporal_edge_applications ==
                measured_interval_diagnostics.temporal_edge_applications,
        "v2 exact interval-intersection boundary completes the same work");

    auto check_exact_work_budget = [&data, &query, &counts, &lifespans,
                                    &measured_interval_options,
                                    &measured_interval_diagnostics](
        const std::string& name,
        std::uint64_t measured,
        const std::string& expected_reason,
        const std::function<void(
            TemporalCostPlannerOptions&, std::uint64_t)>& set_cap) {
        require(measured > 0, name + ": fixture consumes positive work");

        TemporalCostPlannerOptions exact_options =
            measured_interval_options;
        set_cap(exact_options, measured);
        TemporalPlanDiagnostics exact_diagnostics;
        TemporalCostPlanResult exact_result = planTemporalCostV2(
            data, query, 3, counts, lifespans,
            exact_options, &exact_diagnostics);
        require(
            exact_result.selected_tree != nullptr &&
                !exact_diagnostics.v2_budget_exhausted &&
                exact_diagnostics.v2_decision ==
                    measured_interval_diagnostics.v2_decision &&
                exact_diagnostics.selected_plan_fingerprint ==
                    measured_interval_diagnostics
                        .selected_plan_fingerprint,
            name + ": exact measured cap completes deterministically");

        TemporalCostPlannerOptions below_options =
            measured_interval_options;
        set_cap(below_options, measured - 1);
        TemporalPlanDiagnostics below_diagnostics;
        TemporalCostPlanResult below_result = planTemporalCostV2(
            data, query, 3, counts, lifespans,
            below_options, &below_diagnostics);
        require(
            below_result.selected_tree != nullptr &&
                below_diagnostics.v2_budget_exhausted &&
                below_diagnostics.v2_kci_incumbent_selected &&
                below_diagnostics.v2_fallback_reason == expected_reason,
            name +
                ": one-below measured cap returns the complete KCI fallback");
    };
    check_exact_work_budget(
        "catalog budget boundary",
        measured_interval_diagnostics.catalog_probes,
        "catalog-probe-budget",
        [](TemporalCostPlannerOptions& value, std::uint64_t cap) {
            value.max_catalog_probes = cap;
        });
    check_exact_work_budget(
        "state budget boundary",
        measured_interval_diagnostics.state_expansions,
        "state-expansion-budget",
        [](TemporalCostPlannerOptions& value, std::uint64_t cap) {
            value.max_state_expansions = cap;
        });
    check_exact_work_budget(
        "extension budget boundary",
        measured_interval_diagnostics.extension_probes,
        "extension-probe-budget",
        [](TemporalCostPlannerOptions& value, std::uint64_t cap) {
            value.max_extension_probes = cap;
        });

    Graph empty_sample_data;
    empty_sample_data.num_vertices = 3;
    empty_sample_data.external_ids = {70, 80, 90};
    empty_sample_data.vertex_labels = {
        labelFromString("A"), labelFromString("B"),
        labelFromString("C")};
    empty_sample_data.adj.resize(3);
    empty_sample_data.in_adj.resize(3);
    addTemporalEdge(empty_sample_data, 0, 1, {{1, 3}});
    addTemporalEdge(empty_sample_data, 1, 2, {{10, 12}});
    finalizeSyntheticGraph(empty_sample_data);
    std::array<std::size_t, kLabelCount> empty_sample_counts{};
    std::array<double, kLabelCount> empty_sample_lifespans{};
    for (Label label : empty_sample_data.vertex_labels) {
        ++empty_sample_counts[label];
    }
    empty_sample_lifespans.fill(6.0);
    const QueryDecomposition empty_sample_kci = decomposeQuery(
        query, empty_sample_counts, empty_sample_lifespans,
        SelectivityMode::Paper);
    TemporalCostPlannerOptions empty_sample_options = options;
    empty_sample_options.cost_gate_enabled = false;
    empty_sample_options.catalog_probe_cost_weight = 0.0;
    TemporalPlanDiagnostics empty_sample_diagnostics;
    TemporalCostPlanResult empty_sample_result = planTemporalCostV2(
        empty_sample_data, query, 3,
        empty_sample_counts, empty_sample_lifespans,
        empty_sample_options, &empty_sample_diagnostics);
    require(
        empty_sample_result.selected_tree != nullptr &&
            !empty_sample_result.selected_tree->rootCandidates().empty() &&
            empty_sample_result.decomposition.root ==
                empty_sample_kci.root &&
            empty_sample_result.decomposition.dfs_order ==
                empty_sample_kci.dfs_order &&
            empty_sample_result.decomposition.parent ==
                empty_sample_kci.parent &&
            empty_sample_diagnostics.v2_fallback_phase == "admission" &&
            empty_sample_diagnostics.v2_fallback_reason ==
                "sample-empty" &&
            empty_sample_diagnostics.v2_kci_incumbent_selected &&
            empty_sample_diagnostics
                    .incumbent_root_plus_raw_candidates > 0.0,
        "v2 intermediate sample-empty path returns the complete KCI plan/tree");
    const auto empty_sample_path =
        directory / "ours_temporal_cost_v2_empty_sample.dat";
    const MatchSummary empty_sample_summary =
        empty_sample_result.selected_tree->save_res(
            empty_sample_path.string(), MatchOutputMode::CountOnly);
    require(
        empty_sample_summary.output_written &&
            empty_sample_summary.match_count == 0 &&
            bruteForceCount(empty_sample_data, query, 3) == 0,
        "v2 sample-empty fallback preserves the exact zero answer");
    std::filesystem::remove(empty_sample_path);

    TemporalPlanDiagnostics repeated_v2_diagnostics;
    TemporalCostPlanResult repeated_v2 = planTemporalCostV2(
        data,
        query,
        3,
        counts,
        lifespans,
        v2_budget_options,
        &repeated_v2_diagnostics);
    require(
        repeated_v2.decomposition.root ==
                v2_budget_result.decomposition.root &&
            repeated_v2.decomposition.dfs_order ==
                v2_budget_result.decomposition.dfs_order &&
            repeated_v2.decomposition.parent ==
                v2_budget_result.decomposition.parent &&
            repeated_v2_diagnostics.v2_decision ==
                v2_budget_diagnostics.v2_decision &&
            repeated_v2_diagnostics.v2_decision_reason ==
                v2_budget_diagnostics.v2_decision_reason &&
            repeated_v2_diagnostics.pilot_extension_probes ==
                v2_budget_diagnostics.pilot_extension_probes &&
            repeated_v2_diagnostics.state_expansions ==
                v2_budget_diagnostics.state_expansions,
        "v2 decision, plan, and deterministic work trace repeat exactly");

    Graph zero_query = makeTwoVertexQuery(false);
    zero_query.vertex_labels = {
        labelFromString("D"), labelFromString("E")};
    const QueryDecomposition zero_kci = decomposeQuery(
        zero_query,
        counts,
        lifespans,
        SelectivityMode::Paper);
    TemporalPlanDiagnostics zero_diagnostics;
    TemporalCostPlanResult zero_result = planTemporalCostV2(
        data,
        zero_query,
        3,
        counts,
        lifespans,
        options,
        &zero_diagnostics);
    require(
        zero_result.selected_tree != nullptr &&
            zero_result.selected_tree->rootCandidates().empty() &&
            zero_result.decomposition.root == zero_kci.root &&
            zero_result.decomposition.dfs_order == zero_kci.dfs_order &&
            zero_result.decomposition.parent == zero_kci.parent &&
            zero_diagnostics.v2_fallback_reason == "zero-domain" &&
            zero_diagnostics.catalog_probes == 0,
        "v2 zero-domain admission returns the complete KCI fallback without a catalogue");

    Graph disconnected_query = query;
    disconnected_query.num_vertices = 4;
    disconnected_query.external_ids.push_back(3);
    disconnected_query.vertex_labels.push_back(
        labelFromString("A"));
    disconnected_query.adj.resize(4);
    disconnected_query.in_adj.resize(4);
    TemporalPlanDiagnostics disconnected_diagnostics =
        diagnostics;
    const QueryDecomposition disconnected =
        planTemporalCostQuery(
            data,
            disconnected_query,
            3,
            counts,
            lifespans,
            options,
            &disconnected_diagnostics);
    require(
        !disconnected.connected &&
            disconnected_diagnostics.root_plans.empty() &&
            disconnected_diagnostics.beam_width == 0,
        "temporal-cost planner returns a disconnected status "
        "and clears reusable diagnostics instead of throwing");
    std::filesystem::remove(result_path);
    std::filesystem::remove(sampled_result_path);
    std::filesystem::remove(v2_result_path);
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
        serial_path.string(), MatchOutputMode::CountOnly, 1, true);
    const MatchSummary parallel = tree.save_res(
        parallel_path.string(), MatchOutputMode::CountOnly, 32, true);
    require(
        serial.match_count ==
                static_cast<std::uint64_t>(root_count) &&
            parallel.match_count == serial.match_count &&
            !parallel.count_overflow &&
            serial.result_digest_collected &&
            parallel.result_digest_collected &&
            parallel.mapping_digest_xor ==
                serial.mapping_digest_xor &&
            parallel.mapping_digest_sum ==
                serial.mapping_digest_sum,
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

    const std::uint64_t content_fingerprint =
        tree.candidateContentFingerprint();
    TDTree identical_tree(data, query, paper, 3);
    TDTree stricter_tree(data, query, paper, 4);
    require(
        content_fingerprint != 0 &&
            identical_tree.candidateContentFingerprint() ==
                content_fingerprint &&
            stricter_tree.candidateContentFingerprint() !=
                content_fingerprint,
        "candidate content fingerprint is deterministic and binds k");

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
        std::array<std::size_t, kLabelCount> counts{};
        std::array<double, kLabelCount> lifespans{};
        counts[labelFromString("A")] = 2;
        lifespans[labelFromString("A")] = 6.0;
        TemporalPlanDiagnostics v2_diagnostics;
        TemporalCostPlanResult v2_result = planTemporalCostV2(
            data, query, 3, counts, lifespans, {}, &v2_diagnostics);
        const auto v2_path =
            directory / "ours_root_self_loop_v2_matching.dat";
        require(
            v2_result.selected_tree != nullptr &&
                v2_result.selected_tree->save_res(
                    v2_path.string(), MatchOutputMode::CountOnly)
                        .match_count == 1,
            "v2 preserves exact root self-loop semantics");
        requireLegalCompletePlan(
            query, v2_result.decomposition, "v2 self-loop");
        std::filesystem::remove(result_path);
        std::filesystem::remove(v2_path);
    }

    {
        Graph query = makeTwoVertexQuery(false);
        query.adj[1].push_back({1, -1});
        query.in_adj[1].push_back({1, -1});
        std::array<std::size_t, kLabelCount> counts{};
        std::array<double, kLabelCount> lifespans{};
        counts[labelFromString("A")] = 1;
        counts[labelFromString("B")] = 4;
        lifespans[labelFromString("A")] = 5.0;
        lifespans[labelFromString("B")] = 5.0;
        const QueryDecomposition decomposition = decomposeQuery(
            query, counts, lifespans, SelectivityMode::Paper);
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
        counts[labelFromString("A")] = 10;
        counts[labelFromString("B")] = 1;
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
        TemporalPlanDiagnostics v2_diagnostics;
        TemporalCostPlannerOptions v2_options;
        v2_options.max_state_expansions = 0;
        TemporalCostPlanResult v2_result = planTemporalCostV2(
            data, query, 3, counts, lifespans,
            v2_options, &v2_diagnostics);
        const auto v2_result_path =
            directory / "ours_directed_reciprocal_v2.dat";
        require(
            v2_result.selected_tree != nullptr &&
                v2_result.selected_tree->save_res(
                    v2_result_path.string(), MatchOutputMode::CountOnly)
                        .match_count == 1,
            "v2 KCI fallback preserves reciprocal-arc exactness");
        requireLegalCompletePlan(
            query, v2_result.decomposition, "v2 reciprocal query");
        std::ifstream result(result_path, std::ios::binary);
        const std::string contents(
            (std::istreambuf_iterator<char>(result)), std::istreambuf_iterator<char>());
        require(contents.find("active=[2-4]") != std::string::npos,
                "reciprocal arc histories are intersected, not merged");
        result.close();
        std::filesystem::remove(result_path);
        std::filesystem::remove(v2_result_path);
    }
}

void addOracleQueryEdge(Graph& query, int source, int target) {
    query.adj[static_cast<std::size_t>(source)].push_back({target, -1});
    query.in_adj[static_cast<std::size_t>(target)].push_back({source, -1});
}

void normalizeOracleQuery(Graph& query) {
    auto normalize = [](std::vector<Edge>& edges) {
        std::sort(
            edges.begin(), edges.end(),
            [](const Edge& lhs, const Edge& rhs) {
                return lhs.to < rhs.to;
            });
        edges.erase(
            std::unique(
                edges.begin(), edges.end(),
                [](const Edge& lhs, const Edge& rhs) {
                    return lhs.to == rhs.to;
                }),
            edges.end());
    };
    for (auto& edges : query.adj) normalize(edges);
    for (auto& edges : query.in_adj) normalize(edges);
}

void oracleLabelStatistics(
    const Graph& data,
    std::array<std::size_t, kLabelCount>& counts,
    std::array<double, kLabelCount>& lifespans) {
    std::array<double, kLabelCount> duration_sums{};
    for (int vertex = 0; vertex < data.num_vertices; ++vertex) {
        const std::size_t index = static_cast<std::size_t>(vertex);
        const Label label = data.vertex_labels[index];
        ++counts[label];
        duration_sums[label] +=
            static_cast<double>(data.vertex_active_durations[index]);
    }
    for (std::size_t label = 0; label < kLabelCount; ++label) {
        if (counts[label] > 0) {
            lifespans[label] =
                duration_sums[label] / static_cast<double>(counts[label]);
        }
    }
}

void verifyExhaustivePlannerRegretFixture(
    const Graph& data,
    const Graph& query,
    int minimum_duration,
    const std::string& fixture_name,
    const std::filesystem::path& directory,
    bool repeat_full_oracle) {
    const std::uint64_t expected =
        bruteForceCount(data, query, minimum_duration);
    const auto scratch_path =
        directory / ("ours_plan_oracle_" + fixture_name + ".dat");
    const ExhaustivePlanOracleResult oracle = measureAllLegalPlans(
        data, query, minimum_duration, expected, scratch_path);
    if (repeat_full_oracle) {
        const auto most_work = std::max_element(
            oracle.plans.begin(), oracle.plans.end(),
            [](const ExhaustivePlanMeasurement& lhs,
               const ExhaustivePlanMeasurement& rhs) {
                return lhs.root_plus_raw_extensions <
                    rhs.root_plus_raw_extensions;
            });
        require(
            most_work != oracle.plans.end() &&
                most_work->root_plus_raw_extensions > oracle.optimum_work,
            fixture_name +
                ": exhaustive oracle distinguishes plan-dependent work");
    }

    // Enumeration order is itself deterministic and independent of planner
    // tie-breaking.  One fixture also repeats all tree builds/enumerations so
    // that the measured logical-work vector, not only plan generation, is
    // checked for repeatability.
    const std::vector<QueryDecomposition> repeated_plans =
        independentlyEnumerateAllLegalPlans(query);
    require(
        repeated_plans.size() == oracle.plans.size(),
        fixture_name + ": deterministic exhaustive plan count");
    for (std::size_t index = 0; index < repeated_plans.size(); ++index) {
        require(
            oraclePlanIdentity(repeated_plans[index]) ==
                oraclePlanIdentity(oracle.plans[index].decomposition),
            fixture_name + ": deterministic exhaustive plan order");
    }
    if (repeat_full_oracle) {
        const ExhaustivePlanOracleResult repeated = measureAllLegalPlans(
            data,
            query,
            minimum_duration,
            expected,
            directory /
                ("ours_plan_oracle_repeat_" + fixture_name + ".dat"));
        require(
            repeated.optimum_work == oracle.optimum_work &&
                repeated.plans.size() == oracle.plans.size(),
            fixture_name + ": deterministic exhaustive optimum");
        for (std::size_t index = 0; index < oracle.plans.size(); ++index) {
            require(
                oraclePlanIdentity(repeated.plans[index].decomposition) ==
                        oraclePlanIdentity(
                            oracle.plans[index].decomposition) &&
                    repeated.plans[index].answer_count ==
                        oracle.plans[index].answer_count &&
                    repeated.plans[index].root_plus_raw_extensions ==
                        oracle.plans[index].root_plus_raw_extensions,
                fixture_name +
                    ": deterministic exhaustive answer/work trace");
        }
    }

    std::array<std::size_t, kLabelCount> counts{};
    std::array<double, kLabelCount> lifespans{};
    oracleLabelStatistics(data, counts, lifespans);
    const QueryDecomposition kci = decomposeQuery(
        query,
        counts,
        lifespans,
        SelectivityMode::Paper);
    requireLegalCompletePlan(query, kci, fixture_name + ": KCI");
    const ExhaustivePlanMeasurement& kci_measurement = *
        findOraclePlan(oracle, kci, fixture_name + ": KCI");

    TemporalCostPlannerOptions options;
    options.beam_width = 64;
    options.sample_cap = 128;
    options.pilot_prefix_cap = 128;
    options.pilot_extension_probe_cap = 1000000;
    options.rescore_top_l = 8;
    options.max_catalog_probes = 10000000;
    options.max_state_expansions = 1000000;
    options.max_extension_probes = 10000000;
    options.max_interval_intersections = 10000000;
    options.max_candidate_trees_built = 8;
    options.opportunity_margin_fraction = 0.0;
    options.cost_gate_enabled = false;
    TemporalPlanDiagnostics diagnostics;
    TemporalCostPlanResult tcc = planTemporalCostV2(
        data,
        query,
        minimum_duration,
        counts,
        lifespans,
        options,
        &diagnostics);
    require(
        tcc.selected_tree != nullptr,
        fixture_name + ": TCC-v2 returns a complete tree");
    requireLegalCompletePlan(
        query, tcc.decomposition, fixture_name + ": TCC-v2");
    const ExhaustivePlanMeasurement& tcc_measurement = *
        findOraclePlan(oracle, tcc.decomposition, fixture_name + ": TCC-v2");

    const auto tcc_path =
        directory / ("ours_plan_oracle_tcc_" + fixture_name + ".dat");
    const MatchSummary handed_off = tcc.selected_tree->save_res(
        tcc_path.string(), MatchOutputMode::CountOnly, 1);
    std::filesystem::remove(tcc_path);
    require(
        handed_off.output_written && !handed_off.count_overflow &&
            handed_off.match_count == expected &&
            static_cast<std::uint64_t>(handed_off.root_candidate_count) +
                    handed_off.generated_candidates ==
                tcc_measurement.root_plus_raw_extensions,
        fixture_name +
            ": handed-off TCC tree has oracle answer and logical work");

    require(
        kci_measurement.root_plus_raw_extensions >= oracle.optimum_work &&
            tcc_measurement.root_plus_raw_extensions >= oracle.optimum_work,
        fixture_name + ": selected-plan regret is nonnegative");
    const std::uint64_t kci_additive_regret =
        kci_measurement.root_plus_raw_extensions - oracle.optimum_work;
    const std::uint64_t tcc_additive_regret =
        tcc_measurement.root_plus_raw_extensions - oracle.optimum_work;
    const double kci_ratio = oracleRegretRatio(
        kci_measurement.root_plus_raw_extensions, oracle.optimum_work);
    const double tcc_ratio = oracleRegretRatio(
        tcc_measurement.root_plus_raw_extensions, oracle.optimum_work);
    require(
        kci_ratio >= 1.0 && tcc_ratio >= 1.0,
        fixture_name + ": multiplicative regret is at least one");

    // This is test telemetry, not a performance result: all quantities are
    // deterministic logical counters from exhaustive 1T count-only runs.
    std::cout << "planner_regret_oracle fixture=" << fixture_name
              << " legal_plans=" << oracle.plans.size()
              << " answers=" << expected
              << " optimum_work=" << oracle.optimum_work
              << " kci_work="
              << kci_measurement.root_plus_raw_extensions
              << " kci_additive_regret=" << kci_additive_regret
              << " kci_ratio=" << kci_ratio
              << " tcc_work="
              << tcc_measurement.root_plus_raw_extensions
              << " tcc_additive_regret=" << tcc_additive_regret
              << " tcc_ratio=" << tcc_ratio << '\n';
}

void testExhaustiveSmallPlanRegretOracle(
    const std::filesystem::path& directory) {
    constexpr int minimum_duration = 2;
    Graph query;
    query.num_vertices = 5;
    query.external_ids = {0, 1, 2, 3, 4};
    query.vertex_labels = {
        labelFromString("A"), labelFromString("B"),
        labelFromString("C"), labelFromString("A"),
        labelFromString("B")};
    query.adj.resize(5);
    query.in_adj.resize(5);
    addOracleQueryEdge(query, 0, 1);
    addOracleQueryEdge(query, 1, 0); // reciprocal tree-pair possibility
    addOracleQueryEdge(query, 1, 2);
    addOracleQueryEdge(query, 2, 3);
    addOracleQueryEdge(query, 3, 4);
    addOracleQueryEdge(query, 0, 3); // directed non-tree chord
    addOracleQueryEdge(query, 4, 1); // second non-tree chord
    addOracleQueryEdge(query, 2, 2); // non-root or root self-loop
    normalizeOracleQuery(query);

    Graph data;
    data.num_vertices = 7;
    data.external_ids = {10, 20, 30, 40, 50, 60, 70};
    data.vertex_labels = {
        labelFromString("A"), labelFromString("B"),
        labelFromString("C"), labelFromString("A"),
        labelFromString("B"), labelFromString("A"),
        labelFromString("B")};
    data.adj.resize(7);
    data.in_adj.resize(7);
    addTemporalEdge(data, 0, 1, {{1, 5}});
    addTemporalEdge(data, 1, 0, {{1, 4}});
    addTemporalEdge(data, 1, 2, {{2, 5}});
    addTemporalEdge(data, 2, 3, {{1, 5}});
    addTemporalEdge(data, 3, 4, {{1, 4}});
    addTemporalEdge(data, 0, 3, {{2, 5}});
    addTemporalEdge(data, 4, 1, {{1, 5}});
    addTemporalEdge(data, 2, 2, {{1, 5}});
    // Durable but incomplete decoy structure creates plan-dependent work.
    addTemporalEdge(data, 5, 6, {{1, 5}});
    addTemporalEdge(data, 6, 5, {{1, 5}});
    addTemporalEdge(data, 6, 2, {{1, 3}});
    addTemporalEdge(data, 5, 3, {{4, 5}});
    addTemporalEdge(data, 4, 6, {{4, 5}});
    finalizeSyntheticGraph(data);
    verifyExhaustivePlannerRegretFixture(
        data,
        query,
        minimum_duration,
        "directed_reciprocal_self_non_tree_q5",
        directory,
        true);

    // A small seeded random set broadens topology and temporal-history
    // coverage without turning this correctness oracle into a benchmark.
    std::uint32_t state = 0x4f1bbcdcU;
    auto next_random = [&]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    for (int round = 0; round < 3; ++round) {
        Graph random_query;
        random_query.num_vertices = 4;
        random_query.external_ids = {0, 1, 2, 3};
        random_query.vertex_labels = {
            labelFromString("A"), labelFromString("B"),
            labelFromString("C"), labelFromString("A")};
        random_query.adj.resize(4);
        random_query.in_adj.resize(4);
        for (int vertex = 0; vertex < 3; ++vertex) {
            if ((next_random() & 1U) == 0) {
                addOracleQueryEdge(random_query, vertex, vertex + 1);
            } else {
                addOracleQueryEdge(random_query, vertex + 1, vertex);
            }
        }
        for (int source = 0; source < 4; ++source) {
            for (int target = 0; target < 4; ++target) {
                if (source == target || source + 1 == target ||
                    target + 1 == source) {
                    continue;
                }
                if ((next_random() & 3U) == 0) {
                    addOracleQueryEdge(random_query, source, target);
                }
            }
        }
        normalizeOracleQuery(random_query);

        Graph random_data;
        random_data.num_vertices = 6;
        random_data.external_ids = {101, 205, 309, 413, 517, 621};
        random_data.vertex_labels = {
            labelFromString("A"), labelFromString("B"),
            labelFromString("C"), labelFromString("A"),
            labelFromString("B"), labelFromString("C")};
        random_data.adj.resize(6);
        random_data.in_adj.resize(6);
        for (int source = 0; source < 6; ++source) {
            for (int target = 0; target < 6; ++target) {
                const std::uint32_t mask = next_random() & 0x3fU;
                if (mask == 0 || (next_random() & 3U) == 0) continue;
                addTemporalEdge(
                    random_data,
                    source,
                    target,
                    intervalsFromMask(mask));
            }
        }
        finalizeSyntheticGraph(random_data);
        verifyExhaustivePlannerRegretFixture(
            random_data,
            random_query,
            minimum_duration,
            "seeded_q4_" + std::to_string(round),
            directory,
            false);
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
    bool saw_v2_catalogue_path = false;
    bool saw_v2_candidate_tree = false;
    bool checked_candidate_tree_boundary = false;
    bool saw_bounded_top_w = false;
    bool saw_reused_incumbent_score = false;
    bool saw_gate_off_bypass = false;
    std::uint32_t state = 0xa511e9b3U;
    auto next_random = [&]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };

    // A deterministic positive fixture guarantees that Phase B, bounded
    // global top-W, a non-incumbent candidate-tree build, and rescore-budget
    // fallback remain covered even if every seeded random query happens to
    // lose its fixed-plan samples before admission.
    {
        Graph graph = makeMatchingDataGraph();
        Graph query = makeTriangleQuery();
        std::array<std::size_t, kLabelCount> counts{};
        std::array<double, kLabelCount> lifespans{};
        counts[0] = counts[1] = counts[2] = 2;
        // Break KCI ties deterministically so catalogue top-1 can differ from
        // the incumbent while the data/query semantics remain unchanged.
        lifespans[0] = 1.0;
        lifespans[1] = 2.0;
        lifespans[2] = 4.0;
        TemporalCostPlannerOptions options;
        options.beam_width = 16;
        options.sample_cap = 64;
        options.pilot_prefix_cap = 64;
        options.pilot_extension_probe_cap = 1000000;
        options.rescore_top_l = 1;
        options.max_catalog_probes = 1000000;
        options.max_state_expansions = 10000;
        options.max_extension_probes = 1000000;
        options.max_interval_intersections = 1000000;
        options.max_candidate_trees_built = 1;
        options.catalog_probe_cost_weight = 0.0;
        options.opportunity_margin_fraction = 0.0;
        options.cost_gate_enabled = false;
        TemporalPlanDiagnostics diagnostics;
        TemporalCostPlanResult result = planTemporalCostV2(
            graph, query, minimum_duration, counts, lifespans,
            options, &diagnostics);
        require(
            result.selected_tree != nullptr &&
                diagnostics.v2_cost_gate_bypassed &&
                diagnostics.catalogue_complete_plans >
                    options.rescore_top_l &&
                diagnostics.candidate_plans_rescored <=
                    options.rescore_top_l &&
                diagnostics.candidate_trees_built > 0,
            "deterministic positive v2 fixture activates Phase B, bounded "
            "top-W, and a candidate tree");
#ifdef DSM_TCC_TEST_REFERENCE_FULL_SORT
        TemporalCostPlannerOptions reference_options = options;
        reference_options.test_reference_full_sort = true;
        TemporalPlanDiagnostics reference_diagnostics;
        TemporalCostPlanResult reference_result = planTemporalCostV2(
            graph, query, minimum_duration, counts, lifespans,
            reference_options, &reference_diagnostics);
        require(
            reference_result.selected_tree != nullptr &&
                diagnostics.catalogue_complete_plans >
                    options.rescore_top_l &&
                result.decomposition.root ==
                    reference_result.decomposition.root &&
                result.decomposition.dfs_order ==
                    reference_result.decomposition.dfs_order &&
                result.decomposition.parent ==
                    reference_result.decomposition.parent &&
                diagnostics.selected_plan_fingerprint ==
                    reference_diagnostics.selected_plan_fingerprint &&
                diagnostics.v2_decision ==
                    reference_diagnostics.v2_decision,
            "M3 bounded/metadata-only and full-sort/deep-copy reference "
            "select one plan");
        const auto bounded_path =
            directory / "ours_m3_bounded_handoff.dat";
        const auto reference_path =
            directory / "ours_m3_reference_handoff.dat";
        const auto rebuilt_path =
            directory / "ours_m3_bounded_rebuilt.dat";
        const MatchSummary bounded_summary = result.selected_tree->save_res(
            bounded_path.string(), MatchOutputMode::CountOnly);
        const MatchSummary reference_summary =
            reference_result.selected_tree->save_res(
                reference_path.string(), MatchOutputMode::CountOnly);
        TDTree rebuilt_tree(
            graph, query, result.decomposition, minimum_duration);
        const MatchSummary rebuilt_summary = rebuilt_tree.save_res(
            rebuilt_path.string(), MatchOutputMode::CountOnly);
        auto file_bytes = [](const std::filesystem::path& path) {
            std::ifstream input(path, std::ios::binary);
            return std::string(
                (std::istreambuf_iterator<char>(input)),
                std::istreambuf_iterator<char>());
        };
        require(
            bounded_summary.output_written &&
                reference_summary.output_written &&
                rebuilt_summary.output_written &&
                bounded_summary.match_count ==
                    reference_summary.match_count &&
                bounded_summary.match_count == rebuilt_summary.match_count &&
                file_bytes(bounded_path) == file_bytes(reference_path) &&
                file_bytes(bounded_path) == file_bytes(rebuilt_path),
            "M3 old/new selection and tree handoff are byte-identical");
        std::filesystem::remove(bounded_path);
        std::filesystem::remove(reference_path);
        std::filesystem::remove(rebuilt_path);
#endif
        saw_v2_catalogue_path = true;
        saw_v2_candidate_tree = true;
        saw_bounded_top_w = true;
        saw_gate_off_bypass = true;

        TemporalCostPlannerOptions rescore_budget_options = options;
        rescore_budget_options.max_extension_probes =
            diagnostics.incumbent_extension_probes +
            diagnostics.beam_extension_probes;
        TemporalPlanDiagnostics rescore_budget_diagnostics;
        TemporalCostPlanResult rescore_budget_result = planTemporalCostV2(
            graph, query, minimum_duration, counts, lifespans,
            rescore_budget_options, &rescore_budget_diagnostics);
        require(
            rescore_budget_result.selected_tree != nullptr &&
                rescore_budget_diagnostics.v2_budget_exhausted &&
                rescore_budget_diagnostics.v2_fallback_phase == "rescore" &&
                rescore_budget_diagnostics.v2_fallback_reason ==
                    "extension-probe-budget" &&
                rescore_budget_diagnostics
                        .candidate_tree_relation_entries_built > 0 &&
                rescore_budget_diagnostics
                        .rejected_candidate_tree_relation_entries ==
                    rescore_budget_diagnostics
                        .candidate_tree_relation_entries_built &&
                rescore_budget_diagnostics.final_rejected_tree_cost ==
                    static_cast<double>(
                        rescore_budget_diagnostics
                            .candidate_tree_relation_entries_built),
            "deterministic positive rescore-budget fallback charges every "
            "discarded candidate tree");
        checked_candidate_tree_boundary = true;

        const Graph reuse_graph = makeParallelRootDataGraph(2);
        const Graph reuse_query = makeTwoVertexQuery(false);
        std::array<std::size_t, kLabelCount> reuse_counts{};
        std::array<double, kLabelCount> reuse_lifespans{};
        reuse_counts[labelFromString("A")] = 2;
        reuse_counts[labelFromString("B")] = 2;
        reuse_lifespans[labelFromString("A")] = 3.0;
        reuse_lifespans[labelFromString("B")] = 3.0;
        TemporalCostPlannerOptions reuse_options = options;
        reuse_options.rescore_top_l = 8;
        reuse_options.max_candidate_trees_built = 8;
        TemporalPlanDiagnostics reuse_diagnostics;
        TemporalCostPlanResult reuse_result = planTemporalCostV2(
            reuse_graph, reuse_query, minimum_duration,
            reuse_counts, reuse_lifespans,
            reuse_options, &reuse_diagnostics);
        const auto reused_score = std::find_if(
            reuse_diagnostics.fixed_plan_scores.begin(),
            reuse_diagnostics.fixed_plan_scores.end(),
            [](const TemporalFixedPlanEstimate& score) {
                return score.reused_incumbent_score;
            });
        require(
            reuse_result.selected_tree != nullptr &&
                reused_score != reuse_diagnostics.fixed_plan_scores.end() &&
                !reuse_diagnostics.fixed_plan_scores.empty() &&
                reused_score->plan_fingerprint ==
                    reuse_diagnostics.fixed_plan_scores.front()
                        .plan_fingerprint &&
                reused_score->extension_probes == 0 &&
                std::abs(
                    reused_score->estimated_total_candidates -
                    reuse_diagnostics.fixed_plan_scores.front()
                        .estimated_total_candidates) <= 1e-9,
            "deterministic positive fixture reuses the identical incumbent "
            "capped score");
        saw_reused_incumbent_score = true;
    }

    for (int round = 0; round < 100; ++round) {
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

        TemporalCostPlannerOptions v2_options;
        v2_options.beam_width = 16;
        v2_options.sample_cap = 32;
        v2_options.pilot_prefix_cap = 16;
        v2_options.pilot_extension_probe_cap = 64;
        // W=1 forces the catalogue's bounded top-W path whenever more than
        // one root plan completes; candidate-tree budgets are audited against
        // the actual number selected below rather than an assumed W.
        v2_options.rescore_top_l = 1;
        v2_options.max_catalog_probes = 1000000;
        v2_options.max_state_expansions = 10000;
        v2_options.max_extension_probes = 1000000;
        v2_options.max_interval_intersections = 1000000;
        v2_options.max_candidate_trees_built = 4;
        v2_options.catalog_probe_cost_weight =
            (round % 4 == 3) ? 0.0 : 1.0;
        v2_options.opportunity_margin_fraction = 0.0;
        v2_options.cost_gate_enabled = round % 4 != 3;
        if (round % 4 == 0) {
            v2_options.max_state_expansions = 0;
        } else if (round % 4 == 1) {
            v2_options.max_catalog_probes = 0;
        } else if (round % 4 == 2) {
            v2_options.max_extension_probes = 0;
        }

        TemporalPlanDiagnostics v2_diagnostics;
        TemporalCostPlanResult v2_result = planTemporalCostV2(
            graph,
            query,
            minimum_duration,
            counts,
            lifespans,
            v2_options,
            &v2_diagnostics);
        const std::string context =
            "random v2 oracle round " + std::to_string(round);
        require(
            v2_result.selected_tree != nullptr,
            context + ": complete tree handoff");
        requireLegalCompletePlan(query, v2_result.decomposition, context);
        require(
            v2_diagnostics.fixed_plan_scoring_method ==
                "deterministic-capped-fixed-plan",
            context + ": one fixed-plan scoring method");
        if (round % 4 == 3) {
            require(
                !v2_diagnostics.v2_cost_gate_enabled,
                context +
                    ": gate-off arm is explicit");
            if (v2_diagnostics.v2_cost_gate_bypassed) {
                saw_gate_off_bypass = true;
                require(
                    v2_diagnostics.v2_admission_reason ==
                            "cost-gate-disabled" &&
                        (!v2_diagnostics.v2_candidate_selected ||
                         v2_diagnostics.v2_forced_candidate_selected),
                    context +
                        ": a bypassed gate only marks selected candidates "
                        "as forced");
            }
        }
        for (const TemporalFixedPlanEstimate& score :
             v2_diagnostics.fixed_plan_scores) {
            require(
                score.extension_probe_cap ==
                    v2_options.pilot_extension_probe_cap,
                context + ": symmetric per-plan probe cap");
            require(
                score.extension_probes <= score.extension_probe_cap,
                context + ": per-plan probe use respects cap");
            if (score.reused_incumbent_score) {
                saw_reused_incumbent_score = true;
                require(
                    !v2_diagnostics.fixed_plan_scores.empty() &&
                        score.plan_fingerprint ==
                            v2_diagnostics.fixed_plan_scores.front()
                                .plan_fingerprint &&
                        std::abs(
                            score.estimated_total_candidates -
                            v2_diagnostics.fixed_plan_scores.front()
                                .estimated_total_candidates) <= 1e-9,
                    context +
                        ": identical plan reuses an identical capped score");
            }
        }
        require(
            v2_diagnostics.extension_probes ==
                    v2_diagnostics.beam_extension_probes +
                        v2_diagnostics.fixed_plan_extension_probes &&
                v2_diagnostics.fixed_plan_extension_probes ==
                    v2_diagnostics.incumbent_extension_probes +
                        v2_diagnostics.candidate_extension_probes &&
                v2_diagnostics.extension_probes <=
                    v2_options.max_extension_probes &&
                v2_diagnostics.interval_intersections <=
                    v2_options.max_interval_intersections,
            context +
                ": total probes split exactly into beam and fixed-plan work");
        require(
            std::abs(
                v2_diagnostics.final_planner_spent_cost -
                (v2_diagnostics.final_incumbent_build_cost +
                 v2_diagnostics.final_catalog_cost +
                 v2_diagnostics.final_state_expansion_cost +
                 v2_diagnostics.final_extension_probe_cost +
                 v2_diagnostics.final_interval_intersection_cost +
                 v2_diagnostics.final_rejected_tree_cost)) <= 1e-9 &&
                (!v2_diagnostics.v2_candidate_selected ||
                 v2_diagnostics.final_cost_comparison_available),
            context +
                ": final proxy exposes every charged raw term and "
                "candidate comparison availability");
        if (v2_diagnostics.predicted_kci_controllable_cost > 0.0) {
            require(
                std::abs(
                    v2_diagnostics.predicted_kci_controllable_cost -
                    (static_cast<double>(
                         v2_diagnostics.incumbent_relation_entries) +
                     v2_diagnostics
                         .incumbent_root_plus_raw_candidates)) <= 1e-9,
                context + ": KCI H=1 proxy includes incumbent build");
            require(
                std::abs(
                    v2_diagnostics.predicted_tcc_controllable_cost -
                    (v2_diagnostics.opportunity_incumbent_build_cost +
                     v2_diagnostics.opportunity_pilot_spent_cost +
                     v2_diagnostics.opportunity_catalog_floor_cost +
                     v2_diagnostics
                         .opportunity_optimistic_exec_cost)) <= 1e-9,
                context +
                    ": candidate opportunity proxy includes the same "
                    "incumbent build");
        }
        require(
            v2_diagnostics.selected_plan_fingerprint !=
                    v2_diagnostics.incumbent_plan_fingerprint ||
                !v2_diagnostics.v2_candidate_selected,
            context +
                ": identical candidate cannot win by omitting common work");

        const auto v2_path = directory /
            ("ours_general_v2_oracle_" + std::to_string(round) +
             ".dat");
        const MatchSummary v2_summary =
            v2_result.selected_tree->save_res(
                v2_path.string(), MatchOutputMode::CountOnly);
        require(
            v2_summary.output_written && !v2_summary.count_overflow &&
                v2_summary.match_count == expected,
            context + ": exact brute-force count parity");
        requireTreePlanMetadata(
            v2_path, v2_result.decomposition, context);
        std::filesystem::remove(v2_path);

        saw_v2_catalogue_path = saw_v2_catalogue_path ||
            v2_diagnostics.catalogue_complete_plans > 0;
        saw_bounded_top_w = saw_bounded_top_w ||
            v2_diagnostics.catalogue_complete_plans >
                v2_options.rescore_top_l;
        require(
            v2_diagnostics.candidate_plans_rescored <=
                std::min(
                    v2_diagnostics.catalogue_complete_plans,
                    v2_options.rescore_top_l),
            context +
                ": bounded global top-W limits actual plan rescoring");
        saw_v2_candidate_tree = saw_v2_candidate_tree ||
            v2_diagnostics.candidate_trees_built > 0;

        if (!checked_candidate_tree_boundary &&
            v2_diagnostics.candidate_trees_built > 0 &&
            !v2_diagnostics.v2_budget_exhausted) {
            TemporalCostPlannerOptions zero_tree_options = v2_options;
            zero_tree_options.max_candidate_trees_built = 0;
            TemporalPlanDiagnostics zero_tree_diagnostics;
            TemporalCostPlanResult zero_tree_result = planTemporalCostV2(
                graph, query, minimum_duration, counts, lifespans,
                zero_tree_options, &zero_tree_diagnostics);
            require(
                zero_tree_result.selected_tree != nullptr &&
                    zero_tree_diagnostics.v2_budget_exhausted &&
                    zero_tree_diagnostics.v2_fallback_reason ==
                        "candidate-tree-budget" &&
                    zero_tree_diagnostics.candidate_trees_built == 0,
                context + ": zero candidate-tree budget falls back to KCI");

            TemporalCostPlannerOptions exact_tree_options = v2_options;
            exact_tree_options.max_candidate_trees_built =
                v2_diagnostics.candidate_trees_built;
            TemporalPlanDiagnostics exact_tree_diagnostics;
            TemporalCostPlanResult exact_tree_result = planTemporalCostV2(
                graph, query, minimum_duration, counts, lifespans,
                exact_tree_options, &exact_tree_diagnostics);
            require(
                exact_tree_result.selected_tree != nullptr &&
                    exact_tree_diagnostics.v2_fallback_reason !=
                        "candidate-tree-budget" &&
                    exact_tree_diagnostics.candidate_trees_built ==
                        v2_diagnostics.candidate_trees_built,
                context +
                    ": exact candidate-tree budget completes the same work");

            TemporalCostPlannerOptions rescore_budget_options =
                v2_options;
            rescore_budget_options.max_extension_probes =
                v2_diagnostics.incumbent_extension_probes +
                v2_diagnostics.beam_extension_probes;
            TemporalPlanDiagnostics rescore_budget_diagnostics;
            TemporalCostPlanResult rescore_budget_result =
                planTemporalCostV2(
                    graph, query, minimum_duration, counts, lifespans,
                    rescore_budget_options,
                    &rescore_budget_diagnostics);
            require(
                rescore_budget_result.selected_tree != nullptr &&
                    rescore_budget_diagnostics.v2_budget_exhausted &&
                    rescore_budget_diagnostics.v2_fallback_phase ==
                        "rescore" &&
                    rescore_budget_diagnostics.v2_fallback_reason ==
                        "extension-probe-budget" &&
                    rescore_budget_diagnostics
                            .candidate_tree_relation_entries_built > 0 &&
                    rescore_budget_diagnostics
                            .rejected_candidate_tree_relation_entries ==
                        rescore_budget_diagnostics
                            .candidate_tree_relation_entries_built &&
                    std::abs(
                        rescore_budget_diagnostics
                            .final_rejected_tree_cost -
                        static_cast<double>(
                            rescore_budget_diagnostics
                                .candidate_tree_relation_entries_built)) <=
                        1e-9,
                context +
                    ": rescore-budget fallback charges every discarded "
                    "candidate tree");
            checked_candidate_tree_boundary = true;
        }

        if (round % 10 == 3) {
            TemporalPlanDiagnostics repeated_diagnostics;
            TemporalCostPlanResult repeated = planTemporalCostV2(
                graph, query, minimum_duration, counts, lifespans,
                v2_options, &repeated_diagnostics);
            require(
                repeated.decomposition.root ==
                        v2_result.decomposition.root &&
                    repeated.decomposition.dfs_order ==
                        v2_result.decomposition.dfs_order &&
                    repeated.decomposition.parent ==
                        v2_result.decomposition.parent &&
                    repeated_diagnostics.v2_decision ==
                        v2_diagnostics.v2_decision &&
                    repeated_diagnostics.v2_decision_reason ==
                        v2_diagnostics.v2_decision_reason &&
                    repeated_diagnostics.catalog_probes ==
                        v2_diagnostics.catalog_probes &&
                    repeated_diagnostics.state_expansions ==
                        v2_diagnostics.state_expansions &&
                    repeated_diagnostics.extension_probes ==
                        v2_diagnostics.extension_probes &&
                    repeated_diagnostics.candidate_trees_built ==
                        v2_diagnostics.candidate_trees_built,
                context + ": deterministic plan/decision/work trace");
        }
    }
    require(
        saw_v2_catalogue_path && saw_v2_candidate_tree && saw_bounded_top_w &&
            checked_candidate_tree_boundary && saw_gate_off_bypass,
        "random v2 oracle covers bounded top-W, catalogue, and "
        "candidate-tree budget paths (catalogue=" +
            std::to_string(saw_v2_catalogue_path) +
            ", candidate_tree=" + std::to_string(saw_v2_candidate_tree) +
            ", top_w=" + std::to_string(saw_bounded_top_w) +
            ", boundary=" +
            std::to_string(checked_candidate_tree_boundary) +
            ", gate_off=" + std::to_string(saw_gate_off_bypass) + ")");
    require(
        saw_reused_incumbent_score,
        "random v2 oracle covers identical-plan capped-score parity");
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
        testSurvivalSelectivity();
        testTemporalConditionalCostPlanner(temp_directory);
        testExactDurableMatching(temp_directory);
        testCountOnlyRootTaskParallelism(temp_directory);
        testSelfLoopDurability(temp_directory);
        testDirectedTreeOrientationAndReciprocalArcs(temp_directory);
        testExhaustiveSmallPlanRegretOracle(temp_directory);
        testRandomGraphsAgainstBruteForce(temp_directory);
        testRandomGeneralQueriesAgainstBruteForce(temp_directory);
        std::cout << "All ours tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
