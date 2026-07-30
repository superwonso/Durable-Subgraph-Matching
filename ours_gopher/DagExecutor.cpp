#include "DagExecutor.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t elapsedNanoseconds(Clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start).count());
}

Clock::time_point measurementStart(
    const SearchStatisticsCollector& statistics) {
    return statistics.enabled()
        ? Clock::now()
        : Clock::time_point{};
}

std::uint64_t measurementElapsed(
    const SearchStatisticsCollector& statistics,
    Clock::time_point start) {
    return statistics.enabled()
        ? elapsedNanoseconds(start)
        : 0;
}

MemoOptions withBudget(const MemoOptions& source, std::size_t budget) {
    MemoOptions result = source;
    result.budget_bytes = budget;
    result.max_entry_bytes = std::min(result.max_entry_bytes, budget);
    return result;
}

MemoOptions configuredMemoOptions(
    const DagExecutorOptions& options,
    std::size_t budget) {
    MemoOptions result =
        withBudget(options.memo_options, budget);
    result.collect_statistics =
        options.collect_statistics;
    return result;
}

std::size_t structuralBudget(
    const DagExecutorOptions& options) {
    switch (options.memo_mode) {
        case MemoMode::Structural:
            return options.memo_options.budget_bytes;
        case MemoMode::Dag:
            return options.memo_options.budget_bytes / 2;
        case MemoMode::None:
        case MemoMode::Temporal:
            return 0;
    }
    return 0;
}

std::size_t longestBudget(
    const DagExecutorOptions& options) {
    if (options.memo_mode != MemoMode::Temporal &&
        options.memo_mode != MemoMode::Dag) {
        return 0;
    }
    const std::size_t temporal_total =
        options.memo_mode == MemoMode::Dag
        ? options.memo_options.budget_bytes -
              options.memo_options.budget_bytes / 2
        : options.memo_options.budget_bytes;
    return temporal_total / 16;
}

std::size_t intervalBudget(
    const DagExecutorOptions& options) {
    if (options.memo_mode != MemoMode::Temporal &&
        options.memo_mode != MemoMode::Dag) {
        return 0;
    }
    const std::size_t temporal_total =
        options.memo_mode == MemoMode::Dag
        ? options.memo_options.budget_bytes -
              options.memo_options.budget_bytes / 2
        : options.memo_options.budget_bytes;
    return temporal_total - temporal_total / 16;
}

bool intervalsEqual(
    const std::vector<TimeInterval>& lhs,
    const std::vector<TimeInterval>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].start != rhs[i].start || lhs[i].end != rhs[i].end) {
            return false;
        }
    }
    return true;
}

} // namespace

const char* memoModeName(MemoMode mode) {
    switch (mode) {
        case MemoMode::None: return "none";
        case MemoMode::Structural: return "structural";
        case MemoMode::Temporal: return "temporal";
        case MemoMode::Dag: return "dag";
    }
    return "unknown";
}

bool parseMemoMode(const std::string& text, MemoMode& mode) {
    if (text == "none") {
        mode = MemoMode::None;
    } else if (text == "structural") {
        mode = MemoMode::Structural;
    } else if (text == "temporal") {
        mode = MemoMode::Temporal;
    } else if (text == "dag") {
        mode = MemoMode::Dag;
    } else {
        return false;
    }
    return true;
}

std::size_t RuntimeKeyHash::operator()(const RuntimeKey& key) const noexcept {
    std::uint64_t hash =
        key.static_fingerprint ^
        (static_cast<std::uint64_t>(key.node_id) *
         0x9e3779b97f4a7c15ULL);
    for (int binding : key.bindings) {
        const std::uint64_t value =
            static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(binding));
        hash ^= value + 0x9e3779b97f4a7c15ULL +
            (hash << 6U) + (hash >> 2U);
    }
    return static_cast<std::size_t>(hash);
}

