#include "temporal_cost_planner.h"

#include "TDTree.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace {

struct PrefixSample {
    std::vector<int> mapping;
    std::vector<TimeInterval> intervals;
    bool has_intervals = false;
};

struct BeamState {
    std::vector<int> order;
    std::vector<int> parents;
    std::vector<std::uint8_t> scheduled;
    std::vector<PrefixSample> samples;
    std::vector<TemporalTransitionEstimate> transitions;
    double estimated_prefixes = 0.0;
    double estimated_total = 0.0;
    bool exact_samples = true;
    bool estimated_total_exact = true;
};

enum class BudgetPhase {
    None,
    Catalog,
    StateExpansion,
    ExtensionProbe,
    IntervalIntersection,
    CandidateTree
};

enum class ExtensionProbeKind {
    Beam,
    FixedPlan
};

struct PlannerWorkBudget {
    explicit PlannerWorkBudget(
        const TemporalCostPlannerOptions& options)
        : max_catalog_probes(options.max_catalog_probes),
          max_state_expansions(options.max_state_expansions),
          max_extension_probes(options.max_extension_probes),
          max_interval_intersections(
              options.max_interval_intersections),
          max_candidate_trees_built(
              options.max_candidate_trees_built) {}

    bool takeCatalogProbe() {
        return takeOne(
            catalog_probes,
            max_catalog_probes,
            BudgetPhase::Catalog);
    }

    bool takeStateExpansion() {
        return takeOne(
            state_expansions,
            max_state_expansions,
            BudgetPhase::StateExpansion);
    }

    bool reserveExtensionProbes(
        std::uint64_t amount,
        ExtensionProbeKind kind) {
        if (amount == 0) return true;
        if (extension_probes > max_extension_probes ||
            amount > max_extension_probes - extension_probes) {
            exhausted = true;
            exhausted_phase = BudgetPhase::ExtensionProbe;
            return false;
        }
        extension_probes += amount;
        if (kind == ExtensionProbeKind::Beam) {
            beam_extension_probes += amount;
        } else {
            fixed_plan_extension_probes += amount;
        }
        return true;
    }

    bool takeCandidateTree() {
        return takeOne(
            candidate_trees_built,
            max_candidate_trees_built,
            BudgetPhase::CandidateTree);
    }

    bool takeIntervalIntersection() {
        return takeOne(
            interval_intersections,
            max_interval_intersections,
            BudgetPhase::IntervalIntersection);
    }

    void recordTemporalEdgeApplication() {
        if (temporal_edge_applications <
            std::numeric_limits<std::uint64_t>::max()) {
            ++temporal_edge_applications;
        }
    }

    std::uint64_t max_catalog_probes = 0;
    std::uint64_t max_state_expansions = 0;
    std::uint64_t max_extension_probes = 0;
    std::uint64_t max_interval_intersections = 0;
    std::uint64_t max_candidate_trees_built = 0;
    std::uint64_t catalog_probes = 0;
    std::uint64_t state_expansions = 0;
    std::uint64_t extension_probes = 0;
    std::uint64_t beam_extension_probes = 0;
    std::uint64_t fixed_plan_extension_probes = 0;
    std::uint64_t interval_intersections = 0;
    std::uint64_t temporal_edge_applications = 0;
    std::uint64_t candidate_trees_built = 0;
    bool exhausted = false;
    BudgetPhase exhausted_phase = BudgetPhase::None;

private:
    bool takeOne(
        std::uint64_t& used,
        std::uint64_t maximum,
        BudgetPhase phase) {
        if (used >= maximum) {
            exhausted = true;
            exhausted_phase = phase;
            return false;
        }
        ++used;
        return true;
    }
};

const char* budgetReasonName(BudgetPhase phase) {
    switch (phase) {
        case BudgetPhase::Catalog:
            return "catalog-probe-budget";
        case BudgetPhase::StateExpansion:
            return "state-expansion-budget";
        case BudgetPhase::ExtensionProbe:
            return "extension-probe-budget";
        case BudgetPhase::IntervalIntersection:
            return "interval-intersection-budget";
        case BudgetPhase::CandidateTree:
            return "candidate-tree-budget";
        case BudgetPhase::None:
        default:
            return "none";
    }
}

std::uint64_t saturatingAdd(
    std::uint64_t lhs,
    std::uint64_t rhs) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return lhs + rhs;
}

struct HashedSample {
    std::uint64_t hash = 0;
    std::uint64_t serial = 0;
    PrefixSample sample;
};

bool hashedSampleLess(
    const HashedSample& lhs,
    const HashedSample& rhs) {
    if (lhs.hash != rhs.hash) return lhs.hash < rhs.hash;
    // Resolve the (rare but possible) 64-bit hash collision from sample
    // content, not discovery order.  Serial remains only a tertiary key for
    // duplicate mappings, whose retained sample value is identical.
    if (lhs.sample.mapping != rhs.sample.mapping) {
        return lhs.sample.mapping < rhs.sample.mapping;
    }
    return lhs.serial < rhs.serial;
}

struct HashedSampleLess {
    bool operator()(
        const HashedSample& lhs,
        const HashedSample& rhs) const {
        return hashedSampleLess(lhs, rhs);
    }
};

std::uint64_t splitmix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) *
        0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) *
        0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::uint64_t sampleHash(
    const PrefixSample& sample,
    std::uint64_t seed) {
    std::uint64_t hash = splitmix64(seed);
    for (std::size_t query_vertex = 0;
         query_vertex < sample.mapping.size();
         ++query_vertex) {
        const int data_vertex = sample.mapping[query_vertex];
        if (data_vertex < 0) continue;
        const std::uint64_t token =
            (static_cast<std::uint64_t>(query_vertex + 1) << 32U) ^
            static_cast<std::uint32_t>(data_vertex);
        hash = splitmix64(hash ^ splitmix64(token));
    }
    return hash;
}

class BottomKSample {
public:
    BottomKSample(std::size_t cap, std::uint64_t seed)
        : cap_(cap), seed_(seed) {}

    void consider(PrefixSample sample) {
        ++seen_;
        if (cap_ == 0) return;
        HashedSample entry;
        entry.hash = sampleHash(sample, seed_);
        entry.serial = seen_;
        entry.sample = std::move(sample);
        if (heap_.size() < cap_) {
            heap_.push(std::move(entry));
            return;
        }
        if (hashedSampleLess(entry, heap_.top())) {
            heap_.pop();
            heap_.push(std::move(entry));
        }
    }

    std::uint64_t seen() const {
        return seen_;
    }

    std::vector<PrefixSample> finish() {
        std::vector<HashedSample> entries;
        entries.reserve(heap_.size());
        while (!heap_.empty()) {
            entries.push_back(heap_.top());
            heap_.pop();
        }
        std::sort(
            entries.begin(),
            entries.end(),
            [](const HashedSample& lhs,
               const HashedSample& rhs) {
                return hashedSampleLess(lhs, rhs);
            });
        std::vector<PrefixSample> result;
        result.reserve(entries.size());
        for (auto& entry : entries) {
            result.push_back(std::move(entry.sample));
        }
        return result;
    }

private:
    std::size_t cap_ = 0;
    std::uint64_t seed_ = 0;
    std::uint64_t seen_ = 0;
    std::priority_queue<
        HashedSample,
        std::vector<HashedSample>,
        HashedSampleLess> heap_;
};

struct PairRelation {
    int parent = -1;
    int child = -1;
    std::vector<std::size_t> offsets;
    std::vector<int> candidates;

    std::pair<
        std::vector<int>::const_iterator,
        std::vector<int>::const_iterator>
    block(int parent_data_vertex) const {
        if (parent_data_vertex < 0 ||
            static_cast<std::size_t>(parent_data_vertex + 1) >=
                offsets.size()) {
            return {candidates.end(), candidates.end()};
        }
        const std::size_t begin =
            offsets[static_cast<std::size_t>(parent_data_vertex)];
        const std::size_t end =
            offsets[static_cast<std::size_t>(
                parent_data_vertex + 1)];
        return {
            candidates.begin() +
                static_cast<std::ptrdiff_t>(begin),
            candidates.begin() +
                static_cast<std::ptrdiff_t>(end)};
    }
};

struct DurableRelationCatalog {
    int data_vertex_count = 0;
    int query_vertex_count = 0;
    std::vector<std::vector<std::uint8_t>> domains;
    std::vector<std::vector<int>> domain_candidates;
    std::vector<PairRelation> relations;
    std::vector<int> relation_index;

    const PairRelation* find(int parent, int child) const {
        if (parent < 0 || child < 0 ||
            parent >= query_vertex_count ||
            child >= query_vertex_count) {
            return nullptr;
        }
        const int index = relation_index[
            static_cast<std::size_t>(
                parent * query_vertex_count + child)];
        return index < 0
            ? nullptr
            : &relations[static_cast<std::size_t>(index)];
    }

    std::size_t relationEntryCount() const {
        std::size_t count = 0;
        for (const PairRelation& relation : relations) {
            count += relation.candidates.size();
        }
        return count;
    }
};

PairRelation relationFromPairs(
    int parent,
    int child,
    int data_vertex_count,
    std::vector<std::pair<int, int>> pairs) {
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(
        std::unique(pairs.begin(), pairs.end()),
        pairs.end());
    PairRelation result;
    result.parent = parent;
    result.child = child;
    result.offsets.assign(
        static_cast<std::size_t>(data_vertex_count) + 1,
        0);
    for (const auto& pair : pairs) {
        ++result.offsets[
            static_cast<std::size_t>(pair.first + 1)];
    }
    for (std::size_t index = 1;
         index < result.offsets.size();
         ++index) {
        result.offsets[index] +=
            result.offsets[index - 1];
    }
    result.candidates.reserve(pairs.size());
    for (const auto& pair : pairs) {
        result.candidates.push_back(pair.second);
    }
    return result;
}

void installRelation(
    DurableRelationCatalog& catalog,
    PairRelation relation) {
    const int index =
        static_cast<int>(catalog.relations.size());
    catalog.relation_index[
        static_cast<std::size_t>(
            relation.parent *
                catalog.query_vertex_count +
            relation.child)] = index;
    catalog.relations.push_back(std::move(relation));
}

