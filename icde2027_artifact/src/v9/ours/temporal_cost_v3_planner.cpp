#include "temporal_cost_v3_planner.h"

#include "TDTree.h"
#include "cover_aware_tdtree_builder.h"
#include "query_decomposition.h"
#include "validated_graph_snapshot.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace {

constexpr std::uint64_t kSampleSeed = 42;
constexpr std::uint64_t kCostConventionId =
    UINT64_C(0x56332d554e495431);
constexpr std::uint64_t kTraceProposal = UINT64_C(0x1000000000000000);
constexpr std::uint64_t kTraceLazyRequest = UINT64_C(0x2000000000000000);
constexpr std::uint64_t kTraceLazyHit = UINT64_C(0x3000000000000000);
constexpr std::uint64_t kTraceLazyMiss = UINT64_C(0x4000000000000000);
constexpr std::uint64_t kTraceScore = UINT64_C(0x5000000000000000);
constexpr std::uint64_t kTraceMaterialize = UINT64_C(0x6000000000000000);
constexpr std::uint64_t kTraceDecision = UINT64_C(0x7000000000000000);

bool checkedAdd(
    std::uint64_t lhs,
    std::uint64_t rhs,
    std::uint64_t& result) noexcept {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

bool checkedAccumulate(
    std::uint64_t& destination,
    std::uint64_t increment) noexcept {
    std::uint64_t result = 0;
    if (!checkedAdd(destination, increment, result)) return false;
    destination = result;
    return true;
}

bool checkedMultiply(
    std::uint64_t lhs,
    std::uint64_t rhs,
    std::uint64_t& result) noexcept {
    if (lhs != 0 &&
        rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        return false;
    }
    result = lhs * rhs;
    return true;
}

bool sizeToU64(std::size_t value, std::uint64_t& result) noexcept {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (value > static_cast<std::size_t>(
                std::numeric_limits<std::uint64_t>::max())) {
            return false;
        }
    }
    result = static_cast<std::uint64_t>(value);
    return true;
}

bool u64ToSize(std::uint64_t value, std::size_t& result) noexcept {
    if (value > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    result = static_cast<std::size_t>(value);
    return true;
}

void appendTrace(
    std::vector<std::uint64_t>& trace,
    std::uint64_t maximum_entries,
    std::uint64_t value,
    V3PlannerTelemetry& telemetry) {
    std::uint64_t size = 0;
    if (!sizeToU64(trace.size(), size)) {
        throw std::overflow_error("V3 trace size overflow");
    }
    if (size >= maximum_entries) {
        if (!checkedAccumulate(telemetry.trace_entries_dropped, 1)) {
            throw std::overflow_error("V3 trace drop overflow");
        }
        return;
    }
    trace.push_back(value);
}

std::uint64_t splitmix64(std::uint64_t value) noexcept {
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30U)) *
        UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27U)) *
        UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

std::uint64_t traceToken(
    std::uint64_t kind,
    std::uint64_t value) noexcept {
    return kind ^ (splitmix64(value) & UINT64_C(0x0fffffffffffffff));
}

struct FrontierChoice {
    std::uint64_t cover_cardinality = 0;
    RequiredArcMask mask = RequiredArcMask::None;
    int child = -1;
    int parent = -1;
};

bool frontierLess(
    const FrontierChoice& lhs,
    const FrontierChoice& rhs) noexcept {
    return std::tie(
               lhs.cover_cardinality,
               lhs.mask,
               lhs.child,
               lhs.parent) <
        std::tie(
               rhs.cover_cardinality,
               rhs.mask,
               rhs.child,
               rhs.parent);
}

std::vector<FrontierChoice> collectFrontier(
    const Graph& query,
    const CandidateCover& cover,
    const std::vector<std::uint8_t>& scheduled) {
    std::vector<FrontierChoice> frontier;
    for (int parent = 0; parent < query.num_vertices; ++parent) {
        if (scheduled[static_cast<std::size_t>(parent)] == 0) continue;
        for (int child = 0; child < query.num_vertices; ++child) {
            if (scheduled[static_cast<std::size_t>(child)] != 0) continue;
            const RequiredArcMask mask =
                requiredArcMask(query, parent, child);
            if (mask == RequiredArcMask::None) continue;
            std::uint64_t cardinality = 0;
            if (!sizeToU64(
                    cover.domain(static_cast<std::size_t>(child)).size(),
                    cardinality)) {
                throw std::overflow_error(
                    "V3 cover cardinality overflow");
            }
            frontier.push_back(
                {cardinality, mask, child, parent});
        }
    }
    std::sort(frontier.begin(), frontier.end(), frontierLess);
    return frontier;
}

struct ProposalBuilder {
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot;
    const CandidateCover& cover;
    const FixedPlan& incumbent;
    const V3ProposalBudget& budget;
    std::uint64_t trace_cap = 0;
    V3FixedPlanProposalResult& result;
    std::set<std::uint64_t> seen;
    bool state_budget_hit = false;
    bool plan_budget_hit = false;

    ProposalBuilder(
        const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot_value,
        const CandidateCover& cover_value,
        const FixedPlan& incumbent_value,
        const V3ProposalBudget& budget_value,
        std::uint64_t trace_cap_value,
        V3FixedPlanProposalResult& result_value)
        : snapshot(snapshot_value),
          cover(cover_value),
          incumbent(incumbent_value),
          budget(budget_value),
          trace_cap(trace_cap_value),
          result(result_value) {}

    bool add(FixedPlan plan) {
        const FixedPlanValidation validation =
            validateFixedPlan(*snapshot, plan);
        if (!validation.valid) {
            if (!checkedAccumulate(
                    result.telemetry.v3_invalid_plan_proposals, 1)) {
                throw std::overflow_error("V3 invalid proposal overflow");
            }
            return true;
        }
        if (seen.find(plan.fingerprint) != seen.end()) {
            if (!checkedAccumulate(
                    result.telemetry.v3_duplicate_plan_proposals, 1)) {
                throw std::overflow_error("V3 duplicate proposal overflow");
            }
            return true;
        }
        if (result.telemetry.v3_complete_plan_proposals >=
            budget.maximum_complete_plans) {
            plan_budget_hit = true;
            return false;
        }
        seen.insert(plan.fingerprint);
        if (!checkedAccumulate(
                result.telemetry.v3_complete_plan_proposals, 1)) {
            throw std::overflow_error("V3 complete proposal overflow");
        }
        if (plan.fingerprint != incumbent.fingerprint &&
            !checkedAccumulate(
                result.telemetry.v3_nonincumbent_plan_proposals, 1)) {
            throw std::overflow_error("V3 nonincumbent proposal overflow");
        }
        appendTrace(
            result.deterministic_trace,
            trace_cap,
            traceToken(kTraceProposal, plan.fingerprint),
            result.telemetry);
        result.plans.push_back(std::move(plan));
        return true;
    }

    bool takeState() {
        if (result.telemetry.v3_proposal_states_expanded >=
            budget.maximum_states_expanded) {
            state_budget_hit = true;
            return false;
        }
        if (!checkedAccumulate(
                result.telemetry.v3_proposal_states_expanded, 1)) {
            throw std::overflow_error("V3 proposal state overflow");
        }
        return true;
    }
};

FixedPlan rerootIncumbentTree(
    const ValidatedGraphSnapshot& snapshot,
    const FixedPlan& incumbent,
    int root) {
    const int query_vertices = snapshot.query().num_vertices;
    std::vector<std::vector<int>> tree(
        static_cast<std::size_t>(query_vertices));
    for (int child = 0; child < query_vertices; ++child) {
        const int parent =
            incumbent.parent[static_cast<std::size_t>(child)];
        if (parent < 0) continue;
        tree[static_cast<std::size_t>(parent)].push_back(child);
        tree[static_cast<std::size_t>(child)].push_back(parent);
    }
    for (auto& neighbors : tree) {
        std::sort(neighbors.begin(), neighbors.end());
    }
    std::vector<int> order;
    order.reserve(static_cast<std::size_t>(query_vertices));
    std::vector<int> parent(
        static_cast<std::size_t>(query_vertices), -2);
    parent[static_cast<std::size_t>(root)] = -1;
    order.push_back(root);
    for (std::size_t index = 0; index < order.size(); ++index) {
        const int current = order[index];
        for (int neighbor : tree[static_cast<std::size_t>(current)]) {
            if (parent[static_cast<std::size_t>(neighbor)] != -2) continue;
            parent[static_cast<std::size_t>(neighbor)] = current;
            order.push_back(neighbor);
        }
    }
    return makeFixedPlan(
        snapshot, root, std::move(order), std::move(parent));
}

FixedPlan greedyPlan(
    const ValidatedGraphSnapshot& snapshot,
    const CandidateCover& cover,
    int root) {
    const Graph& query = snapshot.query();
    std::vector<int> order;
    order.reserve(static_cast<std::size_t>(query.num_vertices));
    std::vector<int> parent(
        static_cast<std::size_t>(query.num_vertices), -1);
    std::vector<std::uint8_t> scheduled(
        static_cast<std::size_t>(query.num_vertices), 0);
    order.push_back(root);
    scheduled[static_cast<std::size_t>(root)] = 1;
    while (order.size() < static_cast<std::size_t>(query.num_vertices)) {
        std::vector<FrontierChoice> frontier =
            collectFrontier(query, cover, scheduled);
        if (frontier.empty()) break;
        const FrontierChoice& choice = frontier.front();
        order.push_back(choice.child);
        parent[static_cast<std::size_t>(choice.child)] = choice.parent;
        scheduled[static_cast<std::size_t>(choice.child)] = 1;
    }
    return makeFixedPlan(
        snapshot, root, std::move(order), std::move(parent));
}

} // namespace

namespace {

enum class PilotFailure : std::uint8_t {
    None,
    Budget,
    SampleEmpty,
    Overflow,
    Allocation
};

struct BlockOutcome {
    const std::vector<int>* values = nullptr;
    PilotFailure failure = PilotFailure::None;
};

class LazyTransitionOracle;

struct PrefixState {
    std::vector<int> mapping;
    std::vector<TimeInterval> common_intervals;
    bool has_common_intervals = false;
};

std::uint64_t prefixRank(
    const PrefixState& prefix,
    std::size_t depth) noexcept {
    std::uint64_t rank = splitmix64(
        kSampleSeed ^ static_cast<std::uint64_t>(depth));
    for (std::size_t query_vertex = 0;
         query_vertex < prefix.mapping.size();
         ++query_vertex) {
        const int data_vertex = prefix.mapping[query_vertex];
        if (data_vertex < 0) continue;
        const std::uint64_t token =
            (static_cast<std::uint64_t>(query_vertex + 1) << 32U) ^
            static_cast<std::uint32_t>(data_vertex);
        rank = splitmix64(rank ^ splitmix64(token));
    }
    return rank;
}

struct RankedPrefix {
    std::uint64_t rank = 0;
    PrefixState prefix;
};

bool rankedPrefixLess(
    const RankedPrefix& lhs,
    const RankedPrefix& rhs) noexcept {
    if (lhs.rank != rhs.rank) return lhs.rank < rhs.rank;
    return lhs.prefix.mapping < rhs.prefix.mapping;
}

class BottomKPrefixes {
public:
    BottomKPrefixes(
        std::size_t cap,
        std::size_t depth)
        : cap_(cap),
          depth_(depth) {
        kept_.reserve(cap_);
    }