DagExecutor::DagExecutor(
    const Graph& temporal_graph,
    const Graph& query_graph,
    const QueryPlan& plan,
    const CandidateDag& dag,
    const TDTree& td_tree,
    int minimum_duration,
    DagExecutorOptions options,
    SearchStatisticsCollector& statistics)
    : G_(temporal_graph),
      Q_(query_graph),
      plan_(plan),
      dag_(dag),
      td_tree_(td_tree),
      k_threshold_(minimum_duration),
      options_(std::move(options)),
      statistics_(statistics),
      structural_memo_(configuredMemoOptions(
          options_, structuralBudget(options_))),
      temporal_memo_(configuredMemoOptions(
          options_, intervalBudget(options_))),
      longest_memo_(configuredMemoOptions(
          options_, longestBudget(options_))) {}

bool DagExecutor::structuralMemoEnabled() const {
    return (options_.memo_mode == MemoMode::Structural ||
            options_.memo_mode == MemoMode::Dag) &&
        structuralBudget(options_) > 0 &&
        options_.memo_options.max_entry_bytes > 0;
}

bool DagExecutor::temporalMemoEnabled() const {
    return (options_.memo_mode == MemoMode::Temporal ||
            options_.memo_mode == MemoMode::Dag) &&
        intervalBudget(options_) > 0 &&
        options_.memo_options.max_entry_bytes > 0;
}

bool DagExecutor::longestMemoEnabled() const {
    return (options_.memo_mode == MemoMode::Temporal ||
            options_.memo_mode == MemoMode::Dag) &&
        longestBudget(options_) > 0 &&
        options_.memo_options.max_entry_bytes > 0;
}

int DagExecutor::resolveBinding(
    int query_vertex,
    const SearchState& state,
    int overlay_query_vertex,
    int overlay_data_vertex) const {
    if (query_vertex == overlay_query_vertex) return overlay_data_vertex;
    if (query_vertex < 0 ||
        static_cast<std::size_t>(query_vertex) >= state.mapping.size()) {
        return -1;
    }
    return state.mapping[static_cast<std::size_t>(query_vertex)];
}

RuntimeKey DagExecutor::makeRuntimeKey(
    const DagNode& node,
    const SearchState& state,
    int overlay_query_vertex,
    int overlay_data_vertex) const {
    RuntimeKey key;
    key.static_fingerprint = node.static_fingerprint;
    key.node_id = node.id;
    key.bindings.reserve(node.dependencies.size());
    for (int dependency : node.dependencies) {
        key.bindings.push_back(resolveBinding(
            dependency,
            state,
            overlay_query_vertex,
            overlay_data_vertex));
    }
    return key;
}

