#include "DirectEvaluator.h"
#include "AdaptiveDiagnostics.h"
#include "CandidateDagPasses.h"
#include "OpKernels.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

template <bool CollectStatistics>
Clock::time_point measurementStart() {
    if constexpr (CollectStatistics) {
        return Clock::now();
    }
    return Clock::time_point{};
}

template <bool CollectStatistics>
std::uint64_t measurementElapsed(Clock::time_point start) {
    if constexpr (CollectStatistics) {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - start).count());
    }
    (void)start;
    return 0;
}

} // namespace

#if !defined(DSM_GOPHER_OFF_TRANSLATION_UNIT)
DirectEvaluator::DirectEvaluator(
    const Graph& temporal_graph,
    const Graph& query_graph,
    const QueryPlan& plan,
    const TDTree& td_tree,
    int minimum_duration,
    SearchStatisticsCollector& statistics,
    DagPasses passes,
    OperatorCounters* ops,
    AdaptiveDiagnostics* adaptive)
    : G_(temporal_graph),
      Q_(query_graph),
      plan_(plan),
      td_tree_(td_tree),
      k_threshold_(minimum_duration),
      statistics_(statistics),
      passes_(passes),
      ops_(ops),
      adaptive_(adaptive) {
    if (minimum_duration <= 0) {
        throw std::invalid_argument(
            "minimum duration must be positive");
    }
    if (passes_.physicalAny()) {
        throw std::invalid_argument("P4-P6 are compiled physical DAG passes, not Direct recipes");
    }
    if (!passes_.any()) return;
    if (statistics_.enabled()) {
        throw std::invalid_argument("direct_opt requires statistics off; use --ops for census");
    }
    // Pre-enumeration preparation. These estimates and recipes contain no
    // executable DAG, and their cost is not included in enumeration time.
    const DagCompileEstimates estimates = computeDagCompileEstimates(G_, Q_, td_tree_);
    structural_operands_.resize(static_cast<std::size_t>(Q_.num_vertices));
    temporal_operands_.resize(static_cast<std::size_t>(Q_.num_vertices));
    std::vector<int> position(static_cast<std::size_t>(Q_.num_vertices), -1);
    for (std::size_t i = 0; i < plan_.order.size(); ++i) position[static_cast<std::size_t>(plan_.order[i])] = static_cast<int>(i);
    auto add = [&](std::vector<Operand>& operands, DagOp op, int first, int second = -1) {
        operands.push_back({op, first, second, estimates.cardinality(op, first, second),
                            dagOperandCanonicalName(op, first, second)});
    };
    auto by_estimate = [](const Operand& lhs, const Operand& rhs) {
        return lhs.estimate != rhs.estimate ? lhs.estimate < rhs.estimate : lhs.tie_break < rhs.tie_break;
    };
    for (int u : plan_.order) {
        const auto ui = static_cast<std::size_t>(u);
        auto& structural = structural_operands_[ui];
        auto& temporal = temporal_operands_[ui];
        if (position[ui] > 0) add(structural, DagOp::TdBlockLookup, u, plan_.parent[ui]);
        if (GraphUtils::hasEdge(Q_.adj, u, u)) add(temporal, DagOp::EdgeTimeLookup, u, u);
        for (const auto& edge : Q_.adj[ui]) {
            const int x = edge.to;
            if (x == u || position[static_cast<std::size_t>(x)] >= position[ui]) continue;
            add(structural, DagOp::InNeighborLookup, x);
            add(temporal, DagOp::EdgeTimeLookup, u, x);
        }
        for (const auto& edge : Q_.in_adj[ui]) {
            const int x = edge.to;
            if (x == u || position[static_cast<std::size_t>(x)] >= position[ui]) continue;
            add(structural, DagOp::OutNeighborLookup, x);
            add(temporal, DagOp::EdgeTimeLookup, x, u);
        }
        if (passes_.p3) std::stable_sort(structural.begin(), structural.end(), by_estimate);
        if (passes_.p1 || passes_.p3) std::stable_sort(temporal.begin(), temporal.end(), by_estimate);
    }
}
#endif

#if defined(DSM_GOPHER_OFF_TRANSLATION_UNIT)
int DirectEvaluator::resolveBinding(
    int query_vertex,
    const SearchState& state,
    int overlay_query_vertex,
    int overlay_data_vertex) const {
    if (query_vertex == overlay_query_vertex) {
        return overlay_data_vertex;
    }
    if (query_vertex < 0 ||
        static_cast<std::size_t>(query_vertex) >= state.mapping.size()) {
        return -1;
    }
    return state.mapping[static_cast<std::size_t>(query_vertex)];
}
#endif