std::optional<DurableRelationCatalog> buildRelationCatalog(
    const Graph& data,
    const Graph& query,
    int minimum_duration,
    PlannerWorkBudget* budget = nullptr) {
    DurableRelationCatalog catalog;
    catalog.data_vertex_count = data.num_vertices;
    catalog.query_vertex_count = query.num_vertices;
    catalog.domains.assign(
        static_cast<std::size_t>(query.num_vertices),
        std::vector<std::uint8_t>(
            static_cast<std::size_t>(data.num_vertices), 0));
    catalog.relation_index.assign(
        static_cast<std::size_t>(
            query.num_vertices * query.num_vertices),
        -1);
    std::vector<std::vector<int>> initial_domain_candidates(
        static_cast<std::size_t>(query.num_vertices));

    std::vector<std::array<int, kLabelCount>> out_requirements(
        static_cast<std::size_t>(query.num_vertices));
    std::vector<std::array<int, kLabelCount>> in_requirements(
        static_cast<std::size_t>(query.num_vertices));
    for (int query_vertex = 0;
         query_vertex < query.num_vertices;
         ++query_vertex) {
        for (const Edge& edge :
             query.adj[static_cast<std::size_t>(query_vertex)]) {
            const Label label = query.vertex_labels[
                static_cast<std::size_t>(edge.to)];
            if (label < kLabelCount) {
                ++out_requirements[
                    static_cast<std::size_t>(query_vertex)][label];
            }
        }
        for (const Edge& edge :
             query.in_adj[static_cast<std::size_t>(query_vertex)]) {
            const Label label = query.vertex_labels[
                static_cast<std::size_t>(edge.to)];
            if (label < kLabelCount) {
                ++in_requirements[
                    static_cast<std::size_t>(query_vertex)][label];
            }
        }
    }

    for (int query_vertex = 0;
         query_vertex < query.num_vertices;
         ++query_vertex) {
        const bool requires_self_loop = GraphUtils::hasEdge(
            query.adj, query_vertex, query_vertex);
        for (int data_vertex = 0;
             data_vertex < data.num_vertices;
             ++data_vertex) {
            if (budget != nullptr &&
                !budget->takeCatalogProbe()) {
                return std::nullopt;
            }
            const std::size_t d =
                static_cast<std::size_t>(data_vertex);
            const std::size_t q =
                static_cast<std::size_t>(query_vertex);
            if (data.vertex_labels[d] != query.vertex_labels[q] ||
                data.adj[d].size() < query.adj[q].size() ||
                data.in_adj[d].size() < query.in_adj[q].size() ||
                data.vertex_active_durations[d] < minimum_duration) {
                continue;
            }
            bool locally_valid = true;
            for (std::size_t label = 0;
                 label < kLabelCount;
                 ++label) {
                if (data.neighbor_label_counts[d][label] <
                        out_requirements[q][label] ||
                    data.incoming_neighbor_label_counts[d][label] <
                        in_requirements[q][label]) {
                    locally_valid = false;
                    break;
                }
            }
            if (!locally_valid) continue;
            if (requires_self_loop) {
                const TemporalEdge* self_edge =
                    data.findTemporalEdge(data_vertex, data_vertex);
                if (self_edge == nullptr ||
                    !hasMinimumConsecutiveDuration(
                        self_edge->activeIntervals(),
                        minimum_duration)) {
                    continue;
                }
            }
            catalog.domains[
                static_cast<std::size_t>(query_vertex)]
                [static_cast<std::size_t>(data_vertex)] = 1;
            initial_domain_candidates[
                static_cast<std::size_t>(query_vertex)]
                .push_back(data_vertex);
        }
    }

    for (int first = 0;
         first < query.num_vertices;
         ++first) {
        for (int second = first + 1;
             second < query.num_vertices;
             ++second) {
            const bool first_to_second =
                GraphUtils::hasEdge(
                    query.adj, first, second);
            const bool second_to_first =
                GraphUtils::hasEdge(
                    query.adj, second, first);
            if (!first_to_second && !second_to_first) {
                continue;
            }

            std::vector<std::pair<int, int>> pairs;
            for (int parent_data :
                 initial_domain_candidates[
                     static_cast<std::size_t>(first)]) {
                if (budget != nullptr &&
                    !budget->takeCatalogProbe()) {
                    return std::nullopt;
                }
                const auto& expansion_edges =
                    first_to_second
                    ? data.adj[static_cast<std::size_t>(
                          parent_data)]
                    : data.in_adj[static_cast<std::size_t>(
                          parent_data)];
                for (const Edge& edge_ref :
                     expansion_edges) {
                    if (budget != nullptr &&
                        !budget->takeCatalogProbe()) {
                        return std::nullopt;
                    }
                    const int child_data = edge_ref.to;
                    if (catalog.domains[
                            static_cast<std::size_t>(second)]
                            [static_cast<std::size_t>(
                                child_data)] == 0) {
                        continue;
                    }
                    const TemporalEdge* forward_edge =
                        first_to_second
                        ? data.findTemporalEdge(
                              parent_data, child_data)
                        : nullptr;
                    const TemporalEdge* reverse_edge =
                        second_to_first
                        ? data.findTemporalEdge(
                              child_data, parent_data)
                        : nullptr;
                    if ((first_to_second &&
                         (forward_edge == nullptr ||
                          !hasMinimumConsecutiveDuration(
                              forward_edge->activeIntervals(),
                              minimum_duration))) ||
                        (second_to_first &&
                         (reverse_edge == nullptr ||
                          !hasMinimumConsecutiveDuration(
                              reverse_edge->activeIntervals(),
                              minimum_duration)))) {
                        continue;
                    }
                    pairs.emplace_back(
                        parent_data, child_data);
                }
            }
            installRelation(
                catalog,
                relationFromPairs(
                    first,
                    second,
                    data.num_vertices,
                    pairs));
            for (auto& pair : pairs) {
                std::swap(pair.first, pair.second);
            }
            installRelation(
                catalog,
                relationFromPairs(
                    second,
                    first,
                    data.num_vertices,
                    std::move(pairs)));
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (const PairRelation& relation :
             catalog.relations) {
            auto& parent_domain =
                catalog.domains[
                    static_cast<std::size_t>(
                        relation.parent)];
            const auto& child_domain =
                catalog.domains[
                    static_cast<std::size_t>(
                        relation.child)];
            for (int parent_data = 0;
                 parent_data < data.num_vertices;
                 ++parent_data) {
                if (budget != nullptr &&
                    !budget->takeCatalogProbe()) {
                    return std::nullopt;
                }
                if (parent_domain[
                        static_cast<std::size_t>(
                            parent_data)] == 0) {
                    continue;
                }
                const auto block =
                    relation.block(parent_data);
                bool supported = false;
                for (auto it = block.first;
                     it != block.second;
                     ++it) {
                    if (budget != nullptr &&
                        !budget->takeCatalogProbe()) {
                        return std::nullopt;
                    }
                    if (child_domain[
                            static_cast<std::size_t>(*it)] != 0) {
                        supported = true;
                        break;
                    }
                }
                if (!supported) {
                    parent_domain[
                        static_cast<std::size_t>(
                            parent_data)] = 0;
                    changed = true;
                }
            }
        }
    }

    for (PairRelation& relation : catalog.relations) {
        std::vector<std::pair<int, int>> filtered;
        const auto& parent_domain =
            catalog.domains[
                static_cast<std::size_t>(relation.parent)];
        const auto& child_domain =
            catalog.domains[
                static_cast<std::size_t>(relation.child)];
        for (int parent_data = 0;
             parent_data < data.num_vertices;
             ++parent_data) {
            if (budget != nullptr &&
                !budget->takeCatalogProbe()) {
                return std::nullopt;
            }
            if (parent_domain[
                    static_cast<std::size_t>(parent_data)] == 0) {
                continue;
            }
            const auto block = relation.block(parent_data);
            for (auto it = block.first;
                 it != block.second;
                 ++it) {
                if (budget != nullptr &&
                    !budget->takeCatalogProbe()) {
                    return std::nullopt;
                }
                if (child_domain[
                        static_cast<std::size_t>(*it)] != 0) {
                    filtered.emplace_back(parent_data, *it);
                }
            }
        }
        relation = relationFromPairs(
            relation.parent,
            relation.child,
            data.num_vertices,
            std::move(filtered));
    }

    catalog.domain_candidates.resize(
        static_cast<std::size_t>(query.num_vertices));
    for (int query_vertex = 0;
         query_vertex < query.num_vertices;
         ++query_vertex) {
        auto& candidates =
            catalog.domain_candidates[
                static_cast<std::size_t>(query_vertex)];
        for (int data_vertex :
             initial_domain_candidates[
                 static_cast<std::size_t>(query_vertex)]) {
            if (budget != nullptr &&
                !budget->takeCatalogProbe()) {
                return std::nullopt;
            }
            if (catalog.domains[
                    static_cast<std::size_t>(query_vertex)]
                    [static_cast<std::size_t>(
                        data_vertex)] != 0) {
                candidates.push_back(data_vertex);
            }
        }
    }
    // The final beam uses compact candidate lists and pair relations only.
    // Release the mutable arc-consistency bitmap before constructing the
    // per-plan TD-trees instead of retaining an unused |V_Q| x |V_G| array.
    std::vector<std::vector<std::uint8_t>>().swap(
        catalog.domains);
    return catalog;
}

QueryDecomposition makePlanDecomposition(
    const Graph& query,
    const QueryDecomposition& score_template,
    const BeamState& plan,
    SelectivityMode output_mode) {
    QueryDecomposition result = score_template;
    result.selectivity_mode = output_mode;
    result.root =
        plan.order.empty() ? -1 : plan.order.front();
    result.dfs_order = plan.order;
    result.parent = plan.parents;
    result.spanning_tree_adj.assign(
        static_cast<std::size_t>(query.num_vertices), {});
    result.non_tree_edges.clear();
    std::set<std::pair<int, int>> tree_edges;
    for (int child = 0;
         child < query.num_vertices;
         ++child) {
        const int parent =
            result.parent[static_cast<std::size_t>(child)];
        if (parent < 0) continue;
        result.spanning_tree_adj[
            static_cast<std::size_t>(parent)]
            .push_back(child);
        tree_edges.emplace(
            std::min(parent, child),
            std::max(parent, child));
    }
    for (int source = 0;
         source < query.num_vertices;
         ++source) {
        for (const Edge& edge :
             query.adj[static_cast<std::size_t>(source)]) {
            const std::pair<int, int> normalized{
                std::min(source, edge.to),
                std::max(source, edge.to)};
            if (tree_edges.find(normalized) ==
                tree_edges.end()) {
                result.non_tree_edges.emplace_back(
                    source, edge.to);
            }
        }
    }
    result.connected =
        result.dfs_order.size() ==
        static_cast<std::size_t>(query.num_vertices);
    return result;
}

std::optional<bool> applyTemporalEdge(
    const Graph& data,
    int source,
    int target,
    int minimum_duration,
    std::vector<TimeInterval>& intervals,
    bool& has_intervals,
    PlannerWorkBudget* budget = nullptr) {
    if (budget != nullptr) {
        budget->recordTemporalEdgeApplication();
    }
    const TemporalEdge* edge =
        data.findTemporalEdge(source, target);
    if (edge == nullptr) return false;
    if (!has_intervals) {
        intervals.clear();
        for (const TimeInterval& interval :
             edge->activeIntervals()) {
            if (interval.length() >= minimum_duration) {
                intervals.push_back(interval);
            }
        }
        has_intervals = true;
    } else {
        if (budget != nullptr &&
            !budget->takeIntervalIntersection()) {
            return std::nullopt;
        }
        intervals = intersectTimeIntervals(
            intervals,
            edge->activeIntervals(),
            minimum_duration);
    }
    return !intervals.empty();
}

std::optional<bool> extendSample(
    const Graph& data,
    const Graph& query,
    int minimum_duration,
    int query_vertex,
    int candidate,
    const PrefixSample& prefix,
    PrefixSample& result,
    PlannerWorkBudget* budget = nullptr) {
    if (std::find(
            prefix.mapping.begin(),
            prefix.mapping.end(),
            candidate) != prefix.mapping.end()) {
        return false;
    }
    result = prefix;
    result.mapping[
        static_cast<std::size_t>(query_vertex)] = candidate;

    if (GraphUtils::hasEdge(
            query.adj, query_vertex, query_vertex)) {
        const std::optional<bool> applied = applyTemporalEdge(
            data,
            candidate,
            candidate,
            minimum_duration,
            result.intervals,
            result.has_intervals,
            budget);
        if (!applied.has_value()) return std::nullopt;
        if (!*applied) return false;
    }
    for (const Edge& query_edge :
         query.adj[static_cast<std::size_t>(query_vertex)]) {
        const int other_query_vertex = query_edge.to;
        if (other_query_vertex == query_vertex) continue;
        const int other_data_vertex =
            result.mapping[
                static_cast<std::size_t>(other_query_vertex)];
        if (other_data_vertex < 0) continue;
        const std::optional<bool> applied = applyTemporalEdge(
                data,
                candidate,
                other_data_vertex,
                minimum_duration,
                result.intervals,
                result.has_intervals,
                budget);
        if (!applied.has_value()) return std::nullopt;
        if (!*applied) return false;
    }
    for (const Edge& query_edge :
         query.in_adj[static_cast<std::size_t>(query_vertex)]) {
        const int other_query_vertex = query_edge.to;
        if (other_query_vertex == query_vertex) continue;
        const int other_data_vertex =
            result.mapping[
                static_cast<std::size_t>(other_query_vertex)];
        if (other_data_vertex < 0) continue;
        const std::optional<bool> applied = applyTemporalEdge(
                data,
                other_data_vertex,
                candidate,
                minimum_duration,
                result.intervals,
                result.has_intervals,
                budget);
        if (!applied.has_value()) return std::nullopt;
        if (!*applied) return false;
    }
    return true;
}

BeamState childStateWithoutSamples(
    const BeamState& state
#ifdef DSM_TCC_TEST_REFERENCE_FULL_SORT
    , bool reference_deep_copy
#endif
    ) {
#ifdef DSM_TCC_TEST_REFERENCE_FULL_SORT
    if (reference_deep_copy) {
        // Reproduce the pre-M3 implementation for direct parity testing: a
        // child first inherited the complete parent state, including its
        // potentially large sample vector.  Successful extension replaces
        // result.samples later in the caller.
        BeamState result = state;
        return result;
    }
#endif
    BeamState result;
    // Samples dominate BeamState size and are always replaced by the child
    // sample.  Copy only metadata that is semantically inherited.
    result.order = state.order;
    result.parents = state.parents;
    result.scheduled = state.scheduled;
    result.transitions = state.transitions;
    result.estimated_prefixes = state.estimated_prefixes;
    result.estimated_total = state.estimated_total;
    result.exact_samples = state.exact_samples;
    result.estimated_total_exact = state.estimated_total_exact;
    return result;
}

double estimatedRawExtensions(const BeamState& state) {
    double raw = 0.0;
    for (const TemporalTransitionEstimate& transition :
         state.transitions) {
        raw += transition.estimated_raw_extensions;
    }
    return raw;
}

std::uint64_t planFingerprint(
    const QueryDecomposition& decomposition) {
    std::uint64_t hash = UINT64_C(1469598103934665603);
    auto mix = [&](std::uint64_t value) {
        hash ^= splitmix64(value + UINT64_C(0x9e3779b97f4a7c15));
        hash *= UINT64_C(1099511628211);
    };
    mix(static_cast<std::uint64_t>(decomposition.root + 1));
    mix(static_cast<std::uint64_t>(decomposition.dfs_order.size()));
    for (int vertex : decomposition.dfs_order) {
        mix(static_cast<std::uint64_t>(vertex + 1));
    }
    mix(static_cast<std::uint64_t>(decomposition.parent.size()));
    for (int parent : decomposition.parent) {
        mix(static_cast<std::uint64_t>(parent + 2));
    }
    return hash;
}

std::optional<BeamState> initialState(
    const Graph& data,
    const Graph& query,
    const DurableRelationCatalog& catalog,
    int root,
    int minimum_duration,
    const TemporalCostPlannerOptions& options,
    PlannerWorkBudget* budget = nullptr) {
    BeamState state;
    state.order.push_back(root);
    state.parents.assign(
        static_cast<std::size_t>(query.num_vertices), -1);
    state.scheduled.assign(
        static_cast<std::size_t>(query.num_vertices), 0);
    state.scheduled[
        static_cast<std::size_t>(root)] = 1;
    const auto& root_candidates =
        catalog.domain_candidates[
            static_cast<std::size_t>(root)];
    state.estimated_total =
        static_cast<double>(root_candidates.size());

    BottomKSample sampler(
        options.sample_cap,
        options.sample_seed ^
            static_cast<std::uint64_t>(
                root + 1));
    std::uint64_t viable_roots = 0;
    for (int root_candidate : root_candidates) {
        PrefixSample sample;
        sample.mapping.assign(
            static_cast<std::size_t>(query.num_vertices), -1);
        sample.mapping[
            static_cast<std::size_t>(root)] =
            root_candidate;
        if (GraphUtils::hasEdge(
                query.adj,
                root,
                root)) {
            const std::optional<bool> applied = applyTemporalEdge(
                data,
                root_candidate,
                root_candidate,
                minimum_duration,
                sample.intervals,
                sample.has_intervals,
                budget);
            if (!applied.has_value()) return std::nullopt;
            if (!*applied) continue;
        }
        ++viable_roots;
        sampler.consider(std::move(sample));
    }
    state.samples = sampler.finish();
    state.estimated_prefixes =
        static_cast<double>(viable_roots);
    state.exact_samples =
        viable_roots <= options.sample_cap;
    return state;
}

std::optional<BeamState> extendState(
    const Graph& data,
    const Graph& query,
    const DurableRelationCatalog& catalog,
    int minimum_duration,
    const TemporalCostPlannerOptions& options,
    const BeamState& state,
    int parent,
    int next,
    PlannerWorkBudget* budget = nullptr) {
    BeamState result = childStateWithoutSamples(
        state
#ifdef DSM_TCC_TEST_REFERENCE_FULL_SORT
        , options.test_reference_full_sort
#endif
        );
    result.order.push_back(next);
    result.parents[static_cast<std::size_t>(next)] = parent;
    result.scheduled[static_cast<std::size_t>(next)] = 1;

    TemporalTransitionEstimate transition;
    transition.prefix_order = state.order;
    transition.parent = parent;
    transition.next = next;
    transition.sampled_prefixes = state.samples.size();
    transition.input_prefix_population_exact =
        state.exact_samples;

    BottomKSample accepted_samples(
        options.sample_cap,
        options.sample_seed ^
            splitmix64(
                static_cast<std::uint64_t>(
                    result.order.size() * 131U + next)));
    std::uint64_t raw = 0;
    std::uint64_t accepted = 0;
    const PairRelation* relation =
        catalog.find(parent, next);
    if (relation == nullptr) {
        return result;
    }
    if (budget != nullptr) {
        std::uint64_t required_probes = 0;
        for (const PrefixSample& prefix : state.samples) {
            const int parent_data_vertex =
                prefix.mapping[static_cast<std::size_t>(parent)];
            const auto candidates =
                relation->block(parent_data_vertex);
            required_probes = saturatingAdd(
                required_probes,
                static_cast<std::uint64_t>(std::distance(
                    candidates.first,
                    candidates.second)));
        }
        if (!budget->reserveExtensionProbes(
                required_probes, ExtensionProbeKind::Beam)) {
            return std::nullopt;
        }
        raw = required_probes;
    }
    for (const PrefixSample& prefix : state.samples) {
        const int parent_data_vertex =
            prefix.mapping[static_cast<std::size_t>(parent)];
        const auto candidates =
            relation->block(parent_data_vertex);
        const std::size_t candidate_count =
            static_cast<std::size_t>(
                std::distance(
                    candidates.first,
                    candidates.second));
        if (budget == nullptr) {
            raw = saturatingAdd(
                raw,
                static_cast<std::uint64_t>(candidate_count));
        }
        for (auto candidate_it = candidates.first;
             candidate_it != candidates.second;
             ++candidate_it) {
            const int candidate = *candidate_it;
            PrefixSample extended;
            const std::optional<bool> extended_result = extendSample(
                    data,
                    query,
                    minimum_duration,
                    next,
                    candidate,
                    prefix,
                    extended,
                    budget);
            if (!extended_result.has_value()) return std::nullopt;
            if (!*extended_result) continue;
            if (accepted <
                std::numeric_limits<std::uint64_t>::max()) {
                ++accepted;
            }
            accepted_samples.consider(std::move(extended));
        }
    }
    transition.sampled_raw_extensions = raw;
    transition.sampled_accepted_extensions = accepted;
    if (!state.samples.empty()) {
        transition.raw_per_prefix =
            static_cast<double>(raw) /
            static_cast<double>(state.samples.size());
        transition.estimated_raw_extensions =
            state.exact_samples
            ? static_cast<double>(raw)
            : state.estimated_prefixes *
                transition.raw_per_prefix;
        transition.estimated_next_prefixes =
            state.exact_samples
            ? static_cast<double>(accepted)
            : state.estimated_prefixes *
                static_cast<double>(accepted) /
                static_cast<double>(state.samples.size());
    }
    if (raw > 0) {
        transition.overall_extension_pass_rate =
            static_cast<double>(accepted) /
            static_cast<double>(raw);
    }
    transition.output_prefix_population_exact =
        state.exact_samples &&
        accepted <= options.sample_cap;

    result.transitions.push_back(transition);
    result.estimated_total +=
        transition.estimated_raw_extensions;
    result.estimated_total_exact =
        state.estimated_total_exact &&
        state.exact_samples;
    result.estimated_prefixes =
        transition.estimated_next_prefixes;
    result.samples = accepted_samples.finish();
    result.exact_samples =
        transition.output_prefix_population_exact;
    return result;
}

std::optional<BeamState> initialTreeState(
    const Graph& data,
    const Graph& query,
    const QueryDecomposition& decomposition,
    const TDTree& tree,
    int minimum_duration,
    const TemporalCostPlannerOptions& options,
    PlannerWorkBudget* budget = nullptr) {
    BeamState state;
    state.order.push_back(decomposition.root);
    state.parents = decomposition.parent;
    state.scheduled.assign(
        static_cast<std::size_t>(query.num_vertices), 0);
    state.scheduled[
        static_cast<std::size_t>(decomposition.root)] = 1;
    state.estimated_total =
        static_cast<double>(tree.rootCandidates().size());

    BottomKSample sampler(
        options.sample_cap,
        options.sample_seed ^
            splitmix64(
                static_cast<std::uint64_t>(
                    decomposition.root + 1009)));
    std::uint64_t viable_roots = 0;
    for (int root_candidate : tree.rootCandidates()) {
        PrefixSample sample;
        sample.mapping.assign(
            static_cast<std::size_t>(query.num_vertices), -1);
        sample.mapping[
            static_cast<std::size_t>(decomposition.root)] =
            root_candidate;
        if (GraphUtils::hasEdge(
                query.adj,
                decomposition.root,
                decomposition.root)) {
            const std::optional<bool> applied = applyTemporalEdge(
                data,
                root_candidate,
                root_candidate,
                minimum_duration,
                sample.intervals,
                sample.has_intervals,
                budget);
            if (!applied.has_value()) return std::nullopt;
            if (!*applied) continue;
        }
        ++viable_roots;
        sampler.consider(std::move(sample));
    }
    state.samples = sampler.finish();
    state.estimated_prefixes =
        static_cast<double>(viable_roots);
    state.exact_samples =
        viable_roots <= options.sample_cap;
    return state;
}

std::optional<BeamState> extendTreeState(
    const Graph& data,
    const Graph& query,
    const TDTree& tree,
    int minimum_duration,
    const TemporalCostPlannerOptions& options,
    const BeamState& state,
    int parent,
    int next,
    PlannerWorkBudget* budget = nullptr) {
    BeamState result = childStateWithoutSamples(
        state
#ifdef DSM_TCC_TEST_REFERENCE_FULL_SORT
        , options.test_reference_full_sort
#endif
        );
    result.order.push_back(next);
    result.scheduled[static_cast<std::size_t>(next)] = 1;

    TemporalTransitionEstimate transition;
    transition.prefix_order = state.order;
    transition.parent = parent;
    transition.next = next;
    transition.sampled_prefixes = state.samples.size();
    transition.input_prefix_population_exact =
        state.exact_samples;
    BottomKSample accepted_samples(
        options.sample_cap,
        options.sample_seed ^
            splitmix64(
                static_cast<std::uint64_t>(
                    result.order.size() * 4099U + next)));
    std::uint64_t raw = 0;
    std::uint64_t accepted = 0;
    if (budget != nullptr) {
        std::uint64_t required_probes = 0;
        for (const PrefixSample& prefix : state.samples) {
            const int parent_data =
                prefix.mapping[static_cast<std::size_t>(parent)];
            const std::vector<int>* candidates =
                tree.candidatesForParent(next, parent_data);
            if (candidates == nullptr) continue;
            required_probes = saturatingAdd(
                required_probes,
                static_cast<std::uint64_t>(candidates->size()));
        }
        if (!budget->reserveExtensionProbes(
                required_probes, ExtensionProbeKind::FixedPlan)) {
            return std::nullopt;
        }
        raw = required_probes;
    }
    for (const PrefixSample& prefix : state.samples) {
        const int parent_data =
            prefix.mapping[static_cast<std::size_t>(parent)];
        const std::vector<int>* candidates =
            tree.candidatesForParent(next, parent_data);
        if (candidates == nullptr) continue;
        if (budget == nullptr) {
            raw = saturatingAdd(
                raw,
                static_cast<std::uint64_t>(candidates->size()));
        }
        for (int candidate : *candidates) {
            PrefixSample extended;
            const std::optional<bool> extended_result = extendSample(
                    data,
                    query,
                    minimum_duration,
                    next,
                    candidate,
                    prefix,
                    extended,
                    budget);
            if (!extended_result.has_value()) return std::nullopt;
            if (!*extended_result) continue;
            if (accepted <
                std::numeric_limits<std::uint64_t>::max()) {
                ++accepted;
            }
            accepted_samples.consider(std::move(extended));
        }
    }
    transition.sampled_raw_extensions = raw;
    transition.sampled_accepted_extensions = accepted;
    if (!state.samples.empty()) {
        transition.raw_per_prefix =
            static_cast<double>(raw) /
            static_cast<double>(state.samples.size());
        transition.estimated_raw_extensions =
            state.exact_samples
            ? static_cast<double>(raw)
            : state.estimated_prefixes *
                transition.raw_per_prefix;
        transition.estimated_next_prefixes =
            state.exact_samples
            ? static_cast<double>(accepted)
            : state.estimated_prefixes *
                static_cast<double>(accepted) /
                static_cast<double>(state.samples.size());
    }
    if (raw > 0) {
        transition.overall_extension_pass_rate =
            static_cast<double>(accepted) /
            static_cast<double>(raw);
    }
    transition.output_prefix_population_exact =
        state.exact_samples &&
        accepted <= options.sample_cap;
    result.transitions.push_back(transition);
    result.estimated_total +=
        transition.estimated_raw_extensions;
    result.estimated_total_exact =
        state.estimated_total_exact &&
        state.exact_samples;
    result.estimated_prefixes =
        transition.estimated_next_prefixes;
    result.samples = accepted_samples.finish();
    result.exact_samples =
        transition.output_prefix_population_exact;
    return result;
}

std::optional<BeamState> extendTreeStatePilot(
    const Graph& data,
    const Graph& query,
    const TDTree& tree,
    int minimum_duration,
    const TemporalCostPlannerOptions& options,
    const BeamState& state,
    int parent,
    int next,
    std::uint64_t probe_cap,
    std::uint64_t& probes_used,
    PlannerWorkBudget* budget) {
    BeamState result = childStateWithoutSamples(
        state
#ifdef DSM_TCC_TEST_REFERENCE_FULL_SORT
        , options.test_reference_full_sort
#endif
        );
    result.order.push_back(next);
    result.scheduled[static_cast<std::size_t>(next)] = 1;

    TemporalTransitionEstimate transition;
    transition.prefix_order = state.order;
    transition.parent = parent;
    transition.next = next;
    transition.sampled_prefixes = state.samples.size();
    transition.input_prefix_population_exact = state.exact_samples;

    struct PrefixBlock {
        const PrefixSample* prefix = nullptr;
        const std::vector<int>* candidates = nullptr;
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
    };
    std::vector<PrefixBlock> blocks;
    blocks.reserve(state.samples.size());
    std::uint64_t raw = 0;
    for (const PrefixSample& prefix : state.samples) {
        const int parent_data =
            prefix.mapping[static_cast<std::size_t>(parent)];
        const std::vector<int>* candidates =
            tree.candidatesForParent(next, parent_data);
        if (candidates == nullptr || candidates->empty()) continue;
        PrefixBlock block;
        block.prefix = &prefix;
        block.candidates = candidates;
        block.begin = raw;
        raw = saturatingAdd(
            raw,
            static_cast<std::uint64_t>(candidates->size()));
        block.end = raw;
        blocks.push_back(block);
    }

    const std::uint64_t probe_count = std::min(raw, probe_cap);
    if (budget != nullptr &&
        !budget->reserveExtensionProbes(
            probe_count, ExtensionProbeKind::FixedPlan)) {
        return std::nullopt;
    }
    // The deterministic rank set is reserved as one logical unit. Count it
    // before temporal checks so an interval-budget exit preserves exact
    // fixed-plan/global probe accounting.
    probes_used = saturatingAdd(probes_used, probe_count);
    BottomKSample accepted_samples(
        options.sample_cap,
        options.sample_seed ^
            splitmix64(static_cast<std::uint64_t>(
                result.order.size() * 65537U + next)));
    std::uint64_t accepted = 0;
    if (probe_count != 0 && raw != 0) {
        std::uint64_t start = splitmix64(
            options.sample_seed ^
            static_cast<std::uint64_t>(parent + 17) ^
            (static_cast<std::uint64_t>(next + 31) << 32U)) % raw;
        std::uint64_t step = raw == 1
            ? 1
            : splitmix64(start ^ options.sample_seed) % (raw - 1) + 1;
        while (raw > 1 && std::gcd(step, raw) != 1) {
            step = step == raw - 1 ? 1 : step + 1;
        }
        std::vector<std::uint64_t> ranks;
        ranks.reserve(static_cast<std::size_t>(probe_count));
        std::uint64_t rank = start;
        for (std::uint64_t index = 0;
             index < probe_count;
             ++index) {
            ranks.push_back(rank);
            rank = (rank + step) % raw;
        }
        std::sort(ranks.begin(), ranks.end());

        std::size_t block_index = 0;
        for (std::uint64_t selected_rank : ranks) {
            while (block_index + 1 < blocks.size() &&
                   selected_rank >= blocks[block_index].end) {
                ++block_index;
            }
            const PrefixBlock& block = blocks[block_index];
            const std::size_t local_index =
                static_cast<std::size_t>(
                    selected_rank - block.begin);
            PrefixSample extended;
            const std::optional<bool> extended_result = extendSample(
                    data,
                    query,
                    minimum_duration,
                    next,
                    (*block.candidates)[local_index],
                    *block.prefix,
                    extended,
                    budget);
            if (!extended_result.has_value()) return std::nullopt;
            if (!*extended_result) continue;
            ++accepted;
            accepted_samples.consider(std::move(extended));
        }
    }
    transition.sampled_raw_extensions = raw;
    transition.sampled_accepted_extensions = accepted;
    if (!state.samples.empty()) {
        transition.raw_per_prefix =
            static_cast<double>(raw) /
            static_cast<double>(state.samples.size());
        transition.estimated_raw_extensions =
            state.exact_samples
            ? static_cast<double>(raw)
            : state.estimated_prefixes * transition.raw_per_prefix;
    }
    if (probe_count != 0) {
        transition.overall_extension_pass_rate =
            static_cast<double>(accepted) /
            static_cast<double>(probe_count);
        transition.estimated_next_prefixes =
            transition.estimated_raw_extensions *
            transition.overall_extension_pass_rate;
    }
    transition.output_prefix_population_exact =
        state.exact_samples && probe_count == raw &&
        accepted <= options.sample_cap;

    result.transitions.push_back(transition);
    result.estimated_total +=
        transition.estimated_raw_extensions;
    result.estimated_total_exact =
        state.estimated_total_exact &&
        transition.output_prefix_population_exact;
    result.estimated_prefixes =
        transition.estimated_next_prefixes;
    result.samples = accepted_samples.finish();
    result.exact_samples =
        transition.output_prefix_population_exact;
    return result;
}

std::optional<BeamState> rescoreFixedPlanPilot(
    const Graph& data,
    const Graph& query,
    const QueryDecomposition& decomposition,
    const TDTree& tree,
    int minimum_duration,
    const TemporalCostPlannerOptions& options,
    std::uint64_t& probes_used,
    PlannerWorkBudget* budget = nullptr) {
    TemporalCostPlannerOptions pilot_options = options;
    pilot_options.sample_cap = std::min(
        options.sample_cap,
        options.pilot_prefix_cap);
    std::optional<BeamState> initial_state = initialTreeState(
        data,
        query,
        decomposition,
        tree,
        minimum_duration,
        pilot_options,
        budget);
    if (!initial_state.has_value()) return std::nullopt;
    BeamState state = std::move(*initial_state);
    probes_used = 0;
    for (std::size_t depth = 1;
         depth < decomposition.dfs_order.size();
         ++depth) {
        if (budget != nullptr &&
            !budget->takeStateExpansion()) {
            return std::nullopt;
        }
        const std::uint64_t remaining =
            options.pilot_extension_probe_cap > probes_used
            ? options.pilot_extension_probe_cap - probes_used
            : 0;
        const std::uint64_t remaining_depths =
            static_cast<std::uint64_t>(
                decomposition.dfs_order.size() - depth);
        const std::uint64_t depth_cap = remaining_depths == 0
            ? 0
            : (remaining + remaining_depths - 1) /
                remaining_depths;
        const int next = decomposition.dfs_order[depth];
        const int parent = decomposition.parent[
            static_cast<std::size_t>(next)];
        std::optional<BeamState> next_state = extendTreeStatePilot(
            data,
            query,
            tree,
            minimum_duration,
            pilot_options,
            state,
            parent,
            next,
            depth_cap,
            probes_used,
            budget);
        if (!next_state.has_value()) {
            return std::nullopt;
        }
        state = std::move(*next_state);
    }
    return state;
}

std::optional<BeamState> rescoreFixedPlan(
    const Graph& data,
    const Graph& query,
    const QueryDecomposition& decomposition,
    const TDTree& tree,
    int minimum_duration,
    const TemporalCostPlannerOptions& options,
    PlannerWorkBudget* budget = nullptr) {
    std::optional<BeamState> initial_state = initialTreeState(
        data,
        query,
        decomposition,
        tree,
        minimum_duration,
        options,
        budget);
    if (!initial_state.has_value()) return std::nullopt;
    BeamState state = std::move(*initial_state);
    for (std::size_t depth = 1;
         depth < decomposition.dfs_order.size();
         ++depth) {
        const int next = decomposition.dfs_order[depth];
        const int parent =
            decomposition.parent[
                static_cast<std::size_t>(next)];
        if (budget != nullptr &&
            !budget->takeStateExpansion()) {
            return std::nullopt;
        }
        std::optional<BeamState> next_state = extendTreeState(
            data,
            query,
            tree,
            minimum_duration,
            options,
            state,
            parent,
            next,
            budget);
        if (!next_state.has_value()) {
            return std::nullopt;
        }
        state = std::move(*next_state);
    }
    return state;
}

bool stateLess(
    const BeamState& lhs,
    const BeamState& rhs) {
    const double lhs_total =
        std::isfinite(lhs.estimated_total)
            ? lhs.estimated_total
            : std::numeric_limits<double>::infinity();
    const double rhs_total =
        std::isfinite(rhs.estimated_total)
            ? rhs.estimated_total
            : std::numeric_limits<double>::infinity();
    if (lhs_total != rhs_total) {
        return lhs_total < rhs_total;
    }
    const double lhs_prefixes =
        std::isfinite(lhs.estimated_prefixes)
            ? lhs.estimated_prefixes
            : std::numeric_limits<double>::infinity();
    const double rhs_prefixes =
        std::isfinite(rhs.estimated_prefixes)
            ? rhs.estimated_prefixes
            : std::numeric_limits<double>::infinity();
    if (lhs_prefixes != rhs_prefixes) {
        return lhs_prefixes < rhs_prefixes;
    }
    if (lhs.order != rhs.order) {
        return lhs.order < rhs.order;
    }
    return lhs.parents < rhs.parents;
}

template <typename Value, typename Less>
void retainBestDeterministically(
    std::vector<Value>& values,
    std::size_t limit,
    Less less
#ifdef DSM_TCC_TEST_REFERENCE_FULL_SORT
    , bool reference_full_sort
#endif
    ) {
    if (limit == 0) {
        values.clear();
        return;
    }
    if (values.size() > limit) {
#ifdef DSM_TCC_TEST_REFERENCE_FULL_SORT
        if (reference_full_sort) {
            std::sort(values.begin(), values.end(), less);
            values.resize(limit);
            return;
        }
#endif
        // partial_sort returns exactly the same ordered prefix as a complete
        // sort under this total planner tie-break, while bounding ranking work
        // to O(n log limit) rather than sorting every discarded state.
        std::partial_sort(
            values.begin(),
            values.begin() + static_cast<std::ptrdiff_t>(limit),
            values.end(),
            less);
        values.resize(limit);
        return;
    }
    std::sort(values.begin(), values.end(), less);
}

std::optional<BeamState> planForRoot(
    const Graph& data,
    const Graph& query,
    const DurableRelationCatalog& catalog,
    int root,
    int minimum_duration,
    const TemporalCostPlannerOptions& options,
    PlannerWorkBudget* budget = nullptr) {
    std::optional<BeamState> initial_state = initialState(
        data,
        query,
        catalog,
        root,
        minimum_duration,
        options,
        budget);
    if (!initial_state.has_value()) return std::nullopt;
    std::vector<BeamState> beam;
    beam.push_back(std::move(*initial_state));
    for (std::size_t depth = 1;
         depth < static_cast<std::size_t>(query.num_vertices);
         ++depth) {
        std::vector<BeamState> next_beam;
        for (const BeamState& state : beam) {
            for (int candidate = 0;
                 candidate < query.num_vertices;
                 ++candidate) {
                if (state.scheduled[
                        static_cast<std::size_t>(candidate)] != 0) {
                    continue;
                }
                for (int parent = 0;
                     parent < query.num_vertices;
                     ++parent) {
                    if (state.scheduled[
                            static_cast<std::size_t>(parent)] == 0 ||
                        catalog.find(parent, candidate) == nullptr) {
                        continue;
                    }
                    if (budget != nullptr &&
                        !budget->takeStateExpansion()) {
                        return std::nullopt;
                    }
                    std::optional<BeamState> child = extendState(
                        data,
                        query,
                        catalog,
                        minimum_duration,
                        options,
                        state,
                        parent,
                        candidate,
                        budget);
                    if (!child.has_value()) {
                        return std::nullopt;
                    }
                    next_beam.push_back(std::move(*child));
                }
            }
        }
        if (next_beam.empty()) {
            throw std::runtime_error(
                "temporal-cost planner found no connected frontier");
        }
        retainBestDeterministically(
            next_beam,
            options.beam_width,
            stateLess
#ifdef DSM_TCC_TEST_REFERENCE_FULL_SORT
            , options.test_reference_full_sort
#endif
            );
        beam.swap(next_beam);
    }
    const auto best = std::min_element(
        beam.begin(),
        beam.end(),
        stateLess);
    return std::move(*best);
}

} // namespace

