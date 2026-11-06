#include "../TDTree.h"
#include "../Utils.h"
#include "../cover_aware_tdtree_builder.h"
#include "../fixed_plan_feasibility.h"
#include "../query_decomposition.h"
#include "../validated_graph_snapshot.h"
#include "../v3_plan_handoff.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

static_assert(noexcept(validateFixedPlan(
                  std::declval<const ValidatedGraphSnapshot&>(),
                  std::declval<const FixedPlan&>())),
              "snapshot fixed-plan validation must be allocation-free");
static_assert(noexcept(queryDecompositionTopologyIsCanonical(
                  std::declval<const ValidatedGraphSnapshot&>(),
                  std::declval<const QueryDecomposition&>())),
              "snapshot canonical-topology validation must be allocation-free");
static_assert(noexcept(
                  std::declval<const TDTree&>().capacityReceipt()),
              "TD-tree capacity receipt must be noexcept");
static_assert(noexcept(
                  std::declval<const CandidateCover&>().capacityReceipt()),
              "candidate-cover capacity receipt must be noexcept");

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void addQueryArc(Graph& query, int source, int target) {
    query.adj[static_cast<std::size_t>(source)].push_back(
        {target, -1});
    query.in_adj[static_cast<std::size_t>(target)].push_back(
        {source, -1});
}

void normalizeQuery(Graph& query) {
    for (auto& edges : query.adj) {
        std::sort(
            edges.begin(), edges.end(),
            [](const Edge& lhs, const Edge& rhs) {
                return lhs.to < rhs.to;
            });
    }
    for (auto& edges : query.in_adj) {
        std::sort(
            edges.begin(), edges.end(),
            [](const Edge& lhs, const Edge& rhs) {
                return lhs.to < rhs.to;
            });
    }
}

Graph makeQuery(
    std::vector<Label> labels,
    const std::vector<std::pair<int, int>>& arcs) {
    Graph query;
    query.num_vertices = static_cast<int>(labels.size());
    query.vertex_labels = std::move(labels);
    query.external_ids.resize(
        static_cast<std::size_t>(query.num_vertices));
    std::iota(
        query.external_ids.begin(), query.external_ids.end(), 0);
    query.adj.resize(static_cast<std::size_t>(query.num_vertices));
    query.in_adj.resize(
        static_cast<std::size_t>(query.num_vertices));
    for (const auto& arc : arcs) {
        addQueryArc(query, arc.first, arc.second);
    }
    normalizeQuery(query);
    return query;
}

void addTemporalEdge(
    Graph& graph,
    int source,
    int target,
    std::vector<TimeInterval> intervals) {
    std::int64_t active_count = 0;
    for (const TimeInterval& interval : intervals) {
        active_count += interval.length();
    }
    const int edge_id =
        static_cast<int>(graph.temporal_edges.size());
    // The shared constructor validates canonical history and computes its O(1)
    // maximum-run cache.
    graph.temporal_edges.emplace_back(
        source,
        target,
        std::move(intervals),
        active_count);
    graph.adj[static_cast<std::size_t>(source)].push_back(
        {target, edge_id});
    graph.in_adj[static_cast<std::size_t>(target)].push_back(
        {source, edge_id});
}

void finalizeDataGraph(Graph& graph) {
    graph.external_ids.resize(
        static_cast<std::size_t>(graph.num_vertices));
    std::iota(
        graph.external_ids.begin(), graph.external_ids.end(), 100);
    graph.vertex_active_durations.assign(
        static_cast<std::size_t>(graph.num_vertices), 32);
    graph.neighbor_label_counts.assign(
        static_cast<std::size_t>(graph.num_vertices), {});
    graph.incoming_neighbor_label_counts.assign(
        static_cast<std::size_t>(graph.num_vertices), {});
    for (std::size_t vertex = 0;
         vertex < graph.adj.size();
         ++vertex) {
        for (const Edge& edge : graph.adj[vertex]) {
            const Label label = graph.vertex_labels[
                static_cast<std::size_t>(edge.to)];
            if (label < kLabelCount) {
                ++graph.neighbor_label_counts[vertex][label];
            }
        }
        for (const Edge& edge : graph.in_adj[vertex]) {
            const Label label = graph.vertex_labels[
                static_cast<std::size_t>(edge.to)];
            if (label < kLabelCount) {
                ++graph.incoming_neighbor_label_counts[vertex][label];
            }
        }
        std::sort(
            graph.adj[vertex].begin(), graph.adj[vertex].end(),
            [](const Edge& lhs, const Edge& rhs) {
                return lhs.to < rhs.to;
            });
        std::sort(
            graph.in_adj[vertex].begin(), graph.in_adj[vertex].end(),
            [](const Edge& lhs, const Edge& rhs) {
                return lhs.to < rhs.to;
            });
    }
}

QueryDecomposition decompositionFromPlan(
    const Graph& query,
    const FixedPlan& plan) {
    const FixedPlanValidation validation =
        validateFixedPlan(query, plan);
    require(validation.valid, "test fixture plan is legal");

    QueryDecomposition decomposition;
    decomposition.root = plan.root;
    decomposition.dfs_order = plan.order;
    decomposition.parent = plan.parent;
    decomposition.vertex_labels = query.vertex_labels;
    decomposition.connected = true;
    decomposition.selectivity_mode = SelectivityMode::Paper;
    decomposition.temporal_selectivity.assign(
        static_cast<std::size_t>(query.num_vertices), 0.0);
    decomposition.spanning_tree_adj.assign(
        static_cast<std::size_t>(query.num_vertices), {});

    std::vector<std::vector<std::uint8_t>> is_tree_pair(
        static_cast<std::size_t>(query.num_vertices),
        std::vector<std::uint8_t>(
            static_cast<std::size_t>(query.num_vertices), 0));
    for (int child = 0; child < query.num_vertices; ++child) {
        const int parent =
            plan.parent[static_cast<std::size_t>(child)];
        if (parent < 0) continue;
        decomposition.spanning_tree_adj[
            static_cast<std::size_t>(parent)].push_back(child);
        is_tree_pair[static_cast<std::size_t>(parent)]
                    [static_cast<std::size_t>(child)] = 1;
        is_tree_pair[static_cast<std::size_t>(child)]
                    [static_cast<std::size_t>(parent)] = 1;
    }
    for (int source = 0; source < query.num_vertices; ++source) {
        for (const Edge& edge :
             query.adj[static_cast<std::size_t>(source)]) {
            if (is_tree_pair[static_cast<std::size_t>(source)]
                            [static_cast<std::size_t>(edge.to)] == 0) {
                decomposition.non_tree_edges.emplace_back(
                    source, edge.to);
            }
        }
    }
    return decomposition;
}

bool mappingIsDurable(
    const Graph& data,
    const Graph& query,
    const std::vector<int>& mapping,
    int minimum_duration) {
    std::vector<TimeInterval> common;
    bool have_edge = false;
    for (int source = 0; source < query.num_vertices; ++source) {
        for (const Edge& query_edge :
             query.adj[static_cast<std::size_t>(source)]) {
            const TemporalEdge* data_edge = data.findTemporalEdge(
                mapping[static_cast<std::size_t>(source)],
                mapping[static_cast<std::size_t>(query_edge.to)]);
            if (data_edge == nullptr) return false;
            if (!have_edge) {
                common = data_edge->activeIntervals();
                have_edge = true;
            } else {
                common = intersectTimeIntervals(
                    common,
                    data_edge->activeIntervals(),
                    minimum_duration);
            }
            if (common.empty()) return false;
        }
    }
    return have_edge &&
        hasMinimumConsecutiveDuration(common, minimum_duration);
}

std::vector<std::vector<int>> bruteForceDurableMappings(
    const Graph& data,
    const Graph& query,
    int minimum_duration) {
    std::vector<std::vector<int>> matches;
    std::vector<int> mapping(
        static_cast<std::size_t>(query.num_vertices), -1);
    std::vector<std::uint8_t> used(
        static_cast<std::size_t>(data.num_vertices), 0);
    std::function<void(int)> enumerate = [&](int query_vertex) {
        if (query_vertex == query.num_vertices) {
            if (mappingIsDurable(
                    data, query, mapping, minimum_duration)) {
                matches.push_back(mapping);
            }
            return;
        }
        const Label label = query.vertex_labels[
            static_cast<std::size_t>(query_vertex)];
        for (int data_vertex = 0;
             data_vertex < data.num_vertices;
             ++data_vertex) {
            const std::size_t data_index =
                static_cast<std::size_t>(data_vertex);
            if (used[data_index] != 0 ||
                data.vertex_labels[data_index] != label) {
                continue;
            }
            mapping[static_cast<std::size_t>(query_vertex)] =
                data_vertex;
            used[data_index] = 1;
            enumerate(query_vertex + 1);
            used[data_index] = 0;
            mapping[static_cast<std::size_t>(query_vertex)] = -1;
        }
    };
    enumerate(0);
    return matches;
}

void verifyCoverContainment(
    const CandidateCover& cover,
    const std::vector<std::vector<int>>& exact_matches,
    const std::string& context) {
    for (const auto& domain : cover.values()) {
        require(
            std::is_sorted(domain.begin(), domain.end()) &&
                std::adjacent_find(domain.begin(), domain.end()) ==
                    domain.end(),
            context + ": cover domains are sorted unique");
    }
    for (const auto& mapping : exact_matches) {
        require(
            mapping.size() == cover.queryVertexCount(),
            context + ": exact mapping has cover cardinality");
        for (std::size_t query_vertex = 0;
             query_vertex < mapping.size();
             ++query_vertex) {
            const auto& domain = cover.domain(query_vertex);
            require(
                std::binary_search(
                    domain.begin(),
                    domain.end(),
                    mapping[query_vertex]),
                context + ": every exact mapping is contained in cover");
        }
    }
}

