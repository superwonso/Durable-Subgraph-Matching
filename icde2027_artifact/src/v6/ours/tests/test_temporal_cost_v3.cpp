#include "../TDTree.h"
#include "../Utils.h"
#include "../cover_aware_tdtree_builder.h"
#include "../fixed_plan_feasibility.h"
#include "../query_decomposition.h"
#include "../temporal_cost_v3_planner.h"
#include "../validated_graph_snapshot.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void addQueryArc(Graph& query, int source, int target) {
    query.adj[static_cast<std::size_t>(source)].push_back(
        {target, -1});
    query.in_adj[static_cast<std::size_t>(target)].push_back(
        {source, -1});
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
    query.in_adj.resize(static_cast<std::size_t>(query.num_vertices));
    for (const auto& arc : arcs) {
        addQueryArc(query, arc.first, arc.second);
    }
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
    const int edge_id = static_cast<int>(
        graph.temporal_edges.size());
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
        for (const Edge& edge : graph.adj[vertex]) {
            const Label label = graph.vertex_labels[
                static_cast<std::size_t>(edge.to)];
            ++graph.neighbor_label_counts[vertex][label];
        }
        for (const Edge& edge : graph.in_adj[vertex]) {
            const Label label = graph.vertex_labels[
                static_cast<std::size_t>(edge.to)];
            ++graph.incoming_neighbor_label_counts[vertex][label];
        }
    }
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
    std::sort(matches.begin(), matches.end());
    return matches;
}

int compactIdForExternal(const Graph& data, int external_id) {
    for (int data_vertex = 0;
         data_vertex < data.num_vertices;
         ++data_vertex) {
        if (data.externalId(data_vertex) == external_id) {
            return data_vertex;
        }
    }
    return -1;
}

std::vector<std::vector<int>> executeMappings(
    const TDTree& tree,
    const Graph& data,
    int query_vertices,
    const std::string& stem) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("dsm_v3_digest_" + stem + ".txt");
    const MatchSummary summary = tree.save_res(
        path.string(), MatchOutputMode::Full, 1);
    require(
        summary.output_written && !summary.count_overflow,
        stem + ": full match-set receipt is complete");

    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), stem + ": full match-set artifact reopens");
    std::vector<std::vector<int>> matches;
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("Match ", 0) != 0) continue;
        std::vector<int> mapping;
        std::size_t cursor = 0;
        while ((cursor = line.find("->", cursor)) != std::string::npos) {
            cursor += 2;
            const std::size_t end = line.find_first_of(", |", cursor);
            require(end != std::string::npos,
                    stem + ": match row has a bounded external ID");
            const int external_id = std::stoi(
                line.substr(cursor, end - cursor));
            const int compact_id =
                compactIdForExternal(data, external_id);
            require(compact_id >= 0,
                    stem + ": match row external ID belongs to data");
            mapping.push_back(compact_id);
            cursor = end;
        }
        require(
            mapping.size() == static_cast<std::size_t>(query_vertices),
            stem + ": match row covers every query vertex exactly once");
        matches.push_back(std::move(mapping));
    }
    input.close();
    std::filesystem::remove(path);
    require(
        matches.size() == static_cast<std::size_t>(summary.match_count),
        stem + ": parsed match-set size equals executor count");
    std::sort(matches.begin(), matches.end());
    return matches;
}

MatchSummary executeDigest(
    const TDTree& tree,
    const std::string& stem,
    std::size_t threads) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("dsm_v3_digest_count_" + stem + ".txt");
    const MatchSummary summary = tree.save_res(
        path.string(), MatchOutputMode::CountOnly, threads, true);
    std::filesystem::remove(path);
    require(
        summary.output_written && !summary.count_overflow &&
            summary.result_digest_collected,
        stem + ": count-only mapping digest receipt is complete");
    return summary;
}

struct Fixture {
    Graph data;
    Graph query;
};

Fixture makeDirectedFixture() {
    Fixture fixture;
    fixture.data.num_vertices = 6;
    fixture.data.vertex_labels = {
        labelFromString("A"), labelFromString("B"),
        labelFromString("C"), labelFromString("A"),
        labelFromString("B"), labelFromString("C")};
    fixture.data.adj.resize(6);
    fixture.data.in_adj.resize(6);

    // One exact mapping.  This covers a root self-loop, reciprocal pair,
    // reverse sink expansion, non-tree chord and a common k=3 window.
    addTemporalEdge(fixture.data, 0, 0, {{1, 5}});
    addTemporalEdge(fixture.data, 0, 1, {{1, 5}});
    addTemporalEdge(fixture.data, 1, 0, {{1, 5}});
    addTemporalEdge(fixture.data, 2, 1, {{1, 5}});
    addTemporalEdge(fixture.data, 0, 2, {{2, 5}});

    // Pairwise-durable decoy with no common three-snapshot intersection.
    addTemporalEdge(fixture.data, 3, 3, {{10, 12}});
    addTemporalEdge(fixture.data, 3, 4, {{10, 12}});
    addTemporalEdge(fixture.data, 4, 3, {{12, 14}});
    addTemporalEdge(fixture.data, 5, 4, {{10, 12}});
    addTemporalEdge(fixture.data, 3, 5, {{12, 14}});
    finalizeDataGraph(fixture.data);

    fixture.query = makeQuery(
        {labelFromString("A"), labelFromString("B"),
         labelFromString("C")},
        {{0, 0}, {0, 1}, {1, 0}, {2, 1}, {0, 2}});
    return fixture;
}

Fixture makeTwoAnswerFixture() {
    Fixture fixture;
    fixture.data.num_vertices = 6;
    fixture.data.vertex_labels = {
        labelFromString("A"), labelFromString("B"),
        labelFromString("C"), labelFromString("A"),
        labelFromString("B"), labelFromString("C")};
    fixture.data.adj.resize(6);
    fixture.data.in_adj.resize(6);
    for (const std::array<int, 3>& triple :
         {std::array<int, 3>{0, 1, 2},
          std::array<int, 3>{3, 4, 5}}) {
        const int a = triple[0];
        const int b = triple[1];
        const int c = triple[2];
        addTemporalEdge(fixture.data, a, a, {{1, 5}});
        addTemporalEdge(fixture.data, a, b, {{1, 5}});
        addTemporalEdge(fixture.data, b, a, {{1, 5}});
        addTemporalEdge(fixture.data, c, b, {{1, 5}});
        addTemporalEdge(fixture.data, a, c, {{1, 5}});
    }
    finalizeDataGraph(fixture.data);
    fixture.query = makeQuery(
        {labelFromString("A"), labelFromString("B"),
         labelFromString("C")},
        {{0, 0}, {0, 1}, {1, 0}, {2, 1}, {0, 2}});
    return fixture;
}

Fixture makeCommonWindowZeroFixture() {
    Fixture fixture;
    fixture.data.num_vertices = 3;
    fixture.data.vertex_labels = {
        labelFromString("A"), labelFromString("B"),
        labelFromString("C")};
    fixture.data.adj.resize(3);
    fixture.data.in_adj.resize(3);
    addTemporalEdge(fixture.data, 0, 0, {{10, 12}});
    addTemporalEdge(fixture.data, 0, 1, {{10, 12}});
    addTemporalEdge(fixture.data, 1, 0, {{12, 14}});
    addTemporalEdge(fixture.data, 2, 1, {{10, 12}});
    addTemporalEdge(fixture.data, 0, 2, {{12, 14}});
    finalizeDataGraph(fixture.data);
    fixture.query = makeQuery(
        {labelFromString("A"), labelFromString("B"),
         labelFromString("C")},
        {{0, 0}, {0, 1}, {1, 0}, {2, 1}, {0, 2}});
    return fixture;
}

Fixture makeIncumbentOriginMismatchFixture() {
    Fixture fixture = makeDirectedFixture();
    fixture.data.num_vertices = 10;
    fixture.data.vertex_labels.resize(
        static_cast<std::size_t>(fixture.data.num_vertices),
        labelFromString("A"));
    fixture.data.adj.resize(
        static_cast<std::size_t>(fixture.data.num_vertices));
    fixture.data.in_adj.resize(
        static_cast<std::size_t>(fixture.data.num_vertices));
    // Four long-lived A-labelled decoys make corrected c/(d*A_dur) choose
    // q0, while the pure TD c/d control chooses q1.  They need not satisfy
    // the full query: this fixture witnesses incumbent-origin binding, not
    // candidate cardinality.
    for (int vertex = 6; vertex < 10; ++vertex) {
        addTemporalEdge(
            fixture.data, vertex, vertex, {{1, 100}});
    }
    finalizeDataGraph(fixture.data);
    return fixture;
}