DagExecutor::CandidateSetPtr DagExecutor::evaluateCandidateNode(
    DagNodeId node_id,
    const SearchState& state) {
    const DagNode& node = dag_.node(node_id);
    if (node.result_kind != DagResultKind::CandidateSet) {
        throw std::logic_error("candidate DAG node has the wrong result kind");
    }

    switch (node.op) {
        case DagOp::TdBlockLookup:
            statistics_.add(
                statistics_.logical_td_block_lookups);
            break;
        case DagOp::OutNeighborLookup:
        case DagOp::InNeighborLookup:
            statistics_.add(
                statistics_.logical_neighbor_lookups);
            break;
        case DagOp::StructuralIntersection:
            statistics_.add(
                statistics_.logical_structural_intersections);
            break;
        default:
            throw std::logic_error("non-structural operation in candidate DAG");
    }

    const bool use_memo = structuralMemoEnabled();
    const bool cost_aware =
        options_.memo_options.cost_aware_admission;
    const std::size_t minimum_work =
        options_.memo_options.minimum_estimated_work;
    const CandidateSet* td_source = nullptr;
    const std::vector<Edge>* neighbor_source = nullptr;
    std::vector<CandidateSetPtr> parent_values;
    std::size_t estimated_work = 0;

    if (node.op == DagOp::TdBlockLookup) {
        const int parent_data_vertex = resolveBinding(
            node.parameter.second_query_vertex, state, -1, -1);
        td_source = td_tree_.candidatesForParent(
            node.parameter.first_query_vertex, parent_data_vertex);
        estimated_work =
            td_source == nullptr ? 0 : td_source->size();
    } else if (
        node.op == DagOp::OutNeighborLookup ||
        node.op == DagOp::InNeighborLookup) {
        const int anchor = resolveBinding(
            node.parameter.first_query_vertex, state, -1, -1);
        if (anchor >= 0 && anchor < G_.num_vertices) {
            neighbor_source =
                node.op == DagOp::OutNeighborLookup
                ? &G_.adj[static_cast<std::size_t>(anchor)]
                : &G_.in_adj[static_cast<std::size_t>(anchor)];
            estimated_work = neighbor_source->size();
        }
    } else {
        if (node.parents.empty()) {
            throw std::logic_error(
                "structural intersection has no operands");
        }
        if (use_memo && cost_aware) {
            parent_values.reserve(node.parents.size());
            for (DagNodeId parent : node.parents) {
                CandidateSetPtr value =
                    evaluateCandidateNode(parent, state);
                estimated_work += value->size();
                parent_values.push_back(std::move(value));
            }
        }
    }

    const bool memo_eligible =
        use_memo &&
        (!cost_aware || estimated_work >= minimum_work);
    if (use_memo && !memo_eligible) {
        structural_memo_.recordCostBypass();
    }
    RuntimeKey key;
    if (memo_eligible) {
        key = makeRuntimeKey(node, state, -1, -1);
        const auto lookup_start =
            measurementStart(statistics_);
        auto cached = structural_memo_.find(key);
        statistics_.add(
            statistics_.cache_lookup_nanoseconds,
            measurementElapsed(statistics_, lookup_start));
        if (cached) return CandidateSetPtr(std::move(cached));
    }

    CandidateSet result;
    std::uint64_t local_compute_ns = 0;
    if (node.op == DagOp::TdBlockLookup) {
        statistics_.add(
            statistics_.physical_td_block_lookups);
        const auto compute_start =
            measurementStart(statistics_);
        if (td_source != nullptr && !memo_eligible) {
            statistics_.add(
                statistics_.structural_compute_nanoseconds,
                measurementElapsed(statistics_, compute_start));
            // TD-tree blocks are immutable for the complete search.  A
            // non-memoized lookup can therefore expose the stable block
            // directly instead of allocating and copying it at every state.
            return CandidateSetPtr(td_source);
        }
        if (td_source != nullptr) result = *td_source;
        local_compute_ns +=
            measurementElapsed(statistics_, compute_start);
    } else if (
        node.op == DagOp::OutNeighborLookup ||
        node.op == DagOp::InNeighborLookup) {
        statistics_.add(
            statistics_.physical_neighbor_lookups);
        const auto compute_start =
            measurementStart(statistics_);
        if (neighbor_source != nullptr) {
            result.reserve(neighbor_source->size());
            for (const auto& edge : *neighbor_source) {
                result.push_back(edge.to);
            }
        }
        local_compute_ns +=
            measurementElapsed(statistics_, compute_start);
    } else {
        if (parent_values.empty()) {
            parent_values.reserve(node.parents.size());
            for (DagNodeId parent : node.parents) {
                parent_values.push_back(
                    evaluateCandidateNode(parent, state));
            }
        }
        result = *parent_values.front();
        for (std::size_t i = 1;
             i < parent_values.size();
             ++i) {
            if (result.empty()) break;
            const CandidateSetPtr& rhs = parent_values[i];
            const std::size_t input_size =
                result.size() + rhs->size();
            statistics_.add(
                statistics_.structural_input_elements,
                input_size);
            if (!(use_memo && cost_aware)) {
                estimated_work += input_size;
            }
            CandidateSet intersection;
            intersection.reserve(
                std::min(result.size(), rhs->size()));
            const auto compute_start =
                measurementStart(statistics_);
            std::set_intersection(
                result.begin(), result.end(),
                rhs->begin(), rhs->end(),
                std::back_inserter(intersection));
            local_compute_ns +=
                measurementElapsed(statistics_, compute_start);
            statistics_.add(
                statistics_.physical_structural_intersections);
            result.swap(intersection);
        }
    }
    statistics_.add(
        statistics_.structural_compute_nanoseconds,
        local_compute_ns);

    if (memo_eligible) {
        const std::size_t value_bytes =
            result.capacity() * sizeof(CandidateSet::value_type);
        const std::size_t result_size = result.size();
        auto immutable =
            std::make_shared<const CandidateSet>(std::move(result));
        immutable = structural_memo_.insertOrGet(
            key,
            immutable,
            value_bytes,
            std::max(estimated_work, result_size),
            key.bindings.capacity() * sizeof(int),
            result_size);
        return CandidateSetPtr(std::move(immutable));
    }
    return CandidateSetPtr(std::move(result));
}

