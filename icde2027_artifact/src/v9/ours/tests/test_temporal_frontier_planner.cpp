#include "../temporal_frontier_planner.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

bool close(double lhs, double rhs) {
    return std::abs(lhs - rhs) <=
        1e-12 * std::max({1.0, std::abs(lhs), std::abs(rhs)});
}

QueryDecomposition makePathTopology() {
    QueryDecomposition topology;
    topology.vertex_labels.resize(4);
    topology.parent = {-1, 0, 1, 2};
    topology.spanning_tree_adj = {{1}, {2}, {3}, {}};
    topology.connected = true;
    return topology;
}

QueryDecomposition makeTriangleTopology() {
    QueryDecomposition topology;
    topology.vertex_labels.resize(3);
    topology.parent = {-1, 0, 1};
    topology.spanning_tree_adj = {{1}, {2}, {}};
    topology.non_tree_edges = {{0, 2}};
    topology.connected = true;
    return topology;
}

void addRelation(
    TemporalFrontierPlanStats& statistics,
    int parent,
    int child,
    double fanout,
    double non_temporal_pass_rate = 1.0,
    double common_window_pass_rate = 1.0) {
    TemporalFrontierRelationStats relation;
    relation.parent = parent;
    relation.child = child;
    relation.mean_block_fanout = fanout;
    relation.non_temporal_pass_rate =
        non_temporal_pass_rate;
    relation.common_window_pass_rate =
        common_window_pass_rate;
    statistics.relations.push_back(relation);
}

TemporalFrontierPlanStats makePathStatistics() {
    TemporalFrontierPlanStats statistics;
    statistics.root_candidate_counts =
        {100.0, 10.0, 100.0, 100.0};
    addRelation(statistics, 0, 1, 10.0);
    addRelation(statistics, 1, 0, 1.0);
    addRelation(statistics, 1, 2, 1.0);
    addRelation(statistics, 2, 1, 10.0);
    addRelation(statistics, 2, 3, 10.0);
    addRelation(statistics, 3, 2, 10.0);
    return statistics;
}

void testTemporalConnectedFrontierOrder() {
    const QueryDecomposition topology = makePathTopology();
    const TemporalFrontierPlanStats statistics =
        makePathStatistics();

    // The q0 edge is highly effective only after q1-q2 has established a
    // temporal prefix. This mimics a sampled common-k-window estimate.
    const TemporalFrontierTransitionEstimator estimator =
        [](const TemporalFrontierPrefixView& prefix,
           int,
           int next,
           const TemporalFrontierRelationStats&) {
            TemporalFrontierTransitionEstimate estimate;
            const bool q1_selected =
                (*prefix.selected)[1] != 0;
            const bool q2_selected =
                (*prefix.selected)[2] != 0;
            if (next == 0 && q1_selected && q2_selected) {
                estimate.common_window_pass_rate = 0.1;
                estimate.sample_count = 512;
            }
            return estimate;
        };

    TemporalFrontierPlannerOptions options;
    options.search = TemporalFrontierSearch::Exact;
    options.exact_vertex_limit = 8;
    const TemporalFrontierPlanResult result =
        planTemporalFrontier(
            topology, statistics, options, estimator);

    require(result.valid, "exact temporal plan is valid");
    require(
        result.order == std::vector<int>({1, 2, 0, 3}),
        "planner can interleave a sibling before a descendant");
    require(
        result.parents == std::vector<int>({1, -1, 1, 2}),
        "every selected vertex receives an earlier adjacent parent");
    require(
        close(result.estimated_root_candidates, 10.0) &&
            close(result.estimated_raw_extensions, 30.0) &&
            close(
                result.estimated_total_candidate_examinations,
                40.0),
        "plan cost follows root plus reached-block examinations");
    require(
        result.steps.size() == 3 &&
            close(
                result.steps[1].common_window_pass_rate,
                0.1) &&
            result.steps[1].sample_count == 512,
        "sampled common-window diagnostics are retained");
    require(
        result.used_transition_estimator,
        "result records use of the temporal estimator");

    TemporalFrontierPlannerOptions beam_options = options;
    beam_options.search = TemporalFrontierSearch::Beam;
    beam_options.beam_width = 64;
    const TemporalFrontierPlanResult first_beam =
        planTemporalFrontier(
            topology,
            statistics,
            beam_options,
            estimator);
    const TemporalFrontierPlanResult second_beam =
        planTemporalFrontier(
            topology,
            statistics,
            beam_options,
            estimator);
    require(
        first_beam.valid &&
            first_beam.order == result.order &&
            first_beam.parents == result.parents,
        "bounded beam retains the exact plan on the fixture");
    require(
        first_beam.order == second_beam.order &&
            first_beam.parents == second_beam.parents &&
            close(
                first_beam
                    .estimated_total_candidate_examinations,
                second_beam
                    .estimated_total_candidate_examinations),
        "beam planning is deterministic");
}