#if !defined(DSM_GOPHER_OFF_TRANSLATION_UNIT)
DirectEvaluator::CandidateSetPtr DirectEvaluator::structuralCandidates(
    int query_vertex,
    const SearchState& state) const {
#if defined(DSM_ERASE_SEARCH_STATISTICS)
    return structuralCandidatesSpecialized<false>(query_vertex, state);
#else
    return statistics_.enabled()
        ? structuralCandidatesSpecialized<true>(query_vertex, state)
        : structuralCandidatesSpecialized<false>(query_vertex, state);
#endif
}
#endif

template <bool CollectStatistics>
DirectEvaluator::CandidateSetPtr
DirectEvaluator::structuralCandidatesSpecialized(
    int query_vertex,
    const SearchState& state) const {
    if (query_vertex < 0 || query_vertex >= Q_.num_vertices) {
        throw std::out_of_range("query vertex for direct structural evaluation");
    }
    if (adaptive_ != nullptr && adaptive_->stateCountersEnabled()) {
        recordAdaptiveStructureInput(query_vertex, state);
    }
    if (passes_.p3) return optimizedStructuralCandidates(query_vertex, state);

    statistics_.addSpecialized<CollectStatistics>(
        statistics_.logical_td_block_lookups);
    statistics_.addSpecialized<CollectStatistics>(
        statistics_.physical_td_block_lookups);
    OPS_ADD(ops_, td_block_lookups_executed, 1);
    const auto compute_start = measurementStart<CollectStatistics>();

    const int parent =
        plan_.parent[static_cast<std::size_t>(query_vertex)];
    const int parent_candidate =
        resolveBinding(parent, state, -1, -1);
    const CandidateSet* block =
        td_tree_.candidatesForParent(query_vertex, parent_candidate);
    if (ops_ != nullptr) ops_->physical_profile.recordOperand(
        {StructuralProfile::Backend::Direct, -1, query_vertex},
        StructuralProfile::Operand::TdBlock, block == nullptr ? 0 : block->size());

    CandidateSet result;
    std::size_t constraint_count = 0;
    if (ops_ != nullptr) {
        for (const auto& edge : Q_.adj[static_cast<std::size_t>(query_vertex)]) {
            if (edge.to != query_vertex && resolveBinding(edge.to, state, -1, -1) >= 0) ++constraint_count;
        }
        for (const auto& edge : Q_.in_adj[static_cast<std::size_t>(query_vertex)]) {
            if (edge.to != query_vertex && resolveBinding(edge.to, state, -1, -1) >= 0) ++constraint_count;
        }
    }
    if (block != nullptr) {
        result.reserve(block->size());
        for (int candidate : *block) {
            bool valid = true;
            std::size_t checked_constraints = 0;
            for (const auto& edge :
                 Q_.adj[static_cast<std::size_t>(query_vertex)]) {
                if (edge.to == query_vertex) continue;
                const int other =
                    resolveBinding(edge.to, state, -1, -1);
                if (other < 0) continue;
                statistics_.addSpecialized<CollectStatistics>(
                    statistics_.logical_neighbor_lookups);
                statistics_.addSpecialized<CollectStatistics>(
                    statistics_.physical_neighbor_lookups);
                OPS_ADD(ops_, neighbor_lookups_executed, 1);
                if (ops_ != nullptr) {
                    ++checked_constraints;
                    ops_->physical_profile.recordDirectMembership(
                        {StructuralProfile::Backend::Direct, -1, query_vertex});
                }
                if (adaptive_ != nullptr && adaptive_->stateCountersEnabled()) adaptive_->recordDirectProbe(query_vertex);
                if (!GraphUtils::hasEdge(G_.adj, candidate, other)) {
                    valid = false;
                    break;
                }
            }
            if (!valid) {
                OPS_ADD(ops_, neighbor_lookups_skipped, constraint_count - checked_constraints);
                continue;
            }

            for (const auto& edge :
                 Q_.in_adj[static_cast<std::size_t>(query_vertex)]) {
                if (edge.to == query_vertex) continue;
                const int other =
                    resolveBinding(edge.to, state, -1, -1);
                if (other < 0) continue;
                statistics_.addSpecialized<CollectStatistics>(
                    statistics_.logical_neighbor_lookups);
                statistics_.addSpecialized<CollectStatistics>(
                    statistics_.physical_neighbor_lookups);
                OPS_ADD(ops_, neighbor_lookups_executed, 1);
                if (ops_ != nullptr) {
                    ++checked_constraints;
                    ops_->physical_profile.recordDirectMembership(
                        {StructuralProfile::Backend::Direct, -1, query_vertex});
                }
                if (adaptive_ != nullptr && adaptive_->stateCountersEnabled()) adaptive_->recordDirectProbe(query_vertex);
                if (!GraphUtils::hasEdge(G_.adj, other, candidate)) {
                    valid = false;
                    break;
                }
            }
            if (valid) result.push_back(candidate);
            else OPS_ADD(ops_, neighbor_lookups_skipped, constraint_count - checked_constraints);
        }
    }

    if (adaptive_ != nullptr && adaptive_->stateCountersEnabled()) {
        adaptive_->recordStructureOutput(query_vertex, -1, result.size());
    }
    // DirectDFS performs one fused filter over the TD block rather than
    // materializing a Candidate-DAG intersection tree.
    statistics_.addSpecialized<CollectStatistics>(
        statistics_.structural_input_elements,
        block == nullptr ? 0 : block->size());
    statistics_.addSpecialized<CollectStatistics>(
        statistics_.structural_compute_nanoseconds,
        measurementElapsed<CollectStatistics>(compute_start));
    if (ops_ != nullptr) ops_->physical_profile.recordDirectOutput(
        {StructuralProfile::Backend::Direct, -1, query_vertex}, result.size());
    return CandidateSetPtr(std::move(result));
}