DagExecutor::IntervalSetPtr DagExecutor::evaluateIntervalNode(
    DagNodeId node_id,
    int query_vertex,
    int candidate,
    const SearchState& state) {
    const DagNode& node = dag_.node(node_id);
    if (node.result_kind != DagResultKind::IntervalSet) {
        throw std::logic_error("temporal DAG node has the wrong result kind");
    }

    if (node.op == DagOp::EdgeTimeLookup) {
        statistics_.add(
            statistics_.logical_temporal_edge_lookups);
    } else if (node.op == DagOp::TemporalIntersection) {
        statistics_.add(
            statistics_.logical_temporal_intersections);
    } else {
        throw std::logic_error("non-temporal operation in interval DAG");
    }

    const bool use_memo = temporalMemoEnabled();
    const bool cost_aware =
        options_.memo_options.cost_aware_admission;
    const std::size_t minimum_work =
        options_.memo_options.minimum_estimated_work;
    const TemporalEdge* temporal_source = nullptr;
    std::vector<IntervalSetPtr> parent_values;
    std::size_t estimated_work = 0;

    if (node.op == DagOp::EdgeTimeLookup) {
        const int source = resolveBinding(
            node.parameter.first_query_vertex,
            state,
            query_vertex,
            candidate);
        const int target = resolveBinding(
            node.parameter.second_query_vertex,
            state,
            query_vertex,
            candidate);
        temporal_source = G_.findTemporalEdge(source, target);
        estimated_work = temporal_source == nullptr
            ? 0
            : temporal_source->active_intervals.size();
    } else {
        if (node.parents.empty()) {
            throw std::logic_error(
                "temporal intersection has no operands");
        }
        if (use_memo && cost_aware) {
            parent_values.reserve(node.parents.size());
            for (DagNodeId parent : node.parents) {
                IntervalSetPtr value = evaluateIntervalNode(
                    parent, query_vertex, candidate, state);
                estimated_work += value->size();
                parent_values.push_back(std::move(value));
            }
        }
    }

    const bool memo_eligible =
        use_memo &&
        (!cost_aware || estimated_work >= minimum_work);
    if (use_memo && !memo_eligible) {
        temporal_memo_.recordCostBypass();
    }
    RuntimeKey key;
    if (memo_eligible) {
        key = makeRuntimeKey(
            node, state, query_vertex, candidate);
        const auto lookup_start =
            measurementStart(statistics_);
        auto cached = temporal_memo_.find(key);
        statistics_.add(
            statistics_.cache_lookup_nanoseconds,
            measurementElapsed(statistics_, lookup_start));
        if (cached) return IntervalSetPtr(std::move(cached));
    }

    IntervalSet result;
    std::uint64_t local_compute_ns = 0;
    if (node.op == DagOp::EdgeTimeLookup) {
        statistics_.add(
            statistics_.physical_temporal_edge_lookups);
        const auto compute_start =
            measurementStart(statistics_);
        if (temporal_source != nullptr) {
            const bool all_intervals_durable =
                std::all_of(
                    temporal_source->active_intervals.begin(),
                    temporal_source->active_intervals.end(),
                    [this](const TimeInterval& interval) {
                        return interval.length() >= k_threshold_;
                    });
            if (!memo_eligible && all_intervals_durable) {
                statistics_.add(
                    statistics_.temporal_compute_nanoseconds,
                    measurementElapsed(statistics_, compute_start));
                // Temporal edges are immutable after loading.  If filtering
                // would not remove anything, retain a non-owning view rather
                // than constructing the same vector for every DAG visit.
                return IntervalSetPtr(
                    &temporal_source->active_intervals);
            }
            result.reserve(
                temporal_source->active_intervals.size());
            for (const auto& interval :
                 temporal_source->active_intervals) {
                if (interval.length() >= k_threshold_) {
                    result.push_back(interval);
                }
            }
        }
        local_compute_ns +=
            measurementElapsed(statistics_, compute_start);
    } else {
        if (parent_values.empty()) {
            parent_values.reserve(node.parents.size());
            for (DagNodeId parent : node.parents) {
                parent_values.push_back(evaluateIntervalNode(
                    parent, query_vertex, candidate, state));
            }
        }
        result = *parent_values.front();
        for (std::size_t i = 1;
             i < parent_values.size();
             ++i) {
            if (result.empty()) break;
            const IntervalSetPtr& rhs = parent_values[i];
            const std::size_t input_size = result.size() + rhs->size();
            statistics_.add(
                statistics_.temporal_input_intervals,
                input_size);
            if (!(use_memo && cost_aware)) {
                estimated_work += input_size;
            }

            const auto compute_start =
                measurementStart(statistics_);
            IntervalSet intersection = intersectTimeIntervals(
                result, *rhs, k_threshold_);
            local_compute_ns +=
                measurementElapsed(statistics_, compute_start);
            statistics_.add(
                statistics_.physical_temporal_intersections);
            result.swap(intersection);
        }
    }
    statistics_.add(
        statistics_.temporal_compute_nanoseconds,
        local_compute_ns);

    if (memo_eligible) {
        const std::size_t value_bytes =
            result.capacity() * sizeof(IntervalSet::value_type);
        const std::size_t result_size = result.size();
        auto immutable =
            std::make_shared<const IntervalSet>(std::move(result));
        immutable = temporal_memo_.insertOrGet(
            key,
            immutable,
            value_bytes,
            std::max(estimated_work, result_size),
            key.bindings.capacity() * sizeof(int),
            result_size);
        return IntervalSetPtr(std::move(immutable));
    }
    return IntervalSetPtr(std::move(result));
}