    bool consider(PrefixState prefix) {
        if (!checkedAccumulate(seen_, 1)) return false;
        if (cap_ == 0) return true;
        RankedPrefix candidate;
        candidate.rank = prefixRank(prefix, depth_);
        candidate.prefix = std::move(prefix);
        if (kept_.size() < cap_) {
            kept_.push_back(std::move(candidate));
            return true;
        }
        auto largest = std::max_element(
            kept_.begin(), kept_.end(), rankedPrefixLess);
        if (rankedPrefixLess(candidate, *largest)) {
            *largest = std::move(candidate);
        }
        return true;
    }

    std::uint64_t seen() const noexcept { return seen_; }

    std::vector<PrefixState> finish() {
        std::sort(kept_.begin(), kept_.end(), rankedPrefixLess);
        std::vector<PrefixState> prefixes;
        prefixes.reserve(kept_.size());
        for (RankedPrefix& ranked : kept_) {
            prefixes.push_back(std::move(ranked.prefix));
        }
        return prefixes;
    }

private:
    std::size_t cap_ = 0;
    std::size_t depth_ = 0;
    std::uint64_t seen_ = 0;
    std::vector<RankedPrefix> kept_;
};

bool temporalArcUpper(
    const Graph& query,
    int query_vertex,
    const std::vector<int>& mapping,
    std::uint64_t& count) noexcept {
    count = 0;
    if (GraphUtils::hasEdge(
            query.adj, query_vertex, query_vertex)) {
        if (!checkedAccumulate(count, 1)) return false;
    }
    for (const Edge& edge :
         query.adj[static_cast<std::size_t>(query_vertex)]) {
        if (edge.to != query_vertex &&
            mapping[static_cast<std::size_t>(edge.to)] >= 0) {
            if (!checkedAccumulate(count, 1)) return false;
        }
    }
    for (const Edge& edge :
         query.in_adj[static_cast<std::size_t>(query_vertex)]) {
        if (edge.to != query_vertex &&
            mapping[static_cast<std::size_t>(edge.to)] >= 0) {
            if (!checkedAccumulate(count, 1)) return false;
        }
    }
    return true;
}

template <typename Provider>
bool applyTemporalConstraint(
    const Graph& data,
    int source,
    int target,
    int minimum_duration,
    PrefixState& prefix,
    Provider& provider) {
    const TemporalEdge* edge = data.findTemporalEdge(source, target);
    if (edge == nullptr ||
        !hasMinimumConsecutiveDuration(*edge, minimum_duration)) {
        return false;
    }
    if (!prefix.has_common_intervals) {
        prefix.common_intervals.clear();
        prefix.common_intervals.reserve(
            edge->activeIntervals().size());
        for (const TimeInterval& interval : edge->activeIntervals()) {
            if (interval.length() >= minimum_duration) {
                prefix.common_intervals.push_back(interval);
            }
        }
        prefix.has_common_intervals = true;
    } else {
        if (!provider.recordIntersection()) return false;
        prefix.common_intervals = intersectTimeIntervals(
            prefix.common_intervals,
            edge->activeIntervals(),
            minimum_duration);
    }
    return !prefix.common_intervals.empty();
}

template <typename Provider>
bool extendPrefix(
    const Graph& data,
    const Graph& query,
    int minimum_duration,
    int query_vertex,
    int candidate,
    const PrefixState& source_prefix,
    PrefixState& result,
    Provider& provider) {
    if (std::find(
            source_prefix.mapping.begin(),
            source_prefix.mapping.end(),
            candidate) != source_prefix.mapping.end()) {
        return false;
    }
    result = source_prefix;
    result.mapping[static_cast<std::size_t>(query_vertex)] = candidate;
    if (GraphUtils::hasEdge(
            query.adj, query_vertex, query_vertex) &&
        !applyTemporalConstraint(
            data,
            candidate,
            candidate,
            minimum_duration,
            result,
            provider)) {
        return false;
    }
    for (const Edge& edge :
         query.adj[static_cast<std::size_t>(query_vertex)]) {
        if (edge.to == query_vertex) continue;
        const int mapped =
            result.mapping[static_cast<std::size_t>(edge.to)];
        if (mapped >= 0 &&
            !applyTemporalConstraint(
                data,
                candidate,
                mapped,
                minimum_duration,
                result,
                provider)) {
            return false;
        }
    }
    for (const Edge& edge :
         query.in_adj[static_cast<std::size_t>(query_vertex)]) {
        if (edge.to == query_vertex) continue;
        const int mapped =
            result.mapping[static_cast<std::size_t>(edge.to)];
        if (mapped >= 0 &&
            !applyTemporalConstraint(
                data,
                mapped,
                candidate,
                minimum_duration,
                result,
                provider)) {
            return false;
        }
    }
    return true;
}

class LazyPilotProvider {
public:
    explicit LazyPilotProvider(LazyTransitionOracle& oracle);

    const std::vector<int>& roots(int query_vertex) const;

    BlockOutcome block(
        int query_parent,
        int query_child,
        int parent_data,
        RequiredArcMask mask);

    bool reserveExtensionBatch(
        std::uint64_t candidates,
        std::uint64_t maximum_temporal_arcs) noexcept;

    bool recordExtension() noexcept;

    bool recordIntersection() noexcept;

    PilotFailure failure() const noexcept;

private:
    LazyTransitionOracle& oracle_;
};

struct RescoreWorkCounters {
    std::uint64_t block_requests = 0;
    std::uint64_t extension_probes = 0;
    std::uint64_t interval_intersections = 0;
};

class TreePilotProvider {
public:
    TreePilotProvider(
        const TDTree& tree,
        const V3RescoreBudget& budget)
        : tree_(tree),
          remaining_block_requests_(budget.maximum_block_requests),
          remaining_extension_probes_(budget.maximum_extension_probes),
          remaining_interval_intersections_(
              budget.maximum_interval_intersections) {}

    const std::vector<int>& roots(int) const {
        return tree_.rootCandidates();
    }

    BlockOutcome block(
        int,
        int query_child,
        int parent_data,
        RequiredArcMask) {
        if (failure_ != PilotFailure::None) {
            return {nullptr, failure_};
        }
        if (remaining_block_requests_ == 0) {
            failure_ = PilotFailure::Budget;
            return {nullptr, failure_};
        }
        --remaining_block_requests_;
        if (!checkedAccumulate(counters_.block_requests, 1)) {
            failure_ = PilotFailure::Overflow;
            return {nullptr, failure_};
        }
        const std::vector<int>* values = tree_.candidatesForParent(
            query_child, parent_data);
        if (values == nullptr) {
            static const std::vector<int> empty;
            values = &empty;
        }
        return {values, PilotFailure::None};
    }

    bool reserveExtensionBatch(
        std::uint64_t candidates,
        std::uint64_t maximum_temporal_arcs) noexcept {
        std::uint64_t interval_quote = 0;
        if (!checkedMultiply(
                candidates,
                maximum_temporal_arcs,
                interval_quote)) {
            failure_ = PilotFailure::Overflow;
            return false;
        }
        if (candidates > remaining_extension_probes_ ||
            interval_quote > remaining_interval_intersections_) {
            failure_ = PilotFailure::Budget;
            return false;
        }
        remaining_extension_probes_ -= candidates;
        remaining_interval_intersections_ -= interval_quote;
        return true;
    }

    bool recordExtension() noexcept {
        if (!checkedAccumulate(counters_.extension_probes, 1)) {
            failure_ = PilotFailure::Overflow;
            return false;
        }
        return true;
    }

    bool recordIntersection() noexcept {
        if (!checkedAccumulate(counters_.interval_intersections, 1)) {
            failure_ = PilotFailure::Overflow;
            return false;
        }
        return true;
    }