QueryDecomposition planTemporalCostQuery(
    const Graph& temporal_graph,
    const Graph& query_graph,
    int minimum_duration,
    const std::array<std::size_t, kLabelCount>&
        label_vertex_counts,
    const std::array<double, kLabelCount>&
        label_mean_vertex_active_snapshots,
    const TemporalCostPlannerOptions& options,
    TemporalPlanDiagnostics* diagnostics) {
    if (diagnostics != nullptr) {
        *diagnostics = TemporalPlanDiagnostics{};
    }
    if (minimum_duration < 1) {
        throw std::invalid_argument(
            "minimum duration must be positive");
    }
    if (options.beam_width == 0 ||
        options.sample_cap == 0) {
        throw std::invalid_argument(
            "temporal-cost beam width and sample cap must be positive");
    }
    if (query_graph.num_vertices <= 0) {
        return QueryDecomposition{};
    }

    const auto start = std::chrono::steady_clock::now();
    const QueryDecomposition score_template =
        decomposeQuery(
            query_graph,
            label_vertex_counts,
            label_mean_vertex_active_snapshots,
            SelectivityMode::Paper);
    if (!score_template.connected) {
        return score_template;
    }
    const std::optional<DurableRelationCatalog> relation_catalog =
        buildRelationCatalog(
            temporal_graph,
            query_graph,
            minimum_duration);
    if (!relation_catalog.has_value()) {
        throw std::runtime_error(
            "legacy temporal-cost catalogue construction failed");
    }

    TemporalPlanDiagnostics local_diagnostics;
    local_diagnostics.incumbent_selectivity_formula_id =
        selectivityFormulaId(SelectivityMode::Paper);
    local_diagnostics.beam_width = options.beam_width;
    local_diagnostics.sample_cap = options.sample_cap;
    local_diagnostics.sample_seed = options.sample_seed;

    QueryDecomposition best_decomposition;
    double best_cost =
        std::numeric_limits<double>::infinity();
    double best_root_cost = 0.0;
    double best_raw_cost = 0.0;
    std::vector<int> best_order;
    std::vector<int> best_parents;

    // The unchanged corrected durability-aware plan is always a final
    // comparison candidate.  Build
    // and sample it once up front so an identical per-root beam winner can
    // reuse both artifacts instead of repeating the same work.
    TDTree baseline_tree(
        temporal_graph,
        query_graph,
        score_template,
        minimum_duration);
    std::optional<BeamState> baseline_rescore_result =
        rescoreFixedPlan(
            temporal_graph,
            query_graph,
            score_template,
            baseline_tree,
            minimum_duration,
            options);
    if (!baseline_rescore_result.has_value()) {
        throw std::runtime_error(
            "legacy temporal-cost baseline rescore failed");
    }
    const BeamState baseline_rescore =
        std::move(*baseline_rescore_result);

    for (int root = 0;
         root < query_graph.num_vertices;
         ++root) {
        const std::size_t degree =
            query_graph.adj[static_cast<std::size_t>(root)].size() +
            query_graph.in_adj[static_cast<std::size_t>(root)].size();
        if (degree == 0) continue;

        std::optional<BeamState> plan_result =
            planForRoot(
                temporal_graph,
                query_graph,
                *relation_catalog,
                root,
                minimum_duration,
                options);
        if (!plan_result.has_value()) {
            throw std::runtime_error(
                "legacy temporal-cost root planning failed");
        }
        const BeamState plan = std::move(*plan_result);
        QueryDecomposition candidate_decomposition =
            makePlanDecomposition(
                query_graph,
                score_template,
                plan,
                SelectivityMode::TemporalCost);
        const bool is_unchanged_baseline_plan =
            candidate_decomposition.root == score_template.root &&
            candidate_decomposition.dfs_order ==
                score_template.dfs_order &&
            candidate_decomposition.parent == score_template.parent;
        std::unique_ptr<TDTree> owned_candidate_tree;
        const TDTree* candidate_tree = &baseline_tree;
        BeamState owned_rescore;
        const BeamState* rescored_plan = &baseline_rescore;
        if (!is_unchanged_baseline_plan) {
            owned_candidate_tree = std::make_unique<TDTree>(
                temporal_graph,
                query_graph,
                candidate_decomposition,
                minimum_duration);
            candidate_tree = owned_candidate_tree.get();
            std::optional<BeamState> owned_rescore_result =
                rescoreFixedPlan(
                temporal_graph,
                query_graph,
                candidate_decomposition,
                *candidate_tree,
                minimum_duration,
                options);
            if (!owned_rescore_result.has_value()) {
                throw std::runtime_error(
                    "legacy temporal-cost candidate rescore failed");
            }
            owned_rescore = std::move(*owned_rescore_result);
            rescored_plan = &owned_rescore;
        }

        TemporalRootPlanEstimate root_estimate;
        root_estimate.root = root;
        root_estimate.root_candidate_count =
            candidate_tree->rootCandidates().size();
        root_estimate.pilot_relation_entries =
            candidate_tree->candidateRelationCount();
        root_estimate.catalogue_estimated_total_candidates =
            plan.estimated_total;
        root_estimate.estimated_total_candidates =
            rescored_plan->estimated_total;
        root_estimate.estimated_total_exact =
            rescored_plan->estimated_total_exact;
        root_estimate.order = plan.order;
        root_estimate.parent = plan.parents;
        root_estimate.transitions =
            rescored_plan->transitions;
        local_diagnostics.root_plans.push_back(
            std::move(root_estimate));

        const bool lower =
            rescored_plan->estimated_total <
                best_cost - 1e-9;
        const bool tied =
            std::abs(
                rescored_plan->estimated_total -
                best_cost) <= 1e-9;
        if (best_decomposition.root < 0 ||
            lower ||
            (tied &&
             (plan.order < best_order ||
              (plan.order == best_order &&
               plan.parents < best_parents)))) {
            best_cost = rescored_plan->estimated_total;
            best_root_cost = static_cast<double>(
                candidate_tree->rootCandidates().size());
            best_raw_cost = estimatedRawExtensions(*rescored_plan);
            best_order = plan.order;
            best_parents = plan.parents;
            best_decomposition =
                std::move(candidate_decomposition);
        }
    }

    // Include the unchanged TD-Tree plan in the same sampled comparison. This
    // lowers empirical regression risk, but it is not an exact guard whenever
    // a prefix population exceeds the sample cap.
    local_diagnostics
        .sampled_baseline_estimated_total_candidates =
        baseline_rescore.estimated_total;
    local_diagnostics.sampled_baseline_estimate_exact =
        baseline_rescore.estimated_total_exact;
    const bool baseline_lower =
        baseline_rescore.estimated_total <
            best_cost - 1e-9;
    const bool baseline_tied =
        std::abs(
            baseline_rescore.estimated_total -
            best_cost) <= 1e-9;
    if (best_decomposition.root < 0 ||
        baseline_lower ||
        (baseline_tied &&
         (score_template.dfs_order < best_order ||
          (score_template.dfs_order == best_order &&
           score_template.parent < best_parents)))) {
        best_cost = baseline_rescore.estimated_total;
        best_root_cost = static_cast<double>(
            baseline_tree.rootCandidates().size());
        best_raw_cost = estimatedRawExtensions(baseline_rescore);
        best_order = score_template.dfs_order;
        best_parents = score_template.parent;
        best_decomposition = score_template;
        local_diagnostics.sampled_baseline_plan_selected = true;
    }

    if (best_decomposition.root >= 0) {
        best_decomposition.selectivity_mode =
            SelectivityMode::TemporalCost;
        best_decomposition.temporal_selectivity.assign(
            static_cast<std::size_t>(query_graph.num_vertices),
            std::numeric_limits<double>::infinity());
        for (const TemporalRootPlanEstimate& root_plan :
             local_diagnostics.root_plans) {
            best_decomposition.temporal_selectivity[
                static_cast<std::size_t>(root_plan.root)] =
                root_plan.estimated_total_candidates;
        }
        best_decomposition.temporal_selectivity[
            static_cast<std::size_t>(
                best_decomposition.root)] =
            std::min(
                best_decomposition.temporal_selectivity[
                    static_cast<std::size_t>(
                        best_decomposition.root)],
                best_cost);
    }
    local_diagnostics.selected_estimated_total_candidates =
        best_cost;
    local_diagnostics.selected_root_candidates = best_root_cost;
    local_diagnostics.selected_raw_extension_candidates = best_raw_cost;
    local_diagnostics.selected_root_plus_raw_candidates = best_cost;
    const auto planning_elapsed =
        std::chrono::steady_clock::now() - start;
    local_diagnostics.planning_milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            planning_elapsed).count();
    local_diagnostics.planning_nanoseconds =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                planning_elapsed).count());
    if (diagnostics != nullptr) {
        *diagnostics = std::move(local_diagnostics);
    }
    return best_decomposition;
}

