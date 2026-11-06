#include "query_decomposition.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>

namespace {

using LabelPairCounts =
    std::array<std::array<std::size_t, kLabelCount>, kLabelCount>;

std::size_t saturatingAdd(std::size_t lhs, std::size_t rhs) {
    if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        return std::numeric_limits<std::size_t>::max();
    }
    return lhs + rhs;
}

bool isStructurallyCompatible(
    const Graph& temporal_graph,
    const Graph& query_graph,
    int data_vertex,
    int query_vertex,
    int minimum_duration,
    const std::vector<std::array<int, kLabelCount>>&
        query_out_requirements,
    const std::vector<std::array<int, kLabelCount>>&
        query_in_requirements) {
    const std::size_t data_index =
        static_cast<std::size_t>(data_vertex);
    const std::size_t query_index =
        static_cast<std::size_t>(query_vertex);
    if (data_index >= temporal_graph.vertex_labels.size() ||
        query_index >= query_graph.vertex_labels.size() ||
        data_index >= temporal_graph.adj.size() ||
        data_index >= temporal_graph.in_adj.size() ||
        query_index >= query_graph.adj.size() ||
        query_index >= query_graph.in_adj.size() ||
        data_index >= temporal_graph.vertex_active_durations.size() ||
        data_index >= temporal_graph.neighbor_label_counts.size() ||
        data_index >=
            temporal_graph.incoming_neighbor_label_counts.size()) {
        return false;
    }
    if (temporal_graph.vertex_labels[data_index] !=
        query_graph.vertex_labels[query_index]) {
        return false;
    }
    if (temporal_graph.adj[data_index].size() <
            query_graph.adj[query_index].size() ||
        temporal_graph.in_adj[data_index].size() <
            query_graph.in_adj[query_index].size() ||
        temporal_graph.vertex_active_durations[data_index] <
            minimum_duration) {
        return false;
    }
    for (std::size_t label = 0; label < kLabelCount; ++label) {
        if (temporal_graph.neighbor_label_counts[data_index][label] <
                query_out_requirements[query_index][label] ||
            temporal_graph.incoming_neighbor_label_counts[data_index][label] <
                query_in_requirements[query_index][label]) {
            return false;
        }
    }
    return true;
}

} // namespace

const char* selectivityModeName(SelectivityMode mode) {
    switch (mode) {
        case SelectivityMode::TDTree:
            return "tdtree";
        case SelectivityMode::LegacyReciprocal:
            return "legacy-reciprocal";
        case SelectivityMode::Intended:
            return "intended";
        case SelectivityMode::LegacyExtractedProduct:
            return "legacy-extracted-product";
        case SelectivityMode::Structural:
            return "structural";
        case SelectivityMode::Survival:
            return "survival";
        case SelectivityMode::TemporalCost:
            return "temporal-cost";
        case SelectivityMode::TemporalCostV2:
            return "temporal-cost-v2";
        case SelectivityMode::Paper:
        default:
            return "paper";
    }
}

const char* selectivityFormulaId(SelectivityMode mode) {
    switch (mode) {
        case SelectivityMode::TDTree:
            return "label_population_over_query_degree";
        case SelectivityMode::Paper:
        case SelectivityMode::Intended:
            return "label_population_over_product_query_degree_mean_vertex_activity";
        case SelectivityMode::LegacyReciprocal:
            return "query_degree_over_product_label_population_mean_vertex_activity";
        case SelectivityMode::LegacyExtractedProduct:
            return "query_degree_times_mean_vertex_activity_over_label_population";
        case SelectivityMode::Structural:
            return "structural_candidates_over_query_degree";
        case SelectivityMode::Survival:
            return "structural_candidates_times_k_survival_over_query_degree";
        case SelectivityMode::TemporalCost:
            return "sampled_plan_level_conditional_work";
        case SelectivityMode::TemporalCostV2:
            return "guarded_plan_level_conditional_work";
        default:
            return "unknown";
    }
}