    PilotFailure failure() const noexcept { return failure_; }
    const RescoreWorkCounters& counters() const noexcept {
        return counters_;
    }

private:
    const TDTree& tree_;
    std::uint64_t remaining_block_requests_ = 0;
    std::uint64_t remaining_extension_probes_ = 0;
    std::uint64_t remaining_interval_intersections_ = 0;
    RescoreWorkCounters counters_;
    PilotFailure failure_ = PilotFailure::None;
};

struct PilotScoreResult {
    V3PlanScoreReceipt receipt;
    PilotFailure failure = PilotFailure::None;
};

bool observeRetainedPrefixes(
    std::size_t retained,
    std::uint64_t& phase_maximum_observed,
    std::uint64_t& overall_maximum_observed) noexcept {
    std::uint64_t retained_u64 = 0;
    if (!sizeToU64(retained, retained_u64)) return false;
    phase_maximum_observed =
        std::max(phase_maximum_observed, retained_u64);
    overall_maximum_observed =
        std::max(overall_maximum_observed, retained_u64);
    return true;
}

template <typename Provider>
PilotScoreResult scoreFixedPlan(
    const ValidatedGraphSnapshot& snapshot,
    const FixedPlan& plan,
    int minimum_duration,
    std::size_t prefix_cap,
    Provider& provider,
    std::uint64_t& phase_maximum_prefix_samples_observed,
    std::uint64_t& overall_maximum_prefix_samples_observed) {
    PilotScoreResult result;
    result.receipt.plan_fingerprint = plan.fingerprint;
    const Graph& data = snapshot.data();
    const Graph& query = snapshot.query();
    const std::vector<int>& roots = provider.roots(plan.root);
    std::uint64_t root_count = 0;
    if (!sizeToU64(roots.size(), root_count)) {
        result.failure = PilotFailure::Overflow;
        return result;
    }
    result.receipt.root_candidates = root_count;
    std::vector<int> empty_mapping(
        static_cast<std::size_t>(query.num_vertices), -1);
    std::uint64_t root_arcs = 0;
    if (!temporalArcUpper(
            query, plan.root, empty_mapping, root_arcs)) {
        result.failure = PilotFailure::Overflow;
        return result;
    }
    if (!provider.reserveExtensionBatch(root_count, root_arcs)) {
        result.failure = provider.failure();
        return result;
    }
    BottomKPrefixes root_sampler(
        prefix_cap, 0);
    for (int root_candidate : roots) {
        if (!provider.recordExtension()) {
            result.failure = provider.failure();
            return result;
        }
        PrefixState source;
        source.mapping = empty_mapping;
        PrefixState extended;
        if (extendPrefix(
                data,
                query,
                minimum_duration,
                plan.root,
                root_candidate,
                source,
                extended,
                provider) &&
            !root_sampler.consider(std::move(extended))) {
            result.failure = PilotFailure::Overflow;
            return result;
        }
        if (provider.failure() != PilotFailure::None) {
            result.failure = provider.failure();
            return result;
        }
    }
    std::vector<PrefixState> prefixes = root_sampler.finish();
    if (!observeRetainedPrefixes(
            prefixes.size(),
            phase_maximum_prefix_samples_observed,
            overall_maximum_prefix_samples_observed)) {
        result.failure = PilotFailure::Overflow;
        return result;
    }
    if (prefixes.empty()) {
        result.failure = PilotFailure::SampleEmpty;
        return result;
    }
    std::uint64_t prefix_cap_u64 = 0;
    if (!sizeToU64(prefix_cap, prefix_cap_u64)) {
        result.failure = PilotFailure::Overflow;
        return result;
    }
    bool exact = root_sampler.seen() <= prefix_cap_u64;

    for (std::size_t depth = 1;
         depth < plan.order.size();
         ++depth) {
        const int child = plan.order[depth];
        const int parent =
            plan.parent[static_cast<std::size_t>(child)];
        const RequiredArcMask mask =
            requiredArcMask(query, parent, child);
        BottomKPrefixes accepted(
            prefix_cap, depth);
        for (const PrefixState& prefix : prefixes) {
            const int parent_data =
                prefix.mapping[static_cast<std::size_t>(parent)];
            const BlockOutcome outcome = provider.block(
                parent, child, parent_data, mask);
            if (outcome.failure != PilotFailure::None ||
                outcome.values == nullptr) {
                result.failure = outcome.failure == PilotFailure::None
                    ? PilotFailure::Overflow
                    : outcome.failure;
                return result;
            }
            std::uint64_t block_size = 0;
            if (!sizeToU64(outcome.values->size(), block_size) ||
                !checkedAccumulate(
                    result.receipt.raw_attempted_extensions,
                    block_size)) {
                result.failure = PilotFailure::Overflow;
                return result;
            }
            std::uint64_t arc_upper = 0;
            if (!temporalArcUpper(
                    query, child, prefix.mapping, arc_upper)) {
                result.failure = PilotFailure::Overflow;
                return result;
            }
            if (!provider.reserveExtensionBatch(
                    block_size, arc_upper)) {
                result.failure = provider.failure();
                return result;
            }
            for (int candidate : *outcome.values) {
                if (!provider.recordExtension()) {
                    result.failure = provider.failure();
                    return result;
                }
                PrefixState extended;
                if (extendPrefix(
                        data,
                        query,
                        minimum_duration,
                        child,
                        candidate,
                        prefix,
                        extended,
                        provider) &&
                    !accepted.consider(std::move(extended))) {
                    result.failure = PilotFailure::Overflow;
                    return result;
                }
                if (provider.failure() != PilotFailure::None) {
                    result.failure = provider.failure();
                    return result;
                }
            }
        }
        prefixes = accepted.finish();
        if (!observeRetainedPrefixes(
                prefixes.size(),
                phase_maximum_prefix_samples_observed,
                overall_maximum_prefix_samples_observed)) {
            result.failure = PilotFailure::Overflow;
            return result;
        }
        if (prefixes.empty()) {
            result.failure = PilotFailure::SampleEmpty;
            return result;
        }
        exact = exact && accepted.seen() <= prefix_cap_u64;
    }
    if (!checkedAdd(
            result.receipt.root_candidates,
            result.receipt.raw_attempted_extensions,
            result.receipt.objective)) {
        result.failure = PilotFailure::Overflow;
        return result;
    }
    result.receipt.available = true;
    result.receipt.exact = exact;
    return result;
}

std::uint64_t rescoreWorkTotal(
    const RescoreWorkCounters& counters,
    bool& valid) noexcept {
    std::uint64_t total = 0;
    valid = checkedAccumulate(total, counters.block_requests) &&
        checkedAccumulate(total, counters.extension_probes) &&
        checkedAccumulate(total, counters.interval_intersections);
    return total;
}

} // namespace

const char* v3TerminalDecisionName(
    V3TerminalDecision decision) noexcept {
    switch (decision) {
        case V3TerminalDecision::IncumbentEmptyOrDisconnected:
            return "incumbent-empty-or-disconnected";
        case V3TerminalDecision::IncumbentOnly:
            return "incumbent-only";
        case V3TerminalDecision::NoNonincumbentCompleteProposal:
            return "no-nonincumbent-complete-proposal";
        case V3TerminalDecision::NoBuildFeasiblePlan:
            return "no-build-feasible-plan";
        case V3TerminalDecision::LazyTransitionBudget:
            return "lazy-transition-budget";
        case V3TerminalDecision::SampleEmpty:
            return "sample-empty";
        case V3TerminalDecision::CandidateBuildBudget:
            return "candidate-build-budget";
        case V3TerminalDecision::CandidateBuildQuoteViolation:
            return "candidate-build-quote-violation";
        case V3TerminalDecision::GlobalMemoryAdmission:
            return "global-memory-admission";
        case V3TerminalDecision::CandidateReservationAllocationFailure:
            return "candidate-reservation-allocation-failure";
        case V3TerminalDecision::CandidateRescoreBudget:
            return "candidate-rescore-budget";
        case V3TerminalDecision::MetadataBudget:
            return "metadata-budget";
        case V3TerminalDecision::SamePlan:
            return "same-plan";
        case V3TerminalDecision::GainBelowMargin:
            return "gain-below-margin";
        case V3TerminalDecision::CandidateSelected:
            return "candidate-selected";
        case V3TerminalDecision::IncumbentOriginMismatch:
            return "incumbent-origin-mismatch";
    }
    return "incumbent-only";
}

const char* v3ProposalStatusName(V3ProposalStatus status) noexcept {
    switch (status) {
        case V3ProposalStatus::Complete:
            return "complete";
        case V3ProposalStatus::MissingSnapshot:
            return "missing-snapshot";
        case V3ProposalStatus::MissingSnapshotAuthority:
            return "missing-snapshot-authority";
        case V3ProposalStatus::InvalidIncumbent:
            return "invalid-incumbent";
        case V3ProposalStatus::StateBudget:
            return "state-budget";
        case V3ProposalStatus::CompletePlanBudget:
            return "complete-plan-budget";
        case V3ProposalStatus::ArithmeticOverflow:
            return "arithmetic-overflow";
        case V3ProposalStatus::AllocationFailure:
            return "allocation-failure";
    }
    return "invalid-incumbent";
}

bool V3FixedPlanProposalResult::complete() const noexcept {
    return !plans.empty() &&
        (status == V3ProposalStatus::Complete ||
         status == V3ProposalStatus::StateBudget ||
         status == V3ProposalStatus::CompletePlanBudget);
}

V3FixedPlanProposalResult generateV3FixedPlanProposals(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot,
    const CandidateCover& cover,
    const FixedPlan& incumbent_plan,
    const V3ProposalBudget& budget,
    std::uint64_t maximum_trace_entries) noexcept {
    V3FixedPlanProposalResult result;
    if (!snapshot) return result;
    if (!cover.hasValidatedSnapshotAuthority() ||
        !cover.isBoundToValidatedSnapshot(snapshot)) {
        result.status = V3ProposalStatus::MissingSnapshotAuthority;
        return result;
    }
    if (!validateFixedPlan(*snapshot, incumbent_plan).valid ||
        incumbent_plan.fingerprint !=
            cover.incumbentPlanFingerprint()) {
        result.status = V3ProposalStatus::InvalidIncumbent;
        return result;
    }

    try {
        std::size_t trace_capacity = 0;
        if (!u64ToSize(maximum_trace_entries, trace_capacity)) {
            result.status = V3ProposalStatus::ArithmeticOverflow;
            return result;
        }
        result.deterministic_trace.reserve(trace_capacity);
        if (result.deterministic_trace.capacity() < trace_capacity) {
            result.status = V3ProposalStatus::AllocationFailure;
            return result;
        }
        ProposalBuilder builder{
            snapshot,
            cover,
            incumbent_plan,
            budget,
            maximum_trace_entries,
            result};
        if (!builder.add(incumbent_plan)) {
            result.status = V3ProposalStatus::CompletePlanBudget;
            return result;
        }

        const int query_vertices = snapshot->query().num_vertices;
        for (int root = 0; root < query_vertices; ++root) {
            if (!builder.add(rerootIncumbentTree(
                    *snapshot, incumbent_plan, root))) {
                break;
            }
        }
        if (!builder.plan_budget_hit) {
            for (int root = 0; root < query_vertices; ++root) {
                if (!builder.add(greedyPlan(*snapshot, cover, root))) {
                    break;
                }
            }
        }

        if (!builder.plan_budget_hit) {
            for (int root = 0;
                 root < query_vertices &&
                     !builder.state_budget_hit &&
                     !builder.plan_budget_hit;
                 ++root) {
                std::vector<int> order;
                order.reserve(static_cast<std::size_t>(query_vertices));
                std::vector<int> parent(
                    static_cast<std::size_t>(query_vertices), -1);
                std::vector<std::uint8_t> scheduled(
                    static_cast<std::size_t>(query_vertices), 0);
                order.push_back(root);
                scheduled[static_cast<std::size_t>(root)] = 1;
                auto search = [&](auto&& self) -> void {
                    if (builder.state_budget_hit ||
                        builder.plan_budget_hit ||
                        !builder.takeState()) {
                        return;
                    }
                    if (order.size() ==
                        static_cast<std::size_t>(query_vertices)) {
                        builder.add(makeFixedPlan(
                            *snapshot,
                            root,
                            order,
                            parent));
                        return;
                    }
                    const std::vector<FrontierChoice> frontier =
                        collectFrontier(
                            snapshot->query(), cover, scheduled);
                    if (frontier.empty()) {
                        if (!checkedAccumulate(
                                result.telemetry
                                    .v3_invalid_plan_proposals,
                                1)) {
                            throw std::overflow_error(
                                "V3 invalid state overflow");
                        }
                        return;
                    }
                    for (const FrontierChoice& choice : frontier) {
                        if (builder.state_budget_hit ||
                            builder.plan_budget_hit) {
                            break;
                        }
                        order.push_back(choice.child);
                        scheduled[static_cast<std::size_t>(choice.child)] = 1;
                        parent[static_cast<std::size_t>(choice.child)] =
                            choice.parent;
                        self(self);
                        parent[static_cast<std::size_t>(choice.child)] = -1;
                        scheduled[static_cast<std::size_t>(choice.child)] = 0;
                        order.pop_back();
                    }
                };
                search(search);
            }
        }
        result.status = builder.plan_budget_hit
            ? V3ProposalStatus::CompletePlanBudget
            : builder.state_budget_hit
                ? V3ProposalStatus::StateBudget
                : V3ProposalStatus::Complete;
        return result;
    } catch (const std::bad_alloc&) {
        result.status = V3ProposalStatus::AllocationFailure;
        result.plans.clear();
        return result;
    } catch (const std::overflow_error&) {
        result.status = V3ProposalStatus::ArithmeticOverflow;
        result.plans.clear();
        return result;
    } catch (...) {
        result.status = V3ProposalStatus::InvalidIncumbent;
        result.plans.clear();
        return result;
    }
}