void testParentChoiceOnCycle() {
    const QueryDecomposition topology = makeTriangleTopology();
    TemporalFrontierPlanStats statistics;
    statistics.root_candidate_counts = {1.0, 100.0, 100.0};
    addRelation(statistics, 0, 1, 1.0);
    addRelation(statistics, 1, 0, 10.0);
    addRelation(statistics, 1, 2, 2.0);
    addRelation(statistics, 2, 1, 10.0);
    addRelation(statistics, 0, 2, 10.0);
    addRelation(statistics, 2, 0, 10.0);

    TemporalFrontierPlannerOptions options;
    options.search = TemporalFrontierSearch::Exact;
    const TemporalFrontierPlanResult result =
        planTemporalFrontier(topology, statistics, options);
    require(result.valid, "cycle plan is valid");
    require(
        result.order == std::vector<int>({0, 1, 2}),
        "cycle plan uses the low-cost connected order");
    require(
        result.parents == std::vector<int>({-1, 0, 1}),
        "planner chooses the lower-fanout parent among earlier neighbors");
    require(
        close(result.estimated_total_candidate_examinations, 4.0),
        "cycle parent choice is reflected in the plan cost");
}

void testDerivedFanoutAndGreedyConnectivity() {
    QueryDecomposition topology;
    topology.vertex_labels.resize(2);
    topology.parent = {-1, 0};
    topology.spanning_tree_adj = {{1}, {}};
    topology.connected = true;

    TemporalFrontierPlanStats statistics;
    statistics.root_candidate_counts = {10.0, 100.0};
    TemporalFrontierRelationStats forward;
    forward.parent = 0;
    forward.child = 1;
    forward.relation_entries = 20;
    forward.parent_domain_size = 10;
    statistics.relations.push_back(forward);
    addRelation(statistics, 1, 0, 10.0);

    TemporalFrontierPlannerOptions options;
    options.search = TemporalFrontierSearch::Greedy;
    const TemporalFrontierPlanResult result =
        planTemporalFrontier(topology, statistics, options);
    require(
        result.valid &&
            result.order == std::vector<int>({0, 1}) &&
            result.parents[1] == 0,
        "greedy plan remains connected");
    require(
        result.steps.size() == 1 &&
            close(result.steps[0].mean_block_fanout, 2.0) &&
            close(
                result.estimated_total_candidate_examinations,
                30.0),
        "relation entries derive the expected block fanout");
}

void testValidationAndExactFallback() {
    const QueryDecomposition topology = makePathTopology();
    TemporalFrontierPlanStats invalid_statistics =
        makePathStatistics();
    invalid_statistics.relations[0]
        .common_window_pass_rate = 1.1;
    const TemporalFrontierPlanResult invalid =
        planTemporalFrontier(
            topology, invalid_statistics);
    require(
        !invalid.valid &&
            invalid.error.find("[0, 1]") != std::string::npos,
        "invalid probability is rejected with a diagnostic");

    TemporalFrontierPlannerOptions options;
    options.search = TemporalFrontierSearch::Exact;
    options.exact_vertex_limit = 3;
    options.exact_fallback_to_beam = true;
    options.beam_width = 64;
    const TemporalFrontierPlanResult fallback =
        planTemporalFrontier(
            topology, makePathStatistics(), options);
    require(
        fallback.valid &&
            fallback.requested_search ==
                TemporalFrontierSearch::Exact &&
            fallback.actual_search ==
                TemporalFrontierSearch::Beam,
        "bounded exact search reports its beam fallback");
}

void testSingleVertex() {
    QueryDecomposition topology;
    topology.vertex_labels.resize(1);
    topology.spanning_tree_adj.resize(1);
    topology.parent = {-1};
    topology.connected = true;
    TemporalFrontierPlanStats statistics;
    statistics.root_candidate_counts = {7.0};

    TemporalFrontierPlannerOptions options;
    options.search = TemporalFrontierSearch::Exact;
    const TemporalFrontierPlanResult result =
        planTemporalFrontier(topology, statistics, options);
    require(
        result.valid &&
            result.order == std::vector<int>({0}) &&
            result.parents == std::vector<int>({-1}) &&
            close(
                result.estimated_total_candidate_examinations,
                7.0),
        "single-vertex query has a root-only plan");
}

} // namespace

int main() {
    testTemporalConnectedFrontierOrder();
    testParentChoiceOnCycle();
    testDerivedFanoutAndGreedyConnectivity();
    testValidationAndExactFallback();
    testSingleVertex();
    std::cout << "Temporal frontier planner tests passed.\n";
    return 0;
}