int DagExecutor::evaluateLongestNode(
    DagNodeId node_id,
    int query_vertex,
    int candidate,
    const SearchState& state,
    const IntervalSet& known_intervals) {
    const DagNode& node = dag_.node(node_id);
    if (node.op != DagOp::LongestConsecutiveRun ||
        node.result_kind != DagResultKind::Integer) {
        throw std::logic_error("invalid longest-run DAG node");
    }
    statistics_.add(statistics_.logical_longest_run);

    const bool memo_eligible =
        longestMemoEnabled() &&
        (!options_.memo_options.cost_aware_admission ||
         known_intervals.size() >=
             options_.memo_options.minimum_estimated_work);
    if (longestMemoEnabled() && !memo_eligible) {
        longest_memo_.recordCostBypass();
    }
    RuntimeKey key;
    if (memo_eligible) {
        key = makeRuntimeKey(node, state, query_vertex, candidate);
        const auto lookup_start =
            measurementStart(statistics_);
        std::shared_ptr<const int> cached = longest_memo_.find(key);
        statistics_.add(
            statistics_.cache_lookup_nanoseconds,
            measurementElapsed(statistics_, lookup_start));
        if (cached) return *cached;
    }

    statistics_.add(statistics_.physical_longest_run);
    const auto compute_start =
        measurementStart(statistics_);
    int longest = 0;
    for (const auto& interval : known_intervals) {
        longest = std::max(longest, interval.length());
    }
    statistics_.add(
        statistics_.longest_compute_nanoseconds,
        measurementElapsed(statistics_, compute_start));

    if (memo_eligible) {
        std::shared_ptr<const int> value =
            std::make_shared<const int>(longest);
        value = longest_memo_.insertOrGet(
            key,
            value,
            0,
            known_intervals.size(),
            key.bindings.capacity() * sizeof(int),
            1);
        return *value;
    }
    return longest;
}

DagExecutor::CandidateSetPtr DagExecutor::structuralCandidates(
    int query_vertex,
    const SearchState& state) {
    if (query_vertex < 0 ||
        static_cast<std::size_t>(query_vertex) >= dag_.structural_roots.size()) {
        throw std::out_of_range("query vertex for structural DAG");
    }
    const DagNodeId root =
        dag_.structural_roots[static_cast<std::size_t>(query_vertex)];
    if (root == kInvalidDagNode) {
        return CandidateSetPtr(CandidateSet{});
    }

    CandidateSetPtr result = evaluateCandidateNode(root, state);
    if (options_.verify_dag) {
        const CandidateSet reference = verifyStructural(query_vertex, state);
        if (*result != reference) {
            throw std::runtime_error(
                "DAG structural verification failed for q" +
                std::to_string(query_vertex));
        }
    }
    return result;
}