struct Prepared {
    std::shared_ptr<const ValidatedGraphSnapshot> snapshot;
    FixedPlan incumbent_plan;
    std::unique_ptr<TDTree> incumbent_tree;
};

QueryDecomposition correctedPaperDecomposition(
    const ValidatedGraphSnapshot& snapshot) {
    std::array<std::size_t, kLabelCount> label_counts{};
    std::array<double, kLabelCount> activity_sums{};
    const Graph& data = snapshot.data();
    for (int data_vertex = 0;
         data_vertex < data.num_vertices;
         ++data_vertex) {
        const std::size_t index =
            static_cast<std::size_t>(data_vertex);
        const Label label = data.vertex_labels[index];
        if (label >= kLabelCount) continue;
        ++label_counts[label];
        activity_sums[label] += static_cast<double>(
            data.vertex_active_durations[index]);
    }
    std::array<double, kLabelCount> mean_activity{};
    for (std::size_t label = 0; label < kLabelCount; ++label) {
        if (label_counts[label] != 0) {
            mean_activity[label] = activity_sums[label] /
                static_cast<double>(label_counts[label]);
        }
    }
    return decomposeQuery(
        snapshot.query(),
        label_counts,
        mean_activity,
        SelectivityMode::Paper);
}

Prepared prepare(Fixture& fixture) {
    GraphSnapshotSealResult sealed = ValidatedGraphSnapshot::seal(
        fixture.data,
        fixture.query,
        GraphValidationBudget::unbounded());
    require(sealed.complete(), "V3 fixture snapshot seals");
    Prepared prepared;
    prepared.snapshot = sealed.snapshot;
    const QueryDecomposition decomposition =
        correctedPaperDecomposition(*prepared.snapshot);
    require(
        decomposition.connected &&
            decomposition.selectivity_mode == SelectivityMode::Paper &&
            std::string(selectivityFormulaId(decomposition.selectivity_mode)) ==
                "label_population_over_product_query_degree_mean_vertex_activity",
        "V3 incumbent is derived from corrected Paper c/(degree*A_dur) argmin decomposition");
    prepared.incumbent_plan = makeFixedPlan(
        *prepared.snapshot, decomposition);
    prepared.incumbent_tree = TDTree::buildValidatedIncumbent(
        prepared.snapshot,
        decomposition,
        3);
    require(
        !prepared.incumbent_tree->rootCandidates().empty(),
        "V3 fixture incumbent is nonempty");
    return prepared;
}

CandidateBuildBudget generousBuildBudget() {
    CandidateBuildBudget budget;
    budget.adjacency_probes =
        std::numeric_limits<std::uint64_t>::max();
    budget.reverse_edge_lookups =
        std::numeric_limits<std::uint64_t>::max();
    budget.arc_checks = std::numeric_limits<std::uint64_t>::max();
    budget.relation_entries =
        std::numeric_limits<std::uint64_t>::max();
    budget.candidate_value_occurrences =
        std::numeric_limits<std::uint64_t>::max();
    budget.blocks = std::numeric_limits<std::uint64_t>::max();
    budget.semijoin_entry_visits =
        std::numeric_limits<std::uint64_t>::max();
    budget.index_entries =
        std::numeric_limits<std::uint64_t>::max();
    budget.payload_memory_bytes =
        std::numeric_limits<std::uint64_t>::max();
    budget.fixed_memory_headroom_bytes = 0;
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
        lhs.semijoin_entry_visits == rhs.semijoin_entry_visits &&
        lhs.index_entries == rhs.index_entries &&
        lhs.payload_memory_bytes == rhs.payload_memory_bytes &&
        lhs.fixed_memory_headroom_bytes ==
            rhs.fixed_memory_headroom_bytes &&
        lhs.candidate_tree_tokens == rhs.candidate_tree_tokens;
}

TemporalCostV3Options testOptions() {
    TemporalCostV3Options options;
    options.proposal_budget.maximum_states_expanded = 10000;
    options.proposal_budget.maximum_complete_plans = 512;
    options.proposal_budget.maximum_quote_cover_value_visits = 100000;
    options.lazy_budget.maximum_block_requests = 10000;
    options.lazy_budget.maximum_adjacency_probes = 100000;
    options.lazy_budget.maximum_reverse_lookups = 100000;
    options.lazy_budget.maximum_arc_checks = 200000;
    options.lazy_budget.maximum_relation_entries = 100000;
    options.lazy_budget.maximum_cache_entries = 1024;
    options.lazy_budget.maximum_cache_capacity_bytes = 1048576;
    options.lazy_budget.maximum_extension_probes = 100000;
    options.lazy_budget.maximum_interval_intersections = 100000;
    options.lazy_budget.prefix_sample_cap = 64;
    options.rescore_budget.maximum_block_requests = 10000;
    options.rescore_budget.maximum_extension_probes = 100000;
    options.rescore_budget.maximum_interval_intersections = 100000;
    options.rescore_budget.prefix_sample_cap = 64;
    options.metadata_payload_cap = 1048576;
    options.maximum_trace_entries = 10000;
    options.profitability_gate_enabled = false;
    return options;
}

TemporalCostV3Options exactMemoryOptions(
    const CandidateBuildBudget& build_budget,
    std::uint64_t process_memory_max_bytes) {
    TemporalCostV3Options options = testOptions();
    options.global_memory_admission_enabled = true;
    options.process_memory_max_bytes = process_memory_max_bytes;
    options.existing_capacity_phase_generation = 1;
    options.existing_capacity_accounting_model_sha256 =
        existingCapacityAccountingModelArtifactV1().sha256;
    const Graph released;
    options.source_data_graph_release = sourceGraphReleaseReceiptV1(
        released, "source_data_graph");
    options.source_query_graph_release = sourceGraphReleaseReceiptV1(
        released, "source_query_graph");
    require(
        options.source_data_graph_release.invariantHolds() &&
            options.source_query_graph_release.invariantHolds() &&
            build_budget.candidate_tree_tokens == 1,
        "exact-memory options bind released source owners and one token");
    return options;
}

V3MetadataBudget generousMetadataBudget() {
    return {1048576, 1};
}

