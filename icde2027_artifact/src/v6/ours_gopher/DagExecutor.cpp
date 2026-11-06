#include "DagExecutor.h"
#include "OpKernels.h"
#include "PhysicalKernels.h"
#include "JIndexExecution.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t elapsedNanoseconds(Clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start).count());
}

template <bool Measure>
Clock::time_point measurementStart() {
    if constexpr (Measure) {
        return Clock::now();
    }
    return Clock::time_point{};
}

template <bool Measure>
std::uint64_t measurementElapsed(Clock::time_point start) {
    if constexpr (Measure) {
        return elapsedNanoseconds(start);
    }
    (void)start;
    return 0;
}

#if !defined(DSM_GOPHER_OFF_TRANSLATION_UNIT)
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
#endif

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

#if defined(DSM_GOPHER_OFF_TRANSLATION_UNIT)
int resolveRuntimeBinding(
    int query_vertex,
    const SearchState& state,
    int overlay_query_vertex,
    int overlay_data_vertex) {
    if (query_vertex == overlay_query_vertex) return overlay_data_vertex;
    if (query_vertex < 0 ||
        static_cast<std::size_t>(query_vertex) >= state.mapping.size()) {
        return -1;
    }
    return state.mapping[static_cast<std::size_t>(query_vertex)];
}
#endif

} // namespace

#if !defined(DSM_GOPHER_OFF_TRANSLATION_UNIT)
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

const char* memoPolicyName(MemoPolicy policy) {
    switch (policy) {
        case MemoPolicy::Legacy: return "legacy";
        case MemoPolicy::StructuralV2: return "structural-v2";
    }
    return "unknown";
}

bool parseMemoPolicy(const std::string& text, MemoPolicy& policy) {
    if (text == "legacy") {
        policy = MemoPolicy::Legacy;
    } else if (text == "structural-v2") {
        policy = MemoPolicy::StructuralV2;
    } else {
        return false;
    }
    return true;
}

const char* runtimeKeyModeName(RuntimeKeyMode mode) {
    switch (mode) {
        case RuntimeKeyMode::FullPrefix: return "full-prefix";
        case RuntimeKeyMode::RequiredBindings: return "required-bindings";
    }
    return "unknown";
}

bool parseRuntimeKeyMode(
    const std::string& text,
    RuntimeKeyMode& mode) {
    if (text == "full-prefix") {
        mode = RuntimeKeyMode::FullPrefix;
    } else if (text == "required-bindings") {
        mode = RuntimeKeyMode::RequiredBindings;
    } else {
        return false;
    }
    return true;
}

const char* runtimeKeyDiagnosticsModeName(
    RuntimeKeyDiagnosticsMode mode) {
    switch (mode) {
        case RuntimeKeyDiagnosticsMode::Off: return "off";
        case RuntimeKeyDiagnosticsMode::Exact: return "exact";
    }
    return "unknown";
}

bool parseRuntimeKeyDiagnosticsMode(
    const std::string& text,
    RuntimeKeyDiagnosticsMode& mode) {
    if (text == "off") {
        mode = RuntimeKeyDiagnosticsMode::Off;
    } else if (text == "exact") {
        mode = RuntimeKeyDiagnosticsMode::Exact;
    } else {
        return false;
    }
    return true;
}
#endif

#if defined(DSM_GOPHER_OFF_TRANSLATION_UNIT)
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

RuntimeKey buildRuntimeKey(
    RuntimeKeyMode mode,
    const DagNode& node,
    const SearchState& state,
    int overlay_query_vertex,
    int overlay_data_vertex) {
    RuntimeKey key;
    key.static_fingerprint = node.static_fingerprint;
    key.node_id = node.id;

    if (mode == RuntimeKeyMode::FullPrefix) {
        if (overlay_query_vertex >= 0 &&
            static_cast<std::size_t>(overlay_query_vertex) >=
                state.mapping.size()) {
            throw std::out_of_range(
                "candidate overlay is outside the positional query mapping");
        }
        // Keeping one slot per query vertex makes the identity positional.
        // The -1 sentinel preserves which positions are not bound yet, while
        // the candidate overlay supplies the temporary binding used during
        // candidate-specific temporal evaluation.
        key.bindings.reserve(state.mapping.size());
        for (std::size_t position = 0;
             position < state.mapping.size();
             ++position) {
            key.bindings.push_back(resolveRuntimeBinding(
                static_cast<int>(position),
                state,
                overlay_query_vertex,
                overlay_data_vertex));
        }
        return key;
    }

    key.bindings.reserve(node.dependencies.size());
    for (int dependency : node.dependencies) {
        key.bindings.push_back(resolveRuntimeBinding(
            dependency,
            state,
            overlay_query_vertex,
            overlay_data_vertex));
    }
    return key;
}
#endif

class DagExecutor::ExactRuntimeKeyCollector {
public:
    explicit ExactRuntimeKeyCollector(std::uint64_t cap)
        : cap_(cap) {}

    void record(
        DagOp op,
        const RuntimeKey& key,
        std::uint64_t construction_nanoseconds) {
        const std::size_t operation = dagOpIndex(op);
        if (operation >= kDagOpCount) {
            throw std::logic_error(
                "runtime-key diagnostic received an invalid DAG operation");
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto& keys = keys_by_op_[operation];
        const bool is_new = keys.find(key) == keys.end();
        if (is_new && distinct_keys_ >= cap_) {
            throw std::runtime_error(
                "exact runtime-key distinct-key cap exceeded");
        }
        if (invocations_ == std::numeric_limits<std::uint64_t>::max() ||
            invocations_by_op_[operation] ==
                std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error(
                "exact runtime-key invocation counter overflow");
        }
        if (construction_nanoseconds >
                std::numeric_limits<std::uint64_t>::max() -
                    key_construction_nanoseconds_ ||
            construction_nanoseconds >
                std::numeric_limits<std::uint64_t>::max() -
                    key_construction_nanoseconds_by_op_[operation]) {
            throw std::overflow_error(
                "exact runtime-key construction timer overflow");
        }

        ++invocations_;
        ++invocations_by_op_[operation];
        key_construction_nanoseconds_ += construction_nanoseconds;
        key_construction_nanoseconds_by_op_[operation] +=
            construction_nanoseconds;
        if (is_new) {
            const auto inserted = keys.insert(key);
            if (!inserted.second) {
                throw std::logic_error(
                    "exact runtime-key equality changed during insertion");
            }
            ++distinct_keys_;
            ++distinct_keys_by_op_[operation];
        }
    }

    void recordCacheLookup(
        DagOp op,
        std::uint64_t lookup_nanoseconds) {
        const std::size_t operation = dagOpIndex(op);
        if (operation >= kDagOpCount) {
            throw std::logic_error(
                "runtime-key lookup diagnostic received an invalid DAG "
                "operation");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (lookup_nanoseconds >
                std::numeric_limits<std::uint64_t>::max() -
                    cache_lookup_nanoseconds_ ||
            lookup_nanoseconds >
                std::numeric_limits<std::uint64_t>::max() -
                    cache_lookup_nanoseconds_by_op_[operation]) {
            throw std::overflow_error(
                "exact runtime-key cache-lookup timer overflow");
        }
        cache_lookup_nanoseconds_ += lookup_nanoseconds;
        cache_lookup_nanoseconds_by_op_[operation] +=
            lookup_nanoseconds;
    }

    RuntimeKeyDiagnosticsSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeKeyDiagnosticsSnapshot result;
        result.exact = true;
        result.cap = cap_;
        result.invocations = invocations_;
        result.distinct_keys = distinct_keys_;
        result.key_construction_nanoseconds =
            key_construction_nanoseconds_;
        result.cache_lookup_nanoseconds =
            cache_lookup_nanoseconds_;
        result.invocations_by_op = invocations_by_op_;
        result.distinct_keys_by_op = distinct_keys_by_op_;
        result.key_construction_nanoseconds_by_op =
            key_construction_nanoseconds_by_op_;
        result.cache_lookup_nanoseconds_by_op =
            cache_lookup_nanoseconds_by_op_;
        return result;
    }

private:
    const std::uint64_t cap_;
    mutable std::mutex mutex_;
    std::array<
        std::unordered_set<RuntimeKey, RuntimeKeyHash>,
        kDagOpCount> keys_by_op_;
    std::uint64_t invocations_ = 0;
    std::uint64_t distinct_keys_ = 0;
    std::uint64_t key_construction_nanoseconds_ = 0;
    std::uint64_t cache_lookup_nanoseconds_ = 0;
    std::array<std::uint64_t, kDagOpCount> invocations_by_op_{};
    std::array<std::uint64_t, kDagOpCount> distinct_keys_by_op_{};
    std::array<std::uint64_t, kDagOpCount>
        key_construction_nanoseconds_by_op_{};
    std::array<std::uint64_t, kDagOpCount>
        cache_lookup_nanoseconds_by_op_{};
};

#if !defined(DSM_GOPHER_OFF_TRANSLATION_UNIT)
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
          options_, longestBudget(options_))),
      structural_v2_static_cacheable_(
          options_.memo_policy == MemoPolicy::StructuralV2 &&
                  (options_.memo_mode == MemoMode::Structural ||
                   options_.memo_mode == MemoMode::Dag) &&
                  structuralBudget(options_) > 0 &&
                  options_.memo_options.max_entry_bytes > 0
              ? dag.nodes.size()
              : 0,
          0) {
    if (dag_.passes.any() &&
        (options_.memo_mode != MemoMode::None || options_.collect_statistics ||
         options_.verify_dag ||
         options_.runtime_key_diagnostics != RuntimeKeyDiagnosticsMode::Off)) {
        throw std::invalid_argument(
            "rewritten operator DAG requires memo none, statistics off, "
            "verify-dag off, and runtime-key diagnostics off; "
            "use the full-answer differential gate and --ops for operation census");
    }
    if (dag_.passes.physicalAny() && options_.adaptive &&
        (options_.adaptive->stateCountersEnabled() || options_.adaptive->neighborCopyTimerEnabled())) {
        throw std::invalid_argument("A1 state/timer diagnostics are P0-only; physical passes use ops census");
    }
    if (options_.runtime_key_diagnostics ==
        RuntimeKeyDiagnosticsMode::Exact) {
        if (options_.memo_mode != MemoMode::None &&
            options_.memo_mode != MemoMode::Dag) {
            throw std::invalid_argument(
                "exact runtime-key diagnostics require DAG-NoMemo or "
                "all-operation DAG memoization");
        }
        if (options_.memo_policy != MemoPolicy::Legacy ||
            options_.memo_options.cost_aware_admission ||
            options_.memo_options.minimum_estimated_work != 0) {
            throw std::invalid_argument(
                "exact runtime-key diagnostics require the legacy "
                "non-cost-aware eager-admission contract");
        }
        if (options_.exact_runtime_key_cap == 0) {
            throw std::invalid_argument(
                "exact runtime-key diagnostics require a nonzero "
                "distinct-key cap");
        }
        exact_runtime_keys_ = std::make_unique<ExactRuntimeKeyCollector>(
            options_.exact_runtime_key_cap);
    }

    if (options_.memo_policy != MemoPolicy::StructuralV2 ||
        !structuralMemoEnabled()) {
        return;
    }
    std::vector<int> position(
        static_cast<std::size_t>(Q_.num_vertices), -1);
    for (std::size_t index = 0; index < plan_.order.size(); ++index) {
        const int query_vertex = plan_.order[index];
        if (query_vertex >= 0 && query_vertex < Q_.num_vertices) {
            position[static_cast<std::size_t>(query_vertex)] =
                static_cast<int>(index);
        }
    }

    for (const DagNode& node : dag_.nodes) {
        if (node.op == DagOp::StructuralIntersection) {
            ++structural_v2_intersection_nodes_;
        }
    }

    // A structural node can only reuse a key when its dependency projection
    // omits at least one binding from the prefix in which it is evaluated.
    // Mark nodes that have such a context in any structural root. A runtime
    // guard below handles a hash-consed node reached from multiple depths.
    for (int query_vertex : plan_.order) {
        if (query_vertex < 0 || query_vertex >= Q_.num_vertices) continue;
        const std::size_t query_index =
            static_cast<std::size_t>(query_vertex);
        if (query_index >= dag_.structural_roots.size()) continue;
        const DagNodeId root = dag_.structural_roots[query_index];
        if (root == kInvalidDagNode) continue;
        const int prefix_size = position[query_index];
        if (prefix_size <= 0) continue;

        std::vector<std::uint8_t> visited(dag_.nodes.size(), 0);
        std::vector<DagNodeId> pending{root};
        while (!pending.empty()) {
            const DagNodeId node_id = pending.back();
            pending.pop_back();
            if (node_id >= dag_.nodes.size() ||
                visited[static_cast<std::size_t>(node_id)] != 0) {
                continue;
            }
            visited[static_cast<std::size_t>(node_id)] = 1;
            const DagNode& node = dag_.node(node_id);
            if (node.op == DagOp::StructuralIntersection) {
                bool dependencies_are_in_prefix = true;
                for (int dependency : node.dependencies) {
                    if (dependency < 0 ||
                        dependency >= Q_.num_vertices ||
                        position[static_cast<std::size_t>(dependency)] < 0 ||
                        position[static_cast<std::size_t>(dependency)] >=
                            prefix_size) {
                        dependencies_are_in_prefix = false;
                        break;
                    }
                }
                if (dependencies_are_in_prefix &&
                    node.dependencies.size() <
                        static_cast<std::size_t>(prefix_size)) {
                    structural_v2_static_cacheable_[
                        static_cast<std::size_t>(node_id)] = 1;
                }
            }
            pending.insert(
                pending.end(), node.parents.begin(), node.parents.end());
        }
    }
    structural_v2_eligible_nodes_ = static_cast<std::uint64_t>(
        std::count(
            structural_v2_static_cacheable_.begin(),
            structural_v2_static_cacheable_.end(),
            static_cast<std::uint8_t>(1)));
}