TemporalSupport DagExecutor::temporalSupport(
    int query_vertex,
    int candidate,
    const SearchState& state) {
    if (query_vertex < 0 ||
        static_cast<std::size_t>(query_vertex) >= dag_.temporal_roots.size()) {
        throw std::out_of_range("query vertex for temporal DAG");
    }
    const DagNodeId temporal_root =
        dag_.temporal_roots[static_cast<std::size_t>(query_vertex)];
    if (temporal_root == kInvalidDagNode) {
        return {IntervalSetPtr(IntervalSet{}), 0, false};
    }

    IntervalSetPtr intervals = evaluateIntervalNode(
        temporal_root, query_vertex, candidate, state);
    if (options_.verify_dag) {
        const IntervalSet reference =
            verifyTemporal(query_vertex, candidate, state);
        if (!intervalsEqual(*intervals, reference)) {
            throw std::runtime_error(
                "DAG temporal verification failed for q" +
                std::to_string(query_vertex));
        }
    }

    const DagNodeId longest_root =
        dag_.longest_run_roots[static_cast<std::size_t>(query_vertex)];
    const bool longest_memo_candidate =
        longestMemoEnabled() &&
        (!options_.memo_options.cost_aware_admission ||
         intervals->size() >=
             options_.memo_options.minimum_estimated_work);
    const bool need_exact_longest =
        statistics_.enabled() ||
        options_.verify_dag ||
        longest_memo_candidate;
    const int longest =
        longest_root == kInvalidDagNode
        ? 0
        : (need_exact_longest
              ? evaluateLongestNode(
                    longest_root,
                    query_vertex,
                    candidate,
                    state,
                    *intervals)
              // Interval DAG operators already discard every run shorter
              // than k.  Without diagnostics or an admissible memo entry,
              // non-emptiness is the only fact consumed by the matcher.
              : (intervals->empty() ? 0 : k_threshold_));
    return {std::move(intervals), longest, true};
}

DagExecutor::CandidateSet DagExecutor::verifyStructural(
    int query_vertex,
    const SearchState& state) const {
    CandidateSet result;
    const int parent =
        plan_.parent[static_cast<std::size_t>(query_vertex)];
    const int parent_candidate = resolveBinding(parent, state, -1, -1);
    const CandidateSet* block =
        td_tree_.candidatesForParent(query_vertex, parent_candidate);
    if (block == nullptr) return result;

    for (int candidate : *block) {
        bool valid = true;
        for (const auto& edge :
             Q_.adj[static_cast<std::size_t>(query_vertex)]) {
            if (edge.to == query_vertex) continue;
            const int other = resolveBinding(edge.to, state, -1, -1);
            if (other >= 0 &&
                !GraphUtils::hasEdge(G_.adj, candidate, other)) {
                valid = false;
                break;
            }
        }
        if (!valid) continue;
        for (const auto& edge :
             Q_.in_adj[static_cast<std::size_t>(query_vertex)]) {
            if (edge.to == query_vertex) continue;
            const int other = resolveBinding(edge.to, state, -1, -1);
            if (other >= 0 &&
                !GraphUtils::hasEdge(G_.adj, other, candidate)) {
                valid = false;
                break;
            }
        }
        if (valid) result.push_back(candidate);
    }
    return result;
}