namespace {

using LazyBlockKey = V3LazyCacheKeyStorageV1;

bool operator==(
    const LazyBlockKey& lhs,
    const LazyBlockKey& rhs) noexcept {
    return lhs.snapshot_token == rhs.snapshot_token &&
        lhs.cover_fingerprint == rhs.cover_fingerprint &&
        lhs.query_parent == rhs.query_parent &&
        lhs.query_child == rhs.query_child &&
        lhs.parent_data == rhs.parent_data &&
        lhs.mask == rhs.mask;
}

std::uint64_t lazyKeyFingerprint(const LazyBlockKey& key) noexcept {
    std::uint64_t value = splitmix64(key.snapshot_token);
    value = splitmix64(value ^ key.cover_fingerprint);
    value = splitmix64(
        value ^ static_cast<std::uint32_t>(key.query_parent));
    value = splitmix64(
        value ^ static_cast<std::uint32_t>(key.query_child));
    value = splitmix64(
        value ^ static_cast<std::uint32_t>(key.parent_data));
    return splitmix64(
        value ^ static_cast<std::uint64_t>(key.mask));
}

using LazyCacheEntry = V3LazyCacheEntryStorageV1;

const TemporalEdge* edgeFromReference(
    const Graph& data,
    const Edge& edge) noexcept {
    if (edge.temporal_edge_id < 0 ||
        static_cast<std::size_t>(edge.temporal_edge_id) >=
            data.temporal_edges.size()) {
        return nullptr;
    }
    return &data.temporal_edges[
        static_cast<std::size_t>(edge.temporal_edge_id)];
}

class LazyTransitionOracle {
public:
    LazyTransitionOracle(
        std::shared_ptr<const ValidatedGraphSnapshot> snapshot,
        const CandidateCover& cover,
        const V3LazyBudget& budget,
        V3PlannerTelemetry& telemetry,
        std::vector<std::uint64_t>& trace,
        std::uint64_t trace_cap)
        : snapshot_(std::move(snapshot)),
          cover_(cover),
          budget_(budget),
          telemetry_(telemetry),
          trace_(trace),
          trace_cap_(trace_cap),
          remaining_block_requests_(budget.maximum_block_requests),
          remaining_adjacency_probes_(budget.maximum_adjacency_probes),
          remaining_reverse_lookups_(budget.maximum_reverse_lookups),
          remaining_arc_checks_(budget.maximum_arc_checks),
          remaining_relation_entries_(budget.maximum_relation_entries),
          remaining_extension_probes_(budget.maximum_extension_probes),
          remaining_interval_intersections_(
              budget.maximum_interval_intersections) {
        std::size_t cache_entries = 0;
        const std::uint64_t bounded_entries = std::min(
            budget.maximum_cache_entries,
            budget.maximum_block_requests);
        if (!u64ToSize(bounded_entries, cache_entries)) {
            failure_ = PilotFailure::Overflow;
            return;
        }
        std::uint64_t requested_bytes = 0;
        if (!checkedMultiply(
                bounded_entries,
                static_cast<std::uint64_t>(sizeof(LazyCacheEntry)),
                requested_bytes)) {
            failure_ = PilotFailure::Overflow;
            return;
        }
        if (requested_bytes > budget.maximum_cache_capacity_bytes) {
            failure_ = PilotFailure::Budget;
            return;
        }
        cache_.reserve(cache_entries);
        std::uint64_t observed_entries = 0;
        if (!sizeToU64(cache_.capacity(), observed_entries) ||
            observed_entries > bounded_entries ||
            !checkedMultiply(
                observed_entries,
                static_cast<std::uint64_t>(sizeof(LazyCacheEntry)),
                cache_capacity_bytes_) ||
            cache_capacity_bytes_ >
                budget.maximum_cache_capacity_bytes) {
            failure_ = PilotFailure::Budget;
            return;
        }
        telemetry_.v3_lazy_cache_capacity_bytes =
            cache_capacity_bytes_;
    }

    PilotFailure failure() const noexcept { return failure_; }

    LazyCacheCapacityDetailV1 capacityDetail() const noexcept {
        return lazyCacheCapacityDetailV1(cache_);
    }

    LazyCacheCapacityDetailV1 releaseOwnedCapacity() noexcept {
        LazyCacheCapacityDetailV1 detail = capacityDetail();
        std::vector<LazyCacheEntry>().swap(cache_);
        cache_capacity_bytes_ = 0;
        return detail;
    }

    const std::vector<int>& roots(int query_vertex) const {
        return cover_.domain(static_cast<std::size_t>(query_vertex));
    }

    BlockOutcome request(
        int query_parent,
        int query_child,
        int parent_data,
        RequiredArcMask mask) {
        if (failure_ != PilotFailure::None) {
            return {nullptr, failure_};
        }
        if (remaining_block_requests_ == 0) {
            rejectBeforeStart();
            return {nullptr, PilotFailure::Budget};
        }
        --remaining_block_requests_;
        if (!bump(telemetry_.v3_lazy_block_requests)) {
            return {nullptr, PilotFailure::Overflow};
        }
        const LazyBlockKey key{
            snapshot_->snapshotToken(),
            cover_.coverFingerprint(),
            query_parent,
            query_child,
            parent_data,
            mask};
        const std::uint64_t key_fingerprint =
            lazyKeyFingerprint(key);
        appendTrace(
            trace_, trace_cap_,
            traceToken(kTraceLazyRequest, key_fingerprint),
            telemetry_);
        for (const LazyCacheEntry& entry : cache_) {
            if (entry.key == key) {
                if (!bump(telemetry_.v3_lazy_block_hits)) {
                    return {nullptr, PilotFailure::Overflow};
                }
                appendTrace(
                    trace_, trace_cap_,
                    traceToken(kTraceLazyHit, key_fingerprint),
                    telemetry_);
                return {&entry.values, PilotFailure::None};
            }
        }
        if (!bump(telemetry_.v3_lazy_block_misses)) {
            return {nullptr, PilotFailure::Overflow};
        }
        appendTrace(
            trace_, trace_cap_,
            traceToken(kTraceLazyMiss, key_fingerprint),
            telemetry_);

        const Graph& data = snapshot_->data();
        if (parent_data < 0 || parent_data >= data.num_vertices ||
            mask == RequiredArcMask::None ||
            cache_.size() >= cache_.capacity()) {
            rejectBeforeStart();
            return {nullptr, PilotFailure::Budget};
        }
        const auto& outgoing =
            data.adj[static_cast<std::size_t>(parent_data)];
        const auto& incoming =
            data.in_adj[static_cast<std::size_t>(parent_data)];
        const std::vector<Edge>* scan = nullptr;
        bool scanned_outgoing = false;
        if (mask == RequiredArcMask::ParentToChild) {
            scan = &outgoing;
            scanned_outgoing = true;
        } else if (mask == RequiredArcMask::ChildToParent) {
            scan = &incoming;
        } else if (outgoing.size() <= incoming.size()) {
            scan = &outgoing;
            scanned_outgoing = true;
        } else {
            scan = &incoming;
        }

        std::uint64_t scan_size = 0;
        std::uint64_t reverse_quote = 0;
        std::uint64_t arc_quote = 0;
        std::uint64_t value_bytes = 0;
        if (!sizeToU64(scan->size(), scan_size) ||
            !checkedMultiply(
                scan_size,
                mask == RequiredArcMask::Reciprocal ? 2 : 1,
                arc_quote) ||
            !checkedMultiply(
                scan_size,
                static_cast<std::uint64_t>(sizeof(int)),
                value_bytes)) {
            failure_ = PilotFailure::Overflow;
            return {nullptr, failure_};
        }
        if (mask == RequiredArcMask::Reciprocal) {
            reverse_quote = scan_size;
        }
        std::uint64_t requested_cache_bytes = 0;
        if (!checkedAdd(
                cache_capacity_bytes_,
                value_bytes,
                requested_cache_bytes)) {
            failure_ = PilotFailure::Overflow;
            return {nullptr, failure_};
        }
        if (scan_size > remaining_adjacency_probes_ ||
            reverse_quote > remaining_reverse_lookups_ ||
            arc_quote > remaining_arc_checks_ ||
            scan_size > remaining_relation_entries_ ||
            requested_cache_bytes >
                budget_.maximum_cache_capacity_bytes) {
            rejectBeforeStart();
            return {nullptr, PilotFailure::Budget};
        }

        LazyCacheEntry entry;
        entry.key = key;
        entry.values.reserve(scan->size());
        std::uint64_t observed_value_capacity = 0;
        std::uint64_t observed_value_bytes = 0;
        std::uint64_t observed_cache_bytes = 0;
        if (!sizeToU64(
                entry.values.capacity(), observed_value_capacity) ||
            !checkedMultiply(
                observed_value_capacity,
                static_cast<std::uint64_t>(sizeof(int)),
                observed_value_bytes) ||
            !checkedAdd(
                cache_capacity_bytes_,
                observed_value_bytes,
                observed_cache_bytes) ||
            observed_cache_bytes >
                budget_.maximum_cache_capacity_bytes) {
            rejectBeforeStart();
            return {nullptr, PilotFailure::Budget};
        }

        remaining_adjacency_probes_ -= scan_size;
        remaining_reverse_lookups_ -= reverse_quote;
        remaining_arc_checks_ -= arc_quote;
        remaining_relation_entries_ -= scan_size;
        if (!bump(telemetry_.v3_lazy_blocks_started)) {
            return {nullptr, PilotFailure::Overflow};
        }
        const auto& child_domain =
            cover_.domain(static_cast<std::size_t>(query_child));
        for (const Edge& edge_reference : *scan) {
            if (!bump(telemetry_.v3_lazy_adjacency_probes) ||
                !bump(telemetry_.v3_lazy_arc_checks)) {
                return {nullptr, PilotFailure::Overflow};
            }
            const int candidate = edge_reference.to;
            if (!std::binary_search(
                    child_domain.begin(), child_domain.end(), candidate)) {
                continue;
            }
            const TemporalEdge* primary =
                edgeFromReference(data, edge_reference);
            if (primary == nullptr ||
                !hasMinimumConsecutiveDuration(
                    *primary, cover_.minimumDuration())) {
                continue;
            }
            if (mask == RequiredArcMask::Reciprocal) {
                if (!bump(telemetry_.v3_lazy_reverse_lookups) ||
                    !bump(telemetry_.v3_lazy_arc_checks)) {
                    return {nullptr, PilotFailure::Overflow};
                }
                const TemporalEdge* reverse = scanned_outgoing
                    ? data.findTemporalEdge(candidate, parent_data)
                    : data.findTemporalEdge(parent_data, candidate);
                if (reverse == nullptr ||
                    !hasMinimumConsecutiveDuration(
                        *reverse, cover_.minimumDuration())) {
                    continue;
                }
            }
            entry.values.push_back(candidate);
        }
        std::sort(entry.values.begin(), entry.values.end());
        entry.values.erase(
            std::unique(entry.values.begin(), entry.values.end()),
            entry.values.end());
        std::uint64_t emitted = 0;
        if (!sizeToU64(entry.values.size(), emitted) ||
            !checkedAccumulate(
                telemetry_.v3_lazy_relation_entries, emitted)) {
            failure_ = PilotFailure::Overflow;
            return {nullptr, failure_};
        }
        cache_.push_back(std::move(entry));
        cache_capacity_bytes_ = observed_cache_bytes;
        telemetry_.v3_lazy_cache_capacity_bytes =
            cache_capacity_bytes_;
        if (!bump(telemetry_.v3_lazy_blocks_completed)) {
            return {nullptr, PilotFailure::Overflow};
        }
        return {&cache_.back().values, PilotFailure::None};
    }