#if !defined(DSM_GOPHER_OFF_TRANSLATION_UNIT)
TemporalSupport DirectEvaluator::temporalSupport(
    int query_vertex,
    int candidate,
    const SearchState& state) const {
#if defined(DSM_ERASE_SEARCH_STATISTICS)
    return temporalSupportSpecialized<false>(
        query_vertex, candidate, state);
#else
    return statistics_.enabled()
        ? temporalSupportSpecialized<true>(
              query_vertex, candidate, state)
        : temporalSupportSpecialized<false>(
              query_vertex, candidate, state);
#endif
}
#endif

template <bool CollectStatistics>
TemporalSupport DirectEvaluator::temporalSupportSpecialized(
    int query_vertex,
    int candidate,
    const SearchState& state) const {
    if (query_vertex < 0 || query_vertex >= Q_.num_vertices) {
        throw std::out_of_range("query vertex for direct temporal evaluation");
    }
    if (candidate < 0 || candidate >= G_.num_vertices) {
        throw std::out_of_range("candidate for direct temporal evaluation");
    }
    if (passes_.any()) return optimizedTemporalSupport(query_vertex, candidate, state);

    IntervalSet result;
    bool has_constraints = false;
    std::size_t constraint_count = 0;
    std::size_t consumed_constraints = 0;
    if (ops_ != nullptr) {
        if (GraphUtils::hasEdge(Q_.adj, query_vertex, query_vertex)) ++constraint_count;
        for (const auto& edge : Q_.adj[static_cast<std::size_t>(query_vertex)]) {
            if (edge.to != query_vertex && resolveBinding(edge.to, state, query_vertex, candidate) >= 0) ++constraint_count;
        }
        for (const auto& edge : Q_.in_adj[static_cast<std::size_t>(query_vertex)]) {
            if (edge.to != query_vertex && resolveBinding(edge.to, state, query_vertex, candidate) >= 0) ++constraint_count;
        }
    }
    auto record_remaining = [&]() {
        OPS_ADD(ops_, history_lookups_skipped, constraint_count - consumed_constraints);
        OPS_ADD(ops_, time_intersections_skipped, constraint_count - consumed_constraints);
        OPS_ADD(ops_, kappa_skipped, constraint_count - consumed_constraints);
    };

    auto apply_edge = [&](int source, int target) {
        has_constraints = true;
        if (ops_ != nullptr) ++consumed_constraints;
        statistics_.addSpecialized<CollectStatistics>(
            statistics_.logical_temporal_edge_lookups);
        statistics_.addSpecialized<CollectStatistics>(
            statistics_.physical_temporal_edge_lookups);
        OPS_ADD(ops_, history_lookups_executed, 1);
        const auto lookup_start = measurementStart<CollectStatistics>();
        const TemporalEdge* edge = G_.findTemporalEdge(source, target);
        statistics_.addSpecialized<CollectStatistics>(
            statistics_.temporal_compute_nanoseconds,
            measurementElapsed<CollectStatistics>(lookup_start));
        if (edge == nullptr) {
            result.clear();
            return false;
        }
        OPS_ADD(ops_, physical_history_payload_reads, 1);

        if (result.empty()) {
            OPS_ADD(ops_, kappa_executed, 1);
            OPS_ADD(ops_, time_input_elements, edge->activeIntervals().size());
            const auto filter_start =
                measurementStart<CollectStatistics>();
            result.reserve(edge->activeIntervals().size());
            for (const TimeInterval& interval : edge->activeIntervals()) {
                OPS_ADD(ops_, time_elements_read, 1);
                if (interval.length() >= k_threshold_) {
                    result.push_back(interval);
                }
            }
            statistics_.addSpecialized<CollectStatistics>(
                statistics_.temporal_input_intervals,
                edge->activeIntervals().size());
            statistics_.addSpecialized<CollectStatistics>(
                statistics_.temporal_compute_nanoseconds,
                measurementElapsed<CollectStatistics>(filter_start));
            return !result.empty();
        }

        statistics_.addSpecialized<CollectStatistics>(
            statistics_.logical_temporal_intersections);
        statistics_.addSpecialized<CollectStatistics>(
            statistics_.physical_temporal_intersections);
        statistics_.addSpecialized<CollectStatistics>(
            statistics_.temporal_input_intervals,
            result.size() + edge->activeIntervals().size());
        const auto intersection_start =
            measurementStart<CollectStatistics>();
        result = ops_ == nullptr
            ? intersectTimeIntervals(result, edge->activeIntervals(), k_threshold_)
            : OperatorKernels::intersectIntervals(result, edge->activeIntervals(), k_threshold_, false, ops_);
        statistics_.addSpecialized<CollectStatistics>(
            statistics_.temporal_compute_nanoseconds,
            measurementElapsed<CollectStatistics>(intersection_start));
        return !result.empty();
    };

    if (GraphUtils::hasEdge(Q_.adj, query_vertex, query_vertex) &&
        !apply_edge(candidate, candidate)) {
        record_remaining();
        return {DagExecutor::IntervalSetPtr(std::move(result)),
                0,
                has_constraints};
    }
    for (const auto& edge :
         Q_.adj[static_cast<std::size_t>(query_vertex)]) {
        if (edge.to == query_vertex) continue;
        const int other = resolveBinding(
            edge.to, state, query_vertex, candidate);
        if (other >= 0 && !apply_edge(candidate, other)) {
            record_remaining();
            return {DagExecutor::IntervalSetPtr(std::move(result)),
                    0,
                    has_constraints};
        }
    }
    for (const auto& edge :
         Q_.in_adj[static_cast<std::size_t>(query_vertex)]) {
        if (edge.to == query_vertex) continue;
        const int other = resolveBinding(
            edge.to, state, query_vertex, candidate);
        if (other >= 0 && !apply_edge(other, candidate)) {
            record_remaining();
            return {DagExecutor::IntervalSetPtr(std::move(result)),
                    0,
                    has_constraints};
        }
    }

    // All retained intervals already satisfy k.  The matcher only consumes
    // that threshold fact, so mirror the DAG executor's statistics-off fast
    // path and scan for the exact maximum only when diagnostics request it.
    int longest = result.empty() ? 0 : k_threshold_;
    if constexpr (CollectStatistics) {
        if (has_constraints) {
            statistics_.addSpecialized<true>(
                statistics_.logical_longest_run);
            statistics_.addSpecialized<true>(
                statistics_.physical_longest_run);
            const auto longest_start = measurementStart<true>();
            longest = 0;
            for (const TimeInterval& interval : result) {
                longest = std::max(longest, interval.length());
            }
            statistics_.addSpecialized<true>(
                statistics_.longest_compute_nanoseconds,
                measurementElapsed<true>(longest_start));
        }
    }
    return {DagExecutor::IntervalSetPtr(std::move(result)),
            longest,
            has_constraints};
}