DagExecutor::~DagExecutor() = default;
#endif

#if defined(DSM_GOPHER_OFF_TRANSLATION_UNIT)
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

bool DagExecutor::structuralV2ProjectionCanRepeat(
    const DagNode& node,
    const SearchState& state) const {
    if (node.id >= structural_v2_static_cacheable_.size() ||
        structural_v2_static_cacheable_[
            static_cast<std::size_t>(node.id)] == 0) {
        return false;
    }
    std::size_t bound_prefix_size = 0;
    for (int binding : state.mapping) {
        if (binding >= 0) ++bound_prefix_size;
    }
    if (node.dependencies.size() >= bound_prefix_size) return false;
    for (int dependency : node.dependencies) {
        if (dependency < 0 ||
            static_cast<std::size_t>(dependency) >= state.mapping.size() ||
            state.mapping[static_cast<std::size_t>(dependency)] < 0) {
            return false;
        }
    }
    return true;
}

int DagExecutor::resolveBinding(
    int query_vertex,
    const SearchState& state,
    int overlay_query_vertex,
    int overlay_data_vertex) const {
    return resolveRuntimeBinding(
        query_vertex,
        state,
        overlay_query_vertex,
        overlay_data_vertex);
}

RuntimeKey DagExecutor::makeRuntimeKey(
    const DagNode& node,
    const SearchState& state,
    int overlay_query_vertex,
    int overlay_data_vertex) const {
    return buildRuntimeKey(
        options_.runtime_key_mode,
        node,
        state,
        overlay_query_vertex,
        overlay_data_vertex);
}
#endif

template <bool CollectStatistics, bool ExactDiagnostics>
RuntimeKey DagExecutor::makeMeasuredRuntimeKey(
    const DagNode& node,
    const SearchState& state,
    int overlay_query_vertex,
    int overlay_data_vertex) {
    constexpr bool measure = CollectStatistics || ExactDiagnostics;
    const Clock::time_point construction_start =
        measurementStart<measure>();
    RuntimeKey key = makeRuntimeKey(
        node,
        state,
        overlay_query_vertex,
        overlay_data_vertex);
    const std::uint64_t construction_nanoseconds =
        measurementElapsed<measure>(construction_start);
    statistics_.addSpecialized<CollectStatistics>(
        statistics_.runtime_key_construction_nanoseconds,
        construction_nanoseconds);
    statistics_.addSpecialized<CollectStatistics>(
        statistics_.runtime_key_construction_nanoseconds_by_op[
            dagOpIndex(node.op)],
        construction_nanoseconds);
    if constexpr (ExactDiagnostics) {
        if (!exact_runtime_keys_) {
            throw std::logic_error(
                "exact runtime-key invocation without a collector");
        }
        exact_runtime_keys_->record(
            node.op,
            key,
            construction_nanoseconds);
    }
    return key;
}

template <bool CollectStatistics, bool ExactDiagnostics>
void DagExecutor::recordCacheLookupTime(
    DagOp op,
    std::uint64_t nanoseconds) {
    statistics_.addSpecialized<CollectStatistics>(
        statistics_.cache_lookup_nanoseconds,
        nanoseconds);
    statistics_.addSpecialized<CollectStatistics>(
        statistics_.cache_lookup_nanoseconds_by_op[dagOpIndex(op)],
        nanoseconds);
    if constexpr (ExactDiagnostics) {
        if (!exact_runtime_keys_) {
            throw std::logic_error(
                "exact runtime-key lookup without a collector");
        }
        exact_runtime_keys_->recordCacheLookup(op, nanoseconds);
    }
}

template <bool CollectStatistics>
void DagExecutor::recordOperatorInvocation(DagOp op) {
    if constexpr (CollectStatistics) {
        statistics_.addSpecialized<true>(
            statistics_.dag_operator_invocations_by_op[dagOpIndex(op)]);
    } else {
        (void)op;
    }
}

template <bool CollectStatistics>
void DagExecutor::recordCacheProbe(DagOp op, bool hit) {
    if constexpr (CollectStatistics) {
        const std::size_t operation = dagOpIndex(op);
        if (hit) {
            statistics_.addSpecialized<true>(
                statistics_.dag_operator_cache_hits_by_op[operation]);
        } else {
            statistics_.addSpecialized<true>(
                statistics_.dag_operator_cache_misses_by_op[operation]);
        }
    } else {
        (void)op;
        (void)hit;
    }
}

template <bool CollectStatistics>
void DagExecutor::recordOperatorCompute(
    DagOp op,
    std::uint64_t nanoseconds,
    bool cache_probe_missed) {
    if constexpr (CollectStatistics) {
        const std::size_t operation = dagOpIndex(op);
        statistics_.addSpecialized<true>(
            statistics_
                .dag_operator_exclusive_compute_nanoseconds_by_op[operation],
            nanoseconds);
        if (cache_probe_missed) {
            statistics_.addSpecialized<true>(
                statistics_
                    .dag_operator_exclusive_miss_compute_nanoseconds_by_op[
                        operation],
                nanoseconds);
        }
        switch (op) {
            case DagOp::TdBlockLookup:
            case DagOp::OutNeighborLookup:
            case DagOp::InNeighborLookup:
            case DagOp::StructuralIntersection:
                statistics_.addSpecialized<true>(
                    statistics_.structural_compute_nanoseconds,
                    nanoseconds);
                break;
            case DagOp::EdgeTimeLookup:
            case DagOp::TemporalIntersection:
                statistics_.addSpecialized<true>(
                    statistics_.temporal_compute_nanoseconds,
                    nanoseconds);
                break;
            case DagOp::LongestConsecutiveRun:
                statistics_.addSpecialized<true>(
                    statistics_.longest_compute_nanoseconds,
                    nanoseconds);
                break;
            case DagOp::BranchSupportInput:
            case DagOp::KappaNormalize:
            case DagOp::GuardedEdgeTimeLookup:
            case DagOp::TdBlockView:
            case DagOp::OutNeighborView:
            case DagOp::InNeighborView:
            case DagOp::FusedEdgeProbe:
            case DagOp::BoundEdgeTimeLookup:
            case DagOp::RuntimeStructuralChoice:
                throw std::logic_error("rewritten operators use ops census, not legacy timers");
        }
    } else {
        (void)op;
        (void)nanoseconds;
        (void)cache_probe_missed;
    }
}

template <bool CollectStatistics>
void DagExecutor::recordOperatorResult(
    DagOp op,
    std::uint64_t result_units,
    int returned_run_length) {
    if constexpr (CollectStatistics) {
        const std::size_t operation = dagOpIndex(op);
        statistics_.addSpecialized<true>(
            statistics_.dag_operator_result_units_sum_by_op[operation],
            result_units);
        statistics_.addSpecialized<true>(
            statistics_.dag_operator_result_observations_by_op[operation]);
        if (op == DagOp::LongestConsecutiveRun) {
            if (returned_run_length < 0) {
                throw std::logic_error(
                    "longest-run result is missing its returned length");
            }
            statistics_.addSpecialized<true>(
                statistics_.longest_run_returned_length_sum,
                static_cast<std::uint64_t>(returned_run_length));
            statistics_.addSpecialized<true>(
                statistics_.longest_run_returned_length_observations);
        } else if (returned_run_length >= 0) {
            throw std::logic_error(
                "non-longest DAG result supplied a run length");
        }
    } else {
        (void)op;
        (void)result_units;
        (void)returned_run_length;
    }
}

template <bool CollectStatistics>
void DagExecutor::recordCacheAdmission(
    DagOp op,
    const MemoInsertOutcome& outcome) {
    if constexpr (CollectStatistics) {
        if (!outcome.admitted()) return;
        const std::size_t operation = dagOpIndex(op);
        statistics_.addSpecialized<true>(
            statistics_.dag_operator_cache_admissions_by_op[operation]);
        statistics_.addSpecialized<true>(
            statistics_.dag_operator_cache_admitted_bytes_by_op[operation],
            outcome.admitted_bytes);
    } else {
        (void)op;
        (void)outcome;
    }
}

