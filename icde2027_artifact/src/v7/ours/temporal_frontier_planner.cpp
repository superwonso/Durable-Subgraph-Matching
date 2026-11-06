#include "temporal_frontier_planner.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <queue>
#include <utility>

namespace {

struct PlannerState {
    std::vector<int> order;
    std::vector<int> parents;
    std::vector<std::uint8_t> selected;
    std::vector<TemporalFrontierStepDiagnostic> steps;
    double partial_states = 0.0;
    double raw_extensions = 0.0;
    double total_candidates = 0.0;
};

bool isNonnegativeFinite(double value) {
    return std::isfinite(value) && value >= 0.0;
}

bool isProbability(double value) {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

int compareDouble(double lhs, double rhs, double epsilon) {
    if (lhs == rhs) return 0;
    if (std::isinf(lhs) || std::isinf(rhs)) return lhs < rhs ? -1 : 1;
    const double scale =
        std::max({1.0, std::abs(lhs), std::abs(rhs)});
    const double difference = lhs - rhs;
    if (std::abs(difference) <= epsilon * scale) return 0;
    return difference < 0.0 ? -1 : 1;
}

double projectedCost(
    const PlannerState& state,
    std::size_t vertex_count,
    const TemporalFrontierPlannerOptions& options) {
    const std::size_t remaining =
        vertex_count - state.order.size();
    return state.total_candidates +
        options.downstream_pressure_weight *
            state.partial_states *
            static_cast<double>(remaining);
}

bool lexicographicallyLess(
    const std::vector<int>& lhs,
    const std::vector<int>& rhs) {
    return std::lexicographical_compare(
        lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

bool projectedStateLess(
    const PlannerState& lhs,
    const PlannerState& rhs,
    std::size_t vertex_count,
    const TemporalFrontierPlannerOptions& options) {
    int comparison = compareDouble(
        projectedCost(lhs, vertex_count, options),
        projectedCost(rhs, vertex_count, options),
        options.comparison_epsilon);
    if (comparison != 0) return comparison < 0;
    comparison = compareDouble(
        lhs.total_candidates,
        rhs.total_candidates,
        options.comparison_epsilon);
    if (comparison != 0) return comparison < 0;
    comparison = compareDouble(
        lhs.raw_extensions,
        rhs.raw_extensions,
        options.comparison_epsilon);
    if (comparison != 0) return comparison < 0;
    comparison = compareDouble(
        lhs.partial_states,
        rhs.partial_states,
        options.comparison_epsilon);
    if (comparison != 0) return comparison < 0;
    if (lhs.order != rhs.order) {
        return lexicographicallyLess(lhs.order, rhs.order);
    }
    return lexicographicallyLess(lhs.parents, rhs.parents);
}

bool completedStateLess(
    const PlannerState& lhs,
    const PlannerState& rhs,
    const TemporalFrontierPlannerOptions& options) {
    int comparison = compareDouble(
        lhs.total_candidates,
        rhs.total_candidates,
        options.comparison_epsilon);
    if (comparison != 0) return comparison < 0;
    comparison = compareDouble(
        lhs.raw_extensions,
        rhs.raw_extensions,
        options.comparison_epsilon);
    if (comparison != 0) return comparison < 0;
    comparison = compareDouble(
        lhs.partial_states,
        rhs.partial_states,
        options.comparison_epsilon);
    if (comparison != 0) return comparison < 0;
    if (lhs.order != rhs.order) {
        return lexicographicallyLess(lhs.order, rhs.order);
    }
    return lexicographicallyLess(lhs.parents, rhs.parents);
}

bool addWeakEdge(
    std::vector<std::vector<std::uint8_t>>& adjacency,
    int lhs,
    int rhs,
    std::string& error) {
    const int vertex_count =
        static_cast<int>(adjacency.size());
    if (lhs < 0 || rhs < 0 ||
        lhs >= vertex_count || rhs >= vertex_count) {
        error = "query topology contains an out-of-range vertex";
        return false;
    }
    if (lhs == rhs) return true;
    adjacency[static_cast<std::size_t>(lhs)]
             [static_cast<std::size_t>(rhs)] = 1;
    adjacency[static_cast<std::size_t>(rhs)]
             [static_cast<std::size_t>(lhs)] = 1;
    return true;
}

bool buildWeakAdjacency(
    const QueryDecomposition& topology,
    std::size_t vertex_count,
    std::vector<std::vector<std::uint8_t>>& adjacency,
    std::string& error) {
    adjacency.assign(
        vertex_count,
        std::vector<std::uint8_t>(vertex_count, 0));

    for (std::size_t parent = 0;
         parent < topology.spanning_tree_adj.size();
         ++parent) {
        if (parent >= vertex_count &&
            !topology.spanning_tree_adj[parent].empty()) {
            error = "spanning-tree topology exceeds the statistics size";
            return false;
        }
        for (int child : topology.spanning_tree_adj[parent]) {
            if (!addWeakEdge(
                    adjacency,
                    static_cast<int>(parent),
                    child,
                    error)) {
                return false;
            }
        }
    }
    for (std::size_t child = 0;
         child < topology.parent.size() &&
         child < vertex_count;
         ++child) {
        const int parent = topology.parent[child];
        if (parent >= 0 &&
            !addWeakEdge(
                adjacency,
                parent,
                static_cast<int>(child),
                error)) {
            return false;
        }
    }
    for (const auto& edge : topology.non_tree_edges) {
        if (!addWeakEdge(
                adjacency, edge.first, edge.second, error)) {
            return false;
        }
    }

    if (vertex_count <= 1) return true;
    std::vector<std::uint8_t> visited(vertex_count, 0);
    std::queue<std::size_t> frontier;
    visited[0] = 1;
    frontier.push(0);
    while (!frontier.empty()) {
        const std::size_t vertex = frontier.front();
        frontier.pop();
        for (std::size_t neighbor = 0;
             neighbor < vertex_count;
             ++neighbor) {
            if (adjacency[vertex][neighbor] == 0 ||
                visited[neighbor] != 0) {
                continue;
            }
            visited[neighbor] = 1;
            frontier.push(neighbor);
        }
    }
    if (std::find(visited.begin(), visited.end(), 0) !=
        visited.end()) {
        error = "query topology is not weakly connected";
        return false;
    }
    return true;
}

bool staticFanout(
    const TemporalFrontierRelationStats& relation,
    const TemporalFrontierPlanStats& statistics,
    double& fanout,
    std::string& error) {
    if (!std::isnan(relation.mean_block_fanout)) {
        if (!isNonnegativeFinite(relation.mean_block_fanout)) {
            error = "mean block fanout must be finite and nonnegative";
            return false;
        }
        fanout = relation.mean_block_fanout;
        return true;
    }

    double denominator =
        static_cast<double>(relation.parent_domain_size);
    if (relation.parent_domain_size == 0 &&
        relation.parent >= 0 &&
        static_cast<std::size_t>(relation.parent) <
            statistics.root_candidate_counts.size()) {
        denominator =
            statistics.root_candidate_counts[
                static_cast<std::size_t>(relation.parent)];
    }
    if (denominator > 0.0) {
        fanout =
            static_cast<double>(relation.relation_entries) /
            denominator;
        return true;
    }
    if (relation.relation_entries == 0) {
        fanout = 0.0;
        return true;
    }
    error =
        "a nonempty relation needs mean_block_fanout or a positive "
        "parent domain";
    return false;
}

bool applyTransition(
    const PlannerState& state,
    int parent,
    int next,
    const TemporalFrontierRelationStats& relation,
    const TemporalFrontierPlanStats& statistics,
    const TemporalFrontierTransitionEstimator& estimator,
    PlannerState& expanded,
    std::string& error) {
    double fanout = 0.0;
    if (!staticFanout(
            relation, statistics, fanout, error)) {
        return false;
    }
    double non_temporal_pass_rate =
        relation.non_temporal_pass_rate;
    double common_window_pass_rate =
        relation.common_window_pass_rate;
    std::size_t sample_count = 0;

    if (!isProbability(non_temporal_pass_rate) ||
        !isProbability(common_window_pass_rate)) {
        error = "relation pass rates must be within [0, 1]";
        return false;
    }

    if (estimator) {
        const TemporalFrontierPrefixView prefix{
            &state.order,
            &state.parents,
            &state.selected,
            state.partial_states,
            state.raw_extensions,
            state.total_candidates};
        const TemporalFrontierTransitionEstimate estimate =
            estimator(prefix, parent, next, relation);
        if (!std::isnan(estimate.mean_block_fanout)) {
            fanout = estimate.mean_block_fanout;
        }
        if (!std::isnan(estimate.non_temporal_pass_rate)) {
            non_temporal_pass_rate =
                estimate.non_temporal_pass_rate;
        }
        if (!std::isnan(estimate.common_window_pass_rate)) {
            common_window_pass_rate =
                estimate.common_window_pass_rate;
        }
        sample_count = estimate.sample_count;
    }

    if (!isNonnegativeFinite(fanout)) {
        error =
            "transition estimator returned an invalid block fanout";
        return false;
    }
    if (!isProbability(non_temporal_pass_rate) ||
        !isProbability(common_window_pass_rate)) {
        error =
            "transition estimator returned a pass rate outside [0, 1]";
        return false;
    }

    const double raw_increment =
        state.partial_states * fanout;
    const double output_partial_states =
        raw_increment *
        non_temporal_pass_rate *
        common_window_pass_rate;
    const double next_raw =
        state.raw_extensions + raw_increment;
    const double next_total =
        state.total_candidates + raw_increment;
    if (!isNonnegativeFinite(raw_increment) ||
        !isNonnegativeFinite(output_partial_states) ||
        !isNonnegativeFinite(next_raw) ||
        !isNonnegativeFinite(next_total)) {
        error = "estimated candidate cost overflowed";
        return false;
    }

    expanded = state;
    expanded.order.push_back(next);
    expanded.parents[static_cast<std::size_t>(next)] =
        parent;
    expanded.selected[static_cast<std::size_t>(next)] = 1;
    expanded.partial_states = output_partial_states;
    expanded.raw_extensions = next_raw;
    expanded.total_candidates = next_total;
    expanded.steps.push_back({
        expanded.order.size() - 1,
        parent,
        next,
        state.partial_states,
        fanout,
        raw_increment,
        non_temporal_pass_rate,
        common_window_pass_rate,
        output_partial_states,
        next_raw,
        next_total,
        sample_count});
    return true;
}

std::vector<PlannerState> expandState(
    const PlannerState& state,
    const std::vector<std::vector<std::uint8_t>>& adjacency,
    const std::vector<int>& relation_index,
    const TemporalFrontierPlanStats& statistics,
    const TemporalFrontierTransitionEstimator& estimator,
    std::string& error) {
    const std::size_t vertex_count = state.selected.size();
    std::vector<PlannerState> expansions;
    for (std::size_t next = 0;
         next < vertex_count;
         ++next) {
        if (state.selected[next] != 0) continue;
        for (std::size_t parent = 0;
             parent < vertex_count;
             ++parent) {
            if (state.selected[parent] == 0 ||
                adjacency[parent][next] == 0) {
                continue;
            }
            const int index =
                relation_index[parent * vertex_count + next];
            if (index < 0) continue;
            PlannerState expanded;
            if (!applyTransition(
                    state,
                    static_cast<int>(parent),
                    static_cast<int>(next),
                    statistics.relations[
                        static_cast<std::size_t>(index)],
                    statistics,
                    estimator,
                    expanded,
                    error)) {
                return {};
            }
            expansions.push_back(std::move(expanded));
        }
    }
    return expansions;
}

TemporalFrontierPlanResult invalidResult(
    TemporalFrontierSearch requested,
    TemporalFrontierSearch actual,
    const std::string& error,
    std::size_t expanded_states = 0,
    std::size_t retained_states = 0,
    bool used_estimator = false) {
    TemporalFrontierPlanResult result;
    result.requested_search = requested;
    result.actual_search = actual;
    result.error = error;
    result.expanded_states = expanded_states;
    result.retained_states = retained_states;
    result.used_transition_estimator = used_estimator;
    return result;
}

} // namespace

const char* temporalFrontierSearchName(
    TemporalFrontierSearch search) {
    switch (search) {
        case TemporalFrontierSearch::Greedy:
            return "greedy";
        case TemporalFrontierSearch::Exact:
            return "exact";
        case TemporalFrontierSearch::Beam:
        default:
            return "beam";
    }
}

TemporalFrontierPlanResult planTemporalFrontier(
    const QueryDecomposition& topology,
    const TemporalFrontierPlanStats& statistics,
    const TemporalFrontierPlannerOptions& options,
    const TemporalFrontierTransitionEstimator& estimator) {
    const TemporalFrontierSearch requested_search =
        options.search;
    TemporalFrontierSearch actual_search =
        requested_search;
    const bool used_estimator =
        static_cast<bool>(estimator);
    const std::size_t vertex_count =
        statistics.root_candidate_counts.size();

    if (vertex_count == 0) {
        return invalidResult(
            requested_search,
            actual_search,
            "at least one query vertex is required",
            0,
            0,
            used_estimator);
    }
    if (options.beam_width == 0 ||
        options.max_expanded_states == 0 ||
        !isNonnegativeFinite(
            options.downstream_pressure_weight) ||
        !std::isfinite(options.comparison_epsilon) ||
        options.comparison_epsilon < 0.0) {
        return invalidResult(
            requested_search,
            actual_search,
            "planner options are invalid",
            0,
            0,
            used_estimator);
    }
    for (double root_candidates :
         statistics.root_candidate_counts) {
        if (!isNonnegativeFinite(root_candidates)) {
            return invalidResult(
                requested_search,
                actual_search,
                "root candidate estimates must be finite and "
                "nonnegative",
                0,
                0,
                used_estimator);
        }
    }

    if (actual_search == TemporalFrontierSearch::Exact &&
        vertex_count > options.exact_vertex_limit) {
        if (!options.exact_fallback_to_beam) {
            return invalidResult(
                requested_search,
                actual_search,
                "query exceeds the configured exact-search vertex "
                "limit",
                0,
                0,
                used_estimator);
        }
        actual_search = TemporalFrontierSearch::Beam;
    }

    std::vector<std::vector<std::uint8_t>> adjacency;
    std::string error;
    if (!buildWeakAdjacency(
            topology, vertex_count, adjacency, error)) {
        return invalidResult(
            requested_search,
            actual_search,
            error,
            0,
            0,
            used_estimator);
    }

    std::vector<int> relation_index(
        vertex_count * vertex_count, -1);
    for (std::size_t index = 0;
         index < statistics.relations.size();
         ++index) {
        const auto& relation = statistics.relations[index];
        if (relation.parent < 0 || relation.child < 0 ||
            static_cast<std::size_t>(relation.parent) >=
                vertex_count ||
            static_cast<std::size_t>(relation.child) >=
                vertex_count ||
            relation.parent == relation.child) {
            return invalidResult(
                requested_search,
                actual_search,
                "relation statistics contain invalid endpoints",
                0,
                0,
                used_estimator);
        }
        const std::size_t parent =
            static_cast<std::size_t>(relation.parent);
        const std::size_t child =
            static_cast<std::size_t>(relation.child);
        if (adjacency[parent][child] == 0) {
            return invalidResult(
                requested_search,
                actual_search,
                "relation statistics reference a non-query edge",
                0,
                0,
                used_estimator);
        }
        const std::size_t matrix_index =
            parent * vertex_count + child;
        if (relation_index[matrix_index] >= 0) {
            return invalidResult(
                requested_search,
                actual_search,
                "duplicate oriented relation statistics",
                0,
                0,
                used_estimator);
        }
        relation_index[matrix_index] =
            static_cast<int>(index);

        double ignored_fanout = 0.0;
        if (!staticFanout(
                relation,
                statistics,
                ignored_fanout,
                error) ||
            !isProbability(
                relation.non_temporal_pass_rate) ||
            !isProbability(
                relation.common_window_pass_rate)) {
            if (error.empty()) {
                error =
                    "relation pass rates must be within [0, 1]";
            }
            return invalidResult(
                requested_search,
                actual_search,
                error,
                0,
                0,
                used_estimator);
        }
    }

    std::vector<PlannerState> states;
    states.reserve(vertex_count);
    for (std::size_t root = 0;
         root < vertex_count;
         ++root) {
        PlannerState state;
        state.order.push_back(static_cast<int>(root));
        state.parents.assign(vertex_count, -1);
        state.selected.assign(vertex_count, 0);
        state.selected[root] = 1;
        state.partial_states =
            statistics.root_candidate_counts[root];
        state.total_candidates =
            statistics.root_candidate_counts[root];
        states.push_back(std::move(state));
    }

    std::size_t expanded_states = states.size();
    std::size_t retained_states = 0;
    if (expanded_states > options.max_expanded_states) {
        return invalidResult(
            requested_search,
            actual_search,
            "planner exceeded max_expanded_states while creating "
            "root states",
            expanded_states,
            retained_states,
            used_estimator);
    }

    try {
        if (actual_search == TemporalFrontierSearch::Greedy) {
            std::sort(
                states.begin(),
                states.end(),
                [&](const PlannerState& lhs,
                    const PlannerState& rhs) {
                    return projectedStateLess(
                        lhs, rhs, vertex_count, options);
                });
            PlannerState current = std::move(states.front());
            ++retained_states;
            while (current.order.size() < vertex_count) {
                std::vector<PlannerState> expansions =
                    expandState(
                        current,
                        adjacency,
                        relation_index,
                        statistics,
                        estimator,
                        error);
                if (!error.empty()) {
                    return invalidResult(
                        requested_search,
                        actual_search,
                        error,
                        expanded_states,
                        retained_states,
                        used_estimator);
                }
                if (expansions.empty()) {
                    return invalidResult(
                        requested_search,
                        actual_search,
                        "no connected transition with relation "
                        "statistics is available",
                        expanded_states,
                        retained_states,
                        used_estimator);
                }
                expanded_states += expansions.size();
                if (expanded_states >
                    options.max_expanded_states) {
                    return invalidResult(
                        requested_search,
                        actual_search,
                        "planner exceeded max_expanded_states",
                        expanded_states,
                        retained_states,
                        used_estimator);
                }
                std::sort(
                    expansions.begin(),
                    expansions.end(),
                    [&](const PlannerState& lhs,
                        const PlannerState& rhs) {
                        return projectedStateLess(
                            lhs, rhs, vertex_count, options);
                    });
                current = std::move(expansions.front());
                ++retained_states;
            }
            states.clear();
            states.push_back(std::move(current));
        } else {
            if (actual_search == TemporalFrontierSearch::Beam &&
                states.size() > options.beam_width) {
                std::sort(
                    states.begin(),
                    states.end(),
                    [&](const PlannerState& lhs,
                        const PlannerState& rhs) {
                        return projectedStateLess(
                            lhs, rhs, vertex_count, options);
                    });
                states.resize(options.beam_width);
            }
            retained_states += states.size();

            while (!states.empty() &&
                   states.front().order.size() <
                       vertex_count) {
                std::vector<PlannerState> next_states;
                for (const PlannerState& state : states) {
                    std::vector<PlannerState> expansions =
                        expandState(
                            state,
                            adjacency,
                            relation_index,
                            statistics,
                            estimator,
                            error);
                    if (!error.empty()) {
                        return invalidResult(
                            requested_search,
                            actual_search,
                            error,
                            expanded_states,
                            retained_states,
                            used_estimator);
                    }
                    expanded_states += expansions.size();
                    if (expanded_states >
                        options.max_expanded_states) {
                        return invalidResult(
                            requested_search,
                            actual_search,
                            "planner exceeded max_expanded_states",
                            expanded_states,
                            retained_states,
                            used_estimator);
                    }
                    next_states.insert(
                        next_states.end(),
                        std::make_move_iterator(
                            expansions.begin()),
                        std::make_move_iterator(
                            expansions.end()));
                }
                if (next_states.empty()) {
                    return invalidResult(
                        requested_search,
                        actual_search,
                        "no complete connected plan can be formed "
                        "from the supplied oriented relations",
                        expanded_states,
                        retained_states,
                        used_estimator);
                }
                std::sort(
                    next_states.begin(),
                    next_states.end(),
                    [&](const PlannerState& lhs,
                        const PlannerState& rhs) {
                        return projectedStateLess(
                            lhs, rhs, vertex_count, options);
                    });
                if (actual_search ==
                        TemporalFrontierSearch::Beam &&
                    next_states.size() >
                        options.beam_width) {
                    next_states.resize(options.beam_width);
                }
                retained_states += next_states.size();
                states = std::move(next_states);
            }
        }
    } catch (const std::exception& exception) {
        return invalidResult(
            requested_search,
            actual_search,
            std::string("transition estimator failed: ") +
                exception.what(),
            expanded_states,
            retained_states,
            used_estimator);
    } catch (...) {
        return invalidResult(
            requested_search,
            actual_search,
            "transition estimator failed with an unknown error",
            expanded_states,
            retained_states,
            used_estimator);
    }

    if (states.empty()) {
        return invalidResult(
            requested_search,
            actual_search,
            "planner did not produce a state",
            expanded_states,
            retained_states,
            used_estimator);
    }
    std::sort(
        states.begin(),
        states.end(),
        [&](const PlannerState& lhs,
            const PlannerState& rhs) {
            return completedStateLess(lhs, rhs, options);
        });
    const PlannerState& best = states.front();
    if (best.order.size() != vertex_count) {
        return invalidResult(
            requested_search,
            actual_search,
            "planner did not produce a complete order",
            expanded_states,
            retained_states,
            used_estimator);
    }

    TemporalFrontierPlanResult result;
    result.valid = true;
    result.requested_search = requested_search;
    result.actual_search = actual_search;
    result.order = best.order;
    result.parents = best.parents;
    result.steps = best.steps;
    result.estimated_root_candidates =
        statistics.root_candidate_counts[
            static_cast<std::size_t>(
                best.order.front())];
    result.estimated_raw_extensions =
        best.raw_extensions;
    result.estimated_total_candidate_examinations =
        best.total_candidates;
    result.estimated_final_partial_states =
        best.partial_states;
    result.expanded_states = expanded_states;
    result.retained_states = retained_states;
    result.used_transition_estimator = used_estimator;
    return result;
}