#if defined(DSM_GOPHER_OFF_TRANSLATION_UNIT)
void DirectEvaluator::recordAdaptiveStructureInput(
    int query_vertex, const SearchState& state) const {
    const int parent = plan_.parent[static_cast<std::size_t>(query_vertex)];
    const auto* block = td_tree_.candidatesForParent(query_vertex, resolveBinding(parent, state, -1, -1));
    std::vector<std::size_t> neighbor_sizes;
    for (const Edge& edge : Q_.adj[static_cast<std::size_t>(query_vertex)]) {
        if (edge.to == query_vertex) continue;
        const int other = resolveBinding(edge.to, state, -1, -1);
        if (other >= 0) neighbor_sizes.push_back(G_.in_adj[static_cast<std::size_t>(other)].size());
    }
    for (const Edge& edge : Q_.in_adj[static_cast<std::size_t>(query_vertex)]) {
        if (edge.to == query_vertex) continue;
        const int other = resolveBinding(edge.to, state, -1, -1);
        if (other >= 0) neighbor_sizes.push_back(G_.adj[static_cast<std::size_t>(other)].size());
    }
    adaptive_->recordState(query_vertex, -1, block == nullptr ? 0 : block->size(), neighbor_sizes);
}

DirectEvaluator::CandidateSetPtr DirectEvaluator::optimizedStructuralCandidates(
    int query_vertex, const SearchState& state) const {
    const auto& operands = structural_operands_[static_cast<std::size_t>(query_vertex)];
    if (operands.empty()) {
        if (adaptive_ != nullptr && adaptive_->stateCountersEnabled()) adaptive_->recordStructureOutput(query_vertex, -1, 0);
        return CandidateSetPtr(CandidateSet{});
    }
    auto read = [&](const Operand& operand) -> CandidateSetPtr {
        if (operand.operation == DagOp::TdBlockLookup) {
            OPS_ADD(ops_, td_block_lookups_executed, 1);
            const int parent = resolveBinding(operand.second, state, -1, -1);
            const CandidateSet* block = td_tree_.candidatesForParent(operand.first, parent);
            if (ops_ != nullptr) ops_->physical_profile.recordOperand(
                {StructuralProfile::Backend::DirectOpt, -1, query_vertex},
                StructuralProfile::Operand::TdBlock, block == nullptr ? 0 : block->size());
            return block == nullptr ? CandidateSetPtr(CandidateSet{}) : CandidateSetPtr(block);
        }
        OPS_ADD(ops_, neighbor_lookups_executed, 1);
        const int anchor = resolveBinding(operand.first, state, -1, -1);
        CandidateSet result;
        if (anchor >= 0 && anchor < G_.num_vertices) {
            const auto& adjacency = operand.operation == DagOp::OutNeighborLookup ? G_.adj : G_.in_adj;
            const auto& edges = adjacency[static_cast<std::size_t>(anchor)];
            result.reserve(edges.size());
            const bool time_copy = adaptive_ != nullptr && adaptive_->neighborCopyTimerEnabled();
            if (time_copy) {
                adaptive_->copyNeighborIdsTimed(edges, result, query_vertex, -1);
            } else {
                for (const Edge& edge : edges) result.push_back(edge.to);
            }
        }
        if (adaptive_ != nullptr && adaptive_->stateCountersEnabled()) {
            adaptive_->recordNeighborCopy(query_vertex, -1, result.size(), sizeof(CandidateSet::value_type));
        }
        if (ops_ != nullptr) {
            const StructuralProfile::Context context{StructuralProfile::Backend::DirectOpt, -1, query_vertex};
            ops_->physical_profile.recordOperand(context,
                operand.operation == DagOp::OutNeighborLookup ? StructuralProfile::Operand::OutNeighbor : StructuralProfile::Operand::InNeighbor,
                result.size());
            ops_->physical_profile.recordCopy(context, StructuralProfile::Copy::NeighborMaterialization,
                result.size(), sizeof(CandidateSet::value_type));
        }
        return CandidateSetPtr(std::move(result));
    };
    CandidateSet result = *read(operands.front());
    if (ops_ != nullptr) ops_->physical_profile.recordCopy(
        {StructuralProfile::Backend::DirectOpt, -1, query_vertex},
        StructuralProfile::Copy::StructuralSeed, result.size(), sizeof(CandidateSet::value_type));
    for (std::size_t i = 1; i < operands.size(); ++i) {
        if (result.empty()) {
            OPS_ADD(ops_, structural_intersections_skipped, operands.size() - i);
            for (std::size_t j = i; j < operands.size(); ++j) {
                if (operands[j].operation == DagOp::TdBlockLookup) OPS_ADD(ops_, td_block_lookups_skipped, 1);
                else OPS_ADD(ops_, neighbor_lookups_skipped, 1);
            }
            break;
        }
        const CandidateSetPtr rhs = read(operands[i]);
        result = OperatorKernels::intersectCandidates(result, *rhs, true, ops_,
            {StructuralProfile::Backend::DirectOpt, -1, query_vertex});
    }
    if (adaptive_ != nullptr && adaptive_->stateCountersEnabled()) adaptive_->recordStructureOutput(query_vertex, -1, result.size());
    return CandidateSetPtr(std::move(result));
}