template <bool CollectStatistics, bool ExactDiagnostics>
DagExecutor::CandidateSetPtr DagExecutor::evaluateCandidateNode(
    DagNodeId node_id,
    const SearchState& state) {
    const DagNode& node = dag_.node(node_id);
    if (node.result_kind != DagResultKind::CandidateSet) {
        throw std::logic_error("candidate DAG node has the wrong result kind");
    }

    switch (node.op) {
        case DagOp::TdBlockLookup:
            statistics_.addSpecialized<CollectStatistics>(
                statistics_.logical_td_block_lookups);
            break;
        case DagOp::OutNeighborLookup:
        case DagOp::InNeighborLookup:
            statistics_.addSpecialized<CollectStatistics>(
                statistics_.logical_neighbor_lookups);
            break;
        case DagOp::StructuralIntersection:
            statistics_.addSpecialized<CollectStatistics>(
                statistics_.logical_structural_intersections);
            break;
        default:
            throw std::logic_error("non-structural operation in candidate DAG");
    }
    recordOperatorInvocation<CollectStatistics>(node.op);

    RuntimeKey key;
    bool key_initialized = false;
    if constexpr (ExactDiagnostics) {
        key = makeMeasuredRuntimeKey<CollectStatistics, true>(
            node, state, -1, -1);
        key_initialized = true;
    }

    const bool use_memo = structuralMemoEnabled();
    const bool structural_v2 =
        options_.memo_policy == MemoPolicy::StructuralV2;
    const bool structural_v2_projection_reuse =
        structural_v2 &&
        node.op == DagOp::StructuralIntersection &&
        structuralV2ProjectionCanRepeat(node, state);
    const bool node_memo_candidate =
        use_memo &&
        (!structural_v2 ||
         structural_v2_projection_reuse);
    const bool cost_aware =
        options_.memo_options.cost_aware_admission;
    const std::size_t minimum_work =
        options_.memo_options.minimum_estimated_work;
    const CandidateSet* td_source = nullptr;
    const std::vector<Edge>* neighbor_source = nullptr;
    std::vector<CandidateSetPtr> parent_values;
    std::size_t estimated_work = 0;
    bool probed_before_parents = false;
    bool cache_probe_missed = false;
    std::uint64_t local_compute_ns = 0;

    if (use_memo && structural_v2 &&
        node.op == DagOp::StructuralIntersection &&
        !structural_v2_projection_reuse) {
        statistics_.addSpecialized<CollectStatistics>(
            statistics_.structural_v2_projection_bypasses);
    }

    if (node_memo_candidate && structural_v2) {
        // Structural-v2 only admits internal intersection nodes. Their
        // compiler-projected dependencies completely determine the output,
        // so the exact full-binding key is safe to probe before evaluating
        // any parent subtree. Admission still happens after a miss, when the
        // parent sizes required by the existing cost gate are known.
        if (node.op != DagOp::StructuralIntersection ||
            node.parents.empty()) {
            throw std::logic_error(
                "structural-v2 selected a non-internal DAG node");
        }
        if (!key_initialized) {
            key = makeMeasuredRuntimeKey<CollectStatistics, false>(
                node, state, -1, -1);
            key_initialized = true;
        }
        constexpr bool measure_lookup =
            CollectStatistics || ExactDiagnostics;
        const auto lookup_start = measurementStart<measure_lookup>();
        auto cached = structural_memo_.find(key);
        recordCacheLookupTime<CollectStatistics, ExactDiagnostics>(
            node.op,
            measurementElapsed<measure_lookup>(lookup_start));
        recordCacheProbe<CollectStatistics>(node.op, cached != nullptr);
        probed_before_parents = true;
        statistics_.addSpecialized<CollectStatistics>(
            statistics_.structural_v2_eligible_probes);
        if (cached) {
            statistics_.addSpecialized<CollectStatistics>(
                statistics_.structural_intersection_cache_hits);
            recordSkippedSubtree(node_id);
            statistics_.addSpecialized<CollectStatistics>(
                statistics_.structural_parent_subtrees_skipped,
                node.parents.size());
            recordOperatorResult<CollectStatistics>(
                node.op,
                static_cast<std::uint64_t>(cached->size()));
            return CandidateSetPtr(std::move(cached));
        }
        cache_probe_missed = true;
    }

    if (node.op == DagOp::TdBlockLookup) {
        const auto source_start =
            measurementStart<CollectStatistics>();
        const int parent_data_vertex = resolveBinding(
            node.parameter.second_query_vertex, state, -1, -1);
        td_source = td_tree_.candidatesForParent(
            node.parameter.first_query_vertex, parent_data_vertex);
        estimated_work =
            td_source == nullptr ? 0 : td_source->size();
        local_compute_ns +=
            measurementElapsed<CollectStatistics>(source_start);
    } else if (
        node.op == DagOp::OutNeighborLookup ||
        node.op == DagOp::InNeighborLookup) {
        const auto source_start =
            measurementStart<CollectStatistics>();
        const int anchor = resolveBinding(
            node.parameter.first_query_vertex, state, -1, -1);
        if (anchor >= 0 && anchor < G_.num_vertices) {
            neighbor_source =
                node.op == DagOp::OutNeighborLookup
                ? &G_.adj[static_cast<std::size_t>(anchor)]
                : &G_.in_adj[static_cast<std::size_t>(anchor)];
            estimated_work = neighbor_source->size();
        }
        local_compute_ns +=
            measurementElapsed<CollectStatistics>(source_start);
    } else {
        if (node.parents.empty()) {
            throw std::logic_error(
                "structural intersection has no operands");
        }
        if (node_memo_candidate && cost_aware) {
            parent_values.reserve(node.parents.size());
            for (DagNodeId parent : node.parents) {
                CandidateSetPtr value =
                    evaluateCandidateNode<
                        CollectStatistics,
                        ExactDiagnostics>(parent, state);
                estimated_work += value->size();
                parent_values.push_back(std::move(value));
            }
        }
    }

    // Legacy decides whether to probe only after measuring parent work.
    // Structural-v2 has already probed its eligible internal node and applies
    // the same cost gate after the miss; entry/budget gates remain inside
    // insertOrGet.
    const bool memo_eligible =
        node_memo_candidate &&
        (probed_before_parents ||
         !cost_aware || estimated_work >= minimum_work);
    if (node_memo_candidate && !memo_eligible) {
        structural_memo_.recordCostBypass();
    }
    if (memo_eligible && !probed_before_parents) {
        if (!key_initialized) {
            key = makeMeasuredRuntimeKey<CollectStatistics, false>(
                node, state, -1, -1);
            key_initialized = true;
        }
        constexpr bool measure_lookup =
            CollectStatistics || ExactDiagnostics;
        const auto lookup_start = measurementStart<measure_lookup>();
        auto cached = structural_memo_.find(key);
        recordCacheLookupTime<CollectStatistics, ExactDiagnostics>(
            node.op,
            measurementElapsed<measure_lookup>(lookup_start));
        recordCacheProbe<CollectStatistics>(node.op, cached != nullptr);
        if (cached) {
            if (node.op == DagOp::TdBlockLookup) {
                statistics_.addSpecialized<CollectStatistics>(
                    statistics_.td_block_cache_hits);
            } else if (
                node.op == DagOp::OutNeighborLookup ||
                node.op == DagOp::InNeighborLookup) {
                statistics_.addSpecialized<CollectStatistics>(
                    statistics_.neighbor_lookup_cache_hits);
            } else {
                statistics_.addSpecialized<CollectStatistics>(
                    statistics_.structural_intersection_cache_hits);
            }
            // Legacy source resolution may be required before the probe.
            // It is physical local work even when the probe hits, but it is
            // not miss-attributed work.
            recordOperatorCompute<CollectStatistics>(
                node.op,
                local_compute_ns,
                false);
            recordOperatorResult<CollectStatistics>(
                node.op,
                static_cast<std::uint64_t>(cached->size()));
            recordSkippedSubtree(node_id);
            return CandidateSetPtr(std::move(cached));
        }
        cache_probe_missed = true;
    }

    CandidateSet result;
    if (node.op == DagOp::TdBlockLookup) {
        statistics_.addSpecialized<CollectStatistics>(
            statistics_.physical_td_block_lookups);
        OPS_ADD(options_.ops, td_block_lookups_executed, 1);
        if (options_.ops != nullptr) options_.ops->physical_profile.recordOperand(
            {StructuralProfile::Backend::LegacyDag, node.id, node.parameter.first_query_vertex},
            StructuralProfile::Operand::TdBlock, td_source == nullptr ? 0 : td_source->size());
        const auto compute_start =
            measurementStart<CollectStatistics>();
        if (td_source != nullptr && !memo_eligible) {
            local_compute_ns +=
                measurementElapsed<CollectStatistics>(compute_start);
            recordOperatorCompute<CollectStatistics>(
                node.op,
                local_compute_ns,
                cache_probe_missed);
            recordOperatorResult<CollectStatistics>(
                node.op,
                static_cast<std::uint64_t>(td_source->size()));
            // TD-tree blocks are immutable for the complete search.  A
            // non-memoized lookup can therefore expose the stable block
            // directly instead of allocating and copying it at every state.
            return CandidateSetPtr(td_source);
        }
        if (td_source != nullptr) {
            result = *td_source;
            OPS_ADD(options_.ops, physical_seed_copy_elements, result.size());
            OPS_ADD(options_.ops, physical_seed_copy_bytes, result.size() * sizeof(int));
            if (options_.ops != nullptr) options_.ops->physical_profile.recordCopy(
                {StructuralProfile::Backend::LegacyDag, node.id, node.parameter.first_query_vertex},
                StructuralProfile::Copy::TdBlock, result.size(), sizeof(CandidateSet::value_type));
        }
        local_compute_ns +=
            measurementElapsed<CollectStatistics>(compute_start);
    } else if (
        node.op == DagOp::OutNeighborLookup ||
        node.op == DagOp::InNeighborLookup) {
        statistics_.addSpecialized<CollectStatistics>(
            statistics_.physical_neighbor_lookups);
        OPS_ADD(options_.ops, neighbor_lookups_executed, 1);
        const auto compute_start =
            measurementStart<CollectStatistics>();
        if (neighbor_source != nullptr) {
            result.reserve(neighbor_source->size());
            const bool time_copy = options_.adaptive != nullptr &&
                options_.adaptive->neighborCopyTimerEnabled();
            if (time_copy) {
                options_.adaptive->copyNeighborIdsTimed(*neighbor_source, result,
                    node.parameter.first_query_vertex, node.id);
            } else {
                for (const auto& edge : *neighbor_source) {
                    result.push_back(edge.to);
                }
            }
        }
        if (options_.adaptive != nullptr && options_.adaptive->stateCountersEnabled()) {
            options_.adaptive->recordNeighborCopy(node.parameter.first_query_vertex, node.id,
                result.size(), sizeof(CandidateSet::value_type));
        }
        OPS_ADD(options_.ops, physical_neighbor_copies, 1);
        OPS_ADD(options_.ops, physical_neighbor_copy_elements, result.size());
        OPS_ADD(options_.ops, physical_neighbor_copy_bytes, result.size() * sizeof(int));
        if (options_.ops != nullptr) {
            const StructuralProfile::Context context{
                StructuralProfile::Backend::LegacyDag, node.id, node.parameter.first_query_vertex};
            options_.ops->physical_profile.recordOperand(context,
                node.op == DagOp::OutNeighborLookup ? StructuralProfile::Operand::OutNeighbor : StructuralProfile::Operand::InNeighbor,
                result.size());
            options_.ops->physical_profile.recordCopy(context,
                StructuralProfile::Copy::NeighborMaterialization, result.size(), sizeof(CandidateSet::value_type));
        }
        local_compute_ns +=
            measurementElapsed<CollectStatistics>(compute_start);
    } else {
        if (parent_values.empty()) {
            parent_values.reserve(node.parents.size());
            for (DagNodeId parent : node.parents) {
                parent_values.push_back(
                    evaluateCandidateNode<
                        CollectStatistics,
                        ExactDiagnostics>(parent, state));
            }
        }
        const auto seed_start = measurementStart<CollectStatistics>();
        result = *parent_values.front();
        OPS_ADD(options_.ops, physical_seed_copy_elements, result.size());
        OPS_ADD(options_.ops, physical_seed_copy_bytes, result.size() * sizeof(int));
        if (options_.ops != nullptr) options_.ops->physical_profile.recordCopy(
            {StructuralProfile::Backend::LegacyDag, node.id, -1},
            StructuralProfile::Copy::StructuralSeed, result.size(), sizeof(CandidateSet::value_type));
        local_compute_ns +=
            measurementElapsed<CollectStatistics>(seed_start);
        for (std::size_t i = 1;
             i < parent_values.size();
             ++i) {
            if (result.empty()) {
                OPS_ADD(options_.ops, structural_intersections_skipped, parent_values.size() - i);
                break;
            }
            const CandidateSetPtr& rhs = parent_values[i];
            const std::size_t input_size =
                result.size() + rhs->size();
            statistics_.addSpecialized<CollectStatistics>(
                statistics_.structural_input_elements,
                input_size);
            if (!(node_memo_candidate && cost_aware)) {
                estimated_work += input_size;
            }
            CandidateSet intersection;
            intersection.reserve(
                std::min(result.size(), rhs->size()));
            const auto compute_start =
                measurementStart<CollectStatistics>();
            std::set_intersection(
                result.begin(), result.end(),
                rhs->begin(), rhs->end(),
                std::back_inserter(intersection));
            OPS_ADD(options_.ops, structural_intersections_executed, 1);
            OPS_ADD(options_.ops, intersection_output_elements, intersection.size());
            OPS_ADD(options_.ops, physical_output_elements_materialized, intersection.size());
            if (options_.ops != nullptr) options_.ops->physical_profile.recordBinary(
                {StructuralProfile::Backend::LegacyDag, node.id, -1},
                result.size(), rhs->size(), intersection.size(), false);
            local_compute_ns +=
                measurementElapsed<CollectStatistics>(compute_start);
            statistics_.addSpecialized<CollectStatistics>(
                statistics_.physical_structural_intersections);
            result.swap(intersection);
        }
    }
    recordOperatorCompute<CollectStatistics>(
        node.op,
        local_compute_ns,
        cache_probe_missed);

    const bool admit_result =
        memo_eligible &&
        (!probed_before_parents ||
         !cost_aware || estimated_work >= minimum_work);
    if (memo_eligible && !admit_result) {
        structural_memo_.recordCostBypass();
    }
    if (admit_result) {
        const std::size_t value_bytes =
            result.capacity() * sizeof(CandidateSet::value_type);
        const std::size_t result_size = result.size();
        auto immutable =
            std::make_shared<const CandidateSet>(std::move(result));
        MemoInsertOutcome insertion;
        MemoInsertOutcome* insertion_pointer = nullptr;
        if constexpr (CollectStatistics) {
            insertion_pointer = &insertion;
        }
        immutable = structural_memo_.insertOrGet(
            key,
            immutable,
            value_bytes,
            std::max(estimated_work, result_size),
            key.bindings.capacity() * sizeof(int),
            result_size,
            insertion_pointer);
        recordCacheAdmission<CollectStatistics>(node.op, insertion);
        recordOperatorResult<CollectStatistics>(
            node.op,
            static_cast<std::uint64_t>(immutable->size()));
        return CandidateSetPtr(std::move(immutable));
    }
    recordOperatorResult<CollectStatistics>(
        node.op,
        static_cast<std::uint64_t>(result.size()));
    return CandidateSetPtr(std::move(result));
}

