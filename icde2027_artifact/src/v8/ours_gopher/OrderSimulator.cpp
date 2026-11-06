#include "OrderSimulator.h"

#include <algorithm>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace {

void checkedAdd(std::uint64_t& destination, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - destination) {
        throw std::overflow_error("A2 order-simulation counter/support-width overflow");
    }
    destination += value;
}

void atomicAdd(
    std::atomic<std::uint64_t>& destination,
    std::uint64_t value,
    std::atomic<bool>& overflow) {
    std::uint64_t previous = destination.load(std::memory_order_relaxed);
    while (true) {
        const bool exceeds = value > std::numeric_limits<std::uint64_t>::max() - previous;
        const std::uint64_t next = exceeds
            ? std::numeric_limits<std::uint64_t>::max() : previous + value;
        if (destination.compare_exchange_weak(
                previous, next, std::memory_order_relaxed, std::memory_order_relaxed)) {
            if (exceeds) overflow.store(true, std::memory_order_relaxed);
            return;
        }
    }
}

bool unusedCandidate(int candidate, const SearchState& state) {
    return std::find(state.mapping.begin(), state.mapping.end(), candidate) == state.mapping.end();
}

} // namespace

const char* orderSimulationModeName(OrderSimulationMode mode) {
    switch (mode) {
    case OrderSimulationMode::Disabled: return "off";
    case OrderSimulationMode::Static: return "static";
    case OrderSimulationMode::DynMinCand: return "dyn-min-cand";
    case OrderSimulationMode::DynMinCandJ: return "dyn-min-cand-j";
    }
    throw std::invalid_argument("unknown order simulation mode");
}

bool parseOrderSimulationMode(const std::string& text, OrderSimulationMode& mode) {
    if (text == "off") mode = OrderSimulationMode::Disabled;
    else if (text == "static") mode = OrderSimulationMode::Static;
    else if (text == "dyn-min-cand") mode = OrderSimulationMode::DynMinCand;
    else if (text == "dyn-min-cand-j") mode = OrderSimulationMode::DynMinCandJ;
    else return false;
    return true;
}

OrderSimulator::OrderSimulator(
    const Graph& temporal_graph,
    const Graph& query_graph,
    const QueryPlan& plan,
    const TDTree& td_tree,
    int minimum_duration,
    OrderSimulationMode mode)
    : G_(temporal_graph), Q_(query_graph), plan_(plan), td_tree_(td_tree),
      k_threshold_(minimum_duration), mode_(mode) {
    const auto query_size = static_cast<std::size_t>(Q_.num_vertices);
    if (minimum_duration <= 0 || Q_.num_vertices <= 0 ||
        plan_.root < 0 || plan_.root >= Q_.num_vertices ||
        plan_.order.size() != query_size || plan_.parent.size() != query_size ||
        plan_.order.front() != plan_.root) {
        throw std::invalid_argument("A2 requires a complete connected rooted query plan and positive k");
    }
    if (mode_ == OrderSimulationMode::Disabled) {
        throw std::invalid_argument("do not construct an order simulator when its switch is off");
    }
    std::vector<int> rank(query_size, -1);
    for (std::size_t i = 0; i < plan_.order.size(); ++i) {
        const int q = plan_.order[i];
        if (q < 0 || q >= Q_.num_vertices || rank[static_cast<std::size_t>(q)] >= 0) {
            throw std::invalid_argument("A2 query order is not a permutation");
        }
        rank[static_cast<std::size_t>(q)] = static_cast<int>(i);
    }
    for (int q : plan_.order) {
        if (q == plan_.root) continue;
        const int parent = plan_.parent[static_cast<std::size_t>(q)];
        if (parent < 0 || parent >= Q_.num_vertices ||
            rank[static_cast<std::size_t>(parent)] >= rank[static_cast<std::size_t>(q)] ||
            (!GraphUtils::hasEdge(Q_.adj, parent, q) && !GraphUtils::hasEdge(Q_.adj, q, parent))) {
            throw std::invalid_argument("A2 requires the existing connected parent-before-child plan");
        }
    }
    // Explicit static simulation delegates to the historical Direct evaluator
    // and does not even prepare the additional dynamic-order domain copies.
    if (mode_ == OrderSimulationMode::Static) return;
    domains_.resize(query_size);
    tree_children_.resize(query_size);
    for (int q : plan_.order) {
        domains_[static_cast<std::size_t>(q)] = td_tree_.queryCandidateDomain(q);
        const int parent = plan_.parent[static_cast<std::size_t>(q)];
        if (q != plan_.root) tree_children_[static_cast<std::size_t>(parent)].push_back(q);
    }
}

bool OrderSimulator::inFrontier(int q, const SearchState& state) const {
    if (state.mapping[static_cast<std::size_t>(q)] >= 0) return false;
    for (const Edge& edge : Q_.adj[static_cast<std::size_t>(q)]) {
        if (edge.to != q && state.mapping[static_cast<std::size_t>(edge.to)] >= 0) return true;
    }
    for (const Edge& edge : Q_.in_adj[static_cast<std::size_t>(q)]) {
        if (edge.to != q && state.mapping[static_cast<std::size_t>(edge.to)] >= 0) return true;
    }
    return false;
}