TemporalSupport DirectEvaluator::optimizedTemporalSupport(
    int query_vertex, int candidate, const SearchState& state) const {
    const auto& operands = temporal_operands_[static_cast<std::size_t>(query_vertex)];
    if (operands.empty()) return {DagExecutor::IntervalSetPtr(IntervalSet{}), 0, false, passes_.p1};
    IntervalSet result;
    bool seeded = passes_.p1;
    if (passes_.p1) {
        result = state.has_active_intervals ? state.active_intervals
            : IntervalSet{{std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}};
    }
    for (std::size_t i = 0; i < operands.size(); ++i) {
        if (seeded && result.empty()) {
            OPS_ADD(ops_, history_lookups_skipped, operands.size() - i);
            OPS_ADD(ops_, kappa_skipped, operands.size() - i);
            OPS_ADD(ops_, time_intersections_skipped, operands.size() - i);
            OPS_ADD(ops_, kappa_skipped, operands.size() - i);
            break;
        }
        const Operand& operand = operands[i];
        const int source = resolveBinding(operand.first, state, query_vertex, candidate);
        const int target = resolveBinding(operand.second, state, query_vertex, candidate);
        const TemporalEdge* edge = G_.findTemporalEdge(source, target);
        if (passes_.p2) {
            OPS_ADD(ops_, history_guard_checks, 1);
            if (edge == nullptr || edge->maximumConsecutiveRun() < k_threshold_) {
                OPS_ADD(ops_, history_guard_failures, 1);
                OPS_ADD(ops_, history_lookups_skipped, 1);
                OPS_ADD(ops_, kappa_skipped, 1);
                result.clear();
                seeded = true;
                continue;
            }
        }
        OPS_ADD(ops_, history_lookups_executed, 1);
        if (edge == nullptr) {
            result.clear(); seeded = true; continue;
        }
        OPS_ADD(ops_, physical_history_payload_reads, 1);
        const IntervalSet& history = edge->activeIntervals();
        IntervalSet normalized;
        const IntervalSet* value = &history;
        if (passes_.p1) {
            OPS_ADD(ops_, kappa_skipped, 1);
            OPS_ADD(ops_, time_input_elements, history.size());
        } else {
            normalized = OperatorKernels::kappa(history, k_threshold_, ops_);
            value = &normalized;
        }
        if (!seeded) { result = *value; seeded = true; }
        else {
            if (passes_.p1) OPS_ADD(ops_, kappa_skipped, 1);
            result = OperatorKernels::intersectIntervals(
                result, *value, passes_.p1 ? 1 : k_threshold_, passes_.p3, ops_);
        }
    }
    if (passes_.p1) result = OperatorKernels::kappa(result, k_threshold_, ops_);
    const int longest = result.empty() ? 0 : k_threshold_;
    return {DagExecutor::IntervalSetPtr(std::move(result)), longest, true, passes_.p1};
}

template DirectEvaluator::CandidateSetPtr
DirectEvaluator::structuralCandidatesSpecialized<false>(
    int query_vertex,
    const SearchState& state) const;
template TemporalSupport DirectEvaluator::temporalSupportSpecialized<false>(
    int query_vertex,
    int candidate,
    const SearchState& state) const;
#elif !defined(DSM_ERASE_SEARCH_STATISTICS)
template DirectEvaluator::CandidateSetPtr
DirectEvaluator::structuralCandidatesSpecialized<true>(
    int query_vertex,
    const SearchState& state) const;
template TemporalSupport DirectEvaluator::temporalSupportSpecialized<true>(
    int query_vertex,
    int candidate,
    const SearchState& state) const;
#endif