std::vector<TimeInterval> intervalsFromMask(std::uint32_t mask) {
    std::vector<TimeInterval> intervals;
    for (int snapshot = 1; snapshot <= 6; ++snapshot) {
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

void testMaximumConsecutiveRunCache(
    const std::filesystem::path& directory) {
    TemporalEdge manual(
        0, 1, {{1, 2}, {10, 15}}, 8);
    require(
        manual.hasCachedMaximumConsecutiveRun(),
        "manual edge constructor computes the immutable derived cache");
    require(
        manual.maximumConsecutiveRun() == 6 &&
            hasMinimumConsecutiveDuration(manual, 6) &&
            !hasMinimumConsecutiveDuration(manual, 7),
        "manual edge overload matches interval scan");
    const auto input_path =
        directory / "tcc_v3_max_run_cache.dat";
    {
        std::ofstream output(input_path);
        output << "10 20 1\n10 20 2\n10 20 3\n"
               << "10 20 10\n10 20 11\n";
    }
    Graph loaded;
    require(
        readTemporalGraph(input_path.string(), loaded),
        "cache fixture loads");
    require(
        loaded.temporal_edges.size() == 1,
        "cache fixture retains one edge");
    const TemporalEdge& cached = loaded.temporal_edges.front();
    require(
        cached.hasCachedMaximumConsecutiveRun() &&
            cached.maximumConsecutiveRun() == 3,
        "loader records exact maximum consecutive run");
    require(
        hasMinimumConsecutiveDuration(cached, 3) ==
                hasMinimumConsecutiveDuration(
                    cached.activeIntervals(), 3) &&
            hasMinimumConsecutiveDuration(cached, 4) ==
                hasMinimumConsecutiveDuration(
                    cached.activeIntervals(), 4),
        "cached and interval predicates have exact parity");

    const Graph copied = loaded;
    require(
        temporalGraphContentFingerprint(loaded) ==
            temporalGraphContentFingerprint(copied),
        "logical graph fingerprint is stable across cached-edge copies");

    auto rejects = [](std::vector<TimeInterval> intervals,
                      std::int64_t count) {
        try {
            TemporalEdge invalid(0, 1, std::move(intervals), count);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    require(
        rejects({{3, 2}}, 0) &&
            rejects({{5, 6}, {1, 2}}, 4) &&
            rejects({{1, 3}, {3, 5}}, 6) &&
            rejects({{1, 2}, {3, 4}}, 4) &&
            rejects({{1, 2}, {5, 5}}, 2) &&
            rejects({{1, 2}}, -1),
        "manual edge construction rejects malformed, noncanonical, or "
        "count-inconsistent histories");
    const TemporalEdge extreme(
        0,
        1,
        {{std::numeric_limits<int>::min(),
          std::numeric_limits<int>::max()}},
        INT64_C(4294967296));
    require(
        extreme.active_snapshot_count == INT64_C(4294967296) &&
            extreme.maximumConsecutiveRun() ==
                std::numeric_limits<int>::max(),
        "extreme canonical interval uses exact int64 count and a safe cache");
    std::filesystem::remove(input_path);
}

Graph makeTriangleData() {
    Graph data;
    data.num_vertices = 6;
    data.vertex_labels = {
        labelFromString("A"), labelFromString("B"),
        labelFromString("C"), labelFromString("A"),
        labelFromString("B"), labelFromString("C")};
    data.adj.resize(6);
    data.in_adj.resize(6);

    addTemporalEdge(data, 0, 1, {{1, 5}});
    addTemporalEdge(data, 1, 2, {{1, 5}});
    addTemporalEdge(data, 0, 2, {{2, 5}});

    // Every edge has an individual k=3 run, but the common intersection is
    // a single snapshot.  It can remain in the cover but is not an answer.
    addTemporalEdge(data, 3, 4, {{10, 12}});
    addTemporalEdge(data, 4, 5, {{12, 14}});
    addTemporalEdge(data, 3, 5, {{10, 12}});
    finalizeDataGraph(data);
    return data;
}

Graph makeTriangleQuery() {
    return makeQuery(
        {labelFromString("A"), labelFromString("B"),
         labelFromString("C")},
        {{0, 1}, {1, 2}, {0, 2}});
}

void testCandidateCoverContainmentAndIdentity() {
    const Graph data = makeTriangleData();
    const Graph query = makeTriangleQuery();
    const FixedPlan incumbent = makeFixedPlan(
        query, 0, {0, 1, 2}, {-1, 0, 1});
    TDTree tree(
        data,
        query,
        decompositionFromPlan(query, incumbent),
        3);

    const CandidateCover first = tree.exportCandidateCover();
    const CandidateCover second = tree.exportCandidateCover();
    require(
        first.values() == second.values() &&
            first.graphFingerprint() == second.graphFingerprint() &&
            first.queryFingerprint() == second.queryFingerprint() &&
            first.incumbentPlanFingerprint() ==
                second.incumbentPlanFingerprint() &&
            first.coverFingerprint() == second.coverFingerprint(),
        "candidate-cover export identity is deterministic");
    require(
        first.graphFingerprint() ==
                temporalGraphContentFingerprint(data) &&
            first.queryFingerprint() ==
                queryGraphContentFingerprint(query) &&
            first.minimumDuration() == 3 &&
            first.incumbentPlanFingerprint() ==
                incumbent.fingerprint,
        "candidate cover carries graph/query/k/plan identity");

    std::uint64_t unique_candidates = 0;
    std::uint64_t value_capacity = 0;
    for (const auto& domain : first.values()) {
        unique_candidates += domain.size();
        value_capacity += domain.capacity();
    }
    require(
        first.sourceRelationEntries() >= unique_candidates,
        "cover records source relation-entry accounting");

    const CandidateCoverCapacityReceipt first_capacity =
        first.capacityReceipt();
    const CandidateCoverCapacityReceipt second_capacity =
        second.capacityReceipt();
    const std::uint64_t expected_outer_bytes =
        static_cast<std::uint64_t>(first.values().capacity()) *
        sizeof(std::vector<int>);
    const std::uint64_t expected_nested_bytes =
        value_capacity * sizeof(int);
    require(
        first_capacity.complete && !first_capacity.overflowed &&
            first_capacity.invariantHolds() &&
            first_capacity.object_bytes == sizeof(CandidateCover) &&
            first_capacity.domain_count == first.values().size() &&
            first_capacity.domain_capacity == first.values().capacity() &&
            first_capacity.outer_domains_bytes == expected_outer_bytes &&
            first_capacity.value_count == unique_candidates &&
            first_capacity.value_capacity == value_capacity &&
            first_capacity.nested_values_bytes == expected_nested_bytes &&
            first_capacity.dynamic_bytes ==
                expected_outer_bytes + expected_nested_bytes &&
            first_capacity.total_bytes ==
                sizeof(CandidateCover) + expected_outer_bytes +
                    expected_nested_bytes &&
            first_capacity.total_bytes > 0,
        "candidate cover reports exact outer/nested native capacities");
    require(
        second_capacity.complete && second_capacity.invariantHolds() &&
            second_capacity.domain_count == first_capacity.domain_count &&
            second_capacity.domain_capacity == first_capacity.domain_capacity &&
            second_capacity.value_count == first_capacity.value_count &&
            second_capacity.value_capacity == first_capacity.value_capacity &&
            second_capacity.total_bytes == first_capacity.total_bytes,
        "identical cover exports have deterministic exact capacity totals");

    const auto exact_matches =
        bruteForceDurableMappings(data, query, 3);
    require(
        exact_matches.size() == 1,
        "triangle fixture has one common-window durable match");
    verifyCoverContainment(
        first, exact_matches, "triangle cover");

    Graph canonical_copy = query;
    std::reverse(
        canonical_copy.adj[0].begin(),
        canonical_copy.adj[0].end());
    require(
        queryGraphContentFingerprint(query) ==
            queryGraphContentFingerprint(canonical_copy),
        "query identity canonicalizes arc order");

    QueryDecomposition malformed =
        decompositionFromPlan(query, incumbent);
    malformed.non_tree_edges.clear();
    require(
        !queryDecompositionTopologyIsCanonical(query, malformed),
        "missing directed non-tree constraint is rejected");
    bool constructor_rejected = false;
    try {
        TDTree invalid(data, query, malformed, 3);
    } catch (const std::invalid_argument&) {
        constructor_rejected = true;
    }
    require(
        constructor_rejected,
        "TD-tree constructor binds all executable topology fields");

    QueryDecomposition metadata_change =
        decompositionFromPlan(query, incumbent);
    metadata_change.non_tree_edges.clear();
    bool metadata_rejected = false;
    try {
        tree.setDecompositionMetadata(metadata_change);
    } catch (const std::invalid_argument&) {
        metadata_rejected = true;
    }
    require(
        metadata_rejected,
        "metadata update cannot change executable non-tree constraints");
}

void testNativeCapacityOverflowHooks() {
    std::uint64_t result = 777;
    require(
        checkedTDTreeCapacityAddProductForTesting(7, 3, 4, result) &&
            result == 19,
        "TD-tree checked capacity product/add accepts an exact value");
    result = 777;
    require(
        !checkedTDTreeCapacityAddProductForTesting(
            0, std::numeric_limits<std::uint64_t>::max(), 2, result) &&
            result == 777,
        "TD-tree checked capacity multiplication overflow is fail closed");
    result = 777;
    require(
        !checkedTDTreeCapacityAddProductForTesting(
            std::numeric_limits<std::uint64_t>::max(), 1, 1, result) &&
            result == 777,
        "TD-tree checked capacity addition overflow is fail closed");

    result = 777;
    require(
        checkedCandidateCoverCapacityAddProductForTesting(
            11, 5, 4, result) && result == 31,
        "cover checked capacity product/add accepts an exact value");
    result = 777;
    require(
        !checkedCandidateCoverCapacityAddProductForTesting(
            0, std::numeric_limits<std::uint64_t>::max(), 2, result) &&
            result == 777,
        "cover checked capacity multiplication overflow is fail closed");
    result = 777;
    require(
        !checkedCandidateCoverCapacityAddProductForTesting(
            std::numeric_limits<std::uint64_t>::max(), 1, 1, result) &&
            result == 777,
        "cover checked capacity addition overflow is fail closed");
}

void testRandomDirectedSinkCoverContainment() {
    const Graph query = makeQuery(
        {labelFromString("A"), labelFromString("B"),
         labelFromString("C")},
        {{0, 1}, {2, 1}});
    const FixedPlan sink_root = makeFixedPlan(
        query, 1, {1, 0, 2}, {1, -1, 1});
    require(
        validateFixedPlan(query, sink_root).valid &&
            requiredArcMask(query, 1, 0) ==
                RequiredArcMask::ChildToParent,
        "directed sink-root plan is legal and preserves arc direction");

    std::uint32_t state = UINT32_C(0x51f15e1d);
    auto nextMask = [&]() {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        return state & UINT32_C(0x3f);
    };

    for (int seed = 0; seed < 16; ++seed) {
        Graph data;
        data.num_vertices = 6;
        data.vertex_labels = {
            labelFromString("A"), labelFromString("B"),
            labelFromString("C"), labelFromString("A"),
            labelFromString("B"), labelFromString("C")};
        data.adj.resize(6);
        data.in_adj.resize(6);

        for (int source : {0, 3}) {
            for (int target : {1, 4}) {
                std::uint32_t mask = nextMask();
                if (source == 0 && target == 1) {
                    mask = UINT32_C(0x07);
                }
                if (mask != 0) {
                    addTemporalEdge(
                        data,
                        source,
                        target,
                        intervalsFromMask(mask));
                }
            }
        }
        for (int source : {2, 5}) {
            for (int target : {1, 4}) {
                std::uint32_t mask = nextMask();
                if (source == 2 && target == 1) {
                    mask = UINT32_C(0x07);
                }
                if (mask != 0) {
                    addTemporalEdge(
                        data,
                        source,
                        target,
                        intervalsFromMask(mask));
                }
            }
        }
        finalizeDataGraph(data);
        TDTree tree(
            data,
            query,
            decompositionFromPlan(query, sink_root),
            2);
        const CandidateCover cover = tree.exportCandidateCover();
        const auto exact_matches =
            bruteForceDurableMappings(data, query, 2);
        require(
            !exact_matches.empty(),
            "random sink fixture retains its guaranteed exact match");
        verifyCoverContainment(
            cover,
            exact_matches,
            "random directed sink cover seed " +
                std::to_string(seed));
        require(
            cover.coverFingerprint() ==
                tree.exportCandidateCover().coverFingerprint(),
            "random cover fingerprint is deterministic");
    }
}

void expectPlanStatus(
    const Graph& query,
    const FixedPlan& plan,
    FixedPlanValidationStatus expected,
    const std::string& context) {
    const FixedPlanValidation validation =
        validateFixedPlan(query, plan);
    require(
        !validation.valid && validation.status == expected,
        context + ": expected " +
            fixedPlanValidationStatusName(expected) +
            ", observed " +
            fixedPlanValidationStatusName(validation.status));
}

void testFixedPlanLegalityRejections() {
    const Graph query = makeQuery(
        {labelFromString("A"), labelFromString("B"),
         labelFromString("C")},
        {{0, 1}, {1, 2}});
    const FixedPlan valid = makeFixedPlan(
        query, 0, {0, 1, 2}, {-1, 0, 1});
    require(
        validateFixedPlan(query, valid).valid,
        "valid fixed plan passes");

    expectPlanStatus(
        query,
        makeFixedPlan(query, 0, {1, 0, 2}, {-1, 0, 1}),
        FixedPlanValidationStatus::RootNotFirst,
        "root-first rejection");
    expectPlanStatus(
        query,
        makeFixedPlan(query, 0, {0, 1, 1}, {-1, 0, 1}),
        FixedPlanValidationStatus::DuplicateOrderVertex,
        "duplicate-order rejection");
    expectPlanStatus(
        query,
        makeFixedPlan(query, 0, {0, 1, 2}, {-1, 2, 1}),
        FixedPlanValidationStatus::ParentDoesNotPrecedeChild,
        "later-parent rejection");
    expectPlanStatus(
        query,
        makeFixedPlan(query, 0, {0, 1, 2}, {-1, 0, 0}),
        FixedPlanValidationStatus::ParentChildNotAdjacent,
        "non-edge parent rejection");
    expectPlanStatus(
        query,
        makeFixedPlan(query, 0, {0, 1, 2}, {2, 0, 1}),
        FixedPlanValidationStatus::RootParentNotSentinel,
        "root-parent rejection");

    FixedPlan corrupt_fingerprint = valid;
    corrupt_fingerprint.fingerprint ^= UINT64_C(1);
    expectPlanStatus(
        query,
        corrupt_fingerprint,
        FixedPlanValidationStatus::PlanFingerprintMismatch,
        "stale plan fingerprint rejection");

    Graph changed_query = query;
    changed_query.vertex_labels[0] = labelFromString("D");
    expectPlanStatus(
        changed_query,
        valid,
        FixedPlanValidationStatus::QueryFingerprintMismatch,
        "cross-query plan rejection");

    Graph invalid_query = query;
    invalid_query.adj[1].push_back({0, -1});
    expectPlanStatus(
        invalid_query,
        valid,
        FixedPlanValidationStatus::InvalidQueryTopology,
        "inconsistent query topology rejection");

    const Graph reciprocal = makeQuery(
        {labelFromString("A"), labelFromString("B")},
        {{0, 1}, {1, 0}});
    require(
        requiredArcMask(reciprocal, 0, 1) ==
            RequiredArcMask::Reciprocal,
        "reciprocal query pair has explicit two-bit mask");
}

std::pair<Graph, Graph> makeReciprocalQuoteFixture() {
    Graph data;
    data.num_vertices = 4;
    data.vertex_labels = {
        labelFromString("A"), labelFromString("B"),
        labelFromString("A"), labelFromString("B")};
    data.adj.resize(4);
    data.in_adj.resize(4);
    addTemporalEdge(data, 0, 1, {{1, 4}});
    addTemporalEdge(data, 1, 0, {{1, 4}});
    addTemporalEdge(data, 2, 3, {{2, 5}});
    addTemporalEdge(data, 3, 2, {{2, 5}});
    finalizeDataGraph(data);

    Graph query = makeQuery(
        {labelFromString("A"), labelFromString("B")},
        {{0, 1}, {1, 0}});
    return {std::move(data), std::move(query)};
}

CandidateBuildBudget exactBudget(
    const CandidateBuildQuote& quote,
    std::uint64_t headroom) {
    CandidateBuildBudget budget;
    budget.adjacency_probes = quote.adjacency_probes;
    budget.reverse_edge_lookups = quote.reverse_edge_lookups;
    budget.arc_checks = quote.maximum_arc_checks;
    budget.relation_entries = quote.maximum_relation_entries;
    budget.candidate_value_occurrences =
        quote.maximum_candidate_value_occurrences;
    budget.blocks = quote.maximum_blocks;
    budget.semijoin_entry_visits =
        quote.maximum_semijoin_entry_visits;
    budget.index_entries = quote.maximum_index_entries;
    budget.fixed_memory_headroom_bytes = headroom;
    budget.payload_memory_bytes = quote.maximum_payload_bytes;
    budget.candidate_tree_tokens = 1;
    return budget;
}

bool sameBudget(
    const CandidateBuildBudget& lhs,
    const CandidateBuildBudget& rhs) {
    return lhs.adjacency_probes == rhs.adjacency_probes &&
        lhs.reverse_edge_lookups == rhs.reverse_edge_lookups &&
        lhs.arc_checks == rhs.arc_checks &&
        lhs.relation_entries == rhs.relation_entries &&
        lhs.candidate_value_occurrences ==
            rhs.candidate_value_occurrences &&
        lhs.blocks == rhs.blocks &&
        lhs.semijoin_entry_visits ==
            rhs.semijoin_entry_visits &&
        lhs.index_entries == rhs.index_entries &&
        lhs.payload_memory_bytes == rhs.payload_memory_bytes &&
        lhs.fixed_memory_headroom_bytes ==
            rhs.fixed_memory_headroom_bytes &&
        lhs.candidate_tree_tokens == rhs.candidate_tree_tokens;
}

void expectBudgetFitReason(
    const CandidateBuildQuote& quote,
    const CandidateBuildBudget& budget,
    CandidateBuildBudgetFitReason expected,
    const std::string& context) {
    const CandidateBuildBudgetFit fit =
        evaluateCandidateBuildBudget(quote, budget);
    require(
        !fit.fits && fit.reason == expected,
        context + ": expected " +
            candidateBuildBudgetFitReasonName(expected) +
            ", observed " +
            candidateBuildBudgetFitReasonName(fit.reason));
}

void testBuildQuoteAndBudgetBoundaries() {
    auto fixture = makeReciprocalQuoteFixture();
    Graph& data = fixture.first;
    Graph& query = fixture.second;
    const FixedPlan incumbent = makeFixedPlan(
        query, 0, {0, 1}, {-1, 0});
    TDTree tree(
        data,
        query,
        decompositionFromPlan(query, incumbent),
        3);
    const CandidateCover cover = tree.exportCandidateCover();

    const CandidateBuildQuote incumbent_quote =
        quoteCoverAwareBuild(
            data, query, cover, incumbent);
    require(
        !incumbent_quote.complete &&
            incumbent_quote.status ==
                CandidateBuildQuoteStatus::IncumbentPlanRejected,
        "candidate-build quote rejects the incumbent plan");

    const FixedPlan candidate = makeFixedPlan(
        query, 1, {1, 0}, {1, -1});

    const CandidateBuildQuote quote = quoteCoverAwareBuild(
        data, query, cover, candidate);
    require(
        quote.complete && !quote.overflowed &&
            quote.status == CandidateBuildQuoteStatus::Complete &&
            quote.plan_validation.valid,
        "reciprocal build quote is complete");
    require(
        quote.adjacency_probes == 2 &&
            quote.reverse_edge_lookups == 2 &&
            quote.maximum_arc_checks == 4 &&
            quote.proposed_root_cover_cardinality == 2 &&
            quote.maximum_relation_entries == 2 &&
            quote.maximum_candidate_value_occurrences == 4 &&
            quote.maximum_blocks == 2,
        "reciprocal quote follows cover-aware min-degree upper bound");
    require(
        quote.graph_fingerprint == cover.graphFingerprint() &&
            quote.query_fingerprint == cover.queryFingerprint() &&
            quote.cover_fingerprint == cover.coverFingerprint() &&
            quote.plan_fingerprint == candidate.fingerprint,
        "quote preserves all explicit identities");
    require(
        quote.materializerContractBound() &&
            candidateBuildQuoteContractIsValid(quote) &&
            std::string(quote.payloadModel()) ==
                "flat-sorted-exact-reserve-v1" &&
            quote.accountingFingerprint() != 0 &&
            quote.contractFingerprint() != 0,
        "Patch-4 quote is sealed to the exact flat materializer contract");

    const CandidateBuildBudget exact = exactBudget(quote, 17);
    const CandidateBuildBudgetFit fit =
        evaluateCandidateBuildBudget(quote, exact);
    require(
        fit.fits &&
            fit.reason == CandidateBuildBudgetFitReason::FitsQuote &&
            fit.required_payload_bytes ==
                exact.payload_memory_bytes,
        "all exact caps fit the arithmetic quote without admitting a build");

    {
        CandidateBuildLedger ledger(exact);
        CandidateBuildReservationAttempt attempt =
            tryReserveCandidateBuild(quote, ledger);
        require(
            attempt.fit.fits && attempt.reservation != nullptr &&
                attempt.reservation->active(),
            "exact quote is atomically reserved");
        const CandidateBuildBudget remaining = ledger.remaining();
        require(
            remaining.adjacency_probes == 0 &&
                remaining.reverse_edge_lookups == 0 &&
                remaining.arc_checks == 0 &&
                remaining.relation_entries == 0 &&
                remaining.candidate_value_occurrences == 0 &&
                remaining.blocks == 0 &&
                remaining.semijoin_entry_visits == 0 &&
                remaining.index_entries == 0 &&
                remaining.payload_memory_bytes == 0 &&
                remaining.fixed_memory_headroom_bytes == 17 &&
                remaining.candidate_tree_tokens == 0,
            "reservation debits every unit and token while keeping fixed "
            "headroom separate");
        attempt.reservation.reset();
        require(
            sameBudget(ledger.remaining(), exact),
            "uncommitted reservation refunds the complete ledger exactly");
    }

    CandidateBuildBudget reduced = exact;
    --reduced.adjacency_probes;
    expectBudgetFitReason(
        quote, reduced,
        CandidateBuildBudgetFitReason::AdjacencyProbeBudget,
        "adjacency cap-minus-one");
    {
        CandidateBuildLedger ledger(reduced);
        const CandidateBuildBudget before = ledger.remaining();
        CandidateBuildReservationAttempt attempt =
            tryReserveCandidateBuild(quote, ledger);
        require(
            !attempt.fit.fits && !attempt.reservation &&
                sameBudget(ledger.remaining(), before),
            "cap-minus-one rejection performs no partial ledger debit");
    }
    reduced = exact;
    --reduced.reverse_edge_lookups;
    expectBudgetFitReason(
        quote, reduced,
        CandidateBuildBudgetFitReason::ReverseLookupBudget,
        "reverse cap-minus-one");
    reduced = exact;
    --reduced.arc_checks;
    expectBudgetFitReason(
        quote, reduced,
        CandidateBuildBudgetFitReason::ArcCheckBudget,
        "arc-check cap-minus-one");
    reduced = exact;
    --reduced.relation_entries;
    expectBudgetFitReason(
        quote, reduced,
        CandidateBuildBudgetFitReason::RelationEntryBudget,
        "relation-entry cap-minus-one");
    reduced = exact;
    --reduced.candidate_value_occurrences;
    expectBudgetFitReason(
        quote, reduced,
        CandidateBuildBudgetFitReason::CandidateValueBudget,
        "candidate-value cap-minus-one");
    reduced = exact;
    --reduced.blocks;
    expectBudgetFitReason(
        quote, reduced,
        CandidateBuildBudgetFitReason::BlockBudget,
        "block cap-minus-one");
    reduced = exact;
    --reduced.semijoin_entry_visits;
    expectBudgetFitReason(
        quote, reduced,
        CandidateBuildBudgetFitReason::SemijoinVisitBudget,
        "semijoin cap-minus-one");
    reduced = exact;
    --reduced.index_entries;
    expectBudgetFitReason(
        quote, reduced,
        CandidateBuildBudgetFitReason::IndexEntryBudget,
        "index cap-minus-one");
    reduced = exact;
    --reduced.payload_memory_bytes;
    expectBudgetFitReason(
        quote, reduced,
        CandidateBuildBudgetFitReason::PayloadMemoryBudget,
        "payload cap-minus-one");
    reduced = exact;
    reduced.candidate_tree_tokens = 0;
    expectBudgetFitReason(
        quote, reduced,
        CandidateBuildBudgetFitReason::NoCandidateTreeToken,
        "candidate-tree token is mandatory");

    // H is deliberately not charged against P by the local candidate ledger.
    // Even a very large evidence value cannot change a payload-only fit; the
    // later checked global admission is responsible for E+Q_B+H overflow.
    reduced = exact;
    reduced.fixed_memory_headroom_bytes =
        std::numeric_limits<std::uint64_t>::max();
    const CandidateBuildBudgetFit independent_headroom_fit =
        evaluateCandidateBuildBudget(quote, reduced);
    require(
        independent_headroom_fit.fits &&
            independent_headroom_fit.required_payload_bytes ==
                quote.maximum_payload_bytes,
        "candidate payload ceiling is independent of global headroom");

    CandidateBuildAccounting overflow_accounting =
        defaultCandidateBuildAccounting();
    overflow_accounting.node_payload_bytes =
        std::numeric_limits<std::uint64_t>::max();
    const CandidateBuildQuote overflow_quote = quoteCoverAwareBuild(
        data,
        query,
        cover,
        candidate,
        overflow_accounting);
    require(
        !overflow_quote.complete && overflow_quote.overflowed &&
            overflow_quote.status ==
                CandidateBuildQuoteStatus::ArithmeticOverflow,
        "quote arithmetic overflow is explicit");
    expectBudgetFitReason(
        overflow_quote,
        exact,
        CandidateBuildBudgetFitReason::QuoteOverflow,
        "overflow quote cannot fit a budget");

    const CandidateBuildQuote invalid_accounting =
        quoteCoverAwareBuild(
            data,
            query,
            cover,
            candidate,
            CandidateBuildAccounting{});
    require(
        !invalid_accounting.complete &&
            invalid_accounting.status ==
                CandidateBuildQuoteStatus::InvalidAccounting,
        "zero accounting is not treated as a free build");

    CandidateBuildAccounting underquoted_accounting =
        defaultCandidateBuildAccounting();
    --underquoted_accounting.relation_entry_bytes;
    const CandidateBuildQuote underquoted =
        quoteCoverAwareBuild(
            data,
            query,
            cover,
            candidate,
            underquoted_accounting);
    require(
        !underquoted.complete &&
            underquoted.status ==
                CandidateBuildQuoteStatus::InvalidAccounting,
        "caller cannot reduce conservative accounting lower bounds");

    FixedPlan stale_plan = candidate;
    stale_plan.fingerprint ^= UINT64_C(1);
    const CandidateBuildQuote invalid_plan_quote =
        quoteCoverAwareBuild(
            data, query, cover, stale_plan);
    require(
        !invalid_plan_quote.complete &&
            invalid_plan_quote.status ==
                CandidateBuildQuoteStatus::InvalidFixedPlan &&
            invalid_plan_quote.plan_validation.status ==
                FixedPlanValidationStatus::PlanFingerprintMismatch,
        "stale plan identity fails before admission");
    expectBudgetFitReason(
        invalid_plan_quote,
        exact,
        CandidateBuildBudgetFitReason::QuoteIncomplete,
        "invalid plan quote cannot fit a budget");

    const Label original_label = data.vertex_labels[0];
    data.vertex_labels[0] = labelFromString("D");
    const CandidateBuildQuote data_mismatch =
        quoteCoverAwareBuild(
            data, query, cover, candidate);
    require(
        data_mismatch.status ==
            CandidateBuildQuoteStatus::DataGraphIdentityMismatch,
        "same-object data mutation is detected by content identity");
    data.vertex_labels[0] = original_label;

    const std::int64_t saved_snapshot_count =
        data.temporal_edges[0].active_snapshot_count;
    const Label saved_data_label = data.vertex_labels[0];

    data.temporal_edges[0].active_snapshot_count = 0;
    bool export_rejected_malformed_history = false;
    try {
        static_cast<void>(tree.exportCandidateCover());
    } catch (const std::logic_error&) {
        export_rejected_malformed_history = true;
    }
    require(
        export_rejected_malformed_history,
        "cover export fail-closes malformed public temporal history");
    data.temporal_edges[0].active_snapshot_count =
        saved_snapshot_count;

    data.vertex_labels[0] = kInvalidLabel;
    bool export_rejected_invalid_label = false;
    try {
        static_cast<void>(tree.exportCandidateCover());
    } catch (const std::logic_error&) {
        export_rejected_invalid_label = true;
    }
    require(
        export_rejected_invalid_label,
        "cover export fail-closes invalid public data labels");
    data.vertex_labels[0] = saved_data_label;

    const std::vector<Edge> saved_incoming = data.in_adj[1];
    data.in_adj[1].clear();
    const CandidateBuildQuote malformed_data_quote =
        quoteCoverAwareBuild(
            data, query, cover, candidate);
    require(
        malformed_data_quote.status ==
            CandidateBuildQuoteStatus::InvalidDataGraph,
        "quote rejects missing reverse adjacency/index correspondence");
    data.in_adj[1] = saved_incoming;

    data.temporal_edges[0].active_snapshot_count = 0;
    const CandidateBuildQuote malformed_history_quote =
        quoteCoverAwareBuild(data, query, cover, candidate);
    require(
        malformed_history_quote.status ==
            CandidateBuildQuoteStatus::InvalidDataGraph,
        "quote rejects a public-field bypass with interval/count mismatch");
    data.temporal_edges[0].active_snapshot_count =
        saved_snapshot_count;

    TemporalEdge saved_temporal_edge = data.temporal_edges[0];
    TemporalEdge default_bypass;
    default_bypass.u = saved_temporal_edge.u;
    default_bypass.v = saved_temporal_edge.v;
    default_bypass.active_snapshot_count = 1;
    data.temporal_edges[0] = std::move(default_bypass);
    const CandidateBuildQuote default_bypass_quote =
        quoteCoverAwareBuild(data, query, cover, candidate);
    require(
        default_bypass_quote.status ==
            CandidateBuildQuoteStatus::InvalidDataGraph,
        "quote rejects default-constructor bypass with nonzero empty "
        "history");
    data.temporal_edges[0] = std::move(saved_temporal_edge);

    data.vertex_labels[0] = kInvalidLabel;
    const CandidateBuildQuote invalid_data_label_quote =
        quoteCoverAwareBuild(data, query, cover, candidate);
    require(
        invalid_data_label_quote.status ==
            CandidateBuildQuoteStatus::InvalidDataGraph,
        "quote rejects invalid/sentinel data labels");
    data.vertex_labels[0] = saved_data_label;

    const int saved_external_id = data.external_ids.back();
    data.external_ids.pop_back();
    const CandidateBuildQuote malformed_identity_quote =
        quoteCoverAwareBuild(
            data, query, cover, candidate);
    require(
        malformed_identity_quote.status ==
            CandidateBuildQuoteStatus::InvalidDataGraph,
        "quote rejects incomplete compact/external identity mapping");
    data.external_ids.push_back(saved_external_id);

    const Label original_query_label = query.vertex_labels[0];
    query.vertex_labels[0] = labelFromString("D");
    const CandidateBuildQuote query_mismatch =
        quoteCoverAwareBuild(
            data, query, cover, candidate);
    require(
        query_mismatch.status ==
            CandidateBuildQuoteStatus::QueryGraphIdentityMismatch,
        "same-object query mutation is detected by content identity");
    query.vertex_labels[0] = original_query_label;

    query.vertex_labels[0] = kInvalidLabel;
    const CandidateBuildQuote invalid_query_label_quote =
        quoteCoverAwareBuild(data, query, cover, candidate);
    require(
        invalid_query_label_quote.status ==
            CandidateBuildQuoteStatus::InvalidQueryGraph,
        "quote rejects invalid/sentinel query labels");
    query.vertex_labels[0] = original_query_label;

    const Graph copied_data = data;
    const CandidateBuildQuote copied_identity =
        quoteCoverAwareBuild(
            copied_data, query, cover, candidate);
    require(
        copied_identity.status ==
            CandidateBuildQuoteStatus::DataGraphIdentityMismatch,
        "cover is bound to its source graph object, not only equal content");

    CandidateBuildQuote tampered = quote;
    ++tampered.maximum_blocks;
    require(
        !candidateBuildQuoteContractIsValid(tampered),
        "mutating a readable quote field invalidates its private contract "
        "seal");
    CandidateBuildLedger tampered_ledger(exact);
    CandidateBuildReservationAttempt tampered_attempt =
        tryReserveCandidateBuild(tampered, tampered_ledger);
    require(
        !tampered_attempt.reservation &&
            sameBudget(tampered_ledger.remaining(), exact),
        "a tampered quote cannot be reused for reservation");

    // Reservation and ledger share the state lifetime; destroying the public
    // ledger first cannot turn rollback into a dangling access.
    std::unique_ptr<CandidateBuildReservation> escaped;
    {
        CandidateBuildLedger short_lived_ledger(exact);
        CandidateBuildReservationAttempt attempt =
            tryReserveCandidateBuild(quote, short_lived_ledger);
        escaped = std::move(attempt.reservation);
    }
    escaped.reset();
}

std::uint64_t countCandidateTree(
    const TDTree& tree,
    const std::string& tag) {
    const auto path = std::filesystem::temp_directory_path() /
        ("tcc_v3_patch4_" + tag + ".txt");
    const MatchSummary summary = tree.save_res(
        path.string(), MatchOutputMode::CountOnly, 1);
    require(
        summary.output_written && !summary.count_overflow,
        tag + ": candidate executor produced a complete count receipt");
    std::filesystem::remove(path);
    return summary.match_count;
}

CoverAwareBuildResult buildWithExactBudget(
    const Graph& data,
    const Graph& query,
    const CandidateCover& cover,
    const FixedPlan& plan,
    CandidateBuildLedger& ledger,
    const CandidateBuildQuote& quote) {
    CandidateBuildReservationAttempt attempt =
        tryReserveCandidateBuild(quote, ledger);
    require(
        attempt.fit.fits && attempt.reservation != nullptr,
        "exact candidate budget creates a reservation");
    return buildCoverAwareTDTree(
        data,
        query,
        cover,
        plan,
        std::move(attempt.reservation));
}

std::vector<FixedPlan> enumerateLegalPlans(const Graph& query) {
    std::vector<FixedPlan> plans;
    std::vector<int> order(
        static_cast<std::size_t>(query.num_vertices));
    std::iota(order.begin(), order.end(), 0);
    do {
        std::vector<int> parent(
            static_cast<std::size_t>(query.num_vertices), -1);
        std::function<void(std::size_t)> assign =
            [&](std::size_t depth) {
                if (depth == order.size()) {
                    FixedPlan plan = makeFixedPlan(
                        query, order.front(), order, parent);
                    if (validateFixedPlan(query, plan).valid) {
                        plans.push_back(std::move(plan));
                    }
                    return;
                }
                const int child = order[depth];
                for (std::size_t parent_depth = 0;
                     parent_depth < depth;
                     ++parent_depth) {
                    const int candidate_parent = order[parent_depth];
                    if (requiredArcMask(
                            query, candidate_parent, child) ==
                        RequiredArcMask::None) {
                        continue;
                    }
                    parent[static_cast<std::size_t>(child)] =
                        candidate_parent;
                    assign(depth + 1);
                    parent[static_cast<std::size_t>(child)] = -1;
                }
            };
        assign(1);
    } while (std::next_permutation(order.begin(), order.end()));
    return plans;
}

void verifySuccessfulBuild(
    const Graph& data,
    const Graph& query,
    const CandidateCover& cover,
    const FixedPlan& plan,
    std::uint64_t expected_matches,
    const std::string& context) {
    const CandidateBuildQuote quote = quoteCoverAwareBuild(
        data, query, cover, plan);
    require(
        quote.complete && quote.materializerContractBound(),
        context + ": candidate quote is complete and builder-bound");
    const CandidateBuildBudget budget = exactBudget(quote, 23);
    CandidateBuildLedger ledger(budget);
    CoverAwareBuildResult result = buildWithExactBudget(
        data, query, cover, plan, ledger, quote);
    require(
        result.complete() && result.tree != nullptr &&
            result.status == CoverAwareBuildStatus::Complete,
        context + ": transactional candidate build completes");
    require(
        result.tree->executionPlanFingerprint() == plan.fingerprint &&
            result.tree->minimumDuration() == cover.minimumDuration() &&
            result.tree->hasOnlySortedFlatBlocks(),
        context + ": handoff has the requested exact flat plan");
    require(
        result.counters.adjacency_probes <= quote.adjacency_probes &&
            result.counters.reverse_edge_lookups <=
                quote.reverse_edge_lookups &&
            result.counters.arc_checks <= quote.maximum_arc_checks &&
            result.counters.relation_entries <=
                quote.maximum_relation_entries &&
            result.counters.nonempty_blocks <= quote.maximum_blocks &&
            result.counters.semijoin_entry_visits <=
                quote.maximum_semijoin_entry_visits &&
            result.counters.index_entry_visits <=
                quote.maximum_index_entries &&
            result.counters.peak_payload_capacity_bytes <=
                quote.maximum_payload_bytes &&
            result.counters.final_payload_capacity_bytes <=
                quote.maximum_payload_bytes &&
            result.counters.selected_tree_arcs + 1 ==
                static_cast<std::uint64_t>(query.num_vertices) &&
            result.counters.non_tree_prefilter_checks == 0 &&
            result.counters.quote_violations == 0 &&
            result.counters.partial_tree_handoffs == 0,
        context + ": every actual work/capacity counter is quote-dominated");
    require(
        countCandidateTree(*result.tree, context) == expected_matches,
        context + ": candidate executor equals the direct oracle");
    bool metadata_growth_rejected = false;
    try {
        result.tree->setDecompositionMetadata(
            decompositionFromPlan(query, plan));
    } catch (const std::logic_error&) {
        metadata_growth_rejected = true;
    }
    require(
        metadata_growth_rejected,
        context + ": committed candidate payload cannot grow through a "
        "post-reservation metadata update");

    const CandidateBuildBudget remaining = ledger.remaining();
    require(
        remaining.adjacency_probes == 0 &&
            remaining.reverse_edge_lookups == 0 &&
            remaining.arc_checks == 0 &&
            remaining.relation_entries == 0 &&
            remaining.candidate_value_occurrences == 0 &&
            remaining.blocks == 0 &&
            remaining.semijoin_entry_visits == 0 &&
            remaining.index_entries == 0 &&
            remaining.payload_memory_bytes == 0 &&
            remaining.fixed_memory_headroom_bytes == 23 &&
            remaining.candidate_tree_tokens == 0,
        context + ": committed build retains exactly one complete debit");
}

void testTransactionalBuilderAndFailureRollback() {
    auto fixture = makeReciprocalQuoteFixture();
    Graph& data = fixture.first;
    Graph& query = fixture.second;
    const FixedPlan incumbent = makeFixedPlan(
        query, 0, {0, 1}, {-1, 0});
    TDTree incumbent_tree(
        data,
        query,
        decompositionFromPlan(query, incumbent),
        3);
    const CandidateCover cover = incumbent_tree.exportCandidateCover();
    const std::size_t incumbent_entries =
        incumbent_tree.candidateRelationCount();
    const auto incumbent_roots = incumbent_tree.rootCandidates();
    const FixedPlan candidate = makeFixedPlan(
        query, 1, {1, 0}, {1, -1});
    const std::uint64_t exact_matches =
        bruteForceDurableMappings(data, query, 3).size();
    verifySuccessfulBuild(
        data,
        query,
        cover,
        candidate,
        exact_matches,
        "reciprocal");
    require(
        incumbent_tree.executionPlanFingerprint() ==
                incumbent.fingerprint &&
            incumbent_tree.candidateRelationCount() ==
                incumbent_entries &&
            incumbent_tree.rootCandidates() == incumbent_roots,
        "successful candidate commit leaves incumbent bytes/logical state "
        "untouched");

    const CandidateBuildQuote quote = quoteCoverAwareBuild(
        data, query, cover, candidate);
    const CandidateBuildBudget budget = exactBudget(quote, 31);
    const std::vector<CoverAwareBuildFailurePoint> failures = {
        CoverAwareBuildFailurePoint::AllocationBeforeValidation,
        CoverAwareBuildFailurePoint::AfterRoot,
        CoverAwareBuildFailurePoint::AfterRelations,
        CoverAwareBuildFailurePoint::AfterBottomUp,
        CoverAwareBuildFailurePoint::AfterTopDown,
        CoverAwareBuildFailurePoint::AllocationAfterRoot};
    for (CoverAwareBuildFailurePoint failure : failures) {
        CandidateBuildLedger ledger(budget);
        CandidateBuildReservationAttempt attempt =
            tryReserveCandidateBuild(quote, ledger);
        CoverAwareBuildResult result =
            buildCoverAwareTDTreeForTesting(
                data,
                query,
                cover,
                candidate,
                std::move(attempt.reservation),
                failure);
        require(
            !result.complete() && result.tree == nullptr &&
                result.counters.partial_tree_handoffs == 0 &&
                (failure !=
                         CoverAwareBuildFailurePoint::AllocationBeforeValidation ||
                 result.status == CoverAwareBuildStatus::AllocationFailure) &&
                sameBudget(ledger.remaining(), budget),
            "injected phase/allocation failure refunds all units and hands "
            "off no partial TD-tree");
    }

    Graph copied_data = data;
    CandidateBuildLedger wrong_identity_ledger(budget);
    CandidateBuildReservationAttempt wrong_identity_attempt =
        tryReserveCandidateBuild(quote, wrong_identity_ledger);
    CoverAwareBuildResult wrong_identity = buildCoverAwareTDTree(
        copied_data,
        query,
        cover,
        candidate,
        std::move(wrong_identity_attempt.reservation));
    require(
        wrong_identity.status ==
                CoverAwareBuildStatus::SourceIdentityMismatch &&
            wrong_identity.tree == nullptr &&
            sameBudget(wrong_identity_ledger.remaining(), budget),
        "bad source identity fails transactionally with zero handoff");

    struct CapCase {
        const char* name;
        std::function<void(CandidateBuildBudget&)> reduce;
    };
    const std::vector<CapCase> cap_cases = {
        {"adjacency", [](CandidateBuildBudget& b) {
             --b.adjacency_probes;
         }},
        {"reverse", [](CandidateBuildBudget& b) {
             --b.reverse_edge_lookups;
         }},
        {"arc", [](CandidateBuildBudget& b) { --b.arc_checks; }},
        {"relation", [](CandidateBuildBudget& b) {
             --b.relation_entries;
         }},
        {"candidate", [](CandidateBuildBudget& b) {
             --b.candidate_value_occurrences;
         }},
        {"block", [](CandidateBuildBudget& b) { --b.blocks; }},
        {"semijoin", [](CandidateBuildBudget& b) {
             --b.semijoin_entry_visits;
         }},
        {"index", [](CandidateBuildBudget& b) {
             --b.index_entries;
         }},
        {"payload", [](CandidateBuildBudget& b) {
             --b.payload_memory_bytes;
         }},
        {"token", [](CandidateBuildBudget& b) {
             --b.candidate_tree_tokens;
         }}};
    for (const CapCase& cap_case : cap_cases) {
        CandidateBuildBudget reduced = budget;
        cap_case.reduce(reduced);
        CandidateBuildLedger ledger(reduced);
        CandidateBuildReservationAttempt attempt =
            tryReserveCandidateBuild(quote, ledger);
        require(
            !attempt.fit.fits && attempt.reservation == nullptr &&
                sameBudget(ledger.remaining(), reduced),
            std::string(cap_case.name) +
                " cap-minus-one rejects before build with no debit");
    }
}

void testAllLegalPlanAndCommonWindowParity() {
    const Graph data = makeTriangleData();
    const Graph query = makeTriangleQuery();
    const FixedPlan incumbent = makeFixedPlan(
        query, 0, {0, 1, 2}, {-1, 0, 1});
    TDTree tree(
        data,
        query,
        decompositionFromPlan(query, incumbent),
        3);
    const CandidateCover cover = tree.exportCandidateCover();
    const std::uint64_t exact_matches =
        bruteForceDurableMappings(data, query, 3).size();
    require(
        exact_matches == 1,
        "common-window fixture has one true mapping and one pairwise "
        "false positive");

    std::size_t candidates_built = 0;
    for (const FixedPlan& plan : enumerateLegalPlans(query)) {
        if (plan.fingerprint == incumbent.fingerprint) continue;
        verifySuccessfulBuild(
            data,
            query,
            cover,
            plan,
            exact_matches,
            "legal_plan_" + std::to_string(plan.fingerprint));
        ++candidates_built;
    }
    require(
        candidates_built > 1,
        "exhaustive legal-plan fixture materializes multiple "
        "non-incumbent complete plans");
}

void testSinkSelfLoopAndEmptyFixtures() {
    {
        Graph data;
        data.num_vertices = 6;
        data.vertex_labels = {
            labelFromString("A"), labelFromString("B"),
            labelFromString("C"), labelFromString("A"),
            labelFromString("B"), labelFromString("C")};
        data.adj.resize(6);
        data.in_adj.resize(6);
        addTemporalEdge(data, 0, 1, {{1, 4}});
        addTemporalEdge(data, 2, 1, {{1, 4}});
        addTemporalEdge(data, 3, 4, {{1, 4}});
        addTemporalEdge(data, 5, 4, {{10, 13}});
        finalizeDataGraph(data);
        const Graph query = makeQuery(
            {labelFromString("A"), labelFromString("B"),
             labelFromString("C")},
            {{0, 1}, {2, 1}});
        const FixedPlan incumbent = makeFixedPlan(
            query, 1, {1, 0, 2}, {1, -1, 1});
        TDTree tree(
            data,
            query,
            decompositionFromPlan(query, incumbent),
            3);
        const CandidateCover cover = tree.exportCandidateCover();
        const FixedPlan candidate = makeFixedPlan(
            query, 0, {0, 1, 2}, {-1, 0, 1});
        require(
            requiredArcMask(query, 1, 2) ==
                RequiredArcMask::ChildToParent,
            "sink candidate contains a parent-to-child expansion opposite "
            "the query arc");
        verifySuccessfulBuild(
            data,
            query,
            cover,
            candidate,
            bruteForceDurableMappings(data, query, 3).size(),
            "directed_sink");
    }

    {
        Graph data;
        data.num_vertices = 4;
        data.vertex_labels = {
            labelFromString("A"), labelFromString("B"),
            labelFromString("A"), labelFromString("B")};
        data.adj.resize(4);
        data.in_adj.resize(4);
        addTemporalEdge(data, 0, 0, {{1, 4}});
        addTemporalEdge(data, 0, 1, {{1, 4}});
        addTemporalEdge(data, 1, 1, {{1, 4}});
        addTemporalEdge(data, 2, 2, {{1, 4}});
        addTemporalEdge(data, 2, 3, {{1, 4}});
        addTemporalEdge(data, 3, 3, {{10, 13}});
        finalizeDataGraph(data);
        const Graph query = makeQuery(
            {labelFromString("A"), labelFromString("B")},
            {{0, 0}, {0, 1}, {1, 1}});
        const std::uint64_t exact_matches =
            bruteForceDurableMappings(data, query, 3).size();
        require(exact_matches == 1, "self-loop fixture oracle count");

        const FixedPlan incumbent_a = makeFixedPlan(
            query, 0, {0, 1}, {-1, 0});
        TDTree tree_a(
            data,
            query,
            decompositionFromPlan(query, incumbent_a),
            3);
        const CandidateCover cover_a = tree_a.exportCandidateCover();
        const FixedPlan root_b = makeFixedPlan(
            query, 1, {1, 0}, {1, -1});
        const CandidateBuildQuote root_b_quote = quoteCoverAwareBuild(
            data, query, cover_a, root_b);
        require(
            root_b_quote.maximum_root_self_loop_checks ==
                cover_a.domain(1).size(),
            "root self-loop quote covers exactly one check per root value");
        verifySuccessfulBuild(
            data,
            query,
            cover_a,
            root_b,
            exact_matches,
            "self_loop_root_b_nonroot_a");

        TDTree tree_b(
            data,
            query,
            decompositionFromPlan(query, root_b),
            3);
        const CandidateCover cover_b = tree_b.exportCandidateCover();
        verifySuccessfulBuild(
            data,
            query,
            cover_b,
            incumbent_a,
            exact_matches,
            "self_loop_root_a_nonroot_b");
    }

    {
        Graph data;
        data.num_vertices = 2;
        data.vertex_labels = {
            labelFromString("A"), labelFromString("B")};
        data.adj.resize(2);
        data.in_adj.resize(2);
        addTemporalEdge(data, 0, 1, {{1, 1}});
        finalizeDataGraph(data);
        const Graph query = makeQuery(
            {labelFromString("A"), labelFromString("B")},
            {{0, 1}});
        const FixedPlan incumbent = makeFixedPlan(
            query, 1, {1, 0}, {1, -1});
        TDTree tree(
            data,
            query,
            decompositionFromPlan(query, incumbent),
            2);
        const CandidateCover cover = tree.exportCandidateCover();
        require(
            cover.domain(0).empty() && cover.domain(1).empty(),
            "zero-match incumbent exports empty cover domains");
        const FixedPlan candidate = makeFixedPlan(
            query, 0, {0, 1}, {-1, 0});
        verifySuccessfulBuild(
            data, query, cover, candidate, 0, "empty_cover");
    }

    {
        Graph data;
        data.num_vertices = 4;
        data.vertex_labels = {
            labelFromString("A"), labelFromString("B"),
            labelFromString("C"), labelFromString("D")};
        data.adj.resize(4);
        data.in_adj.resize(4);
        finalizeDataGraph(data);
        const Graph query = makeQuery(
            {labelFromString("A"), labelFromString("B"),
             labelFromString("C"), labelFromString("D")},
            {{0, 1}, {1, 2}, {2, 3}, {0, 2}, {0, 3}});
        const FixedPlan incumbent = makeFixedPlan(
            query, 0, {0, 1, 2, 3}, {-1, 0, 1, 2});
        TDTree tree(
            data,
            query,
            decompositionFromPlan(query, incumbent),
            2);
        const CandidateCover cover = tree.exportCandidateCover();
        const FixedPlan branching = makeFixedPlan(
            query, 0, {0, 1, 2, 3}, {-1, 0, 0, 0});
        verifySuccessfulBuild(
            data,
            query,
            cover,
            branching,
            0,
            "branching_empty_cover_exact_child_reserve");
    }

    {
        Graph data;
        data.num_vertices = 8;
        data.vertex_labels = {
            labelFromString("A"), labelFromString("A"),
            labelFromString("B"), labelFromString("B"),
            labelFromString("C"), labelFromString("C"),
            labelFromString("C"), labelFromString("C")};
        data.adj.resize(8);
        data.in_adj.resize(8);
        addTemporalEdge(data, 0, 2, {{1, 4}});
        addTemporalEdge(data, 1, 3, {{1, 4}});
        addTemporalEdge(data, 2, 4, {{1, 4}});
        addTemporalEdge(data, 3, 4, {{1, 4}});
        addTemporalEdge(data, 0, 4, {{1, 4}});
        // A1 is retained through A1->B1->C0 in the incumbent, but its larger
        // A->C adjacency emits nothing because C1--C3 are outside D_C^0.
        addTemporalEdge(data, 1, 5, {{1, 4}});
        addTemporalEdge(data, 1, 6, {{1, 4}});
        addTemporalEdge(data, 1, 7, {{1, 4}});
        finalizeDataGraph(data);
        const Graph query = makeQuery(
            {labelFromString("A"), labelFromString("B"),
             labelFromString("C")},
            {{0, 1}, {1, 2}, {0, 2}});
        const FixedPlan incumbent = makeFixedPlan(
            query, 0, {0, 1, 2}, {-1, 0, 1});
        TDTree tree(
            data,
            query,
            decompositionFromPlan(query, incumbent),
            3);
        const CandidateCover cover = tree.exportCandidateCover();
        require(
            cover.domain(0).size() == 2 &&
                cover.domain(2).size() == 1,
            "transient fixture retains the high-degree parent but excludes "
            "all three of its C decoys");
        const FixedPlan candidate = makeFixedPlan(
            query, 0, {0, 1, 2}, {-1, 0, 0});
        const CandidateBuildQuote quote = quoteCoverAwareBuild(
            data, query, cover, candidate);
        CandidateBuildLedger ledger(exactBudget(quote, 19));
        CoverAwareBuildResult result = buildWithExactBudget(
            data, query, cover, candidate, ledger, quote);
        require(
            result.complete() &&
                result.counters.peak_payload_capacity_bytes >
                    result.counters.final_payload_capacity_bytes,
            "non-emitting high-degree transient block capacity is included "
            "in peak payload telemetry");
    }
}

void testValidatedSnapshotAuthorityAndExternalHandoff() {
    Graph data = makeTriangleData();
    Graph query = makeTriangleQuery();
    Label* retained_label = &data.vertex_labels.front();
    Edge* retained_edge = &data.adj.front().front();
    TemporalEdge* retained_temporal_edge =
        &data.temporal_edges.front();
    Label* retained_query_label = &query.vertex_labels.front();

    const GraphValidationQuote graph_quote =
        quoteGraphValidation(data, query);
    require(
        graph_quote.complete && !graph_quote.overflowed &&
            graph_quote.exact_budget.data_vertices == 6 &&
            graph_quote.exact_budget.query_vertices == 3 &&
            graph_quote.exact_budget.temporal_edges ==
                data.temporal_edges.size() &&
            graph_quote.exact_budget.combined_outgoing_adjacency_entries ==
                data.temporal_edges.size() + 3 &&
            graph_quote.exact_budget.combined_incoming_adjacency_entries ==
                data.temporal_edges.size() + 3 &&
            graph_quote.exact_budget.derived_incident_interval_visits ==
                2 * graph_quote.exact_budget.interval_records &&
            graph_quote.exact_budget.derived_neighbor_label_updates ==
                2 * data.temporal_edges.size() &&
            graph_quote.storage_quote_graphs_visited == 2 &&
            graph_quote.snapshot_owner_control_bytes > 0,
        "native pre-seal quote derives exact structural/storage caps");
    GraphSnapshotSealResult sealed = ValidatedGraphSnapshot::seal(
        data, query, graph_quote.exact_budget);
    require(sealed.complete(), "validated snapshot seals complete");
    require(
        sealed.receipt.graph_validation_calls == 1 &&
            sealed.receipt.storage_quote_passes == 1 &&
            sealed.receipt.storage_quote_graphs_visited == 2 &&
            sealed.receipt.storage_accounting_passes == 2 &&
            sealed.receipt.storage_accounting_graphs_visited == 4 &&
            sealed.receipt.data_fingerprint_passes == 1 &&
            sealed.receipt.query_fingerprint_passes == 1 &&
            sealed.receipt.derived_metadata_rebuild_passes == 1,
        "snapshot reports every quote/accounting/validation pass exactly");
    require(
        sealed.receipt.quoted_seal_peak_live_bytes >=
                sealed.receipt.seal_peak_live_bytes &&
            sealed.receipt.quoted_snapshot_storage_bytes >=
                sealed.receipt.snapshot_storage_bytes &&
            sealed.receipt.quoted_validation_temporary_bytes >=
                sealed.receipt.temporary_validation_capacity_bytes &&
            sealed.receipt.source_live_storage_bytes > 0 &&
            sealed.receipt.snapshot_storage_bytes > 0 &&
            sealed.receipt.snapshot_owner_control_bytes > 0 &&
            sealed.receipt.quoted_snapshot_storage_bytes ==
                graph_quote.exact_budget.snapshot_storage_bytes &&
            sealed.receipt.quoted_validation_temporary_bytes ==
                graph_quote.exact_budget.temporary_validation_bytes &&
            sealed.receipt.quoted_seal_peak_live_bytes ==
                graph_quote.exact_budget.seal_peak_live_bytes,
        "snapshot reports and consumes the exact native pre-seal quote");
    GraphValidationBudget cap_minus_one =
        GraphValidationBudget::unbounded();
    cap_minus_one.seal_peak_live_bytes =
        sealed.receipt.quoted_seal_peak_live_bytes - 1;
    const GraphSnapshotSealResult rejected_peak =
        ValidatedGraphSnapshot::seal(
            makeTriangleData(), makeTriangleQuery(), cap_minus_one);
    require(
        rejected_peak.receipt.status ==
                GraphValidationStatus::BudgetExceeded &&
            rejected_peak.snapshot == nullptr &&
            rejected_peak.receipt.graph_validation_calls == 0,
        "seal peak cap-minus-one rejects before clone/validation");
    auto require_quote_cap_minus_one = [&](int dimension) {
        GraphValidationBudget budget = graph_quote.exact_budget;
        std::uint64_t* target = nullptr;
        switch (dimension) {
            case 0: target = &budget.data_vertices; break;
            case 1: target = &budget.query_vertices; break;
            case 2:
                target = &budget.combined_outgoing_adjacency_entries;
                break;
            case 3:
                target = &budget.combined_incoming_adjacency_entries;
                break;
            case 4: target = &budget.temporal_edges; break;
            case 5: target = &budget.interval_records; break;
            case 6: target = &budget.derived_incident_interval_visits; break;
            case 7: target = &budget.derived_neighbor_label_updates; break;
            case 8: target = &budget.snapshot_storage_bytes; break;
            case 9: target = &budget.temporary_validation_bytes; break;
            case 10: target = &budget.seal_peak_live_bytes; break;
            default: break;
        }
        require(target != nullptr && *target > 0,
            "quoted nonzero cap dimension exists");
        --*target;
        const GraphSnapshotSealResult rejected =
            ValidatedGraphSnapshot::seal(
                makeTriangleData(), makeTriangleQuery(), budget);
        require(
            !rejected.complete() && rejected.snapshot == nullptr &&
                rejected.receipt.status ==
                    GraphValidationStatus::BudgetExceeded,
            "every native graph quote cap-minus-one fails closed");
    };
    for (int dimension = 0; dimension < 11; ++dimension) {
        require_quote_cap_minus_one(dimension);
    }

    Graph empty_data;
    empty_data.num_vertices = 0;
    const GraphValidationQuote empty_quote =
        quoteGraphValidation(empty_data, makeTriangleQuery());
    require(
        empty_quote.complete &&
            empty_quote.exact_budget.data_vertices == 0 &&
            empty_quote.exact_budget.temporal_edges == 0 &&
            empty_quote.exact_budget.interval_records == 0 &&
            empty_quote.exact_budget.derived_incident_interval_visits == 0 &&
            empty_quote.exact_budget.temporary_validation_bytes == 0 &&
            empty_quote.exact_budget.snapshot_storage_bytes > 0 &&
            empty_quote.exact_budget.seal_peak_live_bytes > 0,
        "empty filtered data has a legal zero-work/zero-temporary quote");
    const GraphSnapshotSealResult empty_sealed =
        ValidatedGraphSnapshot::seal(
            empty_data, makeTriangleQuery(), empty_quote.exact_budget);
    require(
        empty_sealed.complete() &&
            empty_sealed.receipt.temporary_validation_capacity_bytes == 0,
        "empty filtered graph seals under its exact native quote");

    Graph temporal_query = makeTriangleQuery();
    temporal_query.temporal_edges.push_back(
        TemporalEdge(0, 1, {{1, 1}}, 1));
    const GraphValidationQuote invalid_query_quote =
        quoteGraphValidation(data, temporal_query);
    require(
        !invalid_query_quote.complete &&
            !invalid_query_quote.overflowed &&
            invalid_query_quote.status ==
                GraphValidationQuoteStatus::InvalidQueryGraph,
        "query temporal history is a typed invalid-query quote");
    const GraphSnapshotSealResult invalid_query_seal =
        ValidatedGraphSnapshot::seal(
            data, temporal_query, GraphValidationBudget::unbounded());
    require(
        !invalid_query_seal.complete() &&
            invalid_query_seal.receipt.status ==
                GraphValidationStatus::InvalidQueryGraph,
        "quote and seal agree that query temporal history is invalid");

    const std::uint64_t data_fingerprint =
        sealed.snapshot->dataFingerprint();
    const std::uint64_t query_fingerprint =
        sealed.snapshot->queryFingerprint();
    *retained_label = labelFromString("E");
    retained_edge->to = 5;
    retained_temporal_edge->u = 5;
    *retained_query_label = labelFromString("E");
    require(
        sealed.snapshot->dataFingerprint() == data_fingerprint &&
            sealed.snapshot->queryFingerprint() == query_fingerprint &&
            sealed.snapshot->data().vertex_labels.front() ==
                labelFromString("A") &&
            sealed.snapshot->data().adj.front().front().to == 1 &&
            sealed.snapshot->data().temporal_edges.front().u == 0 &&
            sealed.snapshot->query().vertex_labels.front() ==
                labelFromString("A"),
        "pre-seal mutable source aliases cannot mutate snapshot storage");

    std::shared_ptr<const ValidatedGraphSnapshot> second =
        ValidatedGraphSnapshot::seal(
            makeTriangleData(), makeTriangleQuery(),
            GraphValidationBudget::unbounded()).snapshot;
    std::shared_ptr<const ValidatedGraphSnapshot> owner_alias(
        sealed.snapshot, second.get());
    require(
        !sharesValidatedGraphSnapshot(
            sealed.snapshot, owner_alias),
        "same control block with another pointee has no authority");
    std::shared_ptr<const ValidatedGraphSnapshot> address_alias(
        sealed.snapshot.get(),
        [](const ValidatedGraphSnapshot*) {});
    require(
        !sharesValidatedGraphSnapshot(
            sealed.snapshot, address_alias),
        "same address with another control block has no authority");

    const FixedPlan incumbent = makeFixedPlan(
        *sealed.snapshot, 0, {0, 1, 2}, {-1, 0, 1});
    std::unique_ptr<TDTree> incumbent_tree =
        TDTree::buildValidatedIncumbent(
            sealed.snapshot,
            decompositionFromPlan(
                sealed.snapshot->query(), incumbent),
            3);
    const CandidateCover cover =
        incumbent_tree->exportCandidateCover();
    require(
        cover.hasValidatedSnapshotAuthority() &&
            cover.snapshotToken() ==
                sealed.snapshot->snapshotToken(),
        "validated incumbent exports an owning authority cover");
    const FixedPlan candidate = makeFixedPlan(
        *sealed.snapshot, 2, {2, 1, 0}, {1, 2, -1});

    resetQueryGraphContentFingerprintCallCount();
    resetTemporalGraphContentFingerprintCallCount();
    const CandidateBuildQuote quote = quoteCoverAwareBuild(
        sealed.snapshot, cover, candidate);
    require(
        quote.complete && quote.validatedSnapshotBound() &&
            quote.graph_validation_rescans == 0 &&
            quote.data_fingerprint_rescans == 0 &&
            quote.query_fingerprint_rescans == 0 &&
            queryGraphContentFingerprintCallCount() == 0 &&
            temporalGraphContentFingerprintCallCount() == 0,
        "snapshot quote performs zero validation/fingerprint rescans");
    CandidateBuildLedger ledger(exactBudget(quote, 1));
    CandidateBuildReservationAttempt reservation =
        tryReserveCandidateBuild(quote, ledger);
    CoverAwareBuildResult built = buildCoverAwareTDTree(
        sealed.snapshot,
        cover,
        candidate,
        std::move(reservation.reservation));
    require(
        built.complete() &&
            built.tree->hasValidatedSnapshotAuthority() &&
            built.tree->validationSnapshotToken() ==
                sealed.snapshot->snapshotToken() &&
            built.counters.graph_validation_rescans == 0 &&
            built.counters.data_fingerprint_rescans == 0 &&
            built.counters.query_fingerprint_rescans == 0 &&
            queryGraphContentFingerprintCallCount() == 0 &&
            temporalGraphContentFingerprintCallCount() == 0,
        "snapshot materializer performs zero hidden fingerprint rescans");

    SelectedPlanMetadata metadata;
    metadata.reporting_mode = SelectivityMode::TemporalCost;
    metadata.root_scores = {1.0, 2.0, 3.0};
    metadata.reporting_selectivity = {0.1, 0.2, 0.3};
    metadata.candidate_counts = {2, 2, 2};
    metadata.decision = PlannerDecision::CandidateSelected;
    metadata.diagnostics.complete_plan_proposals = 2;
    metadata.diagnostics.scored_plan_proposals = 1;
    metadata.diagnostics.materialized_complete_plans = 1;
    metadata.diagnostics.rescored_complete_plans = 1;
    metadata.diagnostics.graph_validation_passes = 1;
    const V3MetadataQuote metadata_quote = quoteV3PlanHandoff(
        sealed.snapshot,
        *built.tree,
        candidate,
        metadata,
        std::numeric_limits<std::uint64_t>::max());
    require(
        metadata_quote.complete &&
            metadata_quote.isBoundToValidatedSnapshot(sealed.snapshot) &&
            !metadata_quote.isBoundToValidatedSnapshot(second) &&
            metadata_quote.snapshot_token ==
                sealed.snapshot->snapshotToken() &&
            metadata_quote.data_fingerprint ==
                sealed.snapshot->dataFingerprint() &&
            metadata_quote.query_fingerprint ==
                sealed.snapshot->queryFingerprint(),
        "external metadata quote is complete and exact-owner bound");
    require(
        metadata_quote.metadata_fingerprint != 0,
        "external metadata quote binds a nonzero content fingerprint");

    SelectedPlanMetadata nan_metadata = metadata;
    nan_metadata.root_scores[0] =
        std::numeric_limits<double>::quiet_NaN();
    require(
        quoteV3PlanHandoff(
            sealed.snapshot, *built.tree, candidate,
            nan_metadata,
            std::numeric_limits<std::uint64_t>::max()).status ==
                V3MetadataQuoteStatus::InvalidDiagnostics,
        "NaN metadata fails closed");
    SelectedPlanMetadata inf_metadata = metadata;
    inf_metadata.reporting_selectivity[0] =
        std::numeric_limits<double>::infinity();
    require(
        quoteV3PlanHandoff(
            sealed.snapshot, *built.tree, candidate,
            inf_metadata,
            std::numeric_limits<std::uint64_t>::max()).status ==
                V3MetadataQuoteStatus::InvalidDiagnostics,
        "infinite metadata fails closed");
    SelectedPlanMetadata invalid_enum_metadata = metadata;
    invalid_enum_metadata.reporting_mode =
        static_cast<SelectivityMode>(255);
    require(
        quoteV3PlanHandoff(
            sealed.snapshot, *built.tree, candidate,
            invalid_enum_metadata,
            std::numeric_limits<std::uint64_t>::max()).status ==
                V3MetadataQuoteStatus::InvalidDiagnostics,
        "invalid reporting enum fails closed");
    SelectedPlanMetadata invalid_decision_metadata = metadata;
    invalid_decision_metadata.decision =
        static_cast<PlannerDecision>(255);
    require(
        quoteV3PlanHandoff(
            sealed.snapshot, *built.tree, candidate,
            invalid_decision_metadata,
            std::numeric_limits<std::uint64_t>::max()).status ==
                V3MetadataQuoteStatus::InvalidDiagnostics,
        "invalid decision enum fails closed");

    SelectedPlanMetadata invalid_shape = metadata;
    invalid_shape.root_scores.pop_back();
    require(
        quoteV3PlanHandoff(
            sealed.snapshot, *built.tree, candidate,
            invalid_shape,
            std::numeric_limits<std::uint64_t>::max()).status ==
                V3MetadataQuoteStatus::VectorShapeMismatch,
        "metadata vectors must have q-or-zero shape");
    require(
        quoteV3PlanHandoff(
            sealed.snapshot, *built.tree, candidate, metadata,
            metadata_quote.payload_bytes - 1).status ==
                V3MetadataQuoteStatus::PayloadCapExceeded,
        "metadata cap-minus-one rejects before allocation");

    SelectedPlanMetadata tampered_metadata = metadata;
    tampered_metadata.root_scores[0] += 1.0;
    V3MetadataBudget tampered_budget{
        metadata_quote.payload_bytes, 1};
    std::unique_ptr<TDTree> tampered_tree = std::move(built.tree);
    V3PlanHandoffResult tampered = commitV3PlanHandoff(
        sealed.snapshot,
        tampered_tree,
        candidate,
        tampered_metadata,
        metadata_quote,
        tampered_budget);
    require(
        tampered.status ==
                V3PlanHandoffStatus::QuoteContractMismatch &&
            tampered_budget.payload_bytes ==
                metadata_quote.payload_bytes &&
            tampered_budget.receipt_tokens == 1,
        "post-quote metadata tamper fails before debit");
    built.tree = std::move(tampered_tree);

    TDTree* tree_identity = built.tree.get();
    auto requireMetadataTamperRejected = [&](const SelectedPlanMetadata& changed,
                                             const std::string& context) {
        V3MetadataBudget budget{
            metadata_quote.payload_bytes, 1};
        V3PlanHandoffResult rejected = commitV3PlanHandoff(
            sealed.snapshot,
            built.tree,
            candidate,
            changed,
            metadata_quote,
            budget);
        require(
            rejected.status ==
                    V3PlanHandoffStatus::QuoteContractMismatch &&
                built.tree.get() == tree_identity &&
                budget.payload_bytes == metadata_quote.payload_bytes &&
                budget.receipt_tokens == 1,
            context);
    };
    SelectedPlanMetadata changed_selectivity = metadata;
    changed_selectivity.reporting_selectivity[1] += 0.25;
    requireMetadataTamperRejected(
        changed_selectivity,
        "post-quote selectivity tamper fails before debit");
    SelectedPlanMetadata changed_counts = metadata;
    ++changed_counts.candidate_counts[1];
    requireMetadataTamperRejected(
        changed_counts,
        "post-quote candidate-count tamper fails before debit");
    SelectedPlanMetadata changed_diagnostics = metadata;
    ++changed_diagnostics.diagnostics.complete_plan_proposals;
    requireMetadataTamperRejected(
        changed_diagnostics,
        "post-quote diagnostics tamper fails before debit");
    SelectedPlanMetadata changed_decision = metadata;
    changed_decision.decision = PlannerDecision::IncumbentOnly;
    requireMetadataTamperRejected(
        changed_decision,
        "post-quote decision tamper fails before debit");
    SelectedPlanMetadata changed_mode = metadata;
    changed_mode.reporting_mode = SelectivityMode::Paper;
    requireMetadataTamperRejected(
        changed_mode,
        "post-quote reporting-mode tamper fails before debit");
    FixedPlan changed_plan = candidate;
    std::swap(changed_plan.order[0], changed_plan.order[1]);
    V3MetadataBudget changed_plan_budget{
        metadata_quote.payload_bytes, 1};
    V3PlanHandoffResult changed_plan_result = commitV3PlanHandoff(
        sealed.snapshot,
        built.tree,
        changed_plan,
        metadata,
        metadata_quote,
        changed_plan_budget);
    require(
        changed_plan_result.status ==
                V3PlanHandoffStatus::QuoteContractMismatch &&
            built.tree.get() == tree_identity,
        "post-quote plan-vector tamper fails before debit");
    V3MetadataQuote corrupted_quote = metadata_quote;
    ++corrupted_quote.payload_bytes;
    V3MetadataBudget corrupted_quote_budget{
        corrupted_quote.payload_bytes, 1};
    V3PlanHandoffResult corrupted_quote_result = commitV3PlanHandoff(
        sealed.snapshot,
        built.tree,
        candidate,
        metadata,
        corrupted_quote,
        corrupted_quote_budget);
    require(
        corrupted_quote_result.status ==
                V3PlanHandoffStatus::QuoteContractMismatch &&
            built.tree.get() == tree_identity,
        "corrupt metadata quote seal fails before debit");

    std::unique_ptr<TDTree> wrong_k_tree =
        TDTree::buildValidatedIncumbent(
            sealed.snapshot,
            decompositionFromPlan(
                sealed.snapshot->query(), candidate),
            2);
    V3MetadataBudget wrong_k_budget{
        metadata_quote.payload_bytes, 1};
    V3PlanHandoffResult wrong_k = commitV3PlanHandoff(
        sealed.snapshot,
        wrong_k_tree,
        candidate,
        metadata,
        metadata_quote,
        wrong_k_budget);
    require(
        wrong_k.status == V3PlanHandoffStatus::TreePlanMismatch &&
            wrong_k_budget.payload_bytes ==
                metadata_quote.payload_bytes,
        "same snapshot and plan with different k cannot substitute");

    std::shared_ptr<const ValidatedGraphSnapshot> alias_owner(
        sealed.snapshot, second.get());
    require(
        quoteV3PlanHandoff(
            alias_owner, *built.tree, candidate, metadata,
            std::numeric_limits<std::uint64_t>::max()).status ==
                V3MetadataQuoteStatus::MissingSnapshotAuthority,
        "aliasing owner cannot quote a handoff");
    require(
        quoteV3PlanHandoff(
            address_alias, *built.tree, candidate, metadata,
            std::numeric_limits<std::uint64_t>::max()).status ==
                V3MetadataQuoteStatus::MissingSnapshotAuthority,
        "same-address different-control owner cannot quote a handoff");

    std::unique_ptr<TDTree> independent_tree =
        TDTree::buildValidatedIncumbent(
            second,
            decompositionFromPlan(second->query(), candidate),
            3);
    require(
        quoteV3PlanHandoff(
            sealed.snapshot, *independent_tree, candidate, metadata,
            std::numeric_limits<std::uint64_t>::max()).status ==
                V3MetadataQuoteStatus::MissingSnapshotAuthority,
        "identical-content independently sealed tree cannot be quoted by A");

    TDTree* const independent_tree_identity = independent_tree.get();
    V3MetadataBudget independent_budget{
        metadata_quote.payload_bytes, 1};
    V3PlanHandoffResult independent_commit = commitV3PlanHandoff(
        second,
        independent_tree,
        candidate,
        metadata,
        metadata_quote,
        independent_budget);
    require(
        independent_commit.status ==
                V3PlanHandoffStatus::QuoteContractMismatch &&
            independent_tree.get() == independent_tree_identity &&
            independent_budget.payload_bytes ==
                metadata_quote.payload_bytes &&
            independent_budget.receipt_tokens == 1,
        "quote from A cannot commit an identical-content tree from B");

    V3MetadataBudget owner_alias_budget{
        metadata_quote.payload_bytes, 1};
    V3PlanHandoffResult owner_alias_commit = commitV3PlanHandoff(
        alias_owner,
        built.tree,
        candidate,
        metadata,
        metadata_quote,
        owner_alias_budget);
    require(
        owner_alias_commit.status ==
                V3PlanHandoffStatus::QuoteContractMismatch &&
            built.tree.get() == tree_identity &&
            owner_alias_budget.payload_bytes ==
                metadata_quote.payload_bytes &&
            owner_alias_budget.receipt_tokens == 1,
        "same-control-block different-pointee alias cannot commit A quote");

    V3MetadataBudget address_alias_budget{
        metadata_quote.payload_bytes, 1};
    V3PlanHandoffResult address_alias_commit = commitV3PlanHandoff(
        address_alias,
        built.tree,
        candidate,
        metadata,
        metadata_quote,
        address_alias_budget);
    require(
        address_alias_commit.status ==
                V3PlanHandoffStatus::QuoteContractMismatch &&
            built.tree.get() == tree_identity &&
            address_alias_budget.payload_bytes ==
                metadata_quote.payload_bytes &&
            address_alias_budget.receipt_tokens == 1,
        "same-pointee different-control-block alias cannot commit A quote");

    V3MetadataQuote identity_tampered_quote = metadata_quote;
    ++identity_tampered_quote.data_fingerprint;
    V3MetadataBudget identity_tampered_budget{
        metadata_quote.payload_bytes, 1};
    V3PlanHandoffResult identity_tampered_commit =
        commitV3PlanHandoff(
            sealed.snapshot,
            built.tree,
            candidate,
            metadata,
            identity_tampered_quote,
            identity_tampered_budget);
    require(
        identity_tampered_commit.status ==
                V3PlanHandoffStatus::QuoteContractMismatch &&
            built.tree.get() == tree_identity &&
            identity_tampered_budget.payload_bytes ==
                metadata_quote.payload_bytes &&
            identity_tampered_budget.receipt_tokens == 1,
        "post-quote graph identity tamper fails before debit");

    V3MetadataBudget failed_budget{
        metadata_quote.payload_bytes - 1, 1};
    std::unique_ptr<TDTree> handoff_tree = std::move(built.tree);
    V3PlanHandoffResult failed_handoff = commitV3PlanHandoff(
        sealed.snapshot,
        handoff_tree,
        candidate,
        metadata,
        metadata_quote,
        failed_budget);
    require(
        !failed_handoff.complete() &&
            handoff_tree.get() == tree_identity &&
            failed_budget.payload_bytes ==
                metadata_quote.payload_bytes - 1 &&
            failed_budget.receipt_tokens == 1,
        "metadata budget rejection is transactional");

    V3MetadataBudget injected_budget{
        metadata_quote.payload_bytes, 1};
    setV3PlanHandoffAllocationFailureCountdown(0);
    V3PlanHandoffResult injected = commitV3PlanHandoff(
        sealed.snapshot,
        handoff_tree,
        candidate,
        metadata,
        metadata_quote,
        injected_budget);
    setV3PlanHandoffAllocationFailureCountdown(-1);
    require(
        injected.status == V3PlanHandoffStatus::AllocationFailure &&
            handoff_tree.get() == tree_identity &&
            injected_budget.payload_bytes ==
                metadata_quote.payload_bytes &&
            injected_budget.receipt_tokens == 1,
        "injected metadata allocation failure is transactional");

    for (std::int64_t phase = 1; phase <= 5; ++phase) {
        V3MetadataBudget phase_budget{
            metadata_quote.payload_bytes, 1};
        setV3PlanHandoffAllocationFailureCountdown(phase);
        V3PlanHandoffResult phase_failure = commitV3PlanHandoff(
            sealed.snapshot,
            handoff_tree,
            candidate,
            metadata,
            metadata_quote,
            phase_budget);
        setV3PlanHandoffAllocationFailureCountdown(-1);
        require(
                phase_failure.status ==
                    V3PlanHandoffStatus::AllocationFailure &&
                handoff_tree.get() == tree_identity &&
                phase_budget.payload_bytes ==
                    metadata_quote.payload_bytes &&
                phase_budget.receipt_tokens == 1,
            "each receipt copy allocation failure is transactional");
    }

    V3MetadataBudget exact_metadata_budget{
        metadata_quote.payload_bytes, 1};
    V3PlanHandoffResult committed = commitV3PlanHandoff(
        sealed.snapshot,
        handoff_tree,
        candidate,
        metadata,
        metadata_quote,
        exact_metadata_budget);
    require(
        committed.complete() &&
            exact_metadata_budget.payload_bytes == 0 &&
            exact_metadata_budget.receipt_tokens == 0 &&
            committed.receipt->plan().fingerprint ==
                committed.selected_tree->executionPlanFingerprint() &&
            committed.receipt->rootScores().size() == 3 &&
            committed.receipt->observedPayloadBytes() <=
                committed.receipt->payloadBytes() &&
            committed.receipt->payloadBytes() <=
                committed.receipt->payloadCap() &&
            committed.receipt->snapshotToken() ==
                sealed.snapshot->snapshotToken(),
        "bounded external receipt commits exact plan identity");

    incumbent_tree.reset();
    second.reset();
    sealed.snapshot.reset();
    data = Graph{};
    query = Graph{};
    require(
        committed.selected_tree->validationSnapshotToken() != 0 &&
            committed.receipt->snapshotToken() ==
                committed.selected_tree->validationSnapshotToken() &&
            committed.selected_tree->candidateRelationCount() > 0,
        "returned tree and receipt retain snapshot after source/scope death");
    require(
        countCandidateTree(
            *committed.selected_tree,
            "validated_owner_lifetime") == 1,
        "returned tree enumerates after raw sources and snapshot handle die");
}

void testValidatedSnapshotConcurrentQuoteAndMaterialize() {
    Graph data = makeTriangleData();
    Graph query = makeTriangleQuery();
    GraphSnapshotSealResult sealed = ValidatedGraphSnapshot::seal(
        data, query, GraphValidationBudget::unbounded());
    require(sealed.complete(), "concurrency snapshot seals");
    const FixedPlan incumbent = makeFixedPlan(
        *sealed.snapshot, 0, {0, 1, 2}, {-1, 0, 1});
    std::unique_ptr<TDTree> incumbent_tree =
        TDTree::buildValidatedIncumbent(
            sealed.snapshot,
            decompositionFromPlan(
                sealed.snapshot->query(), incumbent),
            3);
    const CandidateCover cover =
        incumbent_tree->exportCandidateCover();
    const FixedPlan candidate = makeFixedPlan(
        *sealed.snapshot, 2, {2, 1, 0}, {1, 2, -1});
    std::vector<std::thread> workers;
    std::array<std::atomic<bool>, 8> results{};
    for (auto& result : results) result.store(false);
    for (std::size_t worker = 0; worker < results.size(); ++worker) {
        workers.emplace_back([&, worker]() {
            const CandidateBuildQuote quote = quoteCoverAwareBuild(
                sealed.snapshot, cover, candidate);
            CandidateBuildLedger ledger(exactBudget(quote, 1));
            CandidateBuildReservationAttempt reservation =
                tryReserveCandidateBuild(quote, ledger);
            CoverAwareBuildResult built = buildCoverAwareTDTree(
                sealed.snapshot,
                cover,
                candidate,
                std::move(reservation.reservation));
            results[worker].store(built.complete() &&
                built.tree->validationSnapshotToken() ==
                    sealed.snapshot->snapshotToken());
        });
    }
    for (std::thread& worker : workers) worker.join();
    require(
        std::all_of(results.begin(), results.end(),
                    [](const std::atomic<bool>& value) {
                        return value.load();
                    }),
        "immutable snapshot supports concurrent quote/materialize");
}

void testValidatedSnapshotEscapedTreeLifetime() {
    std::unique_ptr<TDTree> escaped_tree;
    std::unique_ptr<SelectedPlanReceipt> escaped_receipt;
    {
        Graph data = makeTriangleData();
        Graph query = makeTriangleQuery();
        GraphSnapshotSealResult sealed =
            ValidatedGraphSnapshot::seal(
                data, query,
                GraphValidationBudget::unbounded());
        require(sealed.complete(), "escaped snapshot seals");
        const FixedPlan incumbent = makeFixedPlan(
            *sealed.snapshot, 0, {0, 1, 2}, {-1, 0, 1});
        std::unique_ptr<TDTree> incumbent_tree =
            TDTree::buildValidatedIncumbent(
                sealed.snapshot,
                decompositionFromPlan(
                    sealed.snapshot->query(), incumbent),
                3);
        const CandidateCover cover =
            incumbent_tree->exportCandidateCover();
        const FixedPlan candidate = makeFixedPlan(
            *sealed.snapshot, 2, {2, 1, 0}, {1, 2, -1});
        const CandidateBuildQuote quote = quoteCoverAwareBuild(
            sealed.snapshot, cover, candidate);
        CandidateBuildLedger ledger(exactBudget(quote, 1));
        CandidateBuildReservationAttempt reservation =
            tryReserveCandidateBuild(quote, ledger);
        CoverAwareBuildResult built = buildCoverAwareTDTree(
            sealed.snapshot,
            cover,
            candidate,
            std::move(reservation.reservation));
        require(built.complete(), "escaped candidate builds");

        SelectedPlanMetadata metadata;
        metadata.reporting_mode = SelectivityMode::TemporalCost;
        metadata.decision = PlannerDecision::CandidateSelected;
        metadata.diagnostics.complete_plan_proposals = 1;
        metadata.diagnostics.scored_plan_proposals = 1;
        metadata.diagnostics.materialized_complete_plans = 1;
        metadata.diagnostics.rescored_complete_plans = 1;
        metadata.diagnostics.graph_validation_passes = 1;
        const V3MetadataQuote metadata_quote = quoteV3PlanHandoff(
            sealed.snapshot,
            *built.tree,
            candidate,
            metadata,
            std::numeric_limits<std::uint64_t>::max());
        V3MetadataBudget metadata_budget{
            metadata_quote.payload_bytes, 1};
        V3PlanHandoffResult handoff = commitV3PlanHandoff(
            sealed.snapshot,
            built.tree,
            candidate,
            metadata,
            metadata_quote,
            metadata_budget);
        require(handoff.complete(), "escaped handoff commits");
        escaped_tree = std::move(handoff.selected_tree);
        escaped_receipt = std::move(handoff.receipt);
        // Every raw graph, explicit snapshot handle, incumbent, cover, quote,
        // reservation and build result dies at this scope boundary.
    }
    require(
        escaped_tree != nullptr && escaped_receipt != nullptr &&
            escaped_receipt->snapshotToken() ==
                escaped_tree->validationSnapshotToken() &&
            countCandidateTree(
                *escaped_tree,
                "validated_escaped_lifetime") == 1,
        "candidate tree executes after every source/planner owner dies");
}

} // namespace

int main() {
    try {
        testMaximumConsecutiveRunCache(
            std::filesystem::temp_directory_path());
        testCandidateCoverContainmentAndIdentity();
        testNativeCapacityOverflowHooks();
        testRandomDirectedSinkCoverContainment();
        testFixedPlanLegalityRejections();
        testBuildQuoteAndBudgetBoundaries();
        testTransactionalBuilderAndFailureRollback();
        testAllLegalPlanAndCommonWindowParity();
        testSinkSelfLoopAndEmptyFixtures();
        testValidatedSnapshotAuthorityAndExternalHandoff();
        testValidatedSnapshotConcurrentQuoteAndMaterialize();
        testValidatedSnapshotEscapedTreeLifetime();
        std::cout << "All TCC-v3 core feasibility tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