    bool reserveExtensionBatch(
        std::uint64_t candidates,
        std::uint64_t maximum_temporal_arcs) noexcept {
        std::uint64_t interval_quote = 0;
        if (!checkedMultiply(
                candidates,
                maximum_temporal_arcs,
                interval_quote)) {
            failure_ = PilotFailure::Overflow;
            return false;
        }
        if (candidates > remaining_extension_probes_ ||
            interval_quote > remaining_interval_intersections_) {
            failure_ = PilotFailure::Budget;
            return false;
        }
        remaining_extension_probes_ -= candidates;
        remaining_interval_intersections_ -= interval_quote;
        return true;
    }

    bool recordExtension() noexcept {
        return bump(telemetry_.v3_lazy_extension_probes);
    }

    bool recordIntersection() noexcept {
        return bump(telemetry_.v3_lazy_interval_intersections);
    }

private:
    bool bump(std::uint64_t& counter) noexcept {
        if (!checkedAccumulate(counter, 1)) {
            failure_ = PilotFailure::Overflow;
            return false;
        }
        return true;
    }

    void rejectBeforeStart() noexcept {
        if (!checkedAccumulate(
                telemetry_.v3_lazy_budget_rejected_before_start, 1)) {
            failure_ = PilotFailure::Overflow;
        } else {
            failure_ = PilotFailure::Budget;
        }
    }

    std::shared_ptr<const ValidatedGraphSnapshot> snapshot_;
    const CandidateCover& cover_;
    V3LazyBudget budget_;
    V3PlannerTelemetry& telemetry_;
    std::vector<std::uint64_t>& trace_;
    std::uint64_t trace_cap_ = 0;
    std::vector<LazyCacheEntry> cache_;
    std::uint64_t cache_capacity_bytes_ = 0;
    std::uint64_t remaining_block_requests_ = 0;
    std::uint64_t remaining_adjacency_probes_ = 0;
    std::uint64_t remaining_reverse_lookups_ = 0;
    std::uint64_t remaining_arc_checks_ = 0;
    std::uint64_t remaining_relation_entries_ = 0;
    std::uint64_t remaining_extension_probes_ = 0;
    std::uint64_t remaining_interval_intersections_ = 0;
    PilotFailure failure_ = PilotFailure::None;
};

LazyPilotProvider::LazyPilotProvider(
    LazyTransitionOracle& oracle)
    : oracle_(oracle) {}

const std::vector<int>& LazyPilotProvider::roots(
    int query_vertex) const {
    return oracle_.roots(query_vertex);
}

BlockOutcome LazyPilotProvider::block(
    int query_parent,
    int query_child,
    int parent_data,
    RequiredArcMask mask) {
    return oracle_.request(
        query_parent,
        query_child,
        parent_data,
        mask);
}

bool LazyPilotProvider::reserveExtensionBatch(
    std::uint64_t candidates,
    std::uint64_t maximum_temporal_arcs) noexcept {
    return oracle_.reserveExtensionBatch(
        candidates, maximum_temporal_arcs);
}

bool LazyPilotProvider::recordExtension() noexcept {
    return oracle_.recordExtension();
}

bool LazyPilotProvider::recordIntersection() noexcept {
    return oracle_.recordIntersection();
}

PilotFailure LazyPilotProvider::failure() const noexcept {
    return oracle_.failure();
}

} // namespace

namespace {

bool metadataUpperBoundFits(
    std::size_t query_vertices,
    const TemporalCostV3Options& options,
    const V3MetadataBudget& budget,
    std::uint64_t& required_bytes) noexcept {
    std::uint64_t query_vertices_u64 = 0;
    std::uint64_t per_vertex = 0;
    std::uint64_t vector_bytes = 0;
    if (!sizeToU64(query_vertices, query_vertices_u64) ||
        !checkedAdd(
            2 * static_cast<std::uint64_t>(sizeof(int)),
            2 * static_cast<std::uint64_t>(sizeof(double)),
            per_vertex) ||
        !checkedAccumulate(
            per_vertex,
            static_cast<std::uint64_t>(sizeof(std::uint64_t))) ||
        !checkedMultiply(
            query_vertices_u64, per_vertex, vector_bytes) ||
        !checkedAdd(
            static_cast<std::uint64_t>(sizeof(SelectedPlanReceipt)),
            vector_bytes,
            required_bytes)) {
        return false;
    }
    return budget.receipt_tokens >= 1 &&
        required_bytes <= budget.payload_bytes &&
        required_bytes <= options.metadata_payload_cap;
}

bool addBuildWork(
    const CandidateBuildCounters& counters,
    std::uint64_t& total) noexcept {
    return checkedAccumulate(total, counters.adjacency_probes) &&
        checkedAccumulate(total, counters.reverse_edge_lookups) &&
        checkedAccumulate(total, counters.arc_checks) &&
        checkedAccumulate(total, counters.root_self_loop_checks) &&
        checkedAccumulate(total, counters.relation_entries) &&
        checkedAccumulate(total, counters.candidate_value_occurrences) &&
        checkedAccumulate(total, counters.nonempty_blocks) &&
        checkedAccumulate(total, counters.semijoin_entry_visits) &&
        checkedAccumulate(total, counters.index_entry_visits);
}

bool addLazyWork(
    const V3PlannerTelemetry& telemetry,
    std::uint64_t& total) noexcept {
    return checkedAccumulate(total, telemetry.v3_lazy_block_requests) &&
        checkedAccumulate(total, telemetry.v3_lazy_adjacency_probes) &&
        checkedAccumulate(total, telemetry.v3_lazy_reverse_lookups) &&
        checkedAccumulate(total, telemetry.v3_lazy_arc_checks) &&
        checkedAccumulate(total, telemetry.v3_lazy_relation_entries) &&
        checkedAccumulate(total, telemetry.v3_lazy_extension_probes) &&
        checkedAccumulate(
            total, telemetry.v3_lazy_interval_intersections);
}

bool candidateCostTotal(
    const V3PlannerTelemetry& telemetry,
    const V3PlanScoreReceipt& candidate_score,
    std::uint64_t& total) noexcept {
    total = candidate_score.objective;
    return checkedAccumulate(total, telemetry.v3_cover_export_entries) &&
        checkedAccumulate(total, telemetry.v3_proposal_states_expanded) &&
        checkedAccumulate(total, telemetry.v3_complete_plan_proposals) &&
        checkedAccumulate(total, telemetry.v3_quote_cover_value_visits) &&
        addLazyWork(telemetry, total) &&
        checkedAccumulate(total, telemetry.v3_candidate_build_work) &&
        checkedAccumulate(total, telemetry.v3_candidate_rescore_work);
}

bool incumbentCostTotal(
    const V3PlannerTelemetry& telemetry,
    const V3PlanScoreReceipt& incumbent_score,
    std::uint64_t& total) noexcept {
    total = incumbent_score.objective;
    return checkedAccumulate(
        total, telemetry.v3_incumbent_rescore_work);
}

std::uint64_t ninetyPercentFloor(std::uint64_t value) noexcept {
    const std::uint64_t rounded_up_tenth =
        value / 10 + (value % 10 == 0 ? 0 : 1);
    return value - rounded_up_tenth;
}

bool commitSelectedMetadata(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot,
    std::unique_ptr<TDTree>& selected_tree,
    const FixedPlan& selected_plan,
    PlannerDecision planner_decision,
    const V3PlannerTelemetry& telemetry,
    V3MetadataBudget& metadata_budget,
    const TemporalCostV3Options& options,
    TemporalCostV3Result& result) {
    if (!snapshot || !selected_tree) return false;
    const std::size_t query_vertices = static_cast<std::size_t>(
        snapshot->query().num_vertices);
    std::uint64_t required_bytes = 0;
    if (!metadataUpperBoundFits(
            query_vertices,
            options,
            metadata_budget,
            required_bytes)) {
        return false;
    }
    SelectedPlanMetadata metadata;
    metadata.reporting_mode = SelectivityMode::TemporalCost;
    metadata.root_scores.reserve(query_vertices);
    metadata.reporting_selectivity.reserve(query_vertices);
    metadata.candidate_counts.reserve(query_vertices);
    if (metadata.root_scores.capacity() != query_vertices ||
        metadata.reporting_selectivity.capacity() != query_vertices ||
        metadata.candidate_counts.capacity() != query_vertices) {
        return false;
    }
    for (std::size_t query_vertex = 0;
         query_vertex < query_vertices;
         ++query_vertex) {
        const std::size_t count =
            selected_tree->relationEntryCount(
                static_cast<int>(query_vertex));
        std::uint64_t count_u64 = 0;
        if (!sizeToU64(count, count_u64)) return false;
        const double score = static_cast<double>(count_u64);
        if (!std::isfinite(score)) return false;
        metadata.root_scores.push_back(score);
        metadata.reporting_selectivity.push_back(0.0);
        metadata.candidate_counts.push_back(count_u64);
    }
    metadata.decision = planner_decision;
    metadata.diagnostics.complete_plan_proposals =
        telemetry.v3_complete_plan_proposals;
    metadata.diagnostics.scored_plan_proposals =
        telemetry.v3_scored_plan_proposals;
    metadata.diagnostics.materialized_complete_plans =
        telemetry.v3_materialized_complete_plans;
    metadata.diagnostics.rescored_complete_plans =
        telemetry.v3_rescored_complete_plans;
    metadata.diagnostics.partial_tree_handoffs =
        telemetry.v3_partial_candidate_tree_handoffs;
    metadata.diagnostics.graph_validation_passes =
        telemetry.graph_validation_passes;
    metadata.diagnostics.graph_validation_rescans =
        telemetry.graph_validation_rescans;
    metadata.diagnostics.data_fingerprint_rescans =
        telemetry.data_fingerprint_rescans;
    metadata.diagnostics.query_fingerprint_rescans =
        telemetry.query_fingerprint_rescans;

    const V3MetadataQuote quote = quoteV3PlanHandoff(
        snapshot,
        *selected_tree,
        selected_plan,
        metadata,
        options.metadata_payload_cap);
    if (!quote.complete || !quote.fits(metadata_budget) ||
        !quote.isBoundToValidatedSnapshot(snapshot)) {
        return false;
    }
    V3PlanHandoffResult handoff = commitV3PlanHandoff(
        snapshot,
        selected_tree,
        selected_plan,
        metadata,
        quote,
        metadata_budget);
    if (!handoff.complete()) return false;
    result.selected_tree = std::move(handoff.selected_tree);
    result.receipt = std::move(handoff.receipt);
    return true;
}

using TopLazyCandidate = V3SelectedCandidateCarrierV1;

V3CandidateBuildWorkVector projectQuoteWork(
    const CandidateBuildQuote& quote) noexcept {
    V3CandidateBuildWorkVector work;
    work.adjacency_probes = quote.adjacency_probes;
    work.reverse_edge_lookups = quote.reverse_edge_lookups;
    work.arc_checks = quote.maximum_arc_checks;
    work.relation_entries = quote.maximum_relation_entries;
    work.candidate_value_occurrences =
        quote.maximum_candidate_value_occurrences;
    work.blocks = quote.maximum_blocks;
    work.semijoin_entry_visits =
        quote.maximum_semijoin_entry_visits;
    work.index_entries = quote.maximum_index_entries;
    work.payload_memory_bytes = quote.maximum_payload_bytes;
    return work;
}

V3CandidateBuildWorkVector projectActualWork(
    const CandidateBuildCounters& counters) noexcept {
    V3CandidateBuildWorkVector work;
    work.adjacency_probes = counters.adjacency_probes;
    work.reverse_edge_lookups = counters.reverse_edge_lookups;
    work.arc_checks = counters.arc_checks;
    work.relation_entries = counters.relation_entries;
    work.candidate_value_occurrences =
        counters.candidate_value_occurrences;
    work.blocks = counters.nonempty_blocks;
    work.semijoin_entry_visits = counters.semijoin_entry_visits;
    work.index_entries = counters.index_entry_visits;
    work.payload_memory_bytes = counters.peak_payload_capacity_bytes;
    return work;
}

bool workDominates(
    const V3CandidateBuildWorkVector& upper,
    const V3CandidateBuildWorkVector& lower) noexcept {
    return lower.adjacency_probes <= upper.adjacency_probes &&
        lower.reverse_edge_lookups <= upper.reverse_edge_lookups &&
        lower.arc_checks <= upper.arc_checks &&
        lower.relation_entries <= upper.relation_entries &&
        lower.candidate_value_occurrences <=
            upper.candidate_value_occurrences &&
        lower.blocks <= upper.blocks &&
        lower.semijoin_entry_visits <= upper.semijoin_entry_visits &&
        lower.index_entries <= upper.index_entries &&
        lower.payload_memory_bytes <= upper.payload_memory_bytes;
}

bool scorePrecedes(
    const V3PlanScoreReceipt& lhs,
    const V3RetainedSelectedScoreV1& rhs) noexcept {
    return std::tie(lhs.objective, lhs.plan_fingerprint) <
        std::tie(rhs.objective, rhs.plan_fingerprint);
}

void retainScore(
    const V3PlanScoreReceipt& source,
    V3RetainedSelectedScoreV1& destination) noexcept {
    destination.available = source.available;
    destination.exact = source.exact;
    destination.plan_fingerprint = source.plan_fingerprint;
    destination.root_candidates = source.root_candidates;
    destination.raw_attempted_extensions =
        source.raw_attempted_extensions;
    destination.objective = source.objective;
}

} // namespace