bool parseSelectivityMode(
    const std::string& text,
    SelectivityMode& mode) {
    if (text == "tdtree") {
        mode = SelectivityMode::TDTree;
    } else if (text == "paper") {
        mode = SelectivityMode::Paper;
    } else if (text == "legacy-reciprocal") {
        mode = SelectivityMode::LegacyReciprocal;
    } else if (text == "intended") {
        mode = SelectivityMode::Intended;
    } else if (text == "legacy-extracted-product") {
        mode = SelectivityMode::LegacyExtractedProduct;
    } else if (text == "structural") {
        mode = SelectivityMode::Structural;
    } else if (text == "survival") {
        mode = SelectivityMode::Survival;
    } else if (text == "temporal-cost" || text == "tcc") {
        mode = SelectivityMode::TemporalCost;
    } else if (text == "temporal-cost-v2" || text == "tcc-v2") {
        mode = SelectivityMode::TemporalCostV2;
    } else {
        return false;
    }
    return true;
}

SurvivalSelectivityStatistics computeSurvivalSelectivityStatistics(
    const Graph& temporal_graph,
    const Graph& query_graph,
    int minimum_duration) {
    if (minimum_duration < 1) {
        throw std::invalid_argument(
            "minimum duration must be positive");
    }

    const std::size_t query_vertex_count =
        static_cast<std::size_t>(
            std::max(0, query_graph.num_vertices));
    SurvivalSelectivityStatistics result;
    result.structural_candidate_counts.assign(
        query_vertex_count, 0);
    result.compatible_edge_counts.assign(
        query_vertex_count, 0);
    result.durable_compatible_edge_counts.assign(
        query_vertex_count, 0);
    result.durability_survival_rates.assign(
        query_vertex_count, 0.0);

    std::vector<std::array<int, kLabelCount>>
        query_out_requirements(query_vertex_count);
    std::vector<std::array<int, kLabelCount>>
        query_in_requirements(query_vertex_count);
    for (int query_vertex = 0;
         query_vertex < query_graph.num_vertices;
         ++query_vertex) {
        const std::size_t query_index =
            static_cast<std::size_t>(query_vertex);
        if (query_index >= query_graph.adj.size() ||
            query_index >= query_graph.in_adj.size()) {
            continue;
        }
        for (const Edge& edge : query_graph.adj[query_index]) {
            if (edge.to < 0 ||
                edge.to >= query_graph.num_vertices) {
                continue;
            }
            const Label label =
                query_graph.vertex_labels[
                    static_cast<std::size_t>(edge.to)];
            if (label < kLabelCount) {
                ++query_out_requirements[query_index][label];
            }
        }
        for (const Edge& edge : query_graph.in_adj[query_index]) {
            if (edge.to < 0 ||
                edge.to >= query_graph.num_vertices) {
                continue;
            }
            const Label label =
                query_graph.vertex_labels[
                    static_cast<std::size_t>(edge.to)];
            if (label < kLabelCount) {
                ++query_in_requirements[query_index][label];
            }
        }
    }

    for (int query_vertex = 0;
         query_vertex < query_graph.num_vertices;
         ++query_vertex) {
        for (int data_vertex = 0;
             data_vertex < temporal_graph.num_vertices;
             ++data_vertex) {
            if (isStructurallyCompatible(
                    temporal_graph,
                    query_graph,
                    data_vertex,
                    query_vertex,
                    minimum_duration,
                    query_out_requirements,
                    query_in_requirements)) {
                ++result.structural_candidate_counts[
                    static_cast<std::size_t>(query_vertex)];
            }
        }
    }

    LabelPairCounts compatible_by_labels{};
    LabelPairCounts durable_by_labels{};
    for (const TemporalEdge& edge :
         temporal_graph.temporal_edges) {
        if (edge.u < 0 || edge.v < 0 ||
            edge.u >= temporal_graph.num_vertices ||
            edge.v >= temporal_graph.num_vertices) {
            continue;
        }
        const Label source_label =
            temporal_graph.vertex_labels[
                static_cast<std::size_t>(edge.u)];
        const Label target_label =
            temporal_graph.vertex_labels[
                static_cast<std::size_t>(edge.v)];
        if (source_label >= kLabelCount ||
            target_label >= kLabelCount) {
            continue;
        }
        ++compatible_by_labels[source_label][target_label];
        if (hasMinimumConsecutiveDuration(
                edge,
                minimum_duration)) {
            ++durable_by_labels[source_label][target_label];
        }
    }

    for (int query_vertex = 0;
         query_vertex < query_graph.num_vertices;
         ++query_vertex) {
        const std::size_t query_index =
            static_cast<std::size_t>(query_vertex);
        if (query_index >= query_graph.vertex_labels.size() ||
            query_index >= query_graph.adj.size() ||
            query_index >= query_graph.in_adj.size()) {
            continue;
        }
        const Label central_label =
            query_graph.vertex_labels[query_index];
        if (central_label >= kLabelCount) continue;

        std::size_t compatible = 0;
        std::size_t durable = 0;
        for (const Edge& edge : query_graph.adj[query_index]) {
            if (edge.to < 0 ||
                edge.to >= query_graph.num_vertices) {
                continue;
            }
            const Label target_label =
                query_graph.vertex_labels[
                    static_cast<std::size_t>(edge.to)];
            if (target_label >= kLabelCount) continue;
            compatible = saturatingAdd(
                compatible,
                compatible_by_labels[central_label][target_label]);
            durable = saturatingAdd(
                durable,
                durable_by_labels[central_label][target_label]);
        }
        for (const Edge& edge : query_graph.in_adj[query_index]) {
            if (edge.to < 0 ||
                edge.to >= query_graph.num_vertices) {
                continue;
            }
            const Label source_label =
                query_graph.vertex_labels[
                    static_cast<std::size_t>(edge.to)];
            if (source_label >= kLabelCount) continue;
            compatible = saturatingAdd(
                compatible,
                compatible_by_labels[source_label][central_label]);
            durable = saturatingAdd(
                durable,
                durable_by_labels[source_label][central_label]);
        }
        result.compatible_edge_counts[query_index] = compatible;
        result.durable_compatible_edge_counts[query_index] = durable;
        if (compatible > 0) {
            result.durability_survival_rates[query_index] =
                static_cast<double>(durable) /
                static_cast<double>(compatible);
        }
    }
    return result;
}

