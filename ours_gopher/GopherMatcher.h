#ifndef GOPHER_MATCHER_H
#define GOPHER_MATCHER_H

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>

#include "CandidateDag.h"
#include "DagExecutor.h"
#include "SearchStatistics.h"
#include "TDTree.h"

enum class ParallelMode {
    Roots,
    Prefix
};

const char* parallelModeName(ParallelMode mode);
bool parseParallelMode(const std::string& text, ParallelMode& mode);

struct ParallelExecutionStatistics {
    std::uint64_t root_tasks_claimed = 0;
    std::uint64_t prefix_tasks_spawned = 0;
    std::uint64_t prefix_split_attempts = 0;
    std::uint64_t prefix_split_rejected = 0;
    std::uint64_t queue_claims = 0;
    std::uint64_t queue_peak = 0;
    std::uint64_t queue_state_bytes_peak = 0;
    std::uint64_t worker_tasks_min = 0;
    std::uint64_t worker_tasks_max = 0;
    std::uint64_t worker_tasks_total = 0;
    std::uint64_t worker_dfs_states_min = 0;
    std::uint64_t worker_dfs_states_max = 0;
    std::uint64_t worker_dfs_states_total = 0;
    std::uint64_t worker_active_nanoseconds_min = 0;
    std::uint64_t worker_active_nanoseconds_max = 0;
    std::uint64_t worker_active_nanoseconds_total = 0;
    double worker_tasks_cv = 0.0;
    double worker_dfs_states_cv = 0.0;
    double worker_active_nanoseconds_cv = 0.0;
    double worker_utilization = 0.0;
    std::size_t workers_with_tasks = 0;
    std::size_t task_queue_capacity = 0;
    std::size_t prefix_depth_limit = 0;
};

struct GopherMatcherOptions {
    std::size_t threads = 1;
    ParallelMode parallel_mode = ParallelMode::Roots;
    // Zero selects the bounded automatic policy (root plus one query vertex).
    std::size_t prefix_depth = 0;
    // Maximum number of queued prefix subtrees per requested worker.
    std::size_t tasks_per_thread = 8;
    OrderMode order_mode = OrderMode::Paper;
    bool early_prune = true;
    bool result_digest = false;
    DagExecutorOptions executor;
};

struct GopherMatchSummary {
    std::uint64_t match_count = 0;
    bool count_overflow = false;
    bool output_written = false;
    long long enumeration_milliseconds = 0;
    std::uint64_t enumeration_nanoseconds = 0;
    std::size_t threads_used = 0;
    std::string result_digest;
    ParallelMode parallel_mode_requested = ParallelMode::Roots;
    std::string parallel_mode_effective = "roots";
    ParallelExecutionStatistics parallel_statistics;
    SearchStatistics statistics;
    std::size_t cache_memory_bytes = 0;
};

class GopherMatcher {
public:
    GopherMatcher(
        const Graph& temporal_graph,
        const Graph& query_graph,
        const QueryPlan& plan,
        const CandidateDag& dag,
        const TDTree& td_tree,
        int minimum_duration,
        GopherMatcherOptions options);

    GopherMatchSummary run(
        MatchOutputMode mode = MatchOutputMode::CountOnly,
        std::ostream* match_rows = nullptr);

    GopherMatchSummary save_res(
        const std::string& filename,
        MatchOutputMode mode = MatchOutputMode::Full);

private:
    const Graph& G_;
    const Graph& Q_;
    const QueryPlan& plan_;
    const CandidateDag& dag_;
    const TDTree& td_tree_;
    int k_threshold_;
    GopherMatcherOptions options_;

    SearchStatisticsCollector statistics_;
    DagExecutor executor_;
};

#endif