std::vector<int> OrderSimulator::structuralCandidates(
    int q, const SearchState& state, OrderSimulationCounters& counters) const {
    const auto qi = static_cast<std::size_t>(q);
    const int parent = plan_.parent[qi];
    const bool parent_mapped = parent >= 0 && state.mapping[static_cast<std::size_t>(parent)] >= 0;
    const std::vector<int>* seed = nullptr;
    if (parent_mapped) {
        checkedAdd(counters.parent_block_seeds, 1);
        seed = td_tree_.candidatesForParent(q, state.mapping[static_cast<std::size_t>(parent)]);
    } else {
        checkedAdd(counters.union_domain_seeds, 1);
        seed = &domains_[qi];
    }
    std::vector<int> result;
    if (seed == nullptr) return result;
    result.reserve(seed->size());
    // Each examined seed candidate counts once, including the chosen vertex's
    // seed. This is ordering work, not a new logical search extension.
    checkedAdd(counters.ordering_candidate_evaluations, seed->size());
    for (int candidate : *seed) {
        bool valid = true;
        for (const Edge& edge : Q_.adj[qi]) {
            if (edge.to == q) continue;
            const int other = state.mapping[static_cast<std::size_t>(edge.to)];
            if (other >= 0 && !GraphUtils::hasEdge(G_.adj, candidate, other)) {
                valid = false;
                break;
            }
        }
        if (!valid) continue;
        for (const Edge& edge : Q_.in_adj[qi]) {
            if (edge.to == q) continue;
            const int other = state.mapping[static_cast<std::size_t>(edge.to)];
            if (other >= 0 && !GraphUtils::hasEdge(G_.adj, other, candidate)) {
                valid = false;
                break;
            }
        }
        if (!valid) continue;
        // A fixed TD-tree child may already be bound under a dynamic order.
        // Enforce that SAME stored binary relation in its reverse direction;
        // using only a data-graph edge would silently drop existing TD pruning.
        // Every complete answer survives the original TD necessary-condition
        // construction and semijoins, so neither this check nor the domain
        // union can remove an answer. No arbitrary-neighbor lookup is passed
        // to candidatesForParent: its parent key is always the fixed TD parent.
        for (int child : tree_children_[qi]) {
            const int mapped_child = state.mapping[static_cast<std::size_t>(child)];
            if (mapped_child < 0) continue;
            checkedAdd(counters.closed_td_child_checks, 1);
            const auto* child_block = td_tree_.candidatesForParent(child, candidate);
            if (child_block == nullptr || !std::binary_search(child_block->begin(), child_block->end(), mapped_child)) {
                valid = false;
                break;
            }
        }
        if (valid) result.push_back(candidate);
    }
    return result;
}

std::uint64_t OrderSimulator::supportWidth(
    int q,
    const std::vector<int>& candidates,
    const SearchState& state,
    const DirectEvaluator& evaluator,
    OrderSimulationCounters& counters) const {
    std::uint64_t width = 0;
    for (int candidate : candidates) {
        if (!unusedCandidate(candidate, state)) continue;
        checkedAdd(counters.ordering_temporal_evaluations, 1);
        const TemporalSupport support = evaluator.temporalSupport(q, candidate, state);
        if (!support.has_constraints || support.longest_run < k_threshold_ || support.intervals->empty()) continue;
        if (support.includes_cumulative_support) {
            throw std::logic_error("A2 requires unoptimized Direct temporal support");
        }
        const std::vector<TimeInterval> cumulative = state.has_active_intervals
            ? intersectTimeIntervals(state.active_intervals, *support.intervals, k_threshold_)
            : *support.intervals;
        for (const TimeInterval& interval : cumulative) {
            if (interval.end < interval.start) throw std::logic_error("invalid A2 active interval");
            const auto cardinality = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(interval.end) - static_cast<std::int64_t>(interval.start) + 1);
            checkedAdd(width, cardinality);
        }
    }
    return width;
}

