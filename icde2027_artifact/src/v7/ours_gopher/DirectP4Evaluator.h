#ifndef DSM_DIRECT_P4_EVALUATOR_H
#define DSM_DIRECT_P4_EVALUATOR_H

#include <cstddef>
#include <vector>

#include "DagExecutor.h"
#include "PhysicalCandidateBatch.h"
#include "BIndexExecution.h"

// Non-DAG fixed-P4 control. The shared types above are only state/result
// storage: this class never creates a CandidateDag, DagNode, intern table,
// dependency edge, memo cache, or interpreted node-dispatch loop.
class DirectP4Evaluator {
public:
    DirectP4Evaluator(const Graph& temporal_graph, const Graph& query_graph,
                      const QueryPlan& plan, const TDTree& td_tree,
                      int minimum_duration, OperatorCounters* ops = nullptr,
                      bool window_mode = false);

    PhysicalCandidateBatch physicalCandidates(
        int query_vertex, const SearchState& state) const;
    PhysicalCandidateBatch physicalCandidates(
        int query_vertex, const SearchState& state, JIndexExecution& index,
        window_algebra::WindowView prior) const;
    PhysicalCandidateBatch physicalCandidates(
        int query_vertex, const SearchState& state, BIndexExecution& index,
        BIndexExecution::WorkerScratch& scratch, window_algebra::WindowView prior) const;
    void registerIndexDrivers(JIndexExecution& index) const;

    // Called only after the matcher's injectivity test. This returns inner
    // kappa(S), never kappa(J intersect S); the existing matcher owns outer J.
    TemporalSupport temporalSupport(
        int query_vertex, int candidate, const SearchState& state,
        const ProbeHistoryRow* row) const;

    constexpr std::size_t cacheMemoryUsage() const noexcept { return 0; }

private:
    struct DirectedSlot {
        int from = -1;
        int to = -1;
    };
    struct Probe {
        int anchor = -1;
        bool outgoing = false;  // mapped anchor -> candidate
        std::size_t slot = 0;
    };
    struct Recipe {
        std::vector<DirectedSlot> slots;  // numeric (from,to) order
        std::vector<Probe> probes;       // P4 canonical adjacency order
        std::vector<std::size_t> temporal_order;  // P0 EDGE_TIME lexical order
        std::size_t self_slot = static_cast<std::size_t>(-1);
    };

    using Intervals = std::vector<TimeInterval>;
    using IntervalValue = DagValue<Intervals>;
    const Graph& G_;
    const QueryPlan& plan_;
    const TDTree& td_tree_;
    int k_threshold_;
    OperatorCounters* ops_;
    bool window_mode_;
    std::vector<Recipe> recipes_;

    static int binding(int query_vertex, const SearchState& state,
                       int overlay_query_vertex = -1, int overlay_candidate = -1);
    const Recipe& recipe(int query_vertex) const;
    IntervalValue readDurableHistory(
        const DirectedSlot& slot, std::size_t slot_index,
        int query_vertex, int candidate, const SearchState& state,
        const ProbeHistoryRow* row) const;
};

#endif
