#ifndef DAG_EXECUTOR_H
#define DAG_EXECUTOR_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "BoundedMemo.h"
#include "CandidateDag.h"
#include "SearchStatistics.h"
#include "TDTree.h"
#include "Utils.h"

enum class MemoMode {
    None,
    Structural,
    Temporal,
    Dag
};

const char* memoModeName(MemoMode mode);
bool parseMemoMode(const std::string& text, MemoMode& mode);

struct RuntimeKey {
    std::uint64_t static_fingerprint = 0;
    DagNodeId node_id = kInvalidDagNode;
    std::vector<int> bindings;

    bool operator==(const RuntimeKey& other) const {
        return static_fingerprint == other.static_fingerprint &&
            node_id == other.node_id &&
            bindings == other.bindings;
    }
};

struct RuntimeKeyHash {
    std::size_t operator()(const RuntimeKey& key) const noexcept;
};

struct SearchState {
    std::vector<int> mapping;
    std::vector<TimeInterval> active_intervals;
    bool has_active_intervals = false;
};

struct DagExecutorOptions {
    MemoMode memo_mode = MemoMode::Dag;
    MemoOptions memo_options;
    bool verify_dag = false;
    bool collect_statistics = true;
};

template <typename Value>
class DagValue {
public:
    DagValue() = default;
    explicit DagValue(Value value)
        : local_(std::move(value)) {}
    explicit DagValue(std::shared_ptr<const Value> shared)
        : shared_(std::move(shared)) {}
    explicit DagValue(const Value* borrowed)
        : borrowed_(borrowed) {}

    const Value& get() const {
        if (shared_) return *shared_;
        return borrowed_ ? *borrowed_ : local_;
    }
    const Value& operator*() const { return get(); }
    const Value* operator->() const { return &get(); }

private:
    std::shared_ptr<const Value> shared_;
    const Value* borrowed_ = nullptr;
    Value local_;
};

struct TemporalSupport {
    DagValue<std::vector<TimeInterval>> intervals;
    int longest_run = 0;
    bool has_constraints = false;
};

class DagExecutor {
public:
    using CandidateSet = std::vector<int>;
    using IntervalSet = std::vector<TimeInterval>;
    using CandidateSetPtr = DagValue<CandidateSet>;
    using IntervalSetPtr = DagValue<IntervalSet>;

    DagExecutor(
        const Graph& temporal_graph,
        const Graph& query_graph,
        const QueryPlan& plan,
        const CandidateDag& dag,
        const TDTree& td_tree,
        int minimum_duration,
        DagExecutorOptions options,
        SearchStatisticsCollector& statistics);

    CandidateSetPtr structuralCandidates(
        int query_vertex,
        const SearchState& state);

    TemporalSupport temporalSupport(
        int query_vertex,
        int candidate,
        const SearchState& state);

    void augmentStatistics(SearchStatistics& statistics) const;
    std::size_t cacheMemoryUsage() const;

private:
    const Graph& G_;
    const Graph& Q_;
    const QueryPlan& plan_;
    const CandidateDag& dag_;
    const TDTree& td_tree_;
    int k_threshold_;
    DagExecutorOptions options_;
    SearchStatisticsCollector& statistics_;

    BoundedMemo<RuntimeKey, CandidateSet, RuntimeKeyHash> structural_memo_;
    BoundedMemo<RuntimeKey, IntervalSet, RuntimeKeyHash> temporal_memo_;
    BoundedMemo<RuntimeKey, int, RuntimeKeyHash> longest_memo_;

    bool structuralMemoEnabled() const;
    bool temporalMemoEnabled() const;
    bool longestMemoEnabled() const;

    int resolveBinding(
        int query_vertex,
        const SearchState& state,
        int overlay_query_vertex,
        int overlay_data_vertex) const;

    RuntimeKey makeRuntimeKey(
        const DagNode& node,
        const SearchState& state,
        int overlay_query_vertex,
        int overlay_data_vertex) const;

    CandidateSetPtr evaluateCandidateNode(
        DagNodeId node_id,
        const SearchState& state);

    IntervalSetPtr evaluateIntervalNode(
        DagNodeId node_id,
        int query_vertex,
        int candidate,
        const SearchState& state);

    int evaluateLongestNode(
        DagNodeId node_id,
        int query_vertex,
        int candidate,
        const SearchState& state,
        const IntervalSet& known_intervals);

    CandidateSet verifyStructural(
        int query_vertex,
        const SearchState& state) const;

    IntervalSet verifyTemporal(
        int query_vertex,
        int candidate,
        const SearchState& state) const;
};

#endif