std::vector<FixedPlan> enumerateLegalPlans(
    const ValidatedGraphSnapshot& snapshot) {
    const Graph& query = snapshot.query();
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
                        snapshot, order.front(), order, parent);
                    if (validateFixedPlan(snapshot, plan).valid) {
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

bool isTreePair(const FixedPlan& plan, int left, int right) {
    return plan.parent[static_cast<std::size_t>(left)] == right ||
        plan.parent[static_cast<std::size_t>(right)] == left;
}

std::unique_ptr<TDTree> buildCandidateForPlan(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot,
    const CandidateCover& cover,
    const FixedPlan& plan,
    const std::string& context) {
    CandidateBuildQuote quote = quoteCoverAwareBuild(
        snapshot, cover, plan);
    require(
        quote.complete && !quote.overflowed &&
            quote.materializerContractBound() &&
            quote.validatedSnapshotBound() &&
            quote.isBoundToValidatedSnapshot(snapshot),
        context + ": all-legal-plan build quote is complete and sealed");
    CandidateBuildLedger ledger(generousBuildBudget());
    CandidateBuildReservationAttempt reservation =
        tryReserveCandidateBuild(quote, ledger);
    require(
        reservation.fit.fits && reservation.reservation != nullptr,
        context + ": all-legal-plan build reserves atomically");
    CoverAwareBuildResult built = buildCoverAwareTDTree(
        snapshot, cover, plan, std::move(reservation.reservation));
    require(
        built.complete() && built.tree != nullptr &&
            built.counters.quote_violations == 0 &&
            built.counters.partial_tree_handoffs == 0 &&
            built.tree->executionPlanFingerprint() == plan.fingerprint,
        context + ": all-legal-plan candidate materializes completely");
    return std::move(built.tree);
}

void testCompletePlanFirstProposal() {
    Fixture fixture = makeDirectedFixture();
    Prepared prepared = prepare(fixture);
    const CandidateCover cover =
        prepared.incumbent_tree->exportCandidateCover();
    resetQueryGraphContentFingerprintCallCount();
    resetTemporalGraphContentFingerprintCallCount();
    const V3ProposalBudget budget{10000, 512, 100000};
    const V3FixedPlanProposalResult first =
        generateV3FixedPlanProposals(
            prepared.snapshot,
            cover,
            prepared.incumbent_plan,
            budget,
            10000);
    const V3FixedPlanProposalResult second =
        generateV3FixedPlanProposals(
            prepared.snapshot,
            cover,
            prepared.incumbent_plan,
            budget,
            10000);
    require(
        first.complete() && second.complete() &&
            first.plans.size() > 1 &&
            first.plans.front().fingerprint ==
                prepared.incumbent_plan.fingerprint &&
            first.telemetry.v3_nonincumbent_plan_proposals > 0,
        "complete-plan-first generator keeps incumbent then alternatives");
    std::vector<std::uint64_t> first_fingerprints;
    std::vector<std::uint64_t> second_fingerprints;
    for (const FixedPlan& plan : first.plans) {
        require(
            validateFixedPlan(*prepared.snapshot, plan).valid &&
                plan.order.size() == static_cast<std::size_t>(
                    prepared.snapshot->query().num_vertices),
            "proposal output exposes only complete legal fixed plans");
        first_fingerprints.push_back(plan.fingerprint);
    }
    for (const FixedPlan& plan : second.plans) {
        second_fingerprints.push_back(plan.fingerprint);
    }
    require(
        first_fingerprints == second_fingerprints &&
            first.deterministic_trace == second.deterministic_trace,
        "proposal ordering and trace are deterministic");
    require(
        queryGraphContentFingerprintCallCount() == 0 &&
            temporalGraphContentFingerprintCallCount() == 0,
        "proposal reads cached query authority and performs no graph scan");

    V3ProposalBudget incumbent_only = budget;
    incumbent_only.maximum_complete_plans = 1;
    const V3FixedPlanProposalResult capped =
        generateV3FixedPlanProposals(
            prepared.snapshot,
            cover,
            prepared.incumbent_plan,
            incumbent_only,
            16);
    require(
        capped.complete() && capped.plans.size() == 1 &&
            capped.status == V3ProposalStatus::CompletePlanBudget &&
            capped.telemetry.v3_nonincumbent_plan_proposals == 0,
        "complete-plan cap never publishes a partial alternative");
}

void testProposalUsesOnlyTopologyAndCoverCardinality() {
    Fixture first_fixture = makeDirectedFixture();
    Fixture second_fixture = makeTwoAnswerFixture();
    Prepared first = prepare(first_fixture);
    Prepared second = prepare(second_fixture);
    const CandidateCover first_cover =
        first.incumbent_tree->exportCandidateCover();
    const CandidateCover second_cover =
        second.incumbent_tree->exportCandidateCover();
    require(
        first.snapshot->queryFingerprint() ==
                second.snapshot->queryFingerprint() &&
            first.incumbent_plan.fingerprint ==
                second.incumbent_plan.fingerprint,
        "proposal data-independence fixtures share query and incumbent plan");
    for (std::size_t query_vertex = 0;
         query_vertex < first_cover.queryVertexCount();
         ++query_vertex) {
        require(
            first_cover.domain(query_vertex).size() ==
                second_cover.domain(query_vertex).size(),
            "proposal data-independence fixtures share cover cardinalities");
    }

    const V3ProposalBudget budget{10000, 512, 100000};
    const V3FixedPlanProposalResult first_result =
        generateV3FixedPlanProposals(
            first.snapshot,
            first_cover,
            first.incumbent_plan,
            budget,
            10000);
    const V3FixedPlanProposalResult second_result =
        generateV3FixedPlanProposals(
            second.snapshot,
            second_cover,
            second.incumbent_plan,
            budget,
            10000);
    std::vector<std::uint64_t> first_fingerprints;
    std::vector<std::uint64_t> second_fingerprints;
    for (const FixedPlan& plan : first_result.plans) {
        first_fingerprints.push_back(plan.fingerprint);
    }
    for (const FixedPlan& plan : second_result.plans) {
        second_fingerprints.push_back(plan.fingerprint);
    }
    require(
        first_result.complete() && second_result.complete() &&
            first_fingerprints == second_fingerprints &&
            first_result.deterministic_trace ==
                second_result.deterministic_trace &&
            first_result.telemetry.v3_proposal_states_expanded ==
                second_result.telemetry.v3_proposal_states_expanded &&
            first_result.telemetry.v3_complete_plan_proposals ==
                second_result.telemetry.v3_complete_plan_proposals,
        "different data histories with equal cover cardinalities yield the same proposals");
}

void testAllLegalPlanMappingDigestParity() {
    Fixture fixture = makeDirectedFixture();
    Prepared prepared = prepare(fixture);
    const CandidateCover cover =
        prepared.incumbent_tree->exportCandidateCover();
    const Graph& data = prepared.snapshot->data();
    const Graph& query = prepared.snapshot->query();
    const std::vector<std::vector<int>> brute_force =
        bruteForceDurableMappings(data, query, 3);
    require(
        brute_force.size() == 1,
        "all-legal-plan oracle excludes the pairwise-only common-window decoy");
    require(
        executeMappings(
            *prepared.incumbent_tree,
            data,
            query.num_vertices,
            "all_legal_incumbent") == brute_force,
        "validated incumbent match-set digest equals independent brute force");

    bool wrong_parent = false;
    bool reverse_sink = false;
    bool reciprocal = false;
    bool root_self_loop = false;
    bool nonroot_self_loop = false;
    bool non_tree_chord = false;
    std::size_t candidates_built = 0;
    const std::vector<FixedPlan> plans =
        enumerateLegalPlans(*prepared.snapshot);
    for (const FixedPlan& plan : plans) {
        root_self_loop = root_self_loop || plan.root == 0;
        nonroot_self_loop = nonroot_self_loop || plan.root != 0;
        non_tree_chord = non_tree_chord || !isTreePair(plan, 0, 2);
        for (int child = 0; child < query.num_vertices; ++child) {
            const int parent =
                plan.parent[static_cast<std::size_t>(child)];
            if (parent >= 0 &&
                parent != prepared.incumbent_plan.parent[
                    static_cast<std::size_t>(child)]) {
                wrong_parent = true;
            }
            if (parent >= 0) {
                const RequiredArcMask mask =
                    requiredArcMask(query, parent, child);
                reverse_sink = reverse_sink ||
                    mask == RequiredArcMask::ChildToParent;
                reciprocal = reciprocal ||
                    mask == RequiredArcMask::Reciprocal;
            }
        }
        if (plan.fingerprint == prepared.incumbent_plan.fingerprint) {
            continue;
        }
        const std::string context =
            "all_legal_" + std::to_string(plan.fingerprint);
        std::unique_ptr<TDTree> candidate = buildCandidateForPlan(
            prepared.snapshot, cover, plan, context);
        require(
            executeMappings(
                *candidate,
                data,
                query.num_vertices,
                context) == brute_force,
            context +
                ": exact count and canonical match-set digest equal brute force");
        ++candidates_built;
    }
    require(
        plans.size() > 1 && candidates_built + 1 == plans.size() &&
            wrong_parent && reverse_sink && reciprocal &&
            root_self_loop && nonroot_self_loop && non_tree_chord,
        "all legal plans exercise wrong-parent, reverse, reciprocal, root/non-root self-loop and chord semantics");
}

TemporalCostV3Result runPlanner(
    Prepared prepared,
    const TemporalCostV3Options& options,
    CandidateBuildBudget build_budget,
    V3MetadataBudget metadata_budget,
    CandidateBuildBudget* remaining = nullptr,
    V3MetadataBudget* remaining_metadata = nullptr) {
    CandidateBuildLedger ledger(build_budget);
    TemporalCostV3Result result = planTemporalCostV3(
        prepared.snapshot,
        std::move(prepared.incumbent_tree),
        prepared.incumbent_plan,
        ledger,
        metadata_budget,
        options);
    if (remaining != nullptr) *remaining = ledger.remaining();
    if (remaining_metadata != nullptr) {
        *remaining_metadata = metadata_budget;
    }
    return result;
}

void testPositivePlannerAndParity() {
    Fixture fixture = makeDirectedFixture();
    const std::vector<std::vector<int>> brute_force =
        bruteForceDurableMappings(fixture.data, fixture.query, 3);
    Prepared prepared = prepare(fixture);
    TDTree* const incumbent_identity = prepared.incumbent_tree.get();
    resetQueryGraphContentFingerprintCallCount();
    resetTemporalGraphContentFingerprintCallCount();
    const TemporalCostV3Options options = testOptions();
    TemporalCostV3Result result = runPlanner(
        std::move(prepared),
        options,
        generousBuildBudget(),
        generousMetadataBudget());
    require(
        result.decision == V3TerminalDecision::CandidateSelected &&
            result.selectedCandidate() &&
            result.selected_tree != nullptr &&
            result.selected_tree.get() != incumbent_identity &&
            result.receipt != nullptr &&
            result.selected_plan_fingerprint !=
                result.incumbent_plan_fingerprint,
        "gate-off arm selects one fully materialized nonincumbent plan");
    require(
        result.telemetry.v3_materialized_complete_plans == 1 &&
            result.telemetry.v3_rescored_complete_plans == 1 &&
            result.telemetry.v3_scored_plan_proposals >= 1 &&
            result.telemetry.v3_lazy_blocks_started ==
                result.telemetry.v3_lazy_blocks_completed &&
            result.telemetry.v3_lazy_block_hits > 0 &&
            result.telemetry.v3_candidate_build_quote_violations == 0 &&
            result.telemetry.v3_partial_candidate_tree_handoffs == 0 &&
            result.telemetry.v3_lazy_prefix_sample_cap ==
                options.lazy_budget.prefix_sample_cap &&
            result.telemetry.v3_rescore_prefix_sample_cap ==
                options.rescore_budget.prefix_sample_cap &&
            result.telemetry.v3_lazy_maximum_prefix_samples_observed > 0 &&
            result.telemetry.v3_rescore_maximum_prefix_samples_observed > 0 &&
            result.telemetry.v3_maximum_prefix_samples_observed ==
                std::max(
                    result.telemetry
                        .v3_lazy_maximum_prefix_samples_observed,
                    result.telemetry
                        .v3_rescore_maximum_prefix_samples_observed) &&
            result.candidate_build.quote_available &&
            result.candidate_build.quote_dominance_verified &&
            result.candidate_build.partial_handoffs == 0 &&
            result.candidate_build.actual.adjacency_probes <=
                result.candidate_build.sealed_quote.adjacency_probes &&
            result.candidate_build.actual.reverse_edge_lookups <=
                result.candidate_build.sealed_quote.reverse_edge_lookups &&
            result.candidate_build.actual.arc_checks <=
                result.candidate_build.sealed_quote.arc_checks &&
            result.candidate_build.actual.relation_entries <=
                result.candidate_build.sealed_quote.relation_entries &&
            result.candidate_build.actual.payload_memory_bytes <=
                result.candidate_build.sealed_quote.payload_memory_bytes &&
            result.invariantHolds(),
        "positive run preserves planner, handoff, candidate quote/actual, and prefix-cap crossbinding invariants");
    require(
        result.receipt->plan().fingerprint ==
                result.selected_tree->executionPlanFingerprint() &&
            result.receipt->decision() ==
                PlannerDecision::CandidateSelected,
        "external receipt is authoritative for the selected plan");
    const MatchSummary one_thread = executeDigest(
        *result.selected_tree, "positive_1t", 1);
    const MatchSummary sixteen_threads = executeDigest(
        *result.selected_tree, "positive_16t", 16);
    const MatchSummary file_free_one =
        result.selected_tree->countMatchesV3(1);
    const MatchSummary file_free_sixteen =
        result.selected_tree->countMatchesV3(16);
    require(
        brute_force.size() == 1 &&
            executeMappings(
                *result.selected_tree,
                fixture.data,
                fixture.query.num_vertices,
                "positive_full_1t") == brute_force &&
            one_thread.match_count == brute_force.size() &&
            sixteen_threads.match_count == brute_force.size() &&
            one_thread.mapping_digest_xor ==
                sixteen_threads.mapping_digest_xor &&
            one_thread.mapping_digest_sum ==
                sixteen_threads.mapping_digest_sum &&
            one_thread.threads_used == 1 &&
            sixteen_threads.threads_used == 16 &&
            file_free_one.match_count == file_free_sixteen.match_count &&
            file_free_one.mapping_digest_xor ==
                file_free_sixteen.mapping_digest_xor &&
            file_free_one.mapping_digest_sum ==
                file_free_sixteen.mapping_digest_sum &&
            file_free_one.threads_created == 1 &&
            file_free_sixteen.threads_created == 16 &&
            !file_free_one.output_written &&
            !file_free_sixteen.output_written,
        "1T full execution matches the oracle and file-backed/file-free real 1T/16T count-only executions have equal count/xor/sum digests");
    require(
        queryGraphContentFingerprintCallCount() == 0 &&
            temporalGraphContentFingerprintCallCount() == 0 &&
            result.telemetry.extra_full_q_by_n_domain_scans == 0 &&
            result.telemetry.global_query_edge_catalogue_builds == 0 &&
            result.telemetry.global_arc_consistency_passes == 0,
        "V3 planner performs no hidden fingerprint/q-by-n/catalogue/AC pass");
    result.telemetry.v3_lazy_maximum_prefix_samples_observed =
        result.telemetry.v3_lazy_prefix_sample_cap + 1;
    result.telemetry.v3_maximum_prefix_samples_observed =
        result.telemetry.v3_lazy_maximum_prefix_samples_observed;
    require(
        !result.invariantHolds(),
        "terminal invariant rejects a prefix-population receipt above its bound");
}

void testRejectsWrongSelectivityIncumbentOrigin() {
    Fixture fixture = makeIncumbentOriginMismatchFixture();
    GraphSnapshotSealResult sealed = ValidatedGraphSnapshot::seal(
        fixture.data,
        fixture.query,
        GraphValidationBudget::unbounded());
    require(sealed.complete(), "origin-mismatch fixture snapshot seals");

    const QueryDecomposition corrected =
        correctedPaperDecomposition(*sealed.snapshot);
    std::array<std::size_t, kLabelCount> label_counts{};
    std::array<double, kLabelCount> duration_sums{};
    for (std::size_t vertex = 0;
         vertex < sealed.snapshot->data().vertex_labels.size();
         ++vertex) {
        const Label label =
            sealed.snapshot->data().vertex_labels[vertex];
        ++label_counts[label];
        duration_sums[label] += static_cast<double>(
            sealed.snapshot->data().vertex_active_durations[vertex]);
    }
    std::array<double, kLabelCount> mean_activity{};
    for (std::size_t label = 0; label < kLabelCount; ++label) {
        if (label_counts[label] != 0) {
            mean_activity[label] = duration_sums[label] /
                static_cast<double>(label_counts[label]);
        }
    }
    const QueryDecomposition wrong_td = decomposeQuery(
        sealed.snapshot->query(),
        label_counts,
        mean_activity,
        SelectivityMode::TDTree);
    require(
        corrected.root == 0 && wrong_td.root == 1,
        "hostile fixture separates corrected Paper and TD roots");

    Prepared prepared;
    prepared.snapshot = sealed.snapshot;
    prepared.incumbent_plan = makeFixedPlan(
        *sealed.snapshot, wrong_td);
    prepared.incumbent_tree = TDTree::buildValidatedIncumbent(
        sealed.snapshot, wrong_td, 3);
    require(
        !prepared.incumbent_tree->hasCorrectedPaperIncumbentOrigin(
            prepared.incumbent_plan),
        "validated topology alone cannot attest corrected Paper origin");
    TDTree* const identity = prepared.incumbent_tree.get();

    TemporalCostV3Result rejected = runPlanner(
        std::move(prepared),
        testOptions(),
        generousBuildBudget(),
        generousMetadataBudget());
    require(
        rejected.decision ==
                V3TerminalDecision::IncumbentOriginMismatch &&
            rejected.selected_tree.get() == identity &&
            rejected.receipt != nullptr &&
            rejected.receipt->decision() ==
                PlannerDecision::IncumbentOnly &&
            rejected.telemetry.v3_complete_plan_proposals == 0 &&
            rejected.telemetry.v3_materialized_complete_plans == 0 &&
            rejected.telemetry.v3_rescored_complete_plans == 0 &&
            rejected.invariantHolds(),
        "V3 rejects a wrong-mode incumbent before proposal, materialization, or rescore");
}

void testDeterminismAndSourceMutation() {
    Fixture fixture = makeDirectedFixture();
    Prepared first = prepare(fixture);
    Prepared second;
    second.snapshot = first.snapshot;
    second.incumbent_plan = first.incumbent_plan;
    second.incumbent_tree = TDTree::buildValidatedIncumbent(
        second.snapshot,
        correctedPaperDecomposition(*second.snapshot),
        3);
    // Legal source mutation cannot affect the already sealed private state.
    fixture.data.vertex_labels[0] = labelFromString("E");
    fixture.data.adj[0][0].to = 5;
    fixture.query.vertex_labels[0] = labelFromString("E");

    TemporalCostV3Result left = runPlanner(
        std::move(first), testOptions(),
        generousBuildBudget(), generousMetadataBudget());
    TemporalCostV3Result right = runPlanner(
        std::move(second), testOptions(),
        generousBuildBudget(), generousMetadataBudget());
    require(
        left.decision == right.decision &&
            left.selected_plan_fingerprint ==
                right.selected_plan_fingerprint &&
            left.deterministic_trace == right.deterministic_trace &&
            left.telemetry.v3_proposal_states_expanded ==
                right.telemetry.v3_proposal_states_expanded &&
            left.telemetry.v3_lazy_block_requests ==
                right.telemetry.v3_lazy_block_requests &&
            left.telemetry.v3_lazy_block_hits ==
                right.telemetry.v3_lazy_block_hits &&
            left.telemetry.v3_maximum_prefix_samples_observed ==
                right.telemetry.v3_maximum_prefix_samples_observed &&
            left.telemetry.v3_lazy_prefix_sample_cap ==
                right.telemetry.v3_lazy_prefix_sample_cap &&
            left.telemetry.v3_rescore_prefix_sample_cap ==
                right.telemetry.v3_rescore_prefix_sample_cap &&
            left.telemetry.predicted_candidate_total ==
                right.telemetry.predicted_candidate_total,
        "same snapshot yields deterministic plan/block/counter/decision trace");
}

void testDeterministicBottomKSamplePath() {
    Fixture fixture = makeTwoAnswerFixture();
    Prepared first = prepare(fixture);
    Prepared second;
    second.snapshot = first.snapshot;
    second.incumbent_plan = first.incumbent_plan;
    second.incumbent_tree = TDTree::buildValidatedIncumbent(
        second.snapshot,
        correctedPaperDecomposition(*second.snapshot),
        3);
    const std::vector<std::vector<int>> brute_force =
        bruteForceDurableMappings(
            first.snapshot->data(), first.snapshot->query(), 3);
    require(
        brute_force.size() == 2,
        "bottom-k fixture has two exact durable mappings");

    TemporalCostV3Options options = testOptions();
    options.lazy_budget.prefix_sample_cap = 1;
    options.rescore_budget.prefix_sample_cap = 1;
    TemporalCostV3Result left = runPlanner(
        std::move(first), options,
        generousBuildBudget(), generousMetadataBudget());
    TemporalCostV3Result right = runPlanner(
        std::move(second), options,
        generousBuildBudget(), generousMetadataBudget());
    require(
        left.decision == V3TerminalDecision::CandidateSelected &&
            right.decision == V3TerminalDecision::CandidateSelected &&
            left.incumbent_score.available &&
            left.candidate_score.available &&
            !left.incumbent_score.exact &&
            !left.candidate_score.exact &&
            right.incumbent_score.available &&
            right.candidate_score.available &&
            !right.incumbent_score.exact &&
            !right.candidate_score.exact,
        "cap-one pilot exercises the deterministic non-exact bottom-k path on both rescore sides");
    require(
        left.selected_plan_fingerprint ==
                right.selected_plan_fingerprint &&
            left.incumbent_score.objective ==
                right.incumbent_score.objective &&
            left.candidate_score.objective ==
                right.candidate_score.objective &&
            left.deterministic_trace == right.deterministic_trace &&
            left.telemetry.v3_lazy_block_requests ==
                right.telemetry.v3_lazy_block_requests &&
            left.telemetry.v3_lazy_interval_intersections ==
                right.telemetry.v3_lazy_interval_intersections &&
            left.telemetry.v3_lazy_prefix_sample_cap == 1 &&
            left.telemetry.v3_rescore_prefix_sample_cap == 1 &&
            left.telemetry.v3_lazy_maximum_prefix_samples_observed == 1 &&
            left.telemetry.v3_rescore_maximum_prefix_samples_observed == 1 &&
            left.telemetry.v3_maximum_prefix_samples_observed == 1,
        "seed-42 bottom-k selection, score and work trace are repeatable");
    require(
        executeMappings(
            *left.selected_tree,
            fixture.data,
            fixture.query.num_vertices,
            "bottom_k_selected") == brute_force,
        "sampled planning never changes exact selected-tree match semantics");
}

void testIndividualDurabilityWithoutCommonWindowIsEmpty() {
    Fixture fixture = makeCommonWindowZeroFixture();
    Prepared prepared = prepare(fixture);
    TDTree* const incumbent_identity = prepared.incumbent_tree.get();
    const std::vector<std::vector<int>> brute_force =
        bruteForceDurableMappings(
            prepared.snapshot->data(), prepared.snapshot->query(), 3);
    require(
        brute_force.empty() &&
            !prepared.incumbent_tree->rootCandidates().empty(),
        "common-window-zero fixture retains pairwise-durable candidates but has no answer");
    const CandidateBuildBudget original = generousBuildBudget();
    CandidateBuildBudget remaining;
    TemporalCostV3Result result = runPlanner(
        std::move(prepared), testOptions(), original,
        generousMetadataBudget(), &remaining);
    require(
        result.decision == V3TerminalDecision::SampleEmpty &&
            result.selected_tree.get() == incumbent_identity &&
            result.telemetry.v3_materialized_complete_plans == 0 &&
            result.telemetry.v3_partial_candidate_tree_handoffs == 0 &&
            sameBudget(remaining, original) &&
            executeMappings(
                *result.selected_tree,
                fixture.data,
                fixture.query.num_vertices,
                "common_zero") == brute_force,
        "individual edge durability without a common k-window is unavailable and falls back exactly");
}

void testLazyBudgetAndSampleFallback() {
    Fixture fixture = makeDirectedFixture();
    Prepared baseline = prepare(fixture);
    TemporalCostV3Result measured = runPlanner(
        std::move(baseline), testOptions(),
        generousBuildBudget(), generousMetadataBudget());
    require(
        measured.telemetry.v3_lazy_adjacency_probes > 0,
        "positive run observes lazy adjacency work");

    std::uint64_t exact_adjacency_cap = 0;
    for (std::uint64_t cap = 0;
         cap <= measured.telemetry.v3_lazy_adjacency_probes;
         ++cap) {
        Prepared probe = prepare(fixture);
        TemporalCostV3Options probe_options = testOptions();
        probe_options.lazy_budget.maximum_adjacency_probes = cap;
        TemporalCostV3Result probe_result = runPlanner(
            std::move(probe), probe_options,
            generousBuildBudget(), generousMetadataBudget());
        if (probe_result.decision ==
            V3TerminalDecision::CandidateSelected) {
            exact_adjacency_cap = cap;
            break;
        }
    }
    require(
        exact_adjacency_cap > 0,
        "synthetic fixture has a positive exact lazy adjacency boundary");

    Prepared capped = prepare(fixture);
    TDTree* const capped_identity = capped.incumbent_tree.get();
    const std::uint64_t capped_content =
        capped.incumbent_tree->candidateContentFingerprint();
    TemporalCostV3Options cap_options = testOptions();
    cap_options.lazy_budget.maximum_adjacency_probes =
        exact_adjacency_cap - 1;
    const CandidateBuildBudget original_budget = generousBuildBudget();
    CandidateBuildBudget remaining;
    TemporalCostV3Result rejected = runPlanner(
        std::move(capped), cap_options, original_budget,
        generousMetadataBudget(), &remaining);
    require(
        rejected.decision == V3TerminalDecision::LazyTransitionBudget &&
            rejected.selected_tree.get() == capped_identity &&
            rejected.selected_tree->candidateContentFingerprint() ==
                capped_content &&
            rejected.telemetry.v3_lazy_budget_rejected_before_start > 0 &&
            rejected.telemetry.v3_lazy_blocks_started ==
                rejected.telemetry.v3_lazy_blocks_completed &&
            rejected.telemetry.v3_lazy_adjacency_probes <=
                cap_options.lazy_budget.maximum_adjacency_probes &&
            sameBudget(remaining, original_budget),
        "lazy cap-minus-one rejects a whole transition and preserves incumbent/build ledger");

    Prepared zero_sample = prepare(fixture);
    TDTree* const zero_sample_identity = zero_sample.incumbent_tree.get();
    TemporalCostV3Options zero_options = testOptions();
    zero_options.lazy_budget.prefix_sample_cap = 0;
    remaining = {};
    TemporalCostV3Result empty = runPlanner(
        std::move(zero_sample), zero_options, original_budget,
        generousMetadataBudget(), &remaining);
    require(
        empty.decision == V3TerminalDecision::SampleEmpty &&
            empty.selected_tree.get() == zero_sample_identity &&
            empty.telemetry.v3_materialized_complete_plans == 0 &&
            sameBudget(remaining, original_budget),
        "empty deterministic sample is unavailable rather than gain zero");
}

void testAllLazyCapBoundaries() {
    Fixture fixture = makeDirectedFixture();
    const TemporalCostV3Options defaults = testOptions();
    struct CapCase {
        const char* name;
        std::uint64_t high;
        std::function<void(TemporalCostV3Options&, std::uint64_t)> set;
    };
    const std::vector<CapCase> cases = {
        {"block-requests",
         defaults.lazy_budget.maximum_block_requests,
         [](TemporalCostV3Options& options, std::uint64_t value) {
             options.lazy_budget.maximum_block_requests = value;
         }},
        {"adjacency",
         defaults.lazy_budget.maximum_adjacency_probes,
         [](TemporalCostV3Options& options, std::uint64_t value) {
             options.lazy_budget.maximum_adjacency_probes = value;
         }},
        {"reverse",
         defaults.lazy_budget.maximum_reverse_lookups,
         [](TemporalCostV3Options& options, std::uint64_t value) {
             options.lazy_budget.maximum_reverse_lookups = value;
         }},
        {"arc-checks",
         defaults.lazy_budget.maximum_arc_checks,
         [](TemporalCostV3Options& options, std::uint64_t value) {
             options.lazy_budget.maximum_arc_checks = value;
         }},
        {"relation-entries",
         defaults.lazy_budget.maximum_relation_entries,
         [](TemporalCostV3Options& options, std::uint64_t value) {
             options.lazy_budget.maximum_relation_entries = value;
         }},
        {"cache-entries",
         defaults.lazy_budget.maximum_cache_entries,
         [](TemporalCostV3Options& options, std::uint64_t value) {
             options.lazy_budget.maximum_cache_entries = value;
         }},
        {"cache-bytes",
         defaults.lazy_budget.maximum_cache_capacity_bytes,
         [](TemporalCostV3Options& options, std::uint64_t value) {
             options.lazy_budget.maximum_cache_capacity_bytes = value;
         }},
        {"extensions",
         defaults.lazy_budget.maximum_extension_probes,
         [](TemporalCostV3Options& options, std::uint64_t value) {
             options.lazy_budget.maximum_extension_probes = value;
         }},
        {"intersections",
         defaults.lazy_budget.maximum_interval_intersections,
         [](TemporalCostV3Options& options, std::uint64_t value) {
             options.lazy_budget.maximum_interval_intersections = value;
         }}};

    for (const CapCase& cap_case : cases) {
        std::uint64_t low = 0;
        std::uint64_t high = cap_case.high;
        while (low < high) {
            const std::uint64_t middle = low + (high - low) / 2;
            Prepared probe = prepare(fixture);
            TemporalCostV3Options options = defaults;
            cap_case.set(options, middle);
            TemporalCostV3Result result = runPlanner(
                std::move(probe), options,
                generousBuildBudget(), generousMetadataBudget());
            if (result.decision ==
                V3TerminalDecision::CandidateSelected) {
                high = middle;
            } else {
                low = middle + 1;
            }
        }
        require(
            low > 0,
            std::string(cap_case.name) +
                ": fixture has a positive exact boundary");

        Prepared exact = prepare(fixture);
        TemporalCostV3Options exact_options = defaults;
        cap_case.set(exact_options, low);
        TemporalCostV3Result exact_result = runPlanner(
            std::move(exact), exact_options,
            generousBuildBudget(), generousMetadataBudget());
        require(
            exact_result.decision ==
                V3TerminalDecision::CandidateSelected,
            std::string(cap_case.name) +
                ": exact lazy cap selects a complete candidate");

        Prepared minus = prepare(fixture);
        TDTree* const incumbent_identity = minus.incumbent_tree.get();
        TemporalCostV3Options minus_options = defaults;
        cap_case.set(minus_options, low - 1);
        const CandidateBuildBudget original = generousBuildBudget();
        CandidateBuildBudget remaining;
        TemporalCostV3Result minus_result = runPlanner(
            std::move(minus), minus_options, original,
            generousMetadataBudget(), &remaining);
        require(
            minus_result.decision ==
                    V3TerminalDecision::LazyTransitionBudget &&
                minus_result.selected_tree.get() == incumbent_identity &&
                minus_result.telemetry.v3_lazy_blocks_started ==
                    minus_result.telemetry.v3_lazy_blocks_completed &&
                minus_result.telemetry
                        .v3_partial_candidate_tree_handoffs == 0 &&
                sameBudget(remaining, original),
            std::string(cap_case.name) +
                ": cap-minus-one rejects transactionally");
    }
}

void testRescoreCapBoundaries() {
    Fixture fixture = makeDirectedFixture();
    const TemporalCostV3Options defaults = testOptions();
    struct CapCase {
        const char* name;
        std::uint64_t high;
        std::function<void(TemporalCostV3Options&, std::uint64_t)> set;
    };
    const std::vector<CapCase> cases = {
        {"rescore-block-requests",
         defaults.rescore_budget.maximum_block_requests,
         [](TemporalCostV3Options& options, std::uint64_t value) {
             options.rescore_budget.maximum_block_requests = value;
         }},
        {"rescore-extensions",
         defaults.rescore_budget.maximum_extension_probes,
         [](TemporalCostV3Options& options, std::uint64_t value) {
             options.rescore_budget.maximum_extension_probes = value;
         }},
        {"rescore-intersections",
         defaults.rescore_budget.maximum_interval_intersections,
         [](TemporalCostV3Options& options, std::uint64_t value) {
             options.rescore_budget.maximum_interval_intersections = value;
         }}};
    for (const CapCase& cap_case : cases) {
        std::uint64_t low = 0;
        std::uint64_t high = cap_case.high;
        while (low < high) {
            const std::uint64_t middle = low + (high - low) / 2;
            Prepared probe = prepare(fixture);
            TemporalCostV3Options options = defaults;
            cap_case.set(options, middle);
            TemporalCostV3Result result = runPlanner(
                std::move(probe), options,
                generousBuildBudget(), generousMetadataBudget());
            if (result.decision ==
                V3TerminalDecision::CandidateSelected) {
                high = middle;
            } else {
                low = middle + 1;
            }
        }
        require(
            low > 0,
            std::string(cap_case.name) +
                ": fixture has positive symmetric-rescore boundary");
        Prepared exact = prepare(fixture);
        TemporalCostV3Options exact_options = defaults;
        cap_case.set(exact_options, low);
        TemporalCostV3Result exact_result = runPlanner(
            std::move(exact), exact_options,
            generousBuildBudget(), generousMetadataBudget());
        require(
            exact_result.decision ==
                V3TerminalDecision::CandidateSelected,
            std::string(cap_case.name) + ": exact cap succeeds");

        Prepared minus = prepare(fixture);
        TDTree* const incumbent_identity = minus.incumbent_tree.get();
        TemporalCostV3Options minus_options = defaults;
        cap_case.set(minus_options, low - 1);
        TemporalCostV3Result minus_result = runPlanner(
            std::move(minus), minus_options,
            generousBuildBudget(), generousMetadataBudget());
        require(
            minus_result.decision ==
                    V3TerminalDecision::CandidateRescoreBudget &&
                minus_result.selected_tree.get() == incumbent_identity &&
                minus_result.telemetry.v3_materialized_complete_plans == 1 &&
                minus_result.telemetry.v3_rescored_complete_plans == 0 &&
                minus_result.telemetry
                        .v3_partial_candidate_tree_handoffs == 0,
            std::string(cap_case.name) +
                ": cap-minus-one discards candidate and preserves incumbent");
    }

    std::size_t low_prefix = 0;
    std::size_t high_prefix = defaults.rescore_budget.prefix_sample_cap;
    while (low_prefix < high_prefix) {
        const std::size_t middle =
            low_prefix + (high_prefix - low_prefix) / 2;
        Prepared probe = prepare(fixture);
        TemporalCostV3Options options = defaults;
        options.rescore_budget.prefix_sample_cap = middle;
        TemporalCostV3Result result = runPlanner(
            std::move(probe), options,
            generousBuildBudget(), generousMetadataBudget());
        if (result.decision == V3TerminalDecision::CandidateSelected) {
            high_prefix = middle;
        } else {
            low_prefix = middle + 1;
        }
    }
    require(low_prefix > 0, "rescore prefix boundary is positive");
    Prepared prefix_minus = prepare(fixture);
    TDTree* const prefix_identity = prefix_minus.incumbent_tree.get();
    TemporalCostV3Options prefix_options = defaults;
    prefix_options.rescore_budget.prefix_sample_cap = low_prefix - 1;
    TemporalCostV3Result prefix_result = runPlanner(
        std::move(prefix_minus), prefix_options,
        generousBuildBudget(), generousMetadataBudget());
    require(
        prefix_result.decision == V3TerminalDecision::SampleEmpty &&
            prefix_result.selected_tree.get() == prefix_identity &&
            prefix_result.telemetry.v3_materialized_complete_plans == 1 &&
            prefix_result.telemetry.v3_rescored_complete_plans == 0,
        "rescore prefix cap-minus-one is unavailable and transactional");
}

void testAdmissionAndMetadataBoundaries() {
    Fixture fixture = makeDirectedFixture();
    const TemporalCostV3Options defaults = testOptions();
    const CandidateBuildBudget original_build = generousBuildBudget();

    std::uint64_t low_quote = 0;
    std::uint64_t high_quote =
        defaults.proposal_budget.maximum_quote_cover_value_visits;
    while (low_quote < high_quote) {
        const std::uint64_t middle =
            low_quote + (high_quote - low_quote) / 2;
        Prepared probe = prepare(fixture);
        TemporalCostV3Options options = defaults;
        options.proposal_budget.maximum_quote_cover_value_visits =
            middle;
        TemporalCostV3Result result = runPlanner(
            std::move(probe), options,
            original_build, generousMetadataBudget());
        if (result.decision == V3TerminalDecision::CandidateSelected) {
            high_quote = middle;
        } else {
            low_quote = middle + 1;
        }
    }
    require(low_quote > 0, "quote-cover visit boundary is positive");
    Prepared quote_minus = prepare(fixture);
    TDTree* const quote_identity = quote_minus.incumbent_tree.get();
    const std::uint64_t quote_content =
        quote_minus.incumbent_tree->candidateContentFingerprint();
    TemporalCostV3Options quote_options = defaults;
    quote_options.proposal_budget.maximum_quote_cover_value_visits =
        low_quote - 1;
    CandidateBuildBudget quote_remaining;
    TemporalCostV3Result quote_result = runPlanner(
        std::move(quote_minus), quote_options, original_build,
        generousMetadataBudget(), &quote_remaining);
    require(
        quote_result.decision == V3TerminalDecision::NoBuildFeasiblePlan &&
            quote_result.selected_tree.get() == quote_identity &&
            quote_result.selected_tree->candidateContentFingerprint() ==
                quote_content &&
            quote_result.telemetry.v3_materialized_complete_plans == 0 &&
            sameBudget(quote_remaining, original_build),
        "quote-cover cap-minus-one performs no lazy/build work or debit");

    Prepared no_token = prepare(fixture);
    TDTree* const token_identity = no_token.incumbent_tree.get();
    const std::uint64_t token_content =
        no_token.incumbent_tree->candidateContentFingerprint();
    CandidateBuildBudget token_budget = original_build;
    token_budget.candidate_tree_tokens = 0;
    CandidateBuildBudget token_remaining;
    TemporalCostV3Result token_result = runPlanner(
        std::move(no_token), defaults, token_budget,
        generousMetadataBudget(), &token_remaining);
    require(
        token_result.decision == V3TerminalDecision::NoBuildFeasiblePlan &&
            token_result.selected_tree.get() == token_identity &&
            token_result.selected_tree->candidateContentFingerprint() ==
                token_content &&
            token_result.telemetry.v3_lazy_block_requests == 0 &&
            !token_result.candidate_build.quote_available &&
            token_result.candidate_build.invariantHolds() &&
            sameBudget(token_remaining, token_budget),
        "tree token is checked before lazy work and never partially debited");

    Prepared measured = prepare(fixture);
    V3MetadataBudget initial_metadata = generousMetadataBudget();
    V3MetadataBudget measured_remaining;
    TemporalCostV3Result measured_result = runPlanner(
        std::move(measured), defaults, original_build,
        initial_metadata, nullptr, &measured_remaining);
    require(
        measured_result.decision == V3TerminalDecision::CandidateSelected &&
            measured_remaining.payload_bytes <
                initial_metadata.payload_bytes &&
            measured_remaining.receipt_tokens == 0,
        "positive handoff debits one bounded metadata receipt");
    const std::uint64_t exact_metadata_bytes =
        initial_metadata.payload_bytes - measured_remaining.payload_bytes;

    Prepared exact_metadata = prepare(fixture);
    V3MetadataBudget exact_budget{exact_metadata_bytes, 1};
    V3MetadataBudget exact_remaining;
    TemporalCostV3Result exact_result = runPlanner(
        std::move(exact_metadata), defaults, original_build,
        exact_budget, nullptr, &exact_remaining);
    require(
        exact_result.decision == V3TerminalDecision::CandidateSelected &&
            exact_remaining.payload_bytes == 0 &&
            exact_remaining.receipt_tokens == 0,
        "exact metadata cap commits the selected receipt");

    Prepared minus_metadata = prepare(fixture);
    TDTree* const metadata_identity =
        minus_metadata.incumbent_tree.get();
    const std::uint64_t metadata_content =
        minus_metadata.incumbent_tree->candidateContentFingerprint();
    V3MetadataBudget minus_budget{exact_metadata_bytes - 1, 1};
    V3MetadataBudget minus_remaining;
    CandidateBuildBudget build_remaining;
    TemporalCostV3Result minus_result = runPlanner(
        std::move(minus_metadata), defaults, original_build,
        minus_budget, &build_remaining, &minus_remaining);
    require(
        minus_result.decision == V3TerminalDecision::MetadataBudget &&
            minus_result.selected_tree.get() == metadata_identity &&
            minus_result.selected_tree->candidateContentFingerprint() ==
                metadata_content &&
            minus_result.receipt == nullptr &&
            minus_result.telemetry.v3_quote_cover_value_visits == 0 &&
            minus_result.telemetry.v3_lazy_block_requests == 0 &&
            minus_result.telemetry.v3_materialized_complete_plans == 0 &&
            minus_remaining.payload_bytes == exact_metadata_bytes - 1 &&
            minus_remaining.receipt_tokens == 1 &&
            sameBudget(build_remaining, original_build),
        "metadata cap-minus-one fails before candidate build and any debit");
}

void testRescoreMetadataAndProfitabilityFallbacks() {
    Fixture fixture = makeDirectedFixture();
    const CandidateBuildBudget build_budget = generousBuildBudget();

    Prepared rescore = prepare(fixture);
    TDTree* const rescore_identity = rescore.incumbent_tree.get();
    const std::uint64_t rescore_content =
        rescore.incumbent_tree->candidateContentFingerprint();
    TemporalCostV3Options rescore_options = testOptions();
    rescore_options.rescore_budget.maximum_extension_probes = 0;
    TemporalCostV3Result rescore_rejected = runPlanner(
        std::move(rescore), rescore_options, build_budget,
        generousMetadataBudget());
    require(
        rescore_rejected.decision ==
                V3TerminalDecision::CandidateRescoreBudget &&
            rescore_rejected.selected_tree.get() == rescore_identity &&
            rescore_rejected.selected_tree->candidateContentFingerprint() ==
                rescore_content &&
            rescore_rejected.telemetry.v3_materialized_complete_plans == 1 &&
            rescore_rejected.telemetry.v3_rescored_complete_plans == 0,
        "candidate rescore cap falls back to the exact incumbent object");

    Prepared metadata = prepare(fixture);
    TDTree* const metadata_identity = metadata.incumbent_tree.get();
    const std::uint64_t metadata_content =
        metadata.incumbent_tree->candidateContentFingerprint();
    V3MetadataBudget tiny_metadata{0, 0};
    V3MetadataBudget remaining_metadata;
    TemporalCostV3Result metadata_rejected = runPlanner(
        std::move(metadata), testOptions(), build_budget,
        tiny_metadata, nullptr, &remaining_metadata);
    require(
        metadata_rejected.decision == V3TerminalDecision::MetadataBudget &&
            metadata_rejected.selected_tree.get() == metadata_identity &&
            metadata_rejected.selected_tree->candidateContentFingerprint() ==
                metadata_content &&
            metadata_rejected.receipt == nullptr &&
            remaining_metadata.payload_bytes == 0 &&
            remaining_metadata.receipt_tokens == 0,
        "metadata cap rejection is transactional and preserves incumbent");

    Prepared gated = prepare(fixture);
    TDTree* const gated_identity = gated.incumbent_tree.get();
    const std::uint64_t gated_content =
        gated.incumbent_tree->candidateContentFingerprint();
    TemporalCostV3Options gate_options = testOptions();
    gate_options.profitability_gate_enabled = true;
    TemporalCostV3Result gate_rejected = runPlanner(
        std::move(gated), gate_options, build_budget,
        generousMetadataBudget());
    require(
        gate_rejected.decision == V3TerminalDecision::GainBelowMargin &&
            gate_rejected.selected_tree.get() == gated_identity &&
            gate_rejected.selected_tree->candidateContentFingerprint() ==
                gated_content &&
            gate_rejected.receipt != nullptr &&
            gate_rejected.receipt->decision() ==
                PlannerDecision::IncumbentOnly &&
            gate_rejected.telemetry.predicted_candidate_total >
                (gate_rejected.telemetry.predicted_incumbent_total * 9) / 10,
        "frozen 0.90 profitability gate rejects candidate overhead");
    require(
        gate_rejected.invariantHolds(),
        "a normal profitability fallback satisfies terminal invariants");
    gate_rejected.telemetry.v3_materialized_complete_plans = 2;
    gate_rejected.telemetry.v3_rescored_complete_plans = 2;
    require(
        !gate_rejected.invariantHolds(),
        "fallback invariant rejects multiple candidate materializations and rescoring passes");
}

void testExactExistingCapacityPlannerIntegration() {
    Fixture probe_fixture = makeDirectedFixture();
    Prepared probe = prepare(probe_fixture);
    CandidateBuildBudget budget = generousBuildBudget();
    budget.fixed_memory_headroom_bytes = 4096;
    TemporalCostV3Options probe_options = exactMemoryOptions(
        budget, std::numeric_limits<std::uint64_t>::max());
    TemporalCostV3Result probe_result = runPlanner(
        std::move(probe), probe_options, budget,
        generousMetadataBudget());
    require(
        probe_result.decision == V3TerminalDecision::CandidateSelected &&
            probe_result.global_memory_admission_required &&
            probe_result.global_memory_admission_attempted &&
            probe_result.global_memory_admission_consumed &&
            probe_result.existing_capacity.invariantHolds() &&
            probe_result.global_memory_admission.invariantHolds() &&
            probe_result.global_memory_admission.fits &&
            probe_result.existing_capacity.proposal_plans_owned_bytes == 0 &&
            probe_result.existing_capacity.admitted_candidates_owned_bytes == 0 &&
            probe_result.existing_capacity.lazy_cache_owned_bytes == 0 &&
            probe_result.existing_capacity.source_data_graph_owned_bytes == 0 &&
            probe_result.existing_capacity.source_query_graph_owned_bytes == 0 &&
            probe_result.existing_capacity.selected_candidate_plan_owned_bytes > 0 &&
            probe_result.existing_capacity.planner_trace_owned_bytes > 0 &&
            !probe_result.global_memory_admission.full_process_memory_proof &&
            !probe_result.global_memory_admission.production_authorization &&
            probe_result.invariantHolds(),
        "planner destroys transient owners, captures exact E immediately before one-shot reservation, and keeps diagnostic scope closed");

    const std::uint64_t exact_limit =
        probe_result.global_memory_admission.required_capacity_bytes;
    Fixture exact_fixture = makeDirectedFixture();
    Prepared exact_prepared = prepare(exact_fixture);
    CandidateBuildBudget exact_remaining;
    TemporalCostV3Result exact_result = runPlanner(
        std::move(exact_prepared),
        exactMemoryOptions(budget, exact_limit),
        budget, generousMetadataBudget(), &exact_remaining);
    require(
        exact_result.decision == V3TerminalDecision::CandidateSelected &&
            exact_result.global_memory_admission.exact_boundary &&
            exact_result.global_memory_admission_consumed &&
            exact_remaining.candidate_tree_tokens == 0,
        "E+Q+H exact equality admits exactly one reservation");

    Fixture minus_fixture = makeDirectedFixture();
    Prepared minus_prepared = prepare(minus_fixture);
    CandidateBuildBudget minus_remaining;
    TemporalCostV3Result minus_result = runPlanner(
        std::move(minus_prepared),
        exactMemoryOptions(budget, exact_limit - 1),
        budget, generousMetadataBudget(), &minus_remaining);
    require(
        minus_result.decision == V3TerminalDecision::GlobalMemoryAdmission &&
            minus_result.global_memory_admission_attempted &&
            !minus_result.global_memory_admission_consumed &&
            minus_result.global_memory_admission.invariantHolds() &&
            !minus_result.global_memory_admission.fits &&
            minus_result.global_memory_admission.reason ==
                V3GlobalMemoryAdmissionReason::ProcessMemoryLimitExceeded &&
            minus_result.telemetry.v3_materialized_complete_plans == 0 &&
            sameBudget(minus_remaining, budget) &&
            minus_result.invariantHolds(),
        "global cap-minus-one is typed, leaves ledger untouched, and preserves incumbent");

    Fixture allocation_fixture = makeDirectedFixture();
    Prepared allocation_prepared = prepare(allocation_fixture);
    CandidateBuildBudget allocation_remaining;
    setCandidateBuildReservationAllocationFailureForTesting(true);
    TemporalCostV3Result allocation_result = runPlanner(
        std::move(allocation_prepared),
        exactMemoryOptions(budget, exact_limit),
        budget, generousMetadataBudget(), &allocation_remaining);
    require(
        allocation_result.decision ==
                V3TerminalDecision::CandidateReservationAllocationFailure &&
            allocation_result.global_memory_admission_attempted &&
            !allocation_result.global_memory_admission_consumed &&
            allocation_result.global_memory_admission.invariantHolds() &&
            allocation_result.global_memory_admission.fits &&
            allocation_result.telemetry.v3_materialized_complete_plans == 0 &&
            sameBudget(allocation_remaining, budget) &&
            allocation_result.invariantHolds(),
        "reservation allocation failure occurs before seal consumption/debit");

    Fixture source_fixture = makeDirectedFixture();
    Prepared source_prepared = prepare(source_fixture);
    TemporalCostV3Options bad_source = exactMemoryOptions(budget, exact_limit);
    Graph nonreleased;
    nonreleased.adj.reserve(1);
    bad_source.source_data_graph_release = sourceGraphReleaseReceiptV1(
        nonreleased, "source_data_graph");
    CandidateBuildBudget source_remaining;
    TemporalCostV3Result source_result = runPlanner(
        std::move(source_prepared), bad_source, budget,
        generousMetadataBudget(), &source_remaining);
    require(
        source_result.decision == V3TerminalDecision::GlobalMemoryAdmission &&
            source_result.global_memory_admission_attempted &&
            !source_result.existing_capacity.complete &&
            !source_result.global_memory_admission.complete &&
            !source_result.global_memory_admission_consumed &&
            source_result.telemetry.v3_materialized_complete_plans == 0 &&
            sameBudget(source_remaining, budget) &&
            source_result.invariantHolds(),
        "nonreleased source owner rejects before reservation");
}

} // namespace

int main() {
    try {
        testCompletePlanFirstProposal();
        testProposalUsesOnlyTopologyAndCoverCardinality();
        testAllLegalPlanMappingDigestParity();
        testPositivePlannerAndParity();
        testRejectsWrongSelectivityIncumbentOrigin();
        testDeterminismAndSourceMutation();
        testDeterministicBottomKSamplePath();
        testIndividualDurabilityWithoutCommonWindowIsEmpty();
        testLazyBudgetAndSampleFallback();
        testAllLazyCapBoundaries();
        testRescoreCapBoundaries();
        testAdmissionAndMetadataBoundaries();
        testRescoreMetadataAndProfitabilityFallbacks();
        testExactExistingCapacityPlannerIntegration();
        std::cout << "All TCC-v3 Cover-Lazy planner tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