OrderSelection OrderSimulator::select(
    std::size_t depth, const SearchState& state, const DirectEvaluator& evaluator) const {
    if (depth == 0 || depth >= plan_.order.size() || state.mapping.size() != plan_.order.size() ||
        state.mapping[static_cast<std::size_t>(plan_.root)] < 0 || evaluator.hasOptimizationPasses()) {
        throw std::invalid_argument("A2 selection requires an unoptimized Direct evaluator and mapped original root");
    }
    if (mode_ == OrderSimulationMode::Static) {
        const int q = plan_.order[depth];
        if (state.mapping[static_cast<std::size_t>(q)] >= 0) throw std::logic_error("A2 static vertex already mapped");
        return {q, evaluator.structuralCandidates(q, state)};
    }
    struct Choice {
        int query_vertex;
        std::vector<int> candidates;
        std::size_t injective_count;
    };
    OrderSimulationCounters counters;
    counters.decisions = 1;
    std::vector<Choice> choices;
    choices.reserve(plan_.order.size() - depth);
    std::size_t minimum = std::numeric_limits<std::size_t>::max();
    // Iteration is in original plan rank, providing the deterministic final
    // tie-break without depending on worker scheduling or hash-map ordering.
    for (int q : plan_.order) {
        if (!inFrontier(q, state)) continue;
        checkedAdd(counters.ordering_vertex_evaluations, 1);
        std::vector<int> candidates = structuralCandidates(q, state, counters);
        std::size_t injective_count = 0;
        for (int candidate : candidates) if (unusedCandidate(candidate, state)) ++injective_count;
        minimum = std::min(minimum, injective_count);
        choices.push_back({q, std::move(candidates), injective_count});
    }
    if (choices.empty()) {
        throw std::logic_error("empty A2 frontier in incomplete connected query; disconnected queries are unsupported");
    }
    std::size_t best = 0;
    while (choices[best].injective_count != minimum) ++best;
    if (mode_ == OrderSimulationMode::DynMinCandJ && minimum != 0) {
        std::size_t tied = 0;
        for (const Choice& choice : choices) if (choice.injective_count == minimum) ++tied;
        if (tied > 1) {
            std::uint64_t best_width = supportWidth(choices[best].query_vertex, choices[best].candidates, state, evaluator, counters);
            for (std::size_t i = best + 1; i < choices.size(); ++i) {
                if (choices[i].injective_count != minimum) continue;
                const std::uint64_t width = supportWidth(choices[i].query_vertex, choices[i].candidates, state, evaluator, counters);
                if (width < best_width) {
                    best = i;
                    best_width = width;
                }
            }
        }
    }
    // Timing is NOT an A2 metric. The selected structural vector is reused,
    // but support-width tie-break evaluation is deliberately diagnostic-only:
    // the caller repeats ordinary temporal evaluation in its original DFS.
    // Record that extra work separately rather than introducing a hidden
    // branch-state cache or changing the existing support semantics.
    accumulate(counters);
    return {choices[best].query_vertex,
            DagExecutor::CandidateSetPtr(std::move(choices[best].candidates))};
}

void OrderSimulator::accumulate(const OrderSimulationCounters& c) const {
    atomicAdd(decisions_, c.decisions, overflow_);
    atomicAdd(vertex_evaluations_, c.ordering_vertex_evaluations, overflow_);
    atomicAdd(candidate_evaluations_, c.ordering_candidate_evaluations, overflow_);
    atomicAdd(temporal_evaluations_, c.ordering_temporal_evaluations, overflow_);
    atomicAdd(parent_block_seeds_, c.parent_block_seeds, overflow_);
    atomicAdd(union_domain_seeds_, c.union_domain_seeds, overflow_);
    atomicAdd(closed_td_child_checks_, c.closed_td_child_checks, overflow_);
}

OrderSimulationCounters OrderSimulator::snapshot() const {
    OrderSimulationCounters c;
    c.actual_candidates = actual_candidates_.load(std::memory_order_relaxed);
    c.logical_extensions = logical_extensions_.load(std::memory_order_relaxed);
    c.decisions = decisions_.load(std::memory_order_relaxed);
    c.ordering_vertex_evaluations = vertex_evaluations_.load(std::memory_order_relaxed);
    c.ordering_candidate_evaluations = candidate_evaluations_.load(std::memory_order_relaxed);
    c.ordering_temporal_evaluations = temporal_evaluations_.load(std::memory_order_relaxed);
    c.parent_block_seeds = parent_block_seeds_.load(std::memory_order_relaxed);
    c.union_domain_seeds = union_domain_seeds_.load(std::memory_order_relaxed);
    c.closed_td_child_checks = closed_td_child_checks_.load(std::memory_order_relaxed);
    c.overflow = overflow_.load(std::memory_order_relaxed);
    return c;
}

void OrderSimulator::recordActualCandidates(std::uint64_t count) const {
    atomicAdd(actual_candidates_, count, overflow_);
}

void OrderSimulator::recordActualExtension() const {
    atomicAdd(logical_extensions_, 1, overflow_);
}

void OrderSimulationCounters::writeJson(std::ostream& output) const {
    output << '{'
           << "\"actual_candidates\":" << actual_candidates
           << ",\"logical_extensions\":" << logical_extensions
           << ",\"decisions\":" << decisions
           << ",\"ordering_vertex_evaluations\":" << ordering_vertex_evaluations
           << ",\"ordering_candidate_evaluations\":" << ordering_candidate_evaluations
           << ",\"ordering_temporal_evaluations\":" << ordering_temporal_evaluations
           << ",\"parent_block_seeds\":" << parent_block_seeds
           << ",\"union_domain_seeds\":" << union_domain_seeds
           << ",\"closed_td_child_checks\":" << closed_td_child_checks
           << ",\"overflow\":" << (overflow ? "true" : "false")
           << '}';
}
