#ifndef DIRECT_EVALUATOR_H
#define DIRECT_EVALUATOR_H

#include <cstddef>

#include "DagExecutor.h"

class AdaptiveDiagnostics;

// Reference execution backend for the same-stack DurableDFS ablation.
//
// This evaluator consumes the exact Graph, QueryPlan, TDTree, SearchState, and
// SearchStatisticsCollector used by GopherMatcher, but evaluates each
// structural and temporal extension directly.  It deliberately has no
// CandidateDag and no memo table.
class DirectEvaluator {
public:
    using CandidateSet = DagExecutor::CandidateSet;
    using CandidateSetPtr = DagExecutor::CandidateSetPtr;
    using IntervalSet = DagExecutor::IntervalSet;

    DirectEvaluator(
        const Graph& temporal_graph,
        const Graph& query_graph,
        const QueryPlan& plan,
        const TDTree& td_tree,
        int minimum_duration,
        SearchStatisticsCollector& statistics,
        DagPasses passes = {},
        OperatorCounters* ops = nullptr,
        AdaptiveDiagnostics* adaptive = nullptr);

    CandidateSetPtr structuralCandidates(
        int query_vertex,
        const SearchState& state) const;

    template <bool CollectStatistics>
    CandidateSetPtr structuralCandidatesSpecialized(
        int query_vertex,
        const SearchState& state) const;

    TemporalSupport temporalSupport(
        int query_vertex,
        int candidate,
        const SearchState& state) const;

    template <bool CollectStatistics>
    TemporalSupport temporalSupportSpecialized(
        int query_vertex,
        int candidate,
        const SearchState& state) const;

    constexpr std::size_t cacheMemoryUsage() const noexcept {
        return 0;
    }
    bool hasOptimizationPasses() const noexcept { return passes_.any(); }

#if defined(DSM_E3E4_CORE_ONLY)
    // Closed-source-profile proof hook: returns the exact reference retained
    // by the selected native backend, never a rebuilt or copied QueryPlan.
    [[nodiscard]] const QueryPlan&
    e3e4ExecutablePlanReference() const noexcept {
        return plan_;
    }
#endif

private:
    const Graph& G_;
    const Graph& Q_;
    const QueryPlan& plan_;
    const TDTree& td_tree_;
    int k_threshold_;
    SearchStatisticsCollector& statistics_;
    DagPasses passes_;
    OperatorCounters* ops_;
    AdaptiveDiagnostics* adaptive_;
    // Plain direct-loop recipes: no CandidateDag, node IDs, dependency graph,
    // interning or memo table. The same frozen estimate rule/kernel is shared
    // solely to make direct_opt a faithful non-DAG optimization control.
    struct Operand {
        DagOp operation;
        int first;
        int second;
        double estimate;
        std::string tie_break;
    };
    std::vector<std::vector<Operand>> structural_operands_;
    std::vector<std::vector<Operand>> temporal_operands_;
    CandidateSetPtr optimizedStructuralCandidates(
        int query_vertex, const SearchState& state) const;
    TemporalSupport optimizedTemporalSupport(
        int query_vertex, int candidate, const SearchState& state) const;
    void recordAdaptiveStructureInput(
        int query_vertex, const SearchState& state) const;

    int resolveBinding(
        int query_vertex,
        const SearchState& state,
        int overlay_query_vertex,
        int overlay_data_vertex) const;
};

// The ordinary statistics-OFF specializations are emitted only by the
// dedicated OFF object.  E3/E4 compiles it with CORE_ONLY still enabled plus
// DSM_GOPHER_OFF_TRANSLATION_UNIT.  Suppress accidental implicit
// instantiation in the remaining translation units.
extern template DirectEvaluator::CandidateSetPtr
DirectEvaluator::structuralCandidatesSpecialized<false>(
    int query_vertex,
    const SearchState& state) const;
extern template TemporalSupport
DirectEvaluator::temporalSupportSpecialized<false>(
    int query_vertex,
    int candidate,
    const SearchState& state) const;

#endif