DagExecutor::IntervalSet DagExecutor::verifyTemporal(
    int query_vertex,
    int candidate,
    const SearchState& state) const {
    IntervalSet result;
    bool have_result = false;
    auto apply_edge = [&](int source, int target) {
        const TemporalEdge* edge = G_.findTemporalEdge(source, target);
        if (edge == nullptr) {
            result.clear();
            have_result = true;
            return false;
        }
        if (!have_result) {
            for (const auto& interval : edge->active_intervals) {
                if (interval.length() >= k_threshold_) {
                    result.push_back(interval);
                }
            }
            have_result = true;
        } else {
            result = intersectTimeIntervals(
                result, edge->active_intervals, k_threshold_);
        }
        return !result.empty();
    };

    if (GraphUtils::hasEdge(Q_.adj, query_vertex, query_vertex) &&
        !apply_edge(candidate, candidate)) {
        return result;
    }
    for (const auto& edge :
         Q_.adj[static_cast<std::size_t>(query_vertex)]) {
        if (edge.to == query_vertex) continue;
        const int other = resolveBinding(edge.to, state, -1, -1);
        if (other >= 0 && !apply_edge(candidate, other)) return result;
    }
    for (const auto& edge :
         Q_.in_adj[static_cast<std::size_t>(query_vertex)]) {
        if (edge.to == query_vertex) continue;
        const int other = resolveBinding(edge.to, state, -1, -1);
        if (other >= 0 && !apply_edge(other, candidate)) return result;
    }
    return result;
}

void DagExecutor::augmentStatistics(SearchStatistics& statistics) const {
    const MemoSnapshot structural = structural_memo_.snapshot();
    const MemoSnapshot temporal = temporal_memo_.snapshot();
    const MemoSnapshot longest = longest_memo_.snapshot();

    statistics.structural_cache_hits = structural.hits;
    statistics.structural_cache_misses = structural.misses;
    statistics.structural_cache_admitted = structural.admitted;
    statistics.structural_cache_bypassed = structural.bypassed;
    statistics.structural_cache_bytes = structural.bytes;
    statistics.structural_cache_entries = structural.entries;
    statistics.structural_cache_duplicate_inserts =
        structural.duplicate_inserts;
    statistics.structural_cache_rejected_budget =
        structural.rejected_budget;
    statistics.structural_cache_rejected_entry_size =
        structural.rejected_entry_size;
    statistics.structural_cache_rejected_cost =
        structural.rejected_cost;
    statistics.structural_cache_hit_result_elements =
        structural.hit_value_elements;
    statistics.structural_cache_hit_saved_work =
        structural.hit_saved_work;
    statistics.structural_cache_max_hit_result_elements =
        structural.max_hit_value_elements;
    statistics.structural_cache_admitted_result_elements =
        structural.admitted_value_elements;
    statistics.structural_cache_admitted_estimated_work =
        structural.admitted_estimated_work;

    statistics.temporal_cache_hits = temporal.hits;
    statistics.temporal_cache_misses = temporal.misses;
    statistics.temporal_cache_admitted =
        temporal.admitted + longest.admitted;
    statistics.temporal_cache_bypassed =
        temporal.bypassed + longest.bypassed;
    statistics.temporal_cache_bytes =
        temporal.bytes + longest.bytes;
    statistics.temporal_cache_entries =
        temporal.entries + longest.entries;
    statistics.temporal_cache_duplicate_inserts =
        temporal.duplicate_inserts + longest.duplicate_inserts;
    statistics.temporal_cache_rejected_budget =
        temporal.rejected_budget + longest.rejected_budget;
    statistics.temporal_cache_rejected_entry_size =
        temporal.rejected_entry_size +
        longest.rejected_entry_size;
    statistics.temporal_cache_rejected_cost =
        temporal.rejected_cost + longest.rejected_cost;
    statistics.temporal_cache_hit_result_elements =
        temporal.hit_value_elements +
        longest.hit_value_elements;
    statistics.temporal_cache_hit_saved_work =
        temporal.hit_saved_work + longest.hit_saved_work;
    statistics.temporal_cache_max_hit_result_elements =
        std::max(
            temporal.max_hit_value_elements,
            longest.max_hit_value_elements);
    statistics.temporal_cache_admitted_result_elements =
        temporal.admitted_value_elements +
        longest.admitted_value_elements;
    statistics.temporal_cache_admitted_estimated_work =
        temporal.admitted_estimated_work +
        longest.admitted_estimated_work;
    statistics.longest_cache_hits = longest.hits;
    statistics.longest_cache_misses = longest.misses;
}

std::size_t DagExecutor::cacheMemoryUsage() const {
    const MemoSnapshot structural = structural_memo_.snapshot();
    const MemoSnapshot temporal = temporal_memo_.snapshot();
    const MemoSnapshot longest = longest_memo_.snapshot();
    return static_cast<std::size_t>(
        structural.bytes + temporal.bytes + longest.bytes);
}