TemporalCostPlanResult planTemporalCostV2(
    const Graph& temporal_graph,
    const Graph& query_graph,
    int minimum_duration,
    const std::array<std::size_t, kLabelCount>&
        label_vertex_counts,
    const std::array<double, kLabelCount>&
        label_mean_vertex_active_snapshots,
    const TemporalCostPlannerOptions& options,
    TemporalPlanDiagnostics* diagnostics) {
    if (diagnostics != nullptr) {
        *diagnostics = TemporalPlanDiagnostics{};
    }
    if (minimum_duration < 1) {
        throw std::invalid_argument(
            "minimum duration must be positive");
    }
    if (options.beam_width == 0 || options.sample_cap == 0 ||
        options.pilot_prefix_cap == 0 || options.rescore_top_l == 0) {
        throw std::invalid_argument(
            "temporal-cost-v2 beam width, sample cap, and pilot prefix "
            "cap and rescore top-L must be positive");
    }
    if (!std::isfinite(options.opportunity_margin_fraction) ||
        options.opportunity_margin_fraction < 0.0) {
        throw std::invalid_argument(
            "temporal-cost-v2 opportunity margin must be finite and "
            "non-negative");
    }
    if (!std::isfinite(options.catalog_probe_cost_weight) ||
        options.catalog_probe_cost_weight < 0.0) {
        throw std::invalid_argument(
            "temporal-cost-v2 catalog probe cost weight must be finite "
            "and non-negative");
    }

    const auto start = std::chrono::steady_clock::now();
    TemporalPlanDiagnostics local_diagnostics;
    local_diagnostics.beam_width = options.beam_width;
    local_diagnostics.sample_cap = options.sample_cap;
    local_diagnostics.sample_seed = options.sample_seed;
    local_diagnostics.v2_enabled = true;
    local_diagnostics.max_catalog_probes =
        options.max_catalog_probes;
    local_diagnostics.max_state_expansions =
        options.max_state_expansions;
    local_diagnostics.max_extension_probes =
        options.max_extension_probes;
    local_diagnostics.max_interval_intersections =
        options.max_interval_intersections;
    local_diagnostics.max_candidate_trees_built =
        options.max_candidate_trees_built;
    local_diagnostics.pilot_extension_probe_cap =
        options.pilot_extension_probe_cap;
    local_diagnostics.pilot_prefix_cap =
        options.pilot_prefix_cap;
    local_diagnostics.opportunity_margin_fraction =
        options.opportunity_margin_fraction;
    local_diagnostics.catalog_probe_cost_weight =
        options.catalog_probe_cost_weight;
    local_diagnostics.rescore_top_l = options.rescore_top_l;
    local_diagnostics.v2_cost_gate_enabled =
        options.cost_gate_enabled;
    local_diagnostics.fixed_plan_scoring_method =
        "deterministic-capped-fixed-plan";

    auto finish = [&] (
        QueryDecomposition result,
        std::unique_ptr<TDTree> selected_tree = nullptr) {
        if (selected_tree != nullptr) {
            selected_tree->setDecompositionMetadata(result);
        }
        const auto planning_elapsed =
            std::chrono::steady_clock::now() - start;
        local_diagnostics.planning_milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                planning_elapsed).count();
        local_diagnostics.planning_nanoseconds =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    planning_elapsed).count());
        if (diagnostics != nullptr) {
            *diagnostics = local_diagnostics;
        }
        TemporalCostPlanResult plan_result;
        plan_result.decomposition = std::move(result);
        plan_result.selected_tree = std::move(selected_tree);
        return plan_result;
    };

    if (query_graph.num_vertices <= 0) {
        local_diagnostics.v2_admission_reason = "empty-query";
        local_diagnostics.v2_fallback_phase = "admission";
        local_diagnostics.v2_fallback_reason = "empty-query";
        local_diagnostics.v2_kci_incumbent_selected = true;
        return finish(QueryDecomposition{});
    }

    // V2's incumbent and every fallback use the exact same deterministic
    // corrected durability-aware decomposition as the current Ours arm.
    const QueryDecomposition kci_decomposition = decomposeQuery(
        query_graph,
        label_vertex_counts,
        label_mean_vertex_active_snapshots,
        SelectivityMode::Paper);
    local_diagnostics.incumbent_selectivity_formula_id =
        selectivityFormulaId(SelectivityMode::Paper);
    local_diagnostics.incumbent_root = kci_decomposition.root;
    local_diagnostics.incumbent_order = kci_decomposition.dfs_order;
    local_diagnostics.incumbent_parent = kci_decomposition.parent;
    local_diagnostics.incumbent_plan_fingerprint =
        planFingerprint(kci_decomposition);

    if (!kci_decomposition.connected) {
        QueryDecomposition result = kci_decomposition;
        result.selectivity_mode = SelectivityMode::TemporalCostV2;
        local_diagnostics.v2_admission_reason =
            "disconnected-query";
        local_diagnostics.v2_fallback_phase = "admission";
        local_diagnostics.v2_fallback_reason =
            "disconnected-query";
        local_diagnostics.v2_kci_incumbent_selected = true;
        return finish(std::move(result));
    }

    const auto incumbent_tree_start =
        std::chrono::steady_clock::now();
    std::unique_ptr<TDTree> incumbent_tree =
        std::make_unique<TDTree>(
            temporal_graph,
            query_graph,
            kci_decomposition,
            minimum_duration);
    local_diagnostics.incumbent_tree_build_milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - incumbent_tree_start)
            .count();
    PlannerWorkBudget budget(options);
    auto copyBudgetCounters = [&]() {
        local_diagnostics.catalog_probes = budget.catalog_probes;
        local_diagnostics.state_expansions = budget.state_expansions;
        local_diagnostics.extension_probes = budget.extension_probes;
        local_diagnostics.beam_extension_probes =
            budget.beam_extension_probes;
        local_diagnostics.fixed_plan_extension_probes =
            budget.fixed_plan_extension_probes;
        local_diagnostics.interval_intersections =
            budget.interval_intersections;
        local_diagnostics.temporal_edge_applications =
            budget.temporal_edge_applications;
        local_diagnostics.candidate_trees_built =
            static_cast<std::size_t>(budget.candidate_trees_built);
        local_diagnostics.v2_budget_exhausted = budget.exhausted;
    };
    auto updateFinalPlannerCost = [&]() {
        local_diagnostics.final_incumbent_build_cost =
            static_cast<double>(
                local_diagnostics.incumbent_relation_entries);
        local_diagnostics.final_catalog_cost =
            options.catalog_probe_cost_weight *
            static_cast<double>(local_diagnostics.catalog_probes);
        local_diagnostics.final_state_expansion_cost =
            static_cast<double>(local_diagnostics.state_expansions);
        local_diagnostics.final_extension_probe_cost =
            static_cast<double>(local_diagnostics.extension_probes);
        local_diagnostics.final_interval_intersection_cost =
            static_cast<double>(
                local_diagnostics.interval_intersections);
        // temporal_edge_applications is an audit count that includes both
        // first-edge scans and later intersections. Charging it in addition
        // to interval_intersections would double-count the latter; first-edge
        // scan cost also varies with interval-list length. Keep it visible but
        // outside this deliberately simple unit-weight proxy.
        local_diagnostics.final_rejected_tree_cost =
            static_cast<double>(
                local_diagnostics
                    .rejected_candidate_tree_relation_entries);
        local_diagnostics.final_planner_spent_cost =
            local_diagnostics.final_incumbent_build_cost +
            local_diagnostics.final_catalog_cost +
            local_diagnostics.final_state_expansion_cost +
            local_diagnostics.final_extension_probe_cost +
            local_diagnostics.final_interval_intersection_cost +
            local_diagnostics.final_rejected_tree_cost;
        local_diagnostics.final_kci_h1_cost =
            local_diagnostics.predicted_kci_controllable_cost;
    };
    std::uint64_t pilot_probes_used = 0;
    std::optional<BeamState> incumbent_rescore_result =
        rescoreFixedPlanPilot(
        temporal_graph,
        query_graph,
        kci_decomposition,
        *incumbent_tree,
        minimum_duration,
        options,
        pilot_probes_used,
        &budget);
    BeamState incumbent_rescore;
    incumbent_rescore.estimated_total =
        static_cast<double>(incumbent_tree->rootCandidates().size());
    incumbent_rescore.estimated_prefixes =
        incumbent_rescore.estimated_total;
    incumbent_rescore.estimated_total_exact = false;
    const bool incumbent_score_complete =
        incumbent_rescore_result.has_value();
    if (incumbent_score_complete) {
        incumbent_rescore = std::move(*incumbent_rescore_result);
    }
    local_diagnostics.pilot_extension_probes =
        pilot_probes_used;
    local_diagnostics.incumbent_extension_probes =
        pilot_probes_used;
    copyBudgetCounters();
    local_diagnostics.incumbent_root_candidates =
        static_cast<double>(incumbent_tree->rootCandidates().size());
    local_diagnostics.incumbent_relation_entries =
        incumbent_tree->candidateRelationCount();
    local_diagnostics.incumbent_raw_extension_candidates =
        estimatedRawExtensions(incumbent_rescore);
    local_diagnostics.incumbent_root_plus_raw_candidates =
        incumbent_rescore.estimated_total;
    local_diagnostics.predicted_kci_controllable_cost =
        static_cast<double>(local_diagnostics.incumbent_relation_entries) +
        incumbent_rescore.estimated_total;
    local_diagnostics.incumbent_objective_available =
        incumbent_score_complete;
    local_diagnostics.sampled_baseline_estimated_total_candidates =
        incumbent_rescore.estimated_total;
    local_diagnostics.sampled_baseline_estimate_exact =
        incumbent_rescore.estimated_total_exact;
    if (incumbent_score_complete) {
        TemporalFixedPlanEstimate score;
        score.plan_fingerprint =
            local_diagnostics.incumbent_plan_fingerprint;
        score.root = kci_decomposition.root;
        score.incumbent = true;
        score.extension_probe_cap =
            options.pilot_extension_probe_cap;
        score.extension_probes = pilot_probes_used;
        score.estimated_total_candidates =
            incumbent_rescore.estimated_total;
        score.estimated_total_exact =
            incumbent_rescore.estimated_total_exact;
        local_diagnostics.fixed_plan_scores.push_back(
            std::move(score));
    }

    auto kciResult = [&]() {
        copyBudgetCounters();
        updateFinalPlannerCost();
        QueryDecomposition result = kci_decomposition;
        result.selectivity_mode = SelectivityMode::TemporalCostV2;
        result.temporal_selectivity.assign(
            static_cast<std::size_t>(query_graph.num_vertices),
            std::numeric_limits<double>::infinity());
        result.temporal_selectivity[
            static_cast<std::size_t>(result.root)] =
            incumbent_rescore.estimated_total;
        local_diagnostics.selected_estimated_total_candidates =
            incumbent_rescore.estimated_total;
        local_diagnostics.selected_root_candidates =
            local_diagnostics.incumbent_root_candidates;
        local_diagnostics.selected_raw_extension_candidates =
            local_diagnostics.incumbent_raw_extension_candidates;
        local_diagnostics.selected_root_plus_raw_candidates =
            incumbent_rescore.estimated_total;
        local_diagnostics.selected_objective_available =
            incumbent_score_complete;
        local_diagnostics.sampled_baseline_plan_selected = true;
        local_diagnostics.v2_kci_incumbent_selected = true;
        local_diagnostics.v2_candidate_selected = false;
        local_diagnostics.v2_decision = "kci-fallback";
        local_diagnostics.v2_decision_reason =
            local_diagnostics.v2_fallback_reason;
        local_diagnostics.selected_plan_fingerprint =
            local_diagnostics.incumbent_plan_fingerprint;
        local_diagnostics.selected_tree_build_milliseconds =
            local_diagnostics.incumbent_tree_build_milliseconds;
        local_diagnostics.selected_tree_handoff = true;
        return result;
    };

    if (!incumbent_score_complete) {
        local_diagnostics.v2_admission_reason =
            "incumbent-fixed-plan-budget";
        local_diagnostics.v2_fallback_phase = "admission";
        local_diagnostics.v2_fallback_reason =
            budgetReasonName(budget.exhausted_phase);
        copyBudgetCounters();
        return finish(kciResult(), std::move(incumbent_tree));
    }
    if (incumbent_tree->rootCandidates().empty() ||
        incumbent_rescore.estimated_total <= 0.0) {
        local_diagnostics.v2_admission_reason =
            "zero-incumbent-objective";
        local_diagnostics.v2_fallback_phase = "admission";
        local_diagnostics.v2_fallback_reason = "zero-domain";
        return finish(kciResult(), std::move(incumbent_tree));
    }
    if (query_graph.num_vertices > 1 &&
        incumbent_rescore.samples.empty()) {
        local_diagnostics.v2_admission_reason = "sample-empty";
        local_diagnostics.v2_fallback_phase = "admission";
        local_diagnostics.v2_fallback_reason = "sample-empty";
        return finish(kciResult(), std::move(incumbent_tree));
    }
    if (query_graph.num_vertices == 1) {
        local_diagnostics.v2_admission_reason = "single-vertex";
        local_diagnostics.v2_fallback_phase = "admission";
        local_diagnostics.v2_fallback_reason = "same-plan";
        return finish(kciResult(), std::move(incumbent_tree));
    }
    if (options.max_state_expansions == 0) {
        local_diagnostics.v2_admission_reason =
            "state-budget-disabled";
        local_diagnostics.v2_fallback_phase = "admission";
        local_diagnostics.v2_fallback_reason =
            "state-expansion-budget";
        local_diagnostics.v2_budget_exhausted = true;
        return finish(kciResult(), std::move(incumbent_tree));
    }
    // H=1 opportunity gate.  The units are deterministic logical-work proxy
    // units, not wall-clock milliseconds.  Both sides contain the KCI tree
    // already built by v2.  KCI then pays its sampled root+raw execution
    // objective; TCC additionally pays the pilot, the mandatory
    // |V_Q|*|V_G| domain scan, and the incumbent root count as a root-only
    // execution proxy.  The latter is not a lower bound for a different TCC
    // root, so this is deliberately a deterministic admission heuristic, not
    // a proof of either safe rejection or runtime no-regression.
    const long double mandatory_domain_probes =
        static_cast<long double>(query_graph.num_vertices) *
        static_cast<long double>(temporal_graph.num_vertices);
    local_diagnostics.opportunity_pilot_state_cost =
        static_cast<double>(local_diagnostics.state_expansions);
    local_diagnostics.opportunity_pilot_extension_cost =
        static_cast<double>(local_diagnostics.pilot_extension_probes);
    local_diagnostics.opportunity_pilot_interval_cost =
        static_cast<double>(local_diagnostics.interval_intersections);
    local_diagnostics.opportunity_pilot_spent_cost =
        local_diagnostics.opportunity_pilot_state_cost +
        local_diagnostics.opportunity_pilot_extension_cost +
        local_diagnostics.opportunity_pilot_interval_cost;
    local_diagnostics.opportunity_incumbent_build_cost =
        static_cast<double>(local_diagnostics.incumbent_relation_entries);
    local_diagnostics.opportunity_catalog_floor_probes =
        static_cast<double>(mandatory_domain_probes);
    local_diagnostics.opportunity_catalog_floor_cost =
        options.catalog_probe_cost_weight *
        local_diagnostics.opportunity_catalog_floor_probes;
    local_diagnostics.opportunity_optimistic_exec_cost =
        local_diagnostics.incumbent_root_candidates;
    local_diagnostics.predicted_tcc_controllable_cost =
        local_diagnostics.opportunity_incumbent_build_cost +
        local_diagnostics.opportunity_pilot_spent_cost +
        local_diagnostics.opportunity_catalog_floor_cost +
        local_diagnostics.opportunity_optimistic_exec_cost;
    local_diagnostics.opportunity_safety_margin =
        options.opportunity_margin_fraction *
        local_diagnostics.predicted_kci_controllable_cost;
    if (mandatory_domain_probes >
        static_cast<long double>(options.max_catalog_probes)) {
        local_diagnostics.v2_admission_reason =
            "catalog-lower-bound-exceeds-budget";
        local_diagnostics.v2_fallback_phase = "admission";
        local_diagnostics.v2_fallback_reason =
            "catalog-probe-budget";
        local_diagnostics.v2_budget_exhausted = true;
        return finish(kciResult(), std::move(incumbent_tree));
    }
    if (options.cost_gate_enabled &&
        !(local_diagnostics.predicted_tcc_controllable_cost +
              local_diagnostics.opportunity_safety_margin <
          local_diagnostics.predicted_kci_controllable_cost)) {
        local_diagnostics.v2_admission_reason =
            "opportunity-below-margin";
        local_diagnostics.v2_fallback_phase = "admission";
        local_diagnostics.v2_fallback_reason =
            "predicted-gain-below-margin";
        return finish(kciResult(), std::move(incumbent_tree));
    }
    if (options.cost_gate_enabled) {
        local_diagnostics.v2_admission_reason = "eligible";
    } else {
        local_diagnostics.v2_admission_reason =
            "cost-gate-disabled";
        local_diagnostics.v2_cost_gate_bypassed = true;
    }

    const std::optional<DurableRelationCatalog> relation_catalog =
        buildRelationCatalog(
            temporal_graph,
            query_graph,
            minimum_duration,
            &budget);
    auto deterministicFallback = [&](const char* phase,
                                     const char* reason) {
        // A budget fallback retains no non-incumbent tree. Charge every
        // candidate tree already constructed, including the tree whose
        // fixed-plan rescore exhausted the budget, before computing the
        // symmetric H=1 planner cost.
        local_diagnostics.rejected_candidate_tree_relation_entries =
            local_diagnostics.candidate_tree_relation_entries_built;
        local_diagnostics.v2_fallback_phase = phase;
        local_diagnostics.v2_fallback_reason = reason;
        return finish(kciResult(), std::move(incumbent_tree));
    };
    auto budgetFallback = [&](const char* phase) {
        return deterministicFallback(
            phase, budgetReasonName(budget.exhausted_phase));
    };
    if (!relation_catalog.has_value()) {
        return budgetFallback("catalog");
    }

    QueryDecomposition best_candidate;
    double best_cost = std::numeric_limits<double>::infinity();
    double best_root_cost = 0.0;
    double best_raw_cost = 0.0;
    std::size_t best_relation_entries = 0;
    std::vector<int> best_order;
    std::vector<int> best_parents;
    std::unique_ptr<TDTree> best_candidate_tree;
    long long best_candidate_tree_build_ms = 0;

    struct CatalogueCandidate {
        BeamState plan;
        QueryDecomposition decomposition;
    };
    std::vector<CatalogueCandidate> catalogue_candidates;
    for (int root = 0;
         root < query_graph.num_vertices;
         ++root) {
        const std::size_t degree =
            query_graph.adj[static_cast<std::size_t>(root)].size() +
            query_graph.in_adj[static_cast<std::size_t>(root)].size();
        if (degree == 0) continue;
        std::optional<BeamState> plan_result = planForRoot(
            temporal_graph,
            query_graph,
            *relation_catalog,
            root,
            minimum_duration,
            options,
            &budget);
        if (!plan_result.has_value()) {
            return budgetFallback("beam");
        }
        CatalogueCandidate candidate;
        candidate.plan = std::move(*plan_result);
        candidate.decomposition = makePlanDecomposition(
            query_graph,
            kci_decomposition,
            candidate.plan,
            SelectivityMode::TemporalCostV2);
        catalogue_candidates.push_back(std::move(candidate));
    }
    local_diagnostics.catalogue_complete_plans =
        catalogue_candidates.size();
    retainBestDeterministically(
        catalogue_candidates,
        options.rescore_top_l,
        [](const CatalogueCandidate& lhs,
           const CatalogueCandidate& rhs) {
            return stateLess(lhs.plan, rhs.plan);
        }
#ifdef DSM_TCC_TEST_REFERENCE_FULL_SORT
        , options.test_reference_full_sort
#endif
        );

    for (CatalogueCandidate& catalogue_candidate :
         catalogue_candidates) {
        BeamState& plan = catalogue_candidate.plan;
        QueryDecomposition candidate_decomposition =
            std::move(catalogue_candidate.decomposition);
        const bool is_incumbent_plan =
            candidate_decomposition.root == kci_decomposition.root &&
            candidate_decomposition.dfs_order ==
                kci_decomposition.dfs_order &&
            candidate_decomposition.parent == kci_decomposition.parent;

        std::unique_ptr<TDTree> owned_candidate_tree;
        const TDTree* candidate_tree = incumbent_tree.get();
        long long candidate_tree_build_ms =
            local_diagnostics.incumbent_tree_build_milliseconds;
        BeamState owned_rescore;
        const BeamState* rescored_plan = &incumbent_rescore;
        std::uint64_t candidate_probes_used = 0;
        if (!is_incumbent_plan) {
            if (!budget.takeCandidateTree()) {
                return budgetFallback("tree-build");
            }
            const auto candidate_tree_start =
                std::chrono::steady_clock::now();
            owned_candidate_tree = std::make_unique<TDTree>(
                temporal_graph,
                query_graph,
                candidate_decomposition,
                minimum_duration);
            candidate_tree_build_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() -
                    candidate_tree_start)
                    .count();
            local_diagnostics.candidate_tree_relation_entries_built +=
                owned_candidate_tree->candidateRelationCount();
            candidate_tree = owned_candidate_tree.get();
            std::optional<BeamState> rescore_result =
                rescoreFixedPlanPilot(
                    temporal_graph,
                    query_graph,
                    candidate_decomposition,
                    *candidate_tree,
                    minimum_duration,
                    options,
                    candidate_probes_used,
                    &budget);
            local_diagnostics.candidate_extension_probes =
                saturatingAdd(
                    local_diagnostics.candidate_extension_probes,
                    candidate_probes_used);
            if (!rescore_result.has_value()) {
                return budgetFallback("rescore");
            }
            owned_rescore = std::move(*rescore_result);
            if (query_graph.num_vertices > 1 &&
                owned_rescore.samples.empty()) {
                return deterministicFallback(
                    "rescore", "sample-empty");
            }
            rescored_plan = &owned_rescore;
        }
        ++local_diagnostics.candidate_plans_rescored;

        TemporalFixedPlanEstimate fixed_score;
        fixed_score.plan_fingerprint =
            planFingerprint(candidate_decomposition);
        fixed_score.root = candidate_decomposition.root;
        fixed_score.incumbent = is_incumbent_plan;
        fixed_score.reused_incumbent_score = is_incumbent_plan;
        fixed_score.extension_probe_cap =
            options.pilot_extension_probe_cap;
        fixed_score.extension_probes = candidate_probes_used;
        fixed_score.estimated_total_candidates =
            rescored_plan->estimated_total;
        fixed_score.estimated_total_exact =
            rescored_plan->estimated_total_exact;
        local_diagnostics.fixed_plan_scores.push_back(
            std::move(fixed_score));

        TemporalRootPlanEstimate root_estimate;
        root_estimate.root = candidate_decomposition.root;
        root_estimate.root_candidate_count =
            candidate_tree->rootCandidates().size();
        root_estimate.pilot_relation_entries =
            candidate_tree->candidateRelationCount();
        root_estimate.catalogue_estimated_total_candidates =
            plan.estimated_total;
        root_estimate.estimated_total_candidates =
            rescored_plan->estimated_total;
        root_estimate.estimated_total_exact =
            rescored_plan->estimated_total_exact;
        root_estimate.order = plan.order;
        root_estimate.parent = plan.parents;
        root_estimate.transitions = rescored_plan->transitions;
        local_diagnostics.root_plans.push_back(
            std::move(root_estimate));

        const bool lower =
            rescored_plan->estimated_total < best_cost - 1e-9;
        const bool tied = std::abs(
            rescored_plan->estimated_total - best_cost) <= 1e-9;
        if (best_candidate.root < 0 || lower ||
            (tied &&
             (plan.order < best_order ||
              (plan.order == best_order &&
               plan.parents < best_parents)))) {
            best_cost = rescored_plan->estimated_total;
            best_root_cost = static_cast<double>(
                candidate_tree->rootCandidates().size());
            best_raw_cost = estimatedRawExtensions(*rescored_plan);
            best_relation_entries =
                candidate_tree->candidateRelationCount();
            best_order = plan.order;
            best_parents = plan.parents;
            best_candidate = std::move(candidate_decomposition);
            best_candidate_tree_build_ms = candidate_tree_build_ms;
            if (!is_incumbent_plan) {
                best_candidate_tree =
                    std::move(owned_candidate_tree);
            } else {
                best_candidate_tree.reset();
            }
        }
    }

    copyBudgetCounters();
    if (best_candidate.root < 0) {
        local_diagnostics.v2_fallback_phase = "selection";
        local_diagnostics.v2_fallback_reason =
            "no-complete-candidate";
        return finish(kciResult(), std::move(incumbent_tree));
    }
    const bool best_is_incumbent =
        best_candidate.root == kci_decomposition.root &&
        best_candidate.dfs_order == kci_decomposition.dfs_order &&
        best_candidate.parent == kci_decomposition.parent;
    const std::size_t retained_candidate_relation_entries =
        best_is_incumbent ? 0 : best_relation_entries;
    local_diagnostics.rejected_candidate_tree_relation_entries =
        local_diagnostics.candidate_tree_relation_entries_built >=
                retained_candidate_relation_entries
            ? local_diagnostics.candidate_tree_relation_entries_built -
                  retained_candidate_relation_entries
            : 0;
    updateFinalPlannerCost();
    if (best_is_incumbent) {
        local_diagnostics.v2_fallback_phase = "selection";
        local_diagnostics.v2_fallback_reason = "same-plan";
        return finish(kciResult(), std::move(incumbent_tree));
    }
    local_diagnostics.final_candidate_build_exec_cost =
        static_cast<double>(best_relation_entries) + best_cost;
    local_diagnostics.final_candidate_h1_cost =
        local_diagnostics.final_planner_spent_cost +
        local_diagnostics.final_candidate_build_exec_cost;
    local_diagnostics.final_cost_comparison_available = true;
    local_diagnostics.opportunity_safety_margin =
        options.opportunity_margin_fraction *
        local_diagnostics.final_kci_h1_cost;
    if (options.cost_gate_enabled &&
        !(local_diagnostics.final_candidate_h1_cost +
              local_diagnostics.opportunity_safety_margin <
          local_diagnostics.final_kci_h1_cost)) {
        local_diagnostics.v2_fallback_phase = "selection";
        local_diagnostics.v2_fallback_reason =
            "h1-gain-below-margin";
        return finish(kciResult(), std::move(incumbent_tree));
    }

    best_candidate.selectivity_mode =
        SelectivityMode::TemporalCostV2;
    best_candidate.temporal_selectivity.assign(
        static_cast<std::size_t>(query_graph.num_vertices),
        std::numeric_limits<double>::infinity());
    for (const TemporalRootPlanEstimate& root_plan :
         local_diagnostics.root_plans) {
        best_candidate.temporal_selectivity[
            static_cast<std::size_t>(root_plan.root)] =
            root_plan.estimated_total_candidates;
    }
    best_candidate.temporal_selectivity[
        static_cast<std::size_t>(best_candidate.root)] = best_cost;
    local_diagnostics.selected_estimated_total_candidates = best_cost;
    local_diagnostics.selected_root_candidates = best_root_cost;
    local_diagnostics.selected_raw_extension_candidates = best_raw_cost;
    local_diagnostics.selected_root_plus_raw_candidates = best_cost;
    local_diagnostics.sampled_baseline_plan_selected = false;
    local_diagnostics.v2_kci_incumbent_selected = false;
    local_diagnostics.v2_candidate_selected = true;
    local_diagnostics.v2_forced_candidate_selected =
        !options.cost_gate_enabled;
    local_diagnostics.v2_decision = "tcc-candidate";
    local_diagnostics.v2_decision_reason =
        options.cost_gate_enabled
        ? "candidate-selected"
        : "candidate-selected-cost-gate-disabled";
    local_diagnostics.v2_fallback_phase = "none";
    local_diagnostics.v2_fallback_reason = "none";
    local_diagnostics.selected_plan_fingerprint =
        planFingerprint(best_candidate);
    local_diagnostics.selected_tree_build_milliseconds =
        best_candidate_tree_build_ms;
    local_diagnostics.selected_tree_handoff =
        best_candidate_tree != nullptr;
    if (best_candidate_tree == nullptr) {
        throw std::runtime_error(
            "temporal-cost-v2 selected candidate has no complete TD-tree");
    }
    return finish(
        std::move(best_candidate),
        std::move(best_candidate_tree));
}

QueryDecomposition planTemporalCostV2Query(
    const Graph& temporal_graph,
    const Graph& query_graph,
    int minimum_duration,
    const std::array<std::size_t, kLabelCount>&
        label_vertex_counts,
    const std::array<double, kLabelCount>&
        label_mean_vertex_active_snapshots,
    const TemporalCostPlannerOptions& options,
    TemporalPlanDiagnostics* diagnostics) {
    TemporalCostPlanResult result = planTemporalCostV2(
        temporal_graph,
        query_graph,
        minimum_duration,
        label_vertex_counts,
        label_mean_vertex_active_snapshots,
        options,
        diagnostics);
    return std::move(result.decomposition);
}
