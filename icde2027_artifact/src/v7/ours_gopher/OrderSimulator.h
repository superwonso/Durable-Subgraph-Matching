#ifndef ORDER_SIMULATOR_H
#define ORDER_SIMULATOR_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "DirectEvaluator.h"

// A2 diagnostic only. These modes neither compile a dynamic DAG nor enable
// the gated Stage-C dynamic-order execution arm. Disabled is the default.
enum class OrderSimulationMode {
    Disabled,
    Static,
    DynMinCand,
    DynMinCandJ
};

const char* orderSimulationModeName(OrderSimulationMode mode);
bool parseOrderSimulationMode(const std::string& text, OrderSimulationMode& mode);

struct OrderSimulationCounters {
    std::uint64_t actual_candidates = 0;
    std::uint64_t logical_extensions = 0;
    std::uint64_t decisions = 0;
    std::uint64_t ordering_vertex_evaluations = 0;
    std::uint64_t ordering_candidate_evaluations = 0;
    std::uint64_t ordering_temporal_evaluations = 0;
    std::uint64_t parent_block_seeds = 0;
    std::uint64_t union_domain_seeds = 0;
    std::uint64_t closed_td_child_checks = 0;
    bool overflow = false;
    void writeJson(std::ostream& output) const;
};

using OrderSimulationSnapshot = OrderSimulationCounters;

struct OrderSelection {
    int query_vertex = -1;
    // Structural candidates BEFORE injectivity, sorted by internal data ID.
    // The caller retains the original injectivity/temporal/J/extension loop.
    DagExecutor::CandidateSetPtr candidates;
};

class OrderSimulator {
public:
    OrderSimulator(
        const Graph& temporal_graph,
        const Graph& query_graph,
        const QueryPlan& plan,
        const TDTree& td_tree,
        int minimum_duration,
        OrderSimulationMode mode);

    // All branch-specific data are local to this call. Immutable domains may
    // be shared by root-scheduled workers; only diagnostic counters mutate.
    OrderSelection select(
        std::size_t depth,
        const SearchState& state,
        const DirectEvaluator& evaluator) const;

    OrderSimulationCounters snapshot() const;
    OrderSimulationMode mode() const noexcept { return mode_; }
    // Actual DFS work, separately from all frontier-scoring evaluations.
    // Root candidates are not extensions, matching the historical counter.
    void recordActualCandidates(std::uint64_t count) const;
    void recordActualExtension() const;

private:
    const Graph& G_;
    const Graph& Q_;
    const QueryPlan& plan_;
    const TDTree& td_tree_;
    int k_threshold_;
    OrderSimulationMode mode_;
    std::vector<std::vector<int>> domains_;
    std::vector<std::vector<int>> tree_children_;

    mutable std::atomic<std::uint64_t> decisions_{0};
    mutable std::atomic<std::uint64_t> actual_candidates_{0};
    mutable std::atomic<std::uint64_t> logical_extensions_{0};
    mutable std::atomic<std::uint64_t> vertex_evaluations_{0};
    mutable std::atomic<std::uint64_t> candidate_evaluations_{0};
    mutable std::atomic<std::uint64_t> temporal_evaluations_{0};
    mutable std::atomic<std::uint64_t> parent_block_seeds_{0};
    mutable std::atomic<std::uint64_t> union_domain_seeds_{0};
    mutable std::atomic<std::uint64_t> closed_td_child_checks_{0};
    mutable std::atomic<bool> overflow_{false};

    bool inFrontier(int query_vertex, const SearchState& state) const;
    std::vector<int> structuralCandidates(
        int query_vertex,
        const SearchState& state,
        OrderSimulationCounters& counters) const;
    std::uint64_t supportWidth(
        int query_vertex,
        const std::vector<int>& candidates,
        const SearchState& state,
        const DirectEvaluator& evaluator,
        OrderSimulationCounters& counters) const;
    void accumulate(const OrderSimulationCounters& counters) const;
};

#endif // ORDER_SIMULATOR_H