bool TemporalCostV3Result::selectedCandidate() const noexcept {
    return decision == V3TerminalDecision::CandidateSelected;
}

bool V3CandidateBuildEvidence::invariantHolds() const noexcept {
    const V3CandidateBuildWorkVector zero;
    if (!quote_available) {
        return sealed_quote.adjacency_probes == zero.adjacency_probes &&
            sealed_quote.reverse_edge_lookups == zero.reverse_edge_lookups &&
            sealed_quote.arc_checks == zero.arc_checks &&
            sealed_quote.relation_entries == zero.relation_entries &&
            sealed_quote.candidate_value_occurrences ==
                zero.candidate_value_occurrences &&
            sealed_quote.blocks == zero.blocks &&
            sealed_quote.semijoin_entry_visits ==
                zero.semijoin_entry_visits &&
            sealed_quote.index_entries == zero.index_entries &&
            sealed_quote.payload_memory_bytes == zero.payload_memory_bytes &&
            actual.adjacency_probes == 0 &&
            actual.reverse_edge_lookups == 0 &&
            actual.arc_checks == 0 && actual.relation_entries == 0 &&
            actual.candidate_value_occurrences == 0 &&
            actual.blocks == 0 && actual.semijoin_entry_visits == 0 &&
            actual.index_entries == 0 && actual.payload_memory_bytes == 0 &&
            partial_handoffs == 0 && quote_dominance_verified;
    }
    return partial_handoffs == 0 && quote_dominance_verified &&
        workDominates(sealed_quote, actual);
}

bool TemporalCostV3Result::invariantHolds() const noexcept {
    if (!candidate_build.invariantHolds() ||
        telemetry.v3_complete_plan_proposals <
            telemetry.v3_scored_plan_proposals ||
        telemetry.v3_scored_plan_proposals <
            telemetry.v3_materialized_complete_plans ||
        telemetry.v3_materialized_complete_plans <
            telemetry.v3_rescored_complete_plans ||
        telemetry.v3_materialized_complete_plans > 1 ||
        telemetry.v3_rescored_complete_plans > 1 ||
        telemetry.v3_partial_candidate_tree_handoffs != 0 ||
        telemetry.extra_full_q_by_n_domain_scans != 0 ||
        telemetry.global_query_edge_catalogue_builds != 0 ||
        telemetry.global_arc_consistency_passes != 0 ||
        telemetry.v3_implicit_parent_relation_lookups != 0 ||
        telemetry.v3_candidate_build_quote_violations != 0 ||
        telemetry.v3_lazy_maximum_prefix_samples_observed >
            telemetry.v3_lazy_prefix_sample_cap ||
        telemetry.v3_rescore_maximum_prefix_samples_observed >
            telemetry.v3_rescore_prefix_sample_cap ||
        telemetry.v3_maximum_prefix_samples_observed !=
            std::max(
                telemetry.v3_lazy_maximum_prefix_samples_observed,
                telemetry.v3_rescore_maximum_prefix_samples_observed)) {
        return false;
    }

    if (global_memory_admission_required) {
        if (candidate_build.quote_available) {
            if (!global_memory_admission_attempted) return false;
            if (!existing_capacity.invariantHolds()) {
                return decision == V3TerminalDecision::GlobalMemoryAdmission &&
                    !global_memory_admission_consumed &&
                    telemetry.v3_materialized_complete_plans == 0 &&
                    !global_memory_admission.complete;
            }
            if (decision == V3TerminalDecision::GlobalMemoryAdmission) {
                if (global_memory_admission_consumed ||
                    telemetry.v3_materialized_complete_plans != 0 ||
                    !global_memory_admission.invariantHolds() ||
                    global_memory_admission.fits) {
                    return false;
                }
            } else if (decision ==
                    V3TerminalDecision::CandidateReservationAllocationFailure) {
                if (global_memory_admission_consumed ||
                    telemetry.v3_materialized_complete_plans != 0 ||
                    !existing_capacity.invariantHolds() ||
                    !global_memory_admission.invariantHolds() ||
                    !global_memory_admission.fits) {
                    return false;
                }
            } else if (!existing_capacity.invariantHolds() ||
                !global_memory_admission.invariantHolds() ||
                !global_memory_admission_consumed) {
                return false;
            }
        } else if (global_memory_admission_attempted ||
            global_memory_admission_consumed) {
            return false;
        }
    } else if (global_memory_admission_attempted ||
        global_memory_admission_consumed || existing_capacity.complete ||
        global_memory_admission.complete) {
        return false;
    }

    const std::uint64_t materialized =
        telemetry.v3_materialized_complete_plans;
    const std::uint64_t rescored =
        telemetry.v3_rescored_complete_plans;
    if (selectedCandidate()) {
        if (materialized != 1 || rescored != 1 ||
            selected_plan_fingerprint == incumbent_plan_fingerprint ||
            selected_tree == nullptr || receipt == nullptr ||
            receipt->decision() != PlannerDecision::CandidateSelected) {
            return false;
        }
    } else if (selected_plan_fingerprint !=
            incumbent_plan_fingerprint ||
        (receipt != nullptr &&
         receipt->decision() != PlannerDecision::IncumbentOnly)) {
        return false;
    }

    // The terminal reason fixes which single candidate token could have been
    // consumed.  This makes fallback receipts fail closed instead of merely
    // satisfying the monotone proposal/scoring counters.
    switch (decision) {
        case V3TerminalDecision::IncumbentEmptyOrDisconnected:
        case V3TerminalDecision::NoNonincumbentCompleteProposal:
        case V3TerminalDecision::NoBuildFeasiblePlan:
        case V3TerminalDecision::LazyTransitionBudget:
        case V3TerminalDecision::CandidateBuildBudget:
        case V3TerminalDecision::GlobalMemoryAdmission:
        case V3TerminalDecision::CandidateReservationAllocationFailure:
        case V3TerminalDecision::SamePlan:
        case V3TerminalDecision::IncumbentOriginMismatch:
            if (materialized != 0 || rescored != 0) return false;
            break;
        case V3TerminalDecision::SampleEmpty:
            if (!((materialized == 0 && rescored == 0) ||
                  (materialized == 1 && rescored == 0))) {
                return false;
            }
            break;
        case V3TerminalDecision::CandidateRescoreBudget:
            if (materialized != 1 || rescored > 1) return false;
            break;
        case V3TerminalDecision::MetadataBudget:
            if (materialized != rescored) return false;
            break;
        case V3TerminalDecision::GainBelowMargin:
            if (materialized != 1 || rescored != 1) return false;
            break;
        case V3TerminalDecision::CandidateBuildQuoteViolation:
            // A quote/capacity contract violation is itself a failed hard
            // invariant, irrespective of how far materialization progressed.
            return false;
        case V3TerminalDecision::IncumbentOnly:
            // Exception-safe incumbent recovery can occur at any phase; the
            // global one-token and monotonicity checks above still apply.
            break;
        case V3TerminalDecision::CandidateSelected:
            // The exact 1/1 state and selected receipt were checked above.
            break;
    }
    if (receipt != nullptr &&
        (selected_tree == nullptr ||
         receipt->plan().fingerprint !=
             selected_tree->executionPlanFingerprint() ||
         receipt->plan().fingerprint != selected_plan_fingerprint)) {
        return false;
    }
    return telemetry.graph_validation_passes == 1 &&
        telemetry.graph_validation_rescans == 0 &&
        telemetry.data_fingerprint_rescans == 0 &&
        telemetry.query_fingerprint_rescans == 0;
}