template <bool CollectStatistics, bool ExactDiagnostics>
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
        statistics_.addSpecialized<CollectStatistics>(
            statistics_.logical_temporal_edge_lookups);
    } else if (node.op == DagOp::TemporalIntersection) {
        statistics_.addSpecialized<CollectStatistics>(
            statistics_.logical_temporal_intersections);
    } else {
        throw std::logic_error("non-temporal operation in interval DAG");
    }
    recordOperatorInvocation<CollectStatistics>(node.op);

    RuntimeKey key;
    bool key_initialized = false;
    if constexpr (ExactDiagnostics) {
        key = makeMeasuredRuntimeKey<CollectStatistics, true>(
            node,
            state,
            query_vertex,
            candidate);
        key_initialized = true;
    }

    // Structural-v2 intentionally bypasses temporal/longest-run caches. It
    // targets the only operation class with demonstrated reusable work while
    // retaining the immutable edge-time views and count-only fast path.
    const bool use_memo =
        temporalMemoEnabled() &&
        options_.memo_policy == MemoPolicy::Legacy;
    const bool cost_aware =
        options_.memo_options.cost_aware_admission;
    const std::size_t minimum_work =
        options_.memo_options.minimum_estimated_work;
    const TemporalEdge* temporal_source = nullptr;
    std::vector<IntervalSetPtr> parent_values;
    std::size_t estimated_work = 0;
    bool cache_probe_missed = false;
    std::uint64_t local_compute_ns = 0;

    if (node.op == DagOp::EdgeTimeLookup) {
        const auto source_start =
            measurementStart<CollectStatistics>();
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
            : temporal_source->activeIntervals().size();
        local_compute_ns +=
            measurementElapsed<CollectStatistics>(source_start);
    } else {
        if (node.parents.empty()) {
            throw std::logic_error(
                "temporal intersection has no operands");
        }
        if (use_memo && cost_aware) {
            parent_values.reserve(node.parents.size());
            for (DagNodeId parent : node.parents) {
                IntervalSetPtr value = evaluateIntervalNode<
                    CollectStatistics,
                    ExactDiagnostics>(
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
    if (memo_eligible) {
        if (!key_initialized) {
            key = makeMeasuredRuntimeKey<CollectStatistics, false>(
                node,
                state,
                query_vertex,
                candidate);
            key_initialized = true;
        }
        constexpr bool measure_lookup =
            CollectStatistics || ExactDiagnostics;
        const auto lookup_start = measurementStart<measure_lookup>();
        auto cached = temporal_memo_.find(key);
        recordCacheLookupTime<CollectStatistics, ExactDiagnostics>(
            node.op,
            measurementElapsed<measure_lookup>(lookup_start));
        recordCacheProbe<CollectStatistics>(node.op, cached != nullptr);
        if (cached) {
            if (node.op == DagOp::EdgeTimeLookup) {
                statistics_.addSpecialized<CollectStatistics>(
                    statistics_.edge_time_cache_hits);
            } else {
                statistics_.addSpecialized<CollectStatistics>(
                    statistics_.temporal_intersection_cache_hits);
            }
            // Edge-source resolution can precede a legacy memo probe. Keep
            // that incurred local work in the all-invocation total while
            // excluding it from the actual-miss subset.
            recordOperatorCompute<CollectStatistics>(
                node.op,
                local_compute_ns,
                false);
            recordOperatorResult<CollectStatistics>(
                node.op,
                static_cast<std::uint64_t>(cached->size()));
            recordSkippedSubtree(node_id);
            return IntervalSetPtr(std::move(cached));
        }
        cache_probe_missed = true;
    }

    IntervalSet result;
    if (node.op == DagOp::EdgeTimeLookup) {
        statistics_.addSpecialized<CollectStatistics>(
            statistics_.physical_temporal_edge_lookups);
        OPS_ADD(options_.ops, history_lookups_executed, 1);
        const auto compute_start =
            measurementStart<CollectStatistics>();
        if (temporal_source != nullptr) {
            OPS_ADD(options_.ops, physical_history_payload_reads, 1);
            OPS_ADD(options_.ops, kappa_executed, 1);
            OPS_ADD(options_.ops, time_input_elements, temporal_source->activeIntervals().size());
            const bool all_intervals_durable =
                std::all_of(
                    temporal_source->activeIntervals().begin(),
                    temporal_source->activeIntervals().end(),
                    [this](const TimeInterval& interval) {
                        OPS_ADD(options_.ops, time_elements_read, 1);
                        return interval.length() >= k_threshold_;
                    });
            if (!memo_eligible && all_intervals_durable) {
                local_compute_ns +=
                    measurementElapsed<CollectStatistics>(compute_start);
                recordOperatorCompute<CollectStatistics>(
                    node.op,
                    local_compute_ns,
                    cache_probe_missed);
                recordOperatorResult<CollectStatistics>(
                    node.op,
                    static_cast<std::uint64_t>(
                        temporal_source->activeIntervals().size()));
                // Temporal edges are immutable after loading.  If filtering
                // would not remove anything, retain a non-owning view rather
                // than constructing the same vector for every DAG visit.
                return IntervalSetPtr(
                    &temporal_source->activeIntervals());
            }
            result.reserve(
                temporal_source->activeIntervals().size());
            for (const auto& interval :
                 temporal_source->activeIntervals()) {
                OPS_ADD(options_.ops, time_elements_read, 1);
                if (interval.length() >= k_threshold_) {
                    result.push_back(interval);
                }
            }
        }
        local_compute_ns +=
            measurementElapsed<CollectStatistics>(compute_start);
    } else {
        if (parent_values.empty()) {
            parent_values.reserve(node.parents.size());
            for (DagNodeId parent : node.parents) {
                parent_values.push_back(evaluateIntervalNode<
                    CollectStatistics,
                    ExactDiagnostics>(
                        parent, query_vertex, candidate, state));
            }
        }
        const auto seed_start = measurementStart<CollectStatistics>();
        result = *parent_values.front();
        local_compute_ns +=
            measurementElapsed<CollectStatistics>(seed_start);
        for (std::size_t i = 1;
             i < parent_values.size();
             ++i) {
            if (result.empty()) {
                OPS_ADD(options_.ops, time_intersections_skipped, parent_values.size() - i);
                OPS_ADD(options_.ops, kappa_skipped, parent_values.size() - i);
                break;
            }
            const IntervalSetPtr& rhs = parent_values[i];
            const std::size_t input_size = result.size() + rhs->size();
            statistics_.addSpecialized<CollectStatistics>(
                statistics_.temporal_input_intervals,
                input_size);
            if (!(use_memo && cost_aware)) {
                estimated_work += input_size;
            }

            const auto compute_start =
                measurementStart<CollectStatistics>();
            IntervalSet intersection = options_.ops == nullptr
                ? intersectTimeIntervals(result, *rhs, k_threshold_)
                : OperatorKernels::intersectIntervals(result, *rhs, k_threshold_, false, options_.ops);
            local_compute_ns +=
                measurementElapsed<CollectStatistics>(compute_start);
            statistics_.addSpecialized<CollectStatistics>(
                statistics_.physical_temporal_intersections);
            result.swap(intersection);
        }
    }
    recordOperatorCompute<CollectStatistics>(
        node.op,
        local_compute_ns,
        cache_probe_missed);

    if (memo_eligible) {
        const std::size_t value_bytes =
            result.capacity() * sizeof(IntervalSet::value_type);
        const std::size_t result_size = result.size();
        auto immutable =
            std::make_shared<const IntervalSet>(std::move(result));
        MemoInsertOutcome insertion;
        MemoInsertOutcome* insertion_pointer = nullptr;
        if constexpr (CollectStatistics) {
            insertion_pointer = &insertion;
        }
        immutable = temporal_memo_.insertOrGet(
            key,
            immutable,
            value_bytes,
            std::max(estimated_work, result_size),
            key.bindings.capacity() * sizeof(int),
            result_size,
            insertion_pointer);
        recordCacheAdmission<CollectStatistics>(node.op, insertion);
        recordOperatorResult<CollectStatistics>(
            node.op,
            static_cast<std::uint64_t>(immutable->size()));
        return IntervalSetPtr(std::move(immutable));
    }
    recordOperatorResult<CollectStatistics>(
        node.op,
        static_cast<std::uint64_t>(result.size()));
    return IntervalSetPtr(std::move(result));
}

template <bool CollectStatistics, bool ExactDiagnostics>
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
    statistics_.addSpecialized<CollectStatistics>(
        statistics_.logical_longest_run);
    recordOperatorInvocation<CollectStatistics>(node.op);

    RuntimeKey key;
    bool key_initialized = false;
    if constexpr (ExactDiagnostics) {
        key = makeMeasuredRuntimeKey<CollectStatistics, true>(
            node,
            state,
            query_vertex,
            candidate);
        key_initialized = true;
    }

    const bool use_longest_memo =
        longestMemoEnabled() &&
        options_.memo_policy == MemoPolicy::Legacy;
    const bool memo_eligible =
        use_longest_memo &&
        (!options_.memo_options.cost_aware_admission ||
         known_intervals.size() >=
             options_.memo_options.minimum_estimated_work);
    if (use_longest_memo && !memo_eligible) {
        longest_memo_.recordCostBypass();
    }
    bool cache_probe_missed = false;
    if (memo_eligible) {
        if (!key_initialized) {
            key = makeMeasuredRuntimeKey<CollectStatistics, false>(
                node,
                state,
                query_vertex,
                candidate);
            key_initialized = true;
        }
        constexpr bool measure_lookup =
            CollectStatistics || ExactDiagnostics;
        const auto lookup_start = measurementStart<measure_lookup>();
        std::shared_ptr<const int> cached = longest_memo_.find(key);
        recordCacheLookupTime<CollectStatistics, ExactDiagnostics>(
            node.op,
            measurementElapsed<measure_lookup>(lookup_start));
        recordCacheProbe<CollectStatistics>(node.op, cached != nullptr);
        if (cached) {
            statistics_.addSpecialized<CollectStatistics>(
                statistics_.longest_run_cache_hits);
            recordOperatorResult<CollectStatistics>(node.op, 1, *cached);
            return *cached;
        }
        cache_probe_missed = true;
    }

    statistics_.addSpecialized<CollectStatistics>(
        statistics_.physical_longest_run);
    const auto compute_start =
        measurementStart<CollectStatistics>();
    int longest = 0;
    for (const auto& interval : known_intervals) {
        longest = std::max(longest, interval.length());
    }
    recordOperatorCompute<CollectStatistics>(
        node.op,
        measurementElapsed<CollectStatistics>(compute_start),
        cache_probe_missed);

    if (memo_eligible) {
        std::shared_ptr<const int> value =
            std::make_shared<const int>(longest);
        MemoInsertOutcome insertion;
        MemoInsertOutcome* insertion_pointer = nullptr;
        if constexpr (CollectStatistics) {
            insertion_pointer = &insertion;
        }
        value = longest_memo_.insertOrGet(
            key,
            value,
            0,
            known_intervals.size(),
            key.bindings.capacity() * sizeof(int),
            1,
            insertion_pointer);
        recordCacheAdmission<CollectStatistics>(node.op, insertion);
        recordOperatorResult<CollectStatistics>(node.op, 1, *value);
        return *value;
    }
    recordOperatorResult<CollectStatistics>(node.op, 1, longest);
    return longest;
}

#if !defined(DSM_GOPHER_OFF_TRANSLATION_UNIT)
DagExecutor::CandidateSetPtr DagExecutor::structuralCandidates(
    int query_vertex,
    const SearchState& state) {
#if defined(DSM_ERASE_SEARCH_STATISTICS)
    const bool exact_diagnostics =
        options_.runtime_key_diagnostics ==
        RuntimeKeyDiagnosticsMode::Exact;
    return exact_diagnostics
        ? structuralCandidatesSpecialized<false, true>(
              query_vertex, state)
        : structuralCandidatesSpecialized<false, false>(
              query_vertex, state);
#else
    const bool collect_statistics = statistics_.enabled();
    const bool exact_diagnostics = exact_runtime_keys_ != nullptr;
    if (collect_statistics) {
        return exact_diagnostics
            ? structuralCandidatesSpecialized<true, true>(
                  query_vertex, state)
            : structuralCandidatesSpecialized<true, false>(
                  query_vertex, state);
    }
    return exact_diagnostics
        ? structuralCandidatesSpecialized<false, true>(
              query_vertex, state)
        : structuralCandidatesSpecialized<false, false>(
              query_vertex, state);
#endif
}
#endif

template <bool CollectStatistics, bool ExactDiagnostics>
DagExecutor::CandidateSetPtr
DagExecutor::structuralCandidatesSpecialized(
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

    // A1 is a STATE-level proxy, not the sum of intermediate binary costs.
    // Only sizes are observed; no candidate list is copied or extra candidate
    // is filtered. This entire observation is absent when counters are off.
    if (options_.adaptive != nullptr && options_.adaptive->stateCountersEnabled()) {
        const int parent_query = plan_.parent[static_cast<std::size_t>(query_vertex)];
        const int parent_data = parent_query < 0 ? -1 : resolveBinding(parent_query, state, -1, -1);
        const auto* raw = td_tree_.candidatesForParent(query_vertex, parent_data);
        std::vector<std::size_t> neighbor_sizes;
        const auto qi = static_cast<std::size_t>(query_vertex);
        neighbor_sizes.reserve(Q_.adj[qi].size() + Q_.in_adj[qi].size());
        for (const auto& edge : Q_.adj[qi]) {
            if (edge.to == query_vertex) continue;
            const int mapped = resolveBinding(edge.to, state, -1, -1);
            if (mapped >= 0) neighbor_sizes.push_back(mapped < G_.num_vertices
                ? G_.in_adj[static_cast<std::size_t>(mapped)].size() : 0);
        }
        for (const auto& edge : Q_.in_adj[qi]) {
            if (edge.to == query_vertex) continue;
            const int mapped = resolveBinding(edge.to, state, -1, -1);
            if (mapped >= 0) neighbor_sizes.push_back(mapped < G_.num_vertices
                ? G_.adj[static_cast<std::size_t>(mapped)].size() : 0);
        }
        options_.adaptive->recordState(query_vertex, root, raw == nullptr ? 0 : raw->size(), neighbor_sizes);
    }
    CandidateSetPtr result = dag_.passes.p3
        ? evaluateOptimizedCandidateNode(root, state)
        : evaluateCandidateNode<CollectStatistics, ExactDiagnostics>(root, state);
    if (options_.verify_dag) {
        const CandidateSet reference = verifyStructural(query_vertex, state);
        if (*result != reference) {
            throw std::runtime_error(
                "DAG structural verification failed for q" +
                std::to_string(query_vertex));
        }
    }
    if (options_.adaptive != nullptr && options_.adaptive->stateCountersEnabled()) {
        options_.adaptive->recordStructureOutput(query_vertex, root, result->size());
    }
    return result;
}

#if !defined(DSM_GOPHER_OFF_TRANSLATION_UNIT)
TemporalSupport DagExecutor::temporalSupport(
    int query_vertex,
    int candidate,
    const SearchState& state) {
#if defined(DSM_ERASE_SEARCH_STATISTICS)
    const bool exact_diagnostics =
        options_.runtime_key_diagnostics ==
        RuntimeKeyDiagnosticsMode::Exact;
    return exact_diagnostics
        ? temporalSupportSpecialized<false, true>(
              query_vertex, candidate, state)
        : temporalSupportSpecialized<false, false>(
              query_vertex, candidate, state);
#else
    const bool collect_statistics = statistics_.enabled();
    const bool exact_diagnostics = exact_runtime_keys_ != nullptr;
    if (collect_statistics) {
        return exact_diagnostics
            ? temporalSupportSpecialized<true, true>(
                  query_vertex, candidate, state)
            : temporalSupportSpecialized<true, false>(
                  query_vertex, candidate, state);
    }
    return exact_diagnostics
        ? temporalSupportSpecialized<false, true>(
              query_vertex, candidate, state)
        : temporalSupportSpecialized<false, false>(
              query_vertex, candidate, state);
#endif
}
#endif

template <bool CollectStatistics, bool ExactDiagnostics>
TemporalSupport DagExecutor::temporalSupportSpecialized(
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

    if (dag_.passes.temporalRewriteAny()) {
        IntervalSetPtr intervals = evaluateOptimizedIntervalNode(
            temporal_root, query_vertex, candidate, state);
        const int longest = intervals->empty() ? 0 : k_threshold_;
        return {std::move(intervals), longest, true, dag_.cumulative_support};
    }

    IntervalSetPtr intervals = evaluateIntervalNode<
        CollectStatistics,
        ExactDiagnostics>(
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
        options_.memo_policy == MemoPolicy::Legacy &&
        (!options_.memo_options.cost_aware_admission ||
         intervals->size() >=
             options_.memo_options.minimum_estimated_work);
    const bool need_exact_longest =
        CollectStatistics ||
        options_.verify_dag ||
        ExactDiagnostics ||
        longest_memo_candidate;
    const int longest =
        longest_root == kInvalidDagNode
        ? 0
        : (need_exact_longest
              ? evaluateLongestNode<
                    CollectStatistics,
                    ExactDiagnostics>(
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

#if defined(DSM_GOPHER_OFF_TRANSLATION_UNIT)
PhysicalCandidateBatch DagExecutor::physicalCandidates(int query_vertex, const SearchState& state) {
    if (!dag_.passes.physicalAny() || query_vertex < 0 ||
        static_cast<std::size_t>(query_vertex) >= dag_.structural_roots.size())
        throw std::invalid_argument("physicalCandidates requires a physical IR root");
    const auto root = dag_.structural_roots[static_cast<std::size_t>(query_vertex)];
    if (root == kInvalidDagNode) return {};
    auto batch = evaluatePhysicalCandidateNode(root, state);
    if (options_.ops) {
        // Read-only census metadata: derive the logical TD seed even if a P3
        // short circuit skipped its leaf. This observer lookup is NOT counted
        // as an executed IR lookup and is absent from primary ops-OFF runs.
        const int parent_q = plan_.parent[static_cast<std::size_t>(query_vertex)];
        const int parent = resolveBinding(parent_q, state, -1, -1);
        const auto* raw = td_tree_.candidatesForParent(query_vertex, parent);
        const std::size_t raw_size = raw ? raw->size() : 0;
        if (batch.size() > raw_size || batch.lmax_rejected_candidates > raw_size - batch.size())
            throw std::logic_error("physical first-drop funnel exceeds logical TD seed");
        OPS_ADD(options_.ops, physical_raw_candidates, raw_size);
        OPS_ADD(options_.ops, physical_structure_rejections,
            raw_size - batch.size() - batch.lmax_rejected_candidates);
    }
    OPS_ADD(options_.ops, physical_output_candidates, batch.size());
    return batch;
}

void DagExecutor::registerIndexDrivers(JIndexExecution& index) const {
    if (!dag_.passes.p4 || dag_.passes.p1 || dag_.passes.p2 || dag_.passes.p3 ||
        !options_.window_guards)
        throw std::invalid_argument("P8 DAG drivers require P4, W, and P1--P3 off");
    for (int q : plan_.order) {
        if (q == plan_.root) continue;
        const auto root = dag_.structural_roots.at(static_cast<std::size_t>(q));
        if (root == kInvalidDagNode) continue;
        const auto& node = dag_.node(root);
        if (node.op != DagOp::FusedEdgeProbe || node.parameter.first_query_vertex != q)
            throw std::logic_error("P8 driver must belong to its compiled P4 producer");
        if (node.parents.size() < 2) continue;
        const auto& leaf = dag_.node(node.parents[1]);
        const bool outgoing = leaf.op == DagOp::OutNeighborLookup || leaf.op == DagOp::OutNeighborView;
        const bool incoming = leaf.op == DagOp::InNeighborLookup || leaf.op == DagOp::InNeighborView;
        if (!outgoing && !incoming)
            throw std::logic_error("P8 first compiled adjacency is not a neighbor leaf");
        index.registerDriver(q, node.id, leaf.parameter.first_query_vertex, outgoing,
                             JIndexExecution::DriverOrigin::CompiledDag);
    }
}

PhysicalCandidateBatch DagExecutor::physicalCandidates(
    int q, const SearchState& state, JIndexExecution& index,
    window_algebra::WindowView prior) {
    if (!dag_.passes.p4 || dag_.passes.p1 || dag_.passes.p2 || dag_.passes.p3 ||
        !options_.window_guards)
        throw std::invalid_argument("P8 candidate execution requires P4, W, and P1--P3 off");
    if (q < 0 || static_cast<std::size_t>(q) >= dag_.structural_roots.size())
        throw std::out_of_range("P8 query vertex");
    const auto root = dag_.structural_roots[static_cast<std::size_t>(q)];
    if (q == plan_.root || root == kInvalidDagNode)
        return physicalCandidates(q, state);
    const auto& node = dag_.node(root);
    if (node.op != DagOp::FusedEdgeProbe || node.parameter.first_query_vertex != q ||
        node.parents.empty())
        throw std::logic_error("P8 requires the compiled fused candidate producer");
    if (node.parents.size() < 2) return physicalCandidates(q, state);

    // Selection metadata borrows immutable TD/adjacency storage. On fallback
    // this extra preparation is P8 overhead, but does not count as a second
    // execution of the original nodes. The old S1 kernel and counters below
    // remain literally untouched.
    const auto& td = dag_.node(node.parents.front());
    if ((td.op != DagOp::TdBlockLookup && td.op != DagOp::TdBlockView) ||
        td.parameter.first_query_vertex != q)
        throw std::logic_error("P8 producer has no matching TD seed");
    const int parent = resolveBinding(td.parameter.second_query_vertex, state, -1, -1);
    const auto* block = td_tree_.candidatesForParent(q, parent);
    const CandidateReadView original_seed = block ? CandidateReadView(*block) : CandidateReadView{};
    std::vector<CandidateReadView> neighbors;
    neighbors.reserve(node.parents.size() - 1);
    for (std::size_t i = 1; i < node.parents.size(); ++i) {
        const auto& leaf = dag_.node(node.parents[i]);
        const bool outgoing = leaf.op == DagOp::OutNeighborLookup || leaf.op == DagOp::OutNeighborView;
        const bool incoming = leaf.op == DagOp::InNeighborLookup || leaf.op == DagOp::InNeighborView;
        if (!outgoing && !incoming) throw std::logic_error("P8 invalid adjacency descriptor");
        const int anchor = resolveBinding(leaf.parameter.first_query_vertex, state, -1, -1);
        const auto* edges = anchor < 0 || anchor >= G_.num_vertices ? nullptr :
            &(outgoing ? G_.adj : G_.in_adj)[static_cast<std::size_t>(anchor)];
        neighbors.push_back(edges ? CandidateReadView(*edges) : CandidateReadView{});
    }
    const auto& driver = dag_.node(node.parents[1]);
    const bool driver_outgoing = driver.op == DagOp::OutNeighborLookup || driver.op == DagOp::OutNeighborView;
    auto selected = index.select(q, node.id,
        resolveBinding(driver.parameter.first_query_vertex, state, -1, -1),
        driver_outgoing, original_seed, neighbors, prior);
    if (!selected.used) return physicalCandidates(q, state);
    if (selected.candidates.size() != selected.driver_entries.size() ||
        selected.candidates.size() > original_seed.size())
        throw std::logic_error("P8 selection has invalid parallel driver entries");
    const CandidateReadView seed(selected.candidates);
    const auto stride = node.history_slots.size();
    if (stride && seed.size() > std::numeric_limits<std::size_t>::max() / stride)
        throw std::overflow_error("P8 history row size overflow");

    OPS_ADD(options_.ops, td_block_lookups_executed, 1);
    OPS_ADD(options_.ops, physical_input_candidates, seed.size());
    if (td.op == DagOp::TdBlockView) OPS_ADD(options_.ops, physical_projected_views, 1);
    else OPS_ADD(options_.ops, physical_private_descriptors, 1);
    if (options_.ops) options_.ops->physical_profile.recordOperand(
        {StructuralProfile::Backend::RewrittenDag, td.id, q},
        StructuralProfile::Operand::TdBlock, original_seed.size());
    std::vector<std::size_t> slots;
    slots.reserve(neighbors.size());
    for (std::size_t j = 0; j < neighbors.size(); ++j) {
        const auto& leaf = dag_.node(node.parents[j + 1]);
        const bool outgoing = leaf.op == DagOp::OutNeighborLookup || leaf.op == DagOp::OutNeighborView;
        const int from = outgoing ? leaf.parameter.first_query_vertex : q;
        const int to = outgoing ? q : leaf.parameter.first_query_vertex;
        std::size_t slot = 0;
        while (slot < stride &&
               (node.history_slots[slot].from_query_vertex != from ||
                node.history_slots[slot].to_query_vertex != to)) ++slot;
        if (slot == stride) throw std::logic_error("P8 adjacency lacks its directed history slot");
        slots.push_back(slot);
        OPS_ADD(options_.ops, neighbor_lookups_executed, 1);
        if (leaf.op == DagOp::OutNeighborView || leaf.op == DagOp::InNeighborView)
            OPS_ADD(options_.ops, physical_projected_views, 1);
        else OPS_ADD(options_.ops, physical_private_descriptors, 1);
        if (options_.ops) options_.ops->physical_profile.recordOperand(
            {StructuralProfile::Backend::RewrittenDag, leaf.id, leaf.parameter.first_query_vertex},
            outgoing ? StructuralProfile::Operand::OutNeighbor : StructuralProfile::Operand::InNeighbor,
            neighbors[j].size());
    }
    std::vector<std::size_t> self_slots;
    for (std::size_t i = 0; i < stride; ++i)
        if (node.history_slots[i].from_query_vertex == q && node.history_slots[i].to_query_vertex == q)
            self_slots.push_back(i);
    if (slots.size() + self_slots.size() != stride)
        throw std::logic_error("P8 producer has unbound directed history slots");

    auto result = PhysicalCandidateBatch::owned({});
    result.owned_candidates.reserve(seed.size());
    result.producer = node.id;
    result.query_vertex = q;
    result.history_stride = stride;
    std::vector<int> row(stride, -1);
    auto bind_edge = [&](const Edge* entry, std::size_t slot, int candidate) {
        if (!entry || entry->to != candidate || entry->temporal_edge_id < 0 ||
            static_cast<std::size_t>(entry->temporal_edge_id) >= G_.temporal_edges.size())
            throw std::logic_error("P8 invalid adjacency entry");
        const auto& edge = G_.temporal_edges[static_cast<std::size_t>(entry->temporal_edge_id)];
        const auto& identity = node.history_slots[slot];
        if (edge.u != resolveBinding(identity.from_query_vertex, state, q, candidate) ||
            edge.v != resolveBinding(identity.to_query_vertex, state, q, candidate))
            throw std::logic_error("P8 bound edge disagrees with directed slot");
        // W absorbs the necessary Lmax guard; exact cumulative WindowAnd and
        // injectivity are still performed by the matcher after this producer.
        row[slot] = entry->temporal_edge_id;
        OPS_ADD(options_.ops, physical_handle_writes, 1);
    };
    for (std::size_t i = 0; i < seed.size(); ++i) {
        const int candidate = seed[i];
        if (candidate < 0 || candidate >= G_.num_vertices ||
            (i && seed[i - 1] >= candidate))
            throw std::logic_error("P8 candidates must be valid, sorted, and unique");
        // This is the exact immutable entry returned by the driver bucket
        // union. Do not repeat the driver's membership lookup.
        bind_edge(selected.driver_entries[i], slots.front(), candidate);
        bool accepted = true;
        for (std::size_t j = 1; j < neighbors.size(); ++j) {
            OPS_ADD(options_.ops, physical_structural_probes, 1);
            const auto found = PhysicalKernels::lowerBound(neighbors[j], candidate);
            if (found == neighbors[j].size() || neighbors[j][found] != candidate) {
                accepted = false; break;
            }
            bind_edge(neighbors[j].edge(found), slots[j], candidate);
        }
        if (accepted) for (const auto slot : self_slots) {
            const CandidateReadView adjacency(G_.adj[static_cast<std::size_t>(candidate)]);
            OPS_ADD(options_.ops, physical_structural_probes, 1);
            const auto found = PhysicalKernels::lowerBound(adjacency, candidate);
            if (found == adjacency.size() || adjacency[found] != candidate) {
                accepted = false; break;
            }
            bind_edge(adjacency.edge(found), slot, candidate);
        }
        if (!accepted) continue;
        result.owned_candidates.push_back(candidate);
        result.temporal_edge_ids.insert(result.temporal_edge_ids.end(), row.begin(), row.end());
        OPS_ADD(options_.ops, physical_handle_copy_bytes, stride * sizeof(int));
    }
    if (result.temporal_edge_ids.size() != result.size() * stride)
        throw std::logic_error("P8 produced an incomplete history row");
    OPS_ADD(options_.ops, physical_probe_selections, 1);
    OPS_ADD(options_.ops, physical_output_elements_materialized, result.size());
    OPS_ADD(options_.ops, physical_raw_candidates, seed.size());
    OPS_ADD(options_.ops, physical_structure_rejections, seed.size() - result.size());
    OPS_ADD(options_.ops, physical_output_candidates, result.size());
    if (options_.ops) {
        // Keep the original S1 costs, not a model recomputed on narrowed R.
        const auto cost = PhysicalKernels::stateCosts(original_seed.size(), neighbors);
        options_.ops->physical_execution.record(node.id, true, seed.size(), result.size(),
                                                cost.probe, cost.merge);
    }
    return result;
}

TemporalSupport DagExecutor::temporalSupportPhysical(int query_vertex, int candidate,
    const SearchState& state, const ProbeHistoryRow* row) {
    if (!row) {
        if (dag_.passes.p4 && query_vertex != plan_.root)
            throw std::logic_error("P4 temporal evaluation requires its frame-owned history row");
        // P5/P6 do not change temporal evaluation; root self-loops also keep
        // their original lookup path. P1/P2/P3 dispatch remains independent.
        return temporalSupportSpecialized<false, false>(query_vertex, candidate, state);
    }
    if (query_vertex < 0 || static_cast<std::size_t>(query_vertex) >= dag_.temporal_roots.size())
        throw std::out_of_range("physical temporal query vertex");
    const auto root = dag_.temporal_roots[static_cast<std::size_t>(query_vertex)];
    if (root == kInvalidDagNode) return {IntervalSetPtr(IntervalSet{}), 0, false};
    auto intervals = evaluateOptimizedIntervalNode(root, query_vertex, candidate, state, row);
    const int longest = intervals->empty() ? 0 : k_threshold_;
    return {std::move(intervals), longest, true, dag_.cumulative_support};
}

CandidateReadView DagExecutor::physicalDescriptor(const DagNode& node, const SearchState& state) {
    if (node.op == DagOp::TdBlockLookup || node.op == DagOp::TdBlockView) {
        OPS_ADD(options_.ops, td_block_lookups_executed, 1);
        const int parent = resolveBinding(node.parameter.second_query_vertex, state, -1, -1);
        const auto* block = td_tree_.candidatesForParent(node.parameter.first_query_vertex, parent);
        OPS_ADD(options_.ops, physical_input_candidates, block ? block->size() : 0);
        if (node.op == DagOp::TdBlockView) OPS_ADD(options_.ops, physical_projected_views, 1);
        else OPS_ADD(options_.ops, physical_private_descriptors, 1);
        if (options_.ops) options_.ops->physical_profile.recordOperand(
            {StructuralProfile::Backend::RewrittenDag, node.id, node.parameter.first_query_vertex},
            StructuralProfile::Operand::TdBlock, block ? block->size() : 0);
        return block ? CandidateReadView(*block) : CandidateReadView{};
    }
    const bool outgoing = node.op == DagOp::OutNeighborLookup || node.op == DagOp::OutNeighborView;
    const bool incoming = node.op == DagOp::InNeighborLookup || node.op == DagOp::InNeighborView;
    if (!outgoing && !incoming) throw std::logic_error("physical descriptor is not a compiled leaf");
    const int anchor = resolveBinding(node.parameter.first_query_vertex, state, -1, -1);
    OPS_ADD(options_.ops, neighbor_lookups_executed, 1);
    const auto* edges = anchor < 0 || anchor >= G_.num_vertices ? nullptr :
        &(outgoing ? G_.adj : G_.in_adj)[static_cast<std::size_t>(anchor)];
    if (node.op == DagOp::OutNeighborView || node.op == DagOp::InNeighborView)
        OPS_ADD(options_.ops, physical_projected_views, 1);
    else OPS_ADD(options_.ops, physical_private_descriptors, 1);
    if (options_.ops) options_.ops->physical_profile.recordOperand(
        {StructuralProfile::Backend::RewrittenDag, node.id, node.parameter.first_query_vertex},
        outgoing ? StructuralProfile::Operand::OutNeighbor : StructuralProfile::Operand::InNeighbor,
        edges ? edges->size() : 0);
    return edges ? CandidateReadView(*edges) : CandidateReadView{};
}

PhysicalCandidateBatch DagExecutor::evaluatePhysicalCandidateNode(DagNodeId id, const SearchState& state) {
    const DagNode& node = dag_.node(id);
    if (node.op == DagOp::FusedEdgeProbe || node.op == DagOp::RuntimeStructuralChoice)
        return evaluatePhysicalChoiceNode(node, state);
    if (node.op == DagOp::TdBlockLookup || node.op == DagOp::TdBlockView ||
        node.op == DagOp::OutNeighborView || node.op == DagOp::InNeighborView)
        return PhysicalCandidateBatch::borrowed(physicalDescriptor(node, state));
    if (node.op == DagOp::OutNeighborLookup || node.op == DagOp::InNeighborLookup) {
        // Mixed physical IR still respects an explicit materializing leaf.
        const auto view = physicalDescriptor(node, state);
        std::vector<int> ids; ids.reserve(view.size());
        for (std::size_t i = 0; i < view.size(); ++i) ids.push_back(view[i]);
        OPS_ADD(options_.ops, physical_neighbor_copies, 1);
        OPS_ADD(options_.ops, physical_neighbor_copy_elements, ids.size());
        OPS_ADD(options_.ops, physical_neighbor_copy_bytes, ids.size() * sizeof(int));
        return PhysicalCandidateBatch::owned(std::move(ids));
    }
    if (node.op != DagOp::StructuralIntersection || node.parents.empty())
        throw std::logic_error("invalid physical candidate node");
    std::vector<PhysicalCandidateBatch> eager;
    if (!node.short_circuit) {
        eager.reserve(node.parents.size());
        for (auto parent : node.parents) eager.push_back(evaluatePhysicalCandidateNode(parent, state));
    }
    auto result = node.short_circuit ? evaluatePhysicalCandidateNode(node.parents.front(), state) : std::move(eager.front());
    const auto input_size = result.size();
    for (std::size_t i = 1; i < node.parents.size(); ++i) {
        if (result.empty()) {
            OPS_ADD(options_.ops, structural_intersections_skipped, node.parents.size() - i);
            if (node.short_circuit) for (std::size_t j = i; j < node.parents.size(); ++j) recordSkippedSubtree(node.parents[j]);
            break;
        }
        auto rhs = node.short_circuit ? evaluatePhysicalCandidateNode(node.parents[i], state) : std::move(eager[i]);
        result = PhysicalCandidateBatch::owned(PhysicalKernels::intersectViews(
            result.view(), rhs.view(), node.adaptive_intersection, options_.ops, node.id));
    }
    OPS_ADD(options_.ops, physical_merge_selections, 1);
    if (options_.ops) options_.ops->physical_execution.record(node.id, false, input_size, result.size(), 0, 0);
    return result;
}

PhysicalCandidateBatch DagExecutor::evaluatePhysicalChoiceNode(const DagNode& node, const SearchState& state) {
    const bool fused = node.op == DagOp::FusedEdgeProbe;
    if (node.parents.empty()) throw std::logic_error("physical strategy node lacks TD seed");
    const int q = node.parameter.first_query_vertex;
    const auto seed = physicalDescriptor(dag_.node(node.parents.front()), state);
    std::vector<CandidateReadView> neighbors;
    std::vector<std::size_t> slots;
    neighbors.reserve(node.parents.size() - 1);
    slots.reserve(node.parents.size() - 1);
    for (std::size_t i = 1; i < node.parents.size(); ++i) {
        const auto& leaf = dag_.node(node.parents[i]);
        neighbors.push_back(physicalDescriptor(leaf, state));
        if (fused) {
            const bool outgoing = leaf.op == DagOp::OutNeighborLookup || leaf.op == DagOp::OutNeighborView;
            const int from = outgoing ? leaf.parameter.first_query_vertex : q;
            const int to = outgoing ? q : leaf.parameter.first_query_vertex;
            std::size_t slot = 0;
            while (slot < node.history_slots.size() &&
                   (node.history_slots[slot].from_query_vertex != from || node.history_slots[slot].to_query_vertex != to)) ++slot;
            if (slot == node.history_slots.size()) throw std::logic_error("physical adjacency lacks directed history slot");
            slots.push_back(slot);
        }
    }
    const auto cost = node.structural_policy == DagStructuralPolicy::StateCost || options_.ops
        ? PhysicalKernels::stateCosts(seed.size(), neighbors) : PhysicalKernels::StateCosts{};
    const bool probe = node.structural_policy == DagStructuralPolicy::Probe ||
        (node.structural_policy == DagStructuralPolicy::StateCost && PhysicalKernels::chooseProbe(cost, node.physical_alpha));
    OPS_ADD(options_.ops, physical_probe_selections, probe ? 1 : 0);
    OPS_ADD(options_.ops, physical_merge_selections, probe ? 0 : 1);
    const std::size_t stride = fused ? node.history_slots.size() : 0;
    if (stride && seed.size() > std::numeric_limits<std::size_t>::max() / stride)
        throw std::overflow_error("physical history row size overflow");
    std::vector<std::size_t> self_slots;
    if (fused) for (std::size_t i = 0; i < stride; ++i) {
        const auto& slot = node.history_slots[i];
        if (slot.from_query_vertex == q && slot.to_query_vertex == q) self_slots.push_back(i);
    }
    if (fused && slots.size() + self_slots.size() != stride)
        throw std::logic_error("physical producer did not bind every directed history slot");
    std::size_t lmax_rejected = 0;
    auto accept_edge = [&](const Edge* entry, std::size_t slot, int candidate) {
        if (!entry || entry->temporal_edge_id < 0 ||
            static_cast<std::size_t>(entry->temporal_edge_id) >= G_.temporal_edges.size())
            throw std::logic_error("physical adjacency has invalid temporal edge ID");
        const auto& edge = G_.temporal_edges[static_cast<std::size_t>(entry->temporal_edge_id)];
        const auto& identity = node.history_slots[slot];
        const int from = resolveBinding(identity.from_query_vertex, state, q, candidate);
        const int to = resolveBinding(identity.to_query_vertex, state, q, candidate);
        if (edge.u != from || edge.v != to) throw std::logic_error("physical adjacency direction disagrees with slot");
        if (options_.window_guards) return true;
        OPS_ADD(options_.ops, physical_lmax_checks, 1);
        OPS_ADD(options_.ops, history_guard_checks, 1);
        if (edge.maximumConsecutiveRun() < k_threshold_) {
            if (options_.ops) ++lmax_rejected;
            OPS_ADD(options_.ops, physical_lmax_rejections, 1);
            OPS_ADD(options_.ops, history_guard_failures, 1);
            return false;
        }
        return true;
    };
    auto self_edge = [&](int candidate) -> const Edge* {
        if (candidate < 0 || candidate >= G_.num_vertices) throw std::logic_error("invalid physical candidate");
        const auto view = CandidateReadView(G_.adj[static_cast<std::size_t>(candidate)]);
        OPS_ADD(options_.ops, physical_structural_probes, 1);
        const auto index = PhysicalKernels::lowerBound(view, candidate);
        return index < view.size() && view[index] == candidate ? view.edge(index) : nullptr;
    };
    auto result = PhysicalCandidateBatch::borrowed(seed);
    if (probe) {
        result = PhysicalCandidateBatch::owned({});
        result.owned_candidates.reserve(seed.size());
        std::vector<int> row(stride, -1);
        for (std::size_t i = 0; i < seed.size(); ++i) {
            const int candidate = seed[i];
            bool accepted = true;
            for (std::size_t j = 0; j < neighbors.size(); ++j) {
                OPS_ADD(options_.ops, physical_structural_probes, 1);
                const auto found = PhysicalKernels::lowerBound(neighbors[j], candidate);
                if (found == neighbors[j].size() || neighbors[j][found] != candidate) { accepted = false; break; }
                if (fused) {
                    const auto* entry = neighbors[j].edge(found);
                    if (!accept_edge(entry, slots[j], candidate)) { accepted = false; break; }
                    row[slots[j]] = entry->temporal_edge_id;
                    OPS_ADD(options_.ops, physical_handle_writes, 1);
                }
            }
            if (accepted) for (auto slot : self_slots) {
                const auto* entry = self_edge(candidate);
                if (!entry || !accept_edge(entry, slot, candidate)) { accepted = false; break; }
                row[slot] = entry->temporal_edge_id;
                OPS_ADD(options_.ops, physical_handle_writes, 1);
            }
            if (!accepted) continue;
            result.owned_candidates.push_back(candidate);
            if (stride) {
                result.temporal_edge_ids.insert(result.temporal_edge_ids.end(), row.begin(), row.end());
                OPS_ADD(options_.ops, physical_handle_copy_bytes, stride * sizeof(int));
            }
        }
        OPS_ADD(options_.ops, physical_output_elements_materialized, result.size());
    } else {
        for (std::size_t j = 0; j < neighbors.size(); ++j) {
            if (result.empty()) {
                OPS_ADD(options_.ops, structural_intersections_skipped, neighbors.size() - j);
                break;
            }
            if (!fused) {
                result = PhysicalCandidateBatch::owned(PhysicalKernels::intersectViews(
                    result.view(), neighbors[j], false, options_.ops, node.id));
                continue;
            }
            // Merge yields the exact adjacency entry, so its edge handle is
            // collected now. No membership re-probe occurs after the merge.
            auto next = PhysicalCandidateBatch::owned({});
            next.owned_candidates.reserve(std::min(result.size(), neighbors[j].size()));
            const auto current = result.view();
            std::size_t a = 0, b = 0;
            OPS_ADD(options_.ops, structural_intersections_executed, 1);
            while (a < current.size() && b < neighbors[j].size()) {
                if (current[a] < neighbors[j][b]) { ++a; continue; }
                if (neighbors[j][b] < current[a]) { ++b; continue; }
                const auto* entry = neighbors[j].edge(b);
                if (accept_edge(entry, slots[j], current[a])) {
                    next.owned_candidates.push_back(current[a]);
                    const auto offset = next.temporal_edge_ids.size();
                    if (j == 0) next.temporal_edge_ids.resize(offset + stride, -1);
                    else {
                        next.temporal_edge_ids.insert(next.temporal_edge_ids.end(),
                            result.temporal_edge_ids.begin() + static_cast<std::ptrdiff_t>(a * stride),
                            result.temporal_edge_ids.begin() + static_cast<std::ptrdiff_t>((a + 1) * stride));
                        OPS_ADD(options_.ops, physical_handle_copy_bytes, stride * sizeof(int));
                    }
                    next.temporal_edge_ids[offset + slots[j]] = entry->temporal_edge_id;
                    OPS_ADD(options_.ops, physical_handle_writes, 1);
                }
                ++a; ++b;
            }
            if (options_.ops) options_.ops->physical_profile.recordBinary(
                {StructuralProfile::Backend::RewrittenDag, node.id, q}, current.size(), neighbors[j].size(), next.size(), false);
            OPS_ADD(options_.ops, intersection_output_elements, next.size());
            OPS_ADD(options_.ops, physical_output_elements_materialized, next.size());
            result = std::move(next);
        }
        if (!self_slots.empty() && !result.empty()) {
            auto next = PhysicalCandidateBatch::owned({});
            next.owned_candidates.reserve(result.size());
            for (std::size_t i = 0; i < result.size(); ++i) {
                const int candidate = result.candidate(i);
                const auto* entry = self_edge(candidate);
                if (!entry || !accept_edge(entry, self_slots.front(), candidate)) continue;
                next.owned_candidates.push_back(candidate);
                const auto offset = next.temporal_edge_ids.size();
                if (neighbors.empty()) next.temporal_edge_ids.resize(offset + stride, -1);
                else {
                    next.temporal_edge_ids.insert(next.temporal_edge_ids.end(),
                        result.temporal_edge_ids.begin() + static_cast<std::ptrdiff_t>(i * stride),
                        result.temporal_edge_ids.begin() + static_cast<std::ptrdiff_t>((i + 1) * stride));
                    OPS_ADD(options_.ops, physical_handle_copy_bytes, stride * sizeof(int));
                }
                for (auto slot : self_slots) {
                    next.temporal_edge_ids[offset + slot] = entry->temporal_edge_id;
                    OPS_ADD(options_.ops, physical_handle_writes, 1);
                }
            }
            OPS_ADD(options_.ops, physical_output_elements_materialized, next.size());
            result = std::move(next);
        }
    }
    if (fused) {
        result.producer = node.id; result.query_vertex = q; result.history_stride = stride;
        result.lmax_rejected_candidates = lmax_rejected;
        if (result.temporal_edge_ids.size() != result.size() * stride)
            throw std::logic_error("physical result history rows are incomplete");
    }
    if (options_.ops) options_.ops->physical_execution.record(node.id, probe, seed.size(), result.size(), cost.probe, cost.merge);
    return result;
}

// The executor interprets the compiler's rewritten node kinds and explicit
// parent order. It does not choose rewrites or reorder operands at run time.
void DagExecutor::recordSkippedSubtree(DagNodeId id) const {
    if (options_.ops == nullptr) return;
    const DagNode& node = dag_.node(id);
    const std::size_t merges = node.parents.empty() ? 0 : node.parents.size() - 1;
    switch (node.op) {
        case DagOp::TdBlockLookup:
        case DagOp::TdBlockView:
            OPS_ADD(options_.ops, td_block_lookups_skipped, 1); break;
        case DagOp::OutNeighborLookup:
        case DagOp::InNeighborLookup:
        case DagOp::OutNeighborView:
        case DagOp::InNeighborView:
            OPS_ADD(options_.ops, neighbor_lookups_skipped, 1); break;
        case DagOp::StructuralIntersection:
            OPS_ADD(options_.ops, structural_intersections_skipped, merges); break;
        case DagOp::EdgeTimeLookup:
        case DagOp::GuardedEdgeTimeLookup:
            OPS_ADD(options_.ops, history_lookups_skipped, 1);
            OPS_ADD(options_.ops, kappa_skipped, 1);
            break;
        case DagOp::BoundEdgeTimeLookup:
            // Parent is an already materialized frame row, not a lazy subtree.
            OPS_ADD(options_.ops, history_lookups_skipped, 1);
            OPS_ADD(options_.ops, kappa_skipped, 1);
            return;
        case DagOp::FusedEdgeProbe:
        case DagOp::RuntimeStructuralChoice:
            break;
        case DagOp::TemporalIntersection:
            OPS_ADD(options_.ops, time_intersections_skipped, merges);
            OPS_ADD(options_.ops, kappa_skipped, merges);
            break;
        case DagOp::KappaNormalize:
            OPS_ADD(options_.ops, kappa_skipped, 1); break;
        case DagOp::BranchSupportInput:
        case DagOp::LongestConsecutiveRun: break;
    }
    for (DagNodeId parent : node.parents) recordSkippedSubtree(parent);
}

DagExecutor::CandidateSetPtr DagExecutor::evaluateOptimizedCandidateNode(
    DagNodeId id, const SearchState& state) {
    const DagNode& node = dag_.node(id);
    if (node.op == DagOp::TdBlockLookup) {
        OPS_ADD(options_.ops, td_block_lookups_executed, 1);
        const int parent = resolveBinding(node.parameter.second_query_vertex, state, -1, -1);
        const auto* block = td_tree_.candidatesForParent(node.parameter.first_query_vertex, parent);
        if (options_.ops != nullptr) options_.ops->physical_profile.recordOperand(
            {StructuralProfile::Backend::RewrittenDag, node.id, node.parameter.first_query_vertex},
            StructuralProfile::Operand::TdBlock, block == nullptr ? 0 : block->size());
        return block == nullptr ? CandidateSetPtr(CandidateSet{}) : CandidateSetPtr(block);
    }
    if (node.op == DagOp::OutNeighborLookup || node.op == DagOp::InNeighborLookup) {
        OPS_ADD(options_.ops, neighbor_lookups_executed, 1);
        const int anchor = resolveBinding(node.parameter.first_query_vertex, state, -1, -1);
        CandidateSet result;
        if (anchor >= 0 && anchor < G_.num_vertices) {
            const auto& adjacency = node.op == DagOp::OutNeighborLookup ? G_.adj : G_.in_adj;
            const auto& edges = adjacency[static_cast<std::size_t>(anchor)];
            result.reserve(edges.size());
            const bool time_copy = options_.adaptive != nullptr &&
                options_.adaptive->neighborCopyTimerEnabled();
            if (time_copy) {
                options_.adaptive->copyNeighborIdsTimed(edges, result,
                    node.parameter.first_query_vertex, node.id);
            } else {
                for (const Edge& edge : edges) result.push_back(edge.to);
            }
        }
        if (options_.adaptive != nullptr && options_.adaptive->stateCountersEnabled()) {
            options_.adaptive->recordNeighborCopy(node.parameter.first_query_vertex, node.id,
                result.size(), sizeof(CandidateSet::value_type));
        }
        OPS_ADD(options_.ops, physical_neighbor_copies, 1);
        OPS_ADD(options_.ops, physical_neighbor_copy_elements, result.size());
        OPS_ADD(options_.ops, physical_neighbor_copy_bytes, result.size() * sizeof(int));
        if (options_.ops != nullptr) {
            const StructuralProfile::Context context{
                StructuralProfile::Backend::RewrittenDag, node.id, node.parameter.first_query_vertex};
            options_.ops->physical_profile.recordOperand(context,
                node.op == DagOp::OutNeighborLookup ? StructuralProfile::Operand::OutNeighbor : StructuralProfile::Operand::InNeighbor,
                result.size());
            options_.ops->physical_profile.recordCopy(context,
                StructuralProfile::Copy::NeighborMaterialization, result.size(), sizeof(CandidateSet::value_type));
        }
        return CandidateSetPtr(std::move(result));
    }
    if (node.op != DagOp::StructuralIntersection || node.parents.empty()) {
        throw std::logic_error("invalid rewritten structural node");
    }
    CandidateSet result = *evaluateOptimizedCandidateNode(node.parents.front(), state);
    OPS_ADD(options_.ops, physical_seed_copy_elements, result.size());
    OPS_ADD(options_.ops, physical_seed_copy_bytes, result.size() * sizeof(int));
    if (options_.ops != nullptr) options_.ops->physical_profile.recordCopy(
        {StructuralProfile::Backend::RewrittenDag, node.id, -1},
        StructuralProfile::Copy::StructuralSeed, result.size(), sizeof(CandidateSet::value_type));
    for (std::size_t i = 1; i < node.parents.size(); ++i) {
        if (node.short_circuit && result.empty()) {
            OPS_ADD(options_.ops, structural_intersections_skipped, node.parents.size() - i);
            for (std::size_t j = i; j < node.parents.size(); ++j) recordSkippedSubtree(node.parents[j]);
            break;
        }
        const CandidateSetPtr rhs = evaluateOptimizedCandidateNode(node.parents[i], state);
        result = OperatorKernels::intersectCandidates(
            result, *rhs, node.adaptive_intersection, options_.ops,
            {StructuralProfile::Backend::RewrittenDag, node.id, -1});
        OPS_ADD(options_.ops, physical_output_elements_materialized, result.size());
    }
    return CandidateSetPtr(std::move(result));
}

DagExecutor::IntervalSetPtr DagExecutor::evaluateOptimizedIntervalNode(
    DagNodeId id, int query_vertex, int candidate, const SearchState& state,
    const ProbeHistoryRow* row) {
    const DagNode& node = dag_.node(id);
    if (node.op == DagOp::BranchSupportInput) {
        if (state.has_active_intervals) return IntervalSetPtr(&state.active_intervals);
        return IntervalSetPtr(IntervalSet{{std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}});
    }
    if (node.op == DagOp::KappaNormalize) {
        if (node.parents.size() != 1) throw std::logic_error("kappa node arity must be one");
        const IntervalSetPtr value = evaluateOptimizedIntervalNode(
            node.parents.front(), query_vertex, candidate, state, row);
        return IntervalSetPtr(OperatorKernels::kappa(*value, k_threshold_, options_.ops));
    }
    if (node.op == DagOp::EdgeTimeLookup || node.op == DagOp::GuardedEdgeTimeLookup ||
        node.op == DagOp::BoundEdgeTimeLookup) {
        const int source = resolveBinding(node.parameter.first_query_vertex, state, query_vertex, candidate);
        const int target = resolveBinding(node.parameter.second_query_vertex, state, query_vertex, candidate);
        const TemporalEdge* edge = nullptr;
        if (node.op == DagOp::BoundEdgeTimeLookup) {
            if (!node.lmax_proven || !row || row->producer != node.bound_producer || row->query_vertex != query_vertex ||
                row->candidate != candidate || node.bound_slot >= row->size || !row->temporal_edge_ids)
                throw std::logic_error("bound history received a missing or foreign frame row");
            const int edge_id = row->temporal_edge_ids[node.bound_slot];
            if (edge_id < 0 || static_cast<std::size_t>(edge_id) >= G_.temporal_edges.size())
                throw std::logic_error("bound history has an invalid edge ID");
            edge = &G_.temporal_edges[static_cast<std::size_t>(edge_id)];
            if (edge->u != source || edge->v != target)
                throw std::logic_error("bound history edge direction disagrees with compiled slot");
            OPS_ADD(options_.ops, physical_history_handle_reads, 1);
        } else {
            edge = G_.findTemporalEdge(source, target);
        }
        if (node.op == DagOp::GuardedEdgeTimeLookup) {
            OPS_ADD(options_.ops, history_guard_checks, 1);
            // P2 correctness: every answer edge contains the common k-run;
            // hence an edge with cached Lmax<k cannot participate. No interval
            // payload is accessed on this rejecting path.
            if (edge == nullptr || edge->maximumConsecutiveRun() < k_threshold_) {
                OPS_ADD(options_.ops, history_guard_failures, 1);
                OPS_ADD(options_.ops, history_lookups_skipped, 1);
                OPS_ADD(options_.ops, kappa_skipped, 1);
                return IntervalSetPtr(IntervalSet{});
            }
        }
        OPS_ADD(options_.ops, history_lookups_executed, 1);
        if (edge == nullptr) return IntervalSetPtr(IntervalSet{});
        OPS_ADD(options_.ops, physical_history_payload_reads, 1);
        const IntervalSet& intervals = edge->activeIntervals();
        if (node.raw_intervals) {
            OPS_ADD(options_.ops, kappa_skipped, 1);
            OPS_ADD(options_.ops, time_input_elements, intervals.size());
            return IntervalSetPtr(&intervals);
        }
        // Preserve the original no-memo borrowed-view fast path for P2-only
        // and P3-only. A guard/order pass must not introduce an unrelated
        // materialization when every existing interval is already durable.
        OPS_ADD(options_.ops, kappa_executed, 1);
        OPS_ADD(options_.ops, time_input_elements, intervals.size());
        const bool all_durable = std::all_of(intervals.begin(), intervals.end(),
            [this](const TimeInterval& interval) {
                OPS_ADD(options_.ops, time_elements_read, 1);
                return interval.length() >= k_threshold_;
            });
        if (all_durable) return IntervalSetPtr(&intervals);
        IntervalSet result;
        result.reserve(intervals.size());
        for (const auto& interval : intervals) {
            OPS_ADD(options_.ops, time_elements_read, 1);
            if (interval.length() >= k_threshold_) result.push_back(interval);
        }
        return IntervalSetPtr(std::move(result));
    }
    if (node.op != DagOp::TemporalIntersection || node.parents.empty()) {
        throw std::logic_error("invalid rewritten temporal node");
    }
    const int minimum_length = node.raw_intervals ? 1 : k_threshold_;
    std::vector<IntervalSetPtr> eager;
    if (!node.short_circuit) {
        eager.reserve(node.parents.size());
        for (DagNodeId parent : node.parents) {
            eager.push_back(evaluateOptimizedIntervalNode(parent, query_vertex, candidate, state, row));
        }
    }
    IntervalSet result = node.short_circuit
        ? *evaluateOptimizedIntervalNode(node.parents.front(), query_vertex, candidate, state, row)
        : *eager.front();
    for (std::size_t i = 1; i < node.parents.size(); ++i) {
        if (result.empty()) {
            OPS_ADD(options_.ops, time_intersections_skipped, node.parents.size() - i);
            OPS_ADD(options_.ops, kappa_skipped, node.parents.size() - i);
            if (node.short_circuit) {
                for (std::size_t j = i; j < node.parents.size(); ++j) recordSkippedSubtree(node.parents[j]);
            }
            break;
        }
        const IntervalSetPtr rhs = node.short_circuit
            ? evaluateOptimizedIntervalNode(node.parents[i], query_vertex, candidate, state, row)
            : eager[i];
        if (node.raw_intervals) OPS_ADD(options_.ops, kappa_skipped, 1);
        result = OperatorKernels::intersectIntervals(
            result, *rhs, minimum_length, node.adaptive_intersection, options_.ops);
    }
    return IntervalSetPtr(std::move(result));
}

template DagExecutor::CandidateSetPtr
DagExecutor::structuralCandidatesSpecialized<false, false>(
    int query_vertex,
    const SearchState& state);
template TemporalSupport
DagExecutor::temporalSupportSpecialized<false, false>(
    int query_vertex,
    int candidate,
    const SearchState& state);
#else
template DagExecutor::CandidateSetPtr
DagExecutor::structuralCandidatesSpecialized<false, true>(
    int query_vertex,
    const SearchState& state);
template TemporalSupport
DagExecutor::temporalSupportSpecialized<false, true>(
    int query_vertex,
    int candidate,
    const SearchState& state);
#if !defined(DSM_ERASE_SEARCH_STATISTICS)
template DagExecutor::CandidateSetPtr
DagExecutor::structuralCandidatesSpecialized<true, true>(
    int query_vertex,
    const SearchState& state);
template DagExecutor::CandidateSetPtr
DagExecutor::structuralCandidatesSpecialized<true, false>(
    int query_vertex,
    const SearchState& state);
template TemporalSupport
DagExecutor::temporalSupportSpecialized<true, true>(
    int query_vertex,
    int candidate,
    const SearchState& state);
template TemporalSupport
DagExecutor::temporalSupportSpecialized<true, false>(
    int query_vertex,
    int candidate,
    const SearchState& state);
#endif
#endif

#if !defined(DSM_GOPHER_OFF_TRANSLATION_UNIT)
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
            for (const auto& interval : edge->activeIntervals()) {
                if (interval.length() >= k_threshold_) {
                    result.push_back(interval);
                }
            }
            have_result = true;
        } else {
            result = intersectTimeIntervals(
                result, edge->activeIntervals(), k_threshold_);
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

void DagExecutor::validateOperatorStatistics(
    SearchStatistics& statistics,
    const MemoSnapshot& structural,
    const MemoSnapshot& temporal,
    const MemoSnapshot& longest) const {
    auto checked_add = [](
                           std::uint64_t lhs,
                           std::uint64_t rhs,
                           const char* label) {
        if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
            throw std::overflow_error(
                std::string("operator-statistics reconciliation overflow: ") +
                label);
        }
        return lhs + rhs;
    };
    auto sum_range = [&](
                         const std::array<std::uint64_t, kDagOpCount>& values,
                         std::size_t begin,
                         std::size_t end,
                         const char* label) {
        std::uint64_t total = 0;
        for (std::size_t index = begin; index < end; ++index) {
            total = checked_add(total, values[index], label);
        }
        return total;
    };
    auto require_equal = [](
                             std::uint64_t actual,
                             std::uint64_t expected,
                             const char* label) {
        if (actual != expected) {
            throw std::logic_error(
                std::string("operator-statistics reconciliation failed: ") +
                label);
        }
    };

    const auto& invocations =
        statistics.dag_operator_invocations_by_op;
    const auto& hits = statistics.dag_operator_cache_hits_by_op;
    const auto& misses = statistics.dag_operator_cache_misses_by_op;
    const auto& compute =
        statistics.dag_operator_exclusive_compute_nanoseconds_by_op;
    const auto& miss_compute =
        statistics
            .dag_operator_exclusive_miss_compute_nanoseconds_by_op;
    const auto& observations =
        statistics.dag_operator_result_observations_by_op;
    const auto& result_units =
        statistics.dag_operator_result_units_sum_by_op;
    const auto& admissions =
        statistics.dag_operator_cache_admissions_by_op;
    const auto& admitted_bytes =
        statistics.dag_operator_cache_admitted_bytes_by_op;

    for (std::size_t operation = 0;
         operation < kDagOpCount;
         ++operation) {
        require_equal(
            observations[operation],
            invocations[operation],
            "every logical invocation must have one successful result");
        const std::uint64_t probes = checked_add(
            hits[operation],
            misses[operation],
            "per-op cache probes");
        if (probes > invocations[operation]) {
            throw std::logic_error(
                "operator-statistics reconciliation failed: cache probes "
                "exceed logical invocations");
        }
        if (miss_compute[operation] > compute[operation]) {
            throw std::logic_error(
                "operator-statistics reconciliation failed: miss compute "
                "exceeds all exclusive compute");
        }
        if (admissions[operation] > misses[operation]) {
            throw std::logic_error(
                "operator-statistics reconciliation failed: admissions "
                "exceed cache misses");
        }
    }

    require_equal(
        invocations[dagOpIndex(DagOp::TdBlockLookup)],
        statistics.logical_td_block_lookups,
        "TD-block invocation total");
    require_equal(
        checked_add(
            invocations[dagOpIndex(DagOp::OutNeighborLookup)],
            invocations[dagOpIndex(DagOp::InNeighborLookup)],
            "neighbor invocations"),
        statistics.logical_neighbor_lookups,
        "out/in invocation split");
    require_equal(
        invocations[dagOpIndex(DagOp::StructuralIntersection)],
        statistics.logical_structural_intersections,
        "structural-intersection invocation total");
    require_equal(
        invocations[dagOpIndex(DagOp::EdgeTimeLookup)],
        statistics.logical_temporal_edge_lookups,
        "edge-time invocation total");
    require_equal(
        invocations[dagOpIndex(DagOp::TemporalIntersection)],
        statistics.logical_temporal_intersections,
        "temporal-intersection invocation total");
    require_equal(
        invocations[dagOpIndex(DagOp::LongestConsecutiveRun)],
        statistics.logical_longest_run,
        "longest-run invocation total");

    require_equal(
        hits[dagOpIndex(DagOp::TdBlockLookup)],
        statistics.td_block_cache_hits,
        "TD-block hit total");
    require_equal(
        checked_add(
            hits[dagOpIndex(DagOp::OutNeighborLookup)],
            hits[dagOpIndex(DagOp::InNeighborLookup)],
            "neighbor hits"),
        statistics.neighbor_lookup_cache_hits,
        "out/in hit split");
    require_equal(
        hits[dagOpIndex(DagOp::StructuralIntersection)],
        statistics.structural_intersection_cache_hits,
        "structural-intersection hit total");
    require_equal(
        hits[dagOpIndex(DagOp::EdgeTimeLookup)],
        statistics.edge_time_cache_hits,
        "edge-time hit total");
    require_equal(
        hits[dagOpIndex(DagOp::TemporalIntersection)],
        statistics.temporal_intersection_cache_hits,
        "temporal-intersection hit total");
    require_equal(
        hits[dagOpIndex(DagOp::LongestConsecutiveRun)],
        statistics.longest_run_cache_hits,
        "longest-run hit total");

    const std::uint64_t structural_hits =
        sum_range(hits, 0, 4, "structural hits");
    const std::uint64_t structural_misses =
        sum_range(misses, 0, 4, "structural misses");
    const std::uint64_t interval_hits =
        sum_range(hits, 4, 6, "interval hits");
    const std::uint64_t interval_misses =
        sum_range(misses, 4, 6, "interval misses");
    const std::uint64_t longest_hits =
        hits[dagOpIndex(DagOp::LongestConsecutiveRun)];
    const std::uint64_t longest_misses =
        misses[dagOpIndex(DagOp::LongestConsecutiveRun)];
    require_equal(structural_hits, structural.hits, "structural store hits");
    require_equal(
        structural_misses,
        structural.misses,
        "structural store misses");
    require_equal(interval_hits, temporal.hits, "interval store hits");
    require_equal(
        interval_misses,
        temporal.misses,
        "interval store misses");
    require_equal(longest_hits, longest.hits, "longest store hits");
    require_equal(longest_misses, longest.misses, "longest store misses");

    const std::uint64_t structural_admissions =
        sum_range(admissions, 0, 4, "structural admissions");
    const std::uint64_t interval_admissions =
        sum_range(admissions, 4, 6, "interval admissions");
    const std::uint64_t longest_admissions =
        admissions[dagOpIndex(DagOp::LongestConsecutiveRun)];
    require_equal(
        structural_admissions,
        structural.admitted,
        "structural store admissions");
    require_equal(
        interval_admissions,
        temporal.admitted,
        "interval store admissions");
    require_equal(
        longest_admissions,
        longest.admitted,
        "longest store admissions");

    const std::uint64_t structural_bytes =
        sum_range(admitted_bytes, 0, 4, "structural admitted bytes");
    const std::uint64_t interval_bytes =
        sum_range(admitted_bytes, 4, 6, "interval admitted bytes");
    const std::uint64_t longest_bytes =
        admitted_bytes[dagOpIndex(DagOp::LongestConsecutiveRun)];
    require_equal(structural_bytes, structural.bytes, "structural store bytes");
    require_equal(interval_bytes, temporal.bytes, "interval store bytes");
    require_equal(longest_bytes, longest.bytes, "longest store bytes");

    require_equal(
        sum_range(compute, 0, 4, "structural exclusive compute"),
        statistics.structural_compute_nanoseconds,
        "structural exclusive compute aggregate");
    require_equal(
        sum_range(compute, 4, 6, "temporal exclusive compute"),
        statistics.temporal_compute_nanoseconds,
        "temporal exclusive compute aggregate");
    require_equal(
        compute[dagOpIndex(DagOp::LongestConsecutiveRun)],
        statistics.longest_compute_nanoseconds,
        "longest exclusive compute aggregate");
    require_equal(
        sum_range(
            statistics.runtime_key_construction_nanoseconds_by_op,
            0,
            kDagOpCount,
            "key-construction time"),
        statistics.runtime_key_construction_nanoseconds,
        "key-construction time aggregate");
    require_equal(
        sum_range(
            statistics.cache_lookup_nanoseconds_by_op,
            0,
            kDagOpCount,
            "cache-lookup time"),
        statistics.cache_lookup_nanoseconds,
        "cache-lookup time aggregate");

    if (statistics.exact_runtime_key_diagnostics) {
        std::uint64_t exact_invocations = 0;
        std::uint64_t exact_distinct = 0;
        for (std::size_t operation = 0;
             operation < kDagOpCount;
             ++operation) {
            require_equal(
                invocations[operation],
                statistics
                    .exact_runtime_key_invocations_by_op[operation],
                "exact-key/logical invocation equality");
            if (statistics.exact_runtime_key_distinct_by_op[operation] >
                statistics.exact_runtime_key_invocations_by_op[operation]) {
                throw std::logic_error(
                    "operator-statistics reconciliation failed: exact "
                    "distinct keys exceed invocations");
            }
            exact_invocations = checked_add(
                exact_invocations,
                statistics
                    .exact_runtime_key_invocations_by_op[operation],
                "exact invocation total");
            exact_distinct = checked_add(
                exact_distinct,
                statistics
                    .exact_runtime_key_distinct_by_op[operation],
                "exact distinct-key total");
        }
        require_equal(
            exact_invocations,
            statistics.exact_runtime_key_invocations,
            "exact invocation aggregate");
        require_equal(
            exact_distinct,
            statistics.exact_runtime_key_distinct,
            "exact distinct-key aggregate");
    }
    require_equal(
        statistics.longest_run_returned_length_observations,
        observations[dagOpIndex(DagOp::LongestConsecutiveRun)],
        "longest returned-length observations");
    require_equal(
        result_units[dagOpIndex(DagOp::LongestConsecutiveRun)],
        observations[dagOpIndex(DagOp::LongestConsecutiveRun)],
        "longest scalar result-unit total");

    // The legacy aggregate is intentionally asymmetric: temporal cache
    // hits/misses cover interval entries only, whereas its admission/byte
    // totals include the separate longest-run store.  Validate that contract
    // explicitly instead of accidentally treating the fields as symmetric.
    require_equal(
        statistics.structural_cache_hits,
        structural.hits,
        "reported structural hits");
    require_equal(
        statistics.structural_cache_misses,
        structural.misses,
        "reported structural misses");
    require_equal(
        statistics.temporal_cache_hits,
        temporal.hits,
        "reported interval-only temporal hits");
    require_equal(
        statistics.temporal_cache_misses,
        temporal.misses,
        "reported interval-only temporal misses");
    require_equal(
        statistics.longest_cache_hits,
        longest.hits,
        "reported longest hits");
    require_equal(
        statistics.longest_cache_misses,
        longest.misses,
        "reported longest misses");
    require_equal(
        statistics.structural_cache_admitted,
        structural.admitted,
        "reported structural admissions");
    require_equal(
        statistics.temporal_cache_admitted,
        checked_add(
            temporal.admitted,
            longest.admitted,
            "reported temporal-plus-longest admissions"),
        "reported temporal-plus-longest admissions");
    require_equal(
        statistics.structural_cache_bytes,
        structural.bytes,
        "reported structural bytes");
    require_equal(
        statistics.temporal_cache_bytes,
        checked_add(
            temporal.bytes,
            longest.bytes,
            "reported temporal-plus-longest bytes"),
        "reported temporal-plus-longest bytes");
    require_equal(
        structural.entries,
        structural.admitted,
        "structural entries/admissions");
    require_equal(
        temporal.entries,
        temporal.admitted,
        "interval entries/admissions");
    require_equal(
        longest.entries,
        longest.admitted,
        "longest entries/admissions");
    require_equal(
        statistics.structural_cache_entries,
        structural.entries,
        "reported structural entries");
    require_equal(
        statistics.temporal_cache_entries,
        checked_add(
            temporal.entries,
            longest.entries,
            "reported temporal-plus-longest entries"),
        "reported temporal-plus-longest entries");

    statistics.dag_operator_reconciliation_passed = true;
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
    statistics.structural_v2_intersection_nodes =
        structural_v2_intersection_nodes_;
    statistics.structural_v2_eligible_nodes =
        structural_v2_eligible_nodes_;
    statistics.structural_v2_no_projection_reuse_nodes =
        structural_v2_intersection_nodes_ -
        structural_v2_eligible_nodes_;

    const RuntimeKeyDiagnosticsSnapshot runtime_keys =
        runtimeKeyDiagnostics();
    statistics.exact_runtime_key_diagnostics = runtime_keys.exact;
    statistics.exact_runtime_key_cap = runtime_keys.cap;
    statistics.exact_runtime_key_invocations =
        runtime_keys.invocations;
    statistics.exact_runtime_key_distinct =
        runtime_keys.distinct_keys;
    statistics.exact_runtime_key_invocations_by_op =
        runtime_keys.invocations_by_op;
    statistics.exact_runtime_key_distinct_by_op =
        runtime_keys.distinct_keys_by_op;
    if (runtime_keys.exact) {
        // Exact collection times every logical key build even when the
        // ordinary SearchStatisticsCollector is disabled.  It is therefore
        // the authoritative source for the diagnostic run.
        statistics.runtime_key_construction_nanoseconds =
            runtime_keys.key_construction_nanoseconds;
        statistics.runtime_key_construction_nanoseconds_by_op =
            runtime_keys.key_construction_nanoseconds_by_op;
        statistics.cache_lookup_nanoseconds =
            runtime_keys.cache_lookup_nanoseconds;
        statistics.cache_lookup_nanoseconds_by_op =
            runtime_keys.cache_lookup_nanoseconds_by_op;
    }
    if (options_.collect_statistics &&
        !kSearchStatisticsHooksErasedAtBuild) {
        validateOperatorStatistics(
            statistics,
            structural,
            temporal,
            longest);
    }
}

RuntimeKeyDiagnosticsSnapshot DagExecutor::runtimeKeyDiagnostics() const {
    return exact_runtime_keys_
        ? exact_runtime_keys_->snapshot()
        : RuntimeKeyDiagnosticsSnapshot{};
}

std::size_t DagExecutor::cacheMemoryUsage() const {
    const MemoSnapshot structural = structural_memo_.snapshot();
    const MemoSnapshot temporal = temporal_memo_.snapshot();
    const MemoSnapshot longest = longest_memo_.snapshot();
    return static_cast<std::size_t>(
        structural.bytes + temporal.bytes + longest.bytes);
}
#endif