QueryDecomposition decomposeQuery(
    const Graph& query_graph,
    const std::array<std::size_t, kLabelCount>& label_vertex_counts,
    const std::array<double, kLabelCount>&
        label_mean_vertex_active_snapshots,
    SelectivityMode selectivity_mode,
    const SurvivalSelectivityStatistics* survival_statistics) {
    QueryDecomposition result;
    result.selectivity_mode = selectivity_mode;
    const int query_vertex_count = query_graph.num_vertices;
    result.vertex_labels = query_graph.vertex_labels;
    result.spanning_tree_adj.resize(static_cast<std::size_t>(query_vertex_count));
    result.parent.assign(static_cast<std::size_t>(query_vertex_count), -1);
    result.temporal_selectivity.assign(
        static_cast<std::size_t>(query_vertex_count),
        std::numeric_limits<double>::infinity());
    if (survival_statistics != nullptr) {
        result.structural_candidate_counts =
            survival_statistics->structural_candidate_counts;
        result.compatible_edge_counts =
            survival_statistics->compatible_edge_counts;
        result.durable_compatible_edge_counts =
            survival_statistics->durable_compatible_edge_counts;
        result.durability_survival_rates =
            survival_statistics->durability_survival_rates;
    }
    const std::size_t expected_statistics_size =
        static_cast<std::size_t>(
            std::max(0, query_vertex_count));
    if ((selectivity_mode == SelectivityMode::Structural ||
         selectivity_mode == SelectivityMode::Survival) &&
        (survival_statistics == nullptr ||
         result.structural_candidate_counts.size() !=
             expected_statistics_size ||
         result.compatible_edge_counts.size() !=
             expected_statistics_size ||
         result.durable_compatible_edge_counts.size() !=
             expected_statistics_size ||
         result.durability_survival_rates.size() !=
             expected_statistics_size)) {
        throw std::invalid_argument(
            "survival selectivity requires one statistics row per "
            "query vertex");
    }

    std::size_t best_candidate_count = std::numeric_limits<std::size_t>::max();
    for (int query_vertex = 0; query_vertex < query_vertex_count; ++query_vertex) {
        const Label label = query_graph.vertex_labels[static_cast<std::size_t>(query_vertex)];
        const std::size_t degree =
            query_graph.adj[static_cast<std::size_t>(query_vertex)].size() +
            query_graph.in_adj[static_cast<std::size_t>(query_vertex)].size();
        if (label >= kLabelCount || degree == 0) continue;

        const std::size_t candidate_count = label_vertex_counts[label];
        const double mean_vertex_active_snapshots =
            label_mean_vertex_active_snapshots[label];

        // The current method restores TD-Tree's candidate-count/degree
        // direction and keeps A_dur in the denominator so argmin favors a
        // rare, high-degree, long-lived anchor.  This is a ranking heuristic,
        // not an estimator of the number of candidates surviving k.
        double score =
            std::numeric_limits<double>::infinity();
        switch (selectivity_mode) {
            case SelectivityMode::TDTree:
                score =
                    static_cast<double>(candidate_count) /
                    static_cast<double>(degree);
                break;
            case SelectivityMode::Paper:
            case SelectivityMode::Intended:
            case SelectivityMode::TemporalCost:
            case SelectivityMode::TemporalCostV2:
                if (candidate_count == 0) {
                    score = 0.0;
                } else if (mean_vertex_active_snapshots > 0.0) {
                    score =
                        static_cast<double>(candidate_count) /
                        (static_cast<double>(degree) *
                         mean_vertex_active_snapshots);
                }
                break;
            case SelectivityMode::Survival: {
                const std::size_t structural_candidates =
                    result.structural_candidate_counts[
                        static_cast<std::size_t>(query_vertex)];
                const double survival_rate =
                    result.durability_survival_rates[
                        static_cast<std::size_t>(query_vertex)];
                score =
                    static_cast<double>(structural_candidates) /
                    static_cast<double>(degree) *
                    survival_rate;
                break;
            }
            case SelectivityMode::Structural:
                score =
                    static_cast<double>(
                        result.structural_candidate_counts[
                            static_cast<std::size_t>(
                                query_vertex)]) /
                    static_cast<double>(degree);
                break;
            case SelectivityMode::LegacyExtractedProduct:
                if (candidate_count > 0 &&
                    mean_vertex_active_snapshots > 0.0) {
                    score =
                        static_cast<double>(degree) /
                        static_cast<double>(candidate_count) *
                        mean_vertex_active_snapshots;
                }
                break;
            case SelectivityMode::LegacyReciprocal:
                if (candidate_count > 0 &&
                    mean_vertex_active_snapshots > 0.0) {
                    score =
                        static_cast<double>(degree) /
                        (static_cast<double>(candidate_count) *
                         mean_vertex_active_snapshots);
                }
                break;
            default:
                break;
        }
        result.temporal_selectivity[static_cast<std::size_t>(query_vertex)] = score;
        const std::size_t tie_break_candidate_count =
            (selectivity_mode == SelectivityMode::Structural ||
             selectivity_mode == SelectivityMode::Survival)
            ? result.structural_candidate_counts[
                  static_cast<std::size_t>(query_vertex)]
            : candidate_count;

        const double incumbent_score = result.root < 0
            ? std::numeric_limits<double>::infinity()
            : result.temporal_selectivity[
                  static_cast<std::size_t>(result.root)];
        const bool smaller = result.root < 0 || score < incumbent_score;
        // Only exact score equality is a tie.  An epsilon comparison can
        // replace the true numeric argmin with a slightly larger score merely
        // because that vertex has a smaller candidate-count tie breaker.
        // Exact equality also handles equal positive infinities without the
        // NaN produced by `inf - inf`.
        const bool tied = result.root >= 0 &&
            score == incumbent_score;
        if (smaller ||
            (tied &&
             tie_break_candidate_count < best_candidate_count) ||
            (tied &&
             tie_break_candidate_count == best_candidate_count &&
             query_vertex < result.root)) {
            result.root = query_vertex;
            best_candidate_count = tie_break_candidate_count;
        }
    }

    // No finite score means that no query label currently has data candidates.
    // The structurally valid query should still run and report Count: 0 rather
    // than fail, so choose a deterministic fallback root.
    if (result.root < 0) {
        for (int query_vertex = 0; query_vertex < query_vertex_count; ++query_vertex) {
            if (!query_graph.adj[static_cast<std::size_t>(query_vertex)].empty() ||
                !query_graph.in_adj[static_cast<std::size_t>(query_vertex)].empty()) {
                result.root = query_vertex;
                break;
            }
        }
    }
    if (result.root < 0) return result;

    std::vector<bool> visited(static_cast<std::size_t>(query_vertex_count), false);
    std::set<std::pair<int, int>> tree_edges;
    std::function<void(int)> dfs = [&](int query_vertex) {
        visited[static_cast<std::size_t>(query_vertex)] = true;
        result.dfs_order.push_back(query_vertex);

        std::vector<int> neighbors;
        neighbors.reserve(
            query_graph.adj[static_cast<std::size_t>(query_vertex)].size() +
            query_graph.in_adj[static_cast<std::size_t>(query_vertex)].size());
        for (const auto& edge : query_graph.adj[static_cast<std::size_t>(query_vertex)]) {
            neighbors.push_back(edge.to);
        }
        for (const auto& edge : query_graph.in_adj[static_cast<std::size_t>(query_vertex)]) {
            neighbors.push_back(edge.to);
        }
        std::sort(neighbors.begin(), neighbors.end(), [&](int lhs, int rhs) {
            const double lhs_score = result.temporal_selectivity[static_cast<std::size_t>(lhs)];
            const double rhs_score = result.temporal_selectivity[static_cast<std::size_t>(rhs)];
            if (lhs_score != rhs_score) return lhs_score < rhs_score;
            return lhs < rhs;
        });
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());

        for (int neighbor : neighbors) {
            if (visited[static_cast<std::size_t>(neighbor)]) continue;
            result.parent[static_cast<std::size_t>(neighbor)] = query_vertex;
            result.spanning_tree_adj[static_cast<std::size_t>(query_vertex)].push_back(neighbor);
            tree_edges.emplace(std::min(query_vertex, neighbor), std::max(query_vertex, neighbor));
            dfs(neighbor);
        }
    };
    dfs(result.root);

    result.connected = result.dfs_order.size() == static_cast<std::size_t>(query_vertex_count);
    if (!result.connected) return result;

    for (int query_vertex = 0; query_vertex < query_vertex_count; ++query_vertex) {
        for (const auto& edge : query_graph.adj[static_cast<std::size_t>(query_vertex)]) {
            const std::pair<int, int> normalized{
                std::min(query_vertex, edge.to), std::max(query_vertex, edge.to)};
            if (tree_edges.find(normalized) == tree_edges.end()) {
                // Preserve the original query-arc direction. The spanning tree
                // is only a weak-connectivity search skeleton.
                result.non_tree_edges.emplace_back(query_vertex, edge.to);
            }
        }
    }
    return result;
}