TemporalCostV3Result planTemporalCostV3(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot,
    std::unique_ptr<TDTree> incumbent_tree,
    const FixedPlan& incumbent_plan,
    CandidateBuildLedger& candidate_build_ledger,
    V3MetadataBudget& metadata_budget,
    const TemporalCostV3Options& options) noexcept {
    TemporalCostV3Result result;
    result.global_memory_admission_required =
        options.global_memory_admission_enabled;
    result.incumbent_plan_fingerprint = incumbent_plan.fingerprint;
    result.selected_plan_fingerprint = incumbent_plan.fingerprint;
    result.telemetry.cost_convention_id = kCostConventionId;
    std::uint64_t lazy_prefix_sample_cap = 0;
    std::uint64_t rescore_prefix_sample_cap = 0;
    if (!sizeToU64(
            options.lazy_budget.prefix_sample_cap,
            lazy_prefix_sample_cap) ||
        !sizeToU64(
            options.rescore_budget.prefix_sample_cap,
            rescore_prefix_sample_cap)) {
        result.decision = V3TerminalDecision::IncumbentOnly;
        result.selected_tree = std::move(incumbent_tree);
        return result;
    }
    result.telemetry.v3_lazy_prefix_sample_cap =
        lazy_prefix_sample_cap;
    result.telemetry.v3_rescore_prefix_sample_cap =
        rescore_prefix_sample_cap;

    try {
        if (!snapshot || !snapshot->receipt().complete() ||
            !incumbent_tree ||
            !incumbent_tree->isBoundToValidatedSnapshot(snapshot) ||
            !validateFixedPlan(*snapshot, incumbent_plan).valid ||
            incumbent_tree->executionPlanFingerprint() !=
                incumbent_plan.fingerprint ||
            incumbent_tree->minimumDuration() < 1) {
            result.decision =
                V3TerminalDecision::IncumbentEmptyOrDisconnected;
            result.selected_tree = std::move(incumbent_tree);
            return result;
        }
        result.telemetry.graph_validation_passes =
            snapshot->receipt().graph_validation_calls;
        result.telemetry.graph_validation_rescans = 0;
        result.telemetry.data_fingerprint_rescans = 0;
        result.telemetry.query_fingerprint_rescans = 0;
        if (result.telemetry.graph_validation_passes != 1 ||
            incumbent_tree->rootCandidates().empty()) {
            result.decision =
                V3TerminalDecision::IncumbentEmptyOrDisconnected;
            result.selected_tree = std::move(incumbent_tree);
            return result;
        }

        auto finishIncumbent = [&](V3TerminalDecision decision) {
            result.decision = decision;
            result.selected_plan_fingerprint = incumbent_plan.fingerprint;
            if (!result.global_memory_admission_attempted) {
                appendTrace(
                    result.deterministic_trace,
                    options.maximum_trace_entries,
                    traceToken(
                        kTraceDecision,
                        static_cast<std::uint64_t>(decision)),
                    result.telemetry);
            }
            if (!commitSelectedMetadata(
                    snapshot,
                    incumbent_tree,
                    incumbent_plan,
                    PlannerDecision::IncumbentOnly,
                    result.telemetry,
                    metadata_budget,
                    options,
                    result)) {
                result.decision = V3TerminalDecision::MetadataBudget;
                result.selected_tree = std::move(incumbent_tree);
                result.receipt.reset();
            }
        };

        if (!incumbent_tree->hasCorrectedPaperIncumbentOrigin(
                incumbent_plan)) {
            finishIncumbent(
                V3TerminalDecision::IncumbentOriginMismatch);
            return result;
        }

        const CandidateCover cover =
            incumbent_tree->exportCandidateCover();
        if (!cover.hasValidatedSnapshotAuthority() ||
            !cover.isBoundToValidatedSnapshot(snapshot)) {
            finishIncumbent(V3TerminalDecision::IncumbentOnly);
            return result;
        }
        result.telemetry.v3_cover_export_entries =
            cover.sourceRelationEntries();

        V3FixedPlanProposalResult proposals =
            generateV3FixedPlanProposals(
                snapshot,
                cover,
                incumbent_plan,
                options.proposal_budget,
                options.maximum_trace_entries);
        result.telemetry = proposals.telemetry;
        result.telemetry.cost_convention_id = kCostConventionId;
        result.telemetry.graph_validation_passes = 1;
        result.telemetry.v3_lazy_prefix_sample_cap =
            lazy_prefix_sample_cap;
        result.telemetry.v3_rescore_prefix_sample_cap =
            rescore_prefix_sample_cap;
        result.telemetry.v3_cover_export_entries =
            cover.sourceRelationEntries();
        result.deterministic_trace =
            std::move(proposals.deterministic_trace);
        // Do not trust a moved-from vector to have relinquished capacity on
        // every conforming implementation. Its owner is explicitly emptied;
        // the destination trace is the sole retained trace owner.
        std::vector<std::uint64_t>().swap(
            proposals.deterministic_trace);
        if (!proposals.complete() ||
            result.telemetry.v3_nonincumbent_plan_proposals == 0) {
            finishIncumbent(
                V3TerminalDecision::NoNonincumbentCompleteProposal);
            return result;
        }

        std::uint64_t metadata_upper = 0;
        const bool metadata_can_fit = metadataUpperBoundFits(
            static_cast<std::size_t>(snapshot->query().num_vertices),
            options,
            metadata_budget,
            metadata_upper);
        if (!metadata_can_fit) {
            finishIncumbent(V3TerminalDecision::MetadataBudget);
            return result;
        }
        std::vector<TopLazyCandidate> admitted;
        admitted.reserve(proposals.plans.size());
        bool quote_visit_budget_hit = false;
        const CandidateBuildBudget build_budget =
            candidate_build_ledger.remaining();
        for (const FixedPlan& plan : proposals.plans) {
            if (plan.fingerprint == incumbent_plan.fingerprint) continue;
            std::uint64_t plan_cover_visits = 0;
            for (std::size_t depth = 1;
                 depth < plan.order.size();
                 ++depth) {
                const int child = plan.order[depth];
                const int parent =
                    plan.parent[static_cast<std::size_t>(child)];
                std::uint64_t domain_size = 0;
                if (!sizeToU64(
                        cover.domain(
                            static_cast<std::size_t>(parent)).size(),
                        domain_size) ||
                    !checkedAccumulate(
                        plan_cover_visits, domain_size)) {
                    quote_visit_budget_hit = true;
                    break;
                }
            }
            std::uint64_t next_quote_visits = 0;
            if (quote_visit_budget_hit ||
                !checkedAdd(
                    result.telemetry.v3_quote_cover_value_visits,
                    plan_cover_visits,
                    next_quote_visits) ||
                next_quote_visits >
                    options.proposal_budget
                        .maximum_quote_cover_value_visits) {
                quote_visit_budget_hit = true;
                break;
            }
            result.telemetry.v3_quote_cover_value_visits =
                next_quote_visits;
            CandidateBuildQuote quote = quoteCoverAwareBuild(
                snapshot,
                cover,
                plan,
                options.candidate_build_accounting);
            const CandidateBuildBudgetFit fit =
                evaluateCandidateBuildBudget(quote, build_budget);
            if (!quote.complete ||
                !quote.materializerContractBound() ||
                !quote.validatedSnapshotBound() ||
                !quote.isBoundToValidatedSnapshot(snapshot) ||
                !fit.fits) {
                if (!checkedAccumulate(
                        result.telemetry.v3_build_infeasible_plan_quotes,
                        1)) {
                    finishIncumbent(
                        V3TerminalDecision::NoBuildFeasiblePlan);
                    return result;
                }
                continue;
            }
            if (!checkedAccumulate(
                    result.telemetry.v3_build_feasible_plan_quotes, 1)) {
                finishIncumbent(
                    V3TerminalDecision::NoBuildFeasiblePlan);
                return result;
            }
            TopLazyCandidate candidate;
            candidate.available = true;
            candidate.plan = plan;
            candidate.quote = std::move(quote);
            admitted.push_back(std::move(candidate));
        }
        if (admitted.empty()) {
            finishIncumbent(V3TerminalDecision::NoBuildFeasiblePlan);
            return result;
        }

        LazyTransitionOracle oracle(
            snapshot,
            cover,
            options.lazy_budget,
            result.telemetry,
            result.deterministic_trace,
            options.maximum_trace_entries);
        if (oracle.failure() != PilotFailure::None) {
            finishIncumbent(V3TerminalDecision::LazyTransitionBudget);
            return result;
        }
        LazyPilotProvider lazy_provider(oracle);
        TopLazyCandidate top;
        bool saw_lazy_budget = false;
        bool saw_sample_empty = false;
        for (TopLazyCandidate& candidate : admitted) {
            PilotScoreResult score = scoreFixedPlan(
                *snapshot,
                candidate.plan,
                incumbent_tree->minimumDuration(),
                options.lazy_budget.prefix_sample_cap,
                lazy_provider,
                result.telemetry
                    .v3_lazy_maximum_prefix_samples_observed,
                result.telemetry
                    .v3_maximum_prefix_samples_observed);
            if (!score.receipt.available) {
                saw_lazy_budget = saw_lazy_budget ||
                    score.failure == PilotFailure::Budget ||
                    score.failure == PilotFailure::Overflow;
                saw_sample_empty = saw_sample_empty ||
                    score.failure == PilotFailure::SampleEmpty;
                if (score.failure == PilotFailure::Budget ||
                    score.failure == PilotFailure::Overflow) {
                    break;
                }
                continue;
            }
            if (!checkedAccumulate(
                    result.telemetry.v3_scored_plan_proposals, 1)) {
                finishIncumbent(
                    V3TerminalDecision::LazyTransitionBudget);
                return result;
            }
            appendTrace(
                result.deterministic_trace,
                options.maximum_trace_entries,
                traceToken(
                    kTraceScore,
                    candidate.plan.fingerprint ^
                        score.receipt.objective),
                result.telemetry);
            if (!top.available ||
                scorePrecedes(score.receipt, top.score)) {
                top.available = true;
                top.plan = candidate.plan;
                top.quote = std::move(candidate.quote);
                retainScore(score.receipt, top.score);
            }
        }
        if (!top.available) {
            finishIncumbent(
                saw_lazy_budget
                    ? V3TerminalDecision::LazyTransitionBudget
                    : saw_sample_empty
                        ? V3TerminalDecision::SampleEmpty
                        : V3TerminalDecision::NoBuildFeasiblePlan);
            return result;
        }
        if (top.plan.fingerprint == incumbent_plan.fingerprint) {
            finishIncumbent(V3TerminalDecision::SamePlan);
            return result;
        }
        // The top lazy candidate is now the selected sealed quote.  Preserve
        // it even if the subsequent transactional reservation cannot be
        // acquired; otherwise a budget fallback would erase real quote work.
        result.candidate_build.quote_available = true;
        result.candidate_build.sealed_quote = projectQuoteWork(top.quote);
        appendTrace(
            result.deterministic_trace,
            options.maximum_trace_entries,
            traceToken(kTraceMaterialize, top.plan.fingerprint),
            result.telemetry);
        CandidateBuildReservationAttempt reservation;
        bool reservation_allocation_failed = false;
        if (options.global_memory_admission_enabled) {
            // Capture the last-live arithmetic first, then destroy all owning
            // proposal/admitted/lazy buffers. Their closed E components are
            // explicit zero-byte release witnesses, not an assumption based
            // on moved-from state or RSS.
            const PlanContainerCapacityDetailV1 proposal_last_live =
                proposalPlanCapacityDetailV1(proposals.plans);
            const PlanContainerCapacityDetailV1 admitted_last_live =
                admittedCandidateCapacityDetailV1(admitted);
            const LazyCacheCapacityDetailV1 lazy_last_live =
                oracle.releaseOwnedCapacity();
            std::vector<FixedPlan>().swap(proposals.plans);
            std::vector<TopLazyCandidate>().swap(admitted);
            const ReleasedPlanContainerCapacityReceiptV1 proposal_release =
                releasedPlanContainerCapacityReceiptV1(proposal_last_live);
            const ReleasedPlanContainerCapacityReceiptV1 admitted_release =
                releasedPlanContainerCapacityReceiptV1(admitted_last_live);
            const ReleasedLazyCacheCapacityReceiptV1 lazy_release =
                releasedLazyCacheCapacityReceiptV1(lazy_last_live);

            ExistingCapacityCaptureInputsV1 capture;
            capture.snapshot = snapshot;
            capture.incumbent_tree = incumbent_tree.get();
            capture.incumbent_plan = &incumbent_plan;
            capture.cover = &cover;
            capture.selected_candidate = &top;
            capture.proposal_plans = proposal_release;
            capture.admitted_candidates = admitted_release;
            capture.lazy_cache = lazy_release;
            capture.planner_trace = &result.deterministic_trace;
            capture.source_data_graph =
                options.source_data_graph_release;
            capture.source_query_graph =
                options.source_query_graph_release;
            capture.phase_generation =
                options.existing_capacity_phase_generation;
            capture.expected_accounting_model_sha256 =
                options.existing_capacity_accounting_model_sha256;
            result.existing_capacity =
                captureExistingCapacityReceiptV1(capture);
            result.global_memory_admission_attempted = true;
            result.global_memory_admission =
                evaluateV3GlobalMemoryAdmissionV1(
                    result.existing_capacity,
                    top.quote,
                    build_budget.payload_memory_bytes,
                    build_budget.fixed_memory_headroom_bytes,
                    options.process_memory_max_bytes);
            if (!result.existing_capacity.invariantHolds() ||
                !result.global_memory_admission.invariantHolds() ||
                !result.global_memory_admission.fits) {
                finishIncumbent(
                    V3TerminalDecision::GlobalMemoryAdmission);
                return result;
            }
            ExistingCapacityAdmissionSealV1 admission_seal(
                result.existing_capacity,
                result.global_memory_admission);
            try {
                reservation = tryReserveCandidateBuild(
                    top.quote, candidate_build_ledger, admission_seal);
            } catch (const std::bad_alloc&) {
                // The reservation factory allocates before either consuming
                // the admission seal or debiting the ledger. Preserve that
                // exact one-shot failure state as typed planner evidence.
                reservation.fit.fits = false;
                reservation.fit.reason =
                    CandidateBuildBudgetFitReason::GlobalMemoryAdmission;
                reservation_allocation_failed = true;
            }
            result.global_memory_admission_consumed =
                admission_seal.consumed();
        } else {
            reservation = tryReserveCandidateBuild(
                top.quote, candidate_build_ledger);
        }
        if (!reservation.fit.fits || !reservation.reservation) {
            finishIncumbent(
                options.global_memory_admission_enabled
                    ? reservation_allocation_failed
                        ? V3TerminalDecision::
                            CandidateReservationAllocationFailure
                        : V3TerminalDecision::GlobalMemoryAdmission
                    : V3TerminalDecision::CandidateBuildBudget);
            return result;
        }
        CoverAwareBuildResult built = buildCoverAwareTDTree(
            snapshot,
            cover,
            top.plan,
            std::move(reservation.reservation));
        result.telemetry.v3_partial_candidate_tree_handoffs =
            built.counters.partial_tree_handoffs;
        result.candidate_build.actual =
            projectActualWork(built.counters);
        result.candidate_build.partial_handoffs =
            built.counters.partial_tree_handoffs;
        result.candidate_build.quote_dominance_verified =
            workDominates(
                result.candidate_build.sealed_quote,
                result.candidate_build.actual);
        if (!built.complete()) {
            if (built.status == CoverAwareBuildStatus::QuoteViolation ||
                built.status ==
                    CoverAwareBuildStatus::CapacityContractViolation) {
                result.telemetry.v3_candidate_build_quote_violations =
                    built.counters.quote_violations == 0
                    ? 1
                    : built.counters.quote_violations;
                finishIncumbent(
                    V3TerminalDecision::CandidateBuildQuoteViolation);
            } else {
                finishIncumbent(
                    V3TerminalDecision::CandidateBuildBudget);
            }
            return result;
        }
        result.telemetry.v3_materialized_complete_plans = 1;
        if (!addBuildWork(
                built.counters,
                result.telemetry.v3_candidate_build_work)) {
            finishIncumbent(
                V3TerminalDecision::CandidateBuildQuoteViolation);
            return result;
        }
        std::unique_ptr<TDTree> candidate_tree =
            std::move(built.tree);

        TreePilotProvider incumbent_provider(
            *incumbent_tree, options.rescore_budget);
        TreePilotProvider candidate_provider(
            *candidate_tree, options.rescore_budget);
        PilotScoreResult incumbent_rescore = scoreFixedPlan(
            *snapshot,
            incumbent_plan,
            incumbent_tree->minimumDuration(),
            options.rescore_budget.prefix_sample_cap,
            incumbent_provider,
            result.telemetry
                .v3_rescore_maximum_prefix_samples_observed,
            result.telemetry
                .v3_maximum_prefix_samples_observed);
        PilotScoreResult candidate_rescore = scoreFixedPlan(
            *snapshot,
            top.plan,
            candidate_tree->minimumDuration(),
            options.rescore_budget.prefix_sample_cap,
            candidate_provider,
            result.telemetry
                .v3_rescore_maximum_prefix_samples_observed,
            result.telemetry
                .v3_maximum_prefix_samples_observed);
        if (!incumbent_rescore.receipt.available ||
            !candidate_rescore.receipt.available) {
            const bool sample_empty =
                incumbent_rescore.failure == PilotFailure::SampleEmpty ||
                candidate_rescore.failure == PilotFailure::SampleEmpty;
            finishIncumbent(
                sample_empty
                    ? V3TerminalDecision::SampleEmpty
                    : V3TerminalDecision::CandidateRescoreBudget);
            return result;
        }
        bool incumbent_work_valid = false;
        bool candidate_work_valid = false;
        result.telemetry.v3_incumbent_rescore_work =
            rescoreWorkTotal(
                incumbent_provider.counters(), incumbent_work_valid);
        result.telemetry.v3_candidate_rescore_work =
            rescoreWorkTotal(
                candidate_provider.counters(), candidate_work_valid);
        if (!incumbent_work_valid || !candidate_work_valid) {
            finishIncumbent(
                V3TerminalDecision::CandidateRescoreBudget);
            return result;
        }
        result.incumbent_score = incumbent_rescore.receipt;
        result.candidate_score = candidate_rescore.receipt;
        result.telemetry.v3_rescored_complete_plans = 1;
        if (!options.global_memory_admission_enabled) {
            appendTrace(
                result.deterministic_trace,
                options.maximum_trace_entries,
                traceToken(
                    kTraceScore,
                    result.incumbent_score.plan_fingerprint ^
                        result.incumbent_score.objective),
                result.telemetry);
            appendTrace(
                result.deterministic_trace,
                options.maximum_trace_entries,
                traceToken(
                    kTraceScore,
                    result.candidate_score.plan_fingerprint ^
                        result.candidate_score.objective),
                result.telemetry);
        }

        if (!incumbentCostTotal(
                result.telemetry,
                result.incumbent_score,
                result.telemetry.predicted_incumbent_total) ||
            !candidateCostTotal(
                result.telemetry,
                result.candidate_score,
                result.telemetry.predicted_candidate_total)) {
            finishIncumbent(
                V3TerminalDecision::CandidateRescoreBudget);
            return result;
        }
        const bool profitable =
            result.telemetry.predicted_candidate_total <=
            ninetyPercentFloor(
                result.telemetry.predicted_incumbent_total);
        if (options.profitability_gate_enabled && !profitable) {
            finishIncumbent(V3TerminalDecision::GainBelowMargin);
            return result;
        }

        result.decision = V3TerminalDecision::CandidateSelected;
        result.selected_plan_fingerprint = top.plan.fingerprint;
        if (!options.global_memory_admission_enabled) {
            appendTrace(
                result.deterministic_trace,
                options.maximum_trace_entries,
                traceToken(
                    kTraceDecision,
                    static_cast<std::uint64_t>(
                        V3TerminalDecision::CandidateSelected)),
                result.telemetry);
        }
        if (!commitSelectedMetadata(
                snapshot,
                candidate_tree,
                top.plan,
                PlannerDecision::CandidateSelected,
                result.telemetry,
                metadata_budget,
                options,
                result)) {
            finishIncumbent(V3TerminalDecision::MetadataBudget);
            return result;
        }
        incumbent_tree.reset();
        return result;
    } catch (const std::bad_alloc&) {
        result.decision = V3TerminalDecision::IncumbentOnly;
        result.selected_plan_fingerprint = incumbent_plan.fingerprint;
        result.selected_tree = std::move(incumbent_tree);
        result.receipt.reset();
        return result;
    } catch (...) {
        result.decision = V3TerminalDecision::IncumbentOnly;
        result.selected_plan_fingerprint = incumbent_plan.fingerprint;
        result.selected_tree = std::move(incumbent_tree);
        result.receipt.reset();
        return result;
    }
}
