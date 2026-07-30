#include "GopherMatcher.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct DigestAccumulator {
    std::uint64_t xor_value = 0;
    std::uint64_t sum_value = 0;

    void add(std::uint64_t value) {
        xor_value ^= value;
        sum_value += value;
    }

    void merge(const DigestAccumulator& other) {
        xor_value ^= other.xor_value;
        sum_value += other.sum_value;
    }

    std::string text() const {
        std::ostringstream output;
        output << std::hex << std::setfill('0')
               << std::setw(16) << xor_value << ':'
               << std::setw(16) << sum_value;
        return output.str();
    }
};

std::uint64_t mix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::uint64_t mappingHash(
    const Graph& graph,
    const std::vector<int>& mapping) {
    std::uint64_t hash = 0x6a09e667f3bcc909ULL;
    for (std::size_t query_vertex = 0;
         query_vertex < mapping.size();
         ++query_vertex) {
        const std::uint64_t external = static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(
                graph.externalId(mapping[query_vertex])));
        hash = mix64(
            hash ^ mix64(
                external +
                static_cast<std::uint64_t>(query_vertex) *
                    0x9e3779b97f4a7c15ULL));
    }
    return hash;
}

struct RootResult {
    CheckedMatchCounter counter;
    DigestAccumulator digest;
};

struct SearchTask {
    bool is_root = false;
    std::size_t root_index = 0;
    SearchState state;
    std::size_t depth = 1;
    std::size_t accounted_bytes = 0;
};

struct WorkerMetrics {
    std::uint64_t tasks = 0;
    std::uint64_t dfs_states = 0;
    std::uint64_t active_nanoseconds = 0;
};

// A bounded centralized work queue is used only at prefix split points.  The
// queue stays small (threads * tasks_per_thread), while idle workers can take
// subtrees created inside an otherwise long-running root DFS.  Root tasks are
// generated lazily so a graph with millions of root candidates does not
// materialize millions of SearchState objects.
class DynamicPrefixQueue {
public:
    DynamicPrefixQueue(
        std::size_t root_count,
        std::size_t prefix_capacity)
        : root_count_(root_count),
          prefix_capacity_(std::max<std::size_t>(1, prefix_capacity)) {}

    bool mayAcceptPrefix() const {
        return queued_prefixes_.load(std::memory_order_relaxed) <
            prefix_capacity_;
    }

    bool tryPushPrefix(SearchTask task) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++split_attempts_;
        if (stopping_ || prefix_tasks_.size() >= prefix_capacity_) {
            ++split_rejected_;
            return false;
        }
        task.is_root = false;
        task.accounted_bytes =
            sizeof(SearchTask) +
            task.state.mapping.capacity() * sizeof(int) +
            task.state.active_intervals.capacity() *
                sizeof(TimeInterval);
        current_state_bytes_ += task.accounted_bytes;
        peak_state_bytes_ = std::max(
            peak_state_bytes_, current_state_bytes_);
        prefix_tasks_.push_back(std::move(task));
        queued_prefixes_.store(
            prefix_tasks_.size(), std::memory_order_relaxed);
        ++prefix_tasks_spawned_;
        queue_peak_ = std::max<std::uint64_t>(
            queue_peak_,
            static_cast<std::uint64_t>(prefix_tasks_.size()));
        condition_.notify_one();
        return true;
    }

    bool take(SearchTask& task) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&]() {
            return stopping_ ||
                !prefix_tasks_.empty() ||
                next_root_ < root_count_ ||
                active_tasks_ == 0;
        });
        if (stopping_) return false;

        if (!prefix_tasks_.empty()) {
            task = std::move(prefix_tasks_.front());
            prefix_tasks_.pop_front();
            if (task.accounted_bytes <= current_state_bytes_) {
                current_state_bytes_ -= task.accounted_bytes;
            } else {
                current_state_bytes_ = 0;
            }
            queued_prefixes_.store(
                prefix_tasks_.size(), std::memory_order_relaxed);
        } else if (next_root_ < root_count_) {
            task = SearchTask{};
            task.is_root = true;
            task.root_index = next_root_++;
            ++root_tasks_claimed_;
        } else {
            stopping_ = true;
            condition_.notify_all();
            return false;
        }
        ++active_tasks_;
        ++queue_claims_;
        return true;
    }

    void complete() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_tasks_ > 0) --active_tasks_;
        if (active_tasks_ == 0 &&
            prefix_tasks_.empty() &&
            next_root_ >= root_count_) {
            stopping_ = true;
            condition_.notify_all();
        } else {
            condition_.notify_all();
        }
    }

    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        prefix_tasks_.clear();
        queued_prefixes_.store(0, std::memory_order_relaxed);
        condition_.notify_all();
    }

    ParallelExecutionStatistics snapshot(
        std::size_t prefix_depth_limit) const {
        std::lock_guard<std::mutex> lock(mutex_);
        ParallelExecutionStatistics result;
        result.root_tasks_claimed = root_tasks_claimed_;
        result.prefix_tasks_spawned = prefix_tasks_spawned_;
        result.prefix_split_attempts = split_attempts_;
        result.prefix_split_rejected = split_rejected_;
        result.queue_claims = queue_claims_;
        result.queue_peak = queue_peak_;
        result.queue_state_bytes_peak =
            static_cast<std::uint64_t>(peak_state_bytes_);
        result.task_queue_capacity = prefix_capacity_;
        result.prefix_depth_limit = prefix_depth_limit;
        return result;
    }

private:
    const std::size_t root_count_;
    const std::size_t prefix_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<SearchTask> prefix_tasks_;
    std::atomic<std::size_t> queued_prefixes_{0};
    std::size_t next_root_ = 0;
    std::size_t active_tasks_ = 0;
    bool stopping_ = false;
    std::uint64_t root_tasks_claimed_ = 0;
    std::uint64_t prefix_tasks_spawned_ = 0;
    std::uint64_t split_attempts_ = 0;
    std::uint64_t split_rejected_ = 0;
    std::uint64_t queue_claims_ = 0;
    std::uint64_t queue_peak_ = 0;
    std::size_t current_state_bytes_ = 0;
    std::size_t peak_state_bytes_ = 0;
};

bool checkedAdd(
    CheckedMatchCounter& total,
    std::uint64_t value) {
    if (total.overflowed) return false;
    if (value > std::numeric_limits<std::uint64_t>::max() - total.value) {
        total.value = std::numeric_limits<std::uint64_t>::max();
        total.overflowed = true;
        return false;
    }
    total.value += value;
    return true;
}

} // namespace

const char* parallelModeName(ParallelMode mode) {
    switch (mode) {
        case ParallelMode::Roots:
            return "roots";
        case ParallelMode::Prefix:
            return "prefix";
    }
    return "roots";
}

bool parseParallelMode(
    const std::string& text,
    ParallelMode& mode) {
    if (text == "roots") {
        mode = ParallelMode::Roots;
        return true;
    }
    if (text == "prefix") {
        mode = ParallelMode::Prefix;
        return true;
    }
    return false;
}

GopherMatcher::GopherMatcher(
    const Graph& temporal_graph,
    const Graph& query_graph,
    const QueryPlan& plan,
    const CandidateDag& dag,
    const TDTree& td_tree,
    int minimum_duration,
    GopherMatcherOptions options)
    : G_(temporal_graph),
      Q_(query_graph),
      plan_(plan),
      dag_(dag),
      td_tree_(td_tree),
      k_threshold_(minimum_duration),
      options_(std::move(options)),
      statistics_(options_.executor.collect_statistics),
      executor_(
          temporal_graph,
          query_graph,
          plan,
          dag,
          td_tree,
          minimum_duration,
          options_.executor,
          statistics_) {
    if (options_.threads == 0) options_.threads = 1;
}

GopherMatchSummary GopherMatcher::run(
    MatchOutputMode mode,
    std::ostream* match_rows) {
    GopherMatchSummary summary;
    summary.parallel_mode_requested = options_.parallel_mode;
    const auto search_start = std::chrono::steady_clock::now();
    const auto& roots = td_tree_.rootCandidates();
    std::vector<RootResult> root_results(roots.size());
    std::atomic<bool> cancel{false};

    auto write_match = [&](const SearchState& state,
                           std::uint64_t match_index) {
        if (mode != MatchOutputMode::Full || match_rows == nullptr) return;
        std::ostringstream row;
        row << "Match " << match_index << ": ";
        for (int query_vertex = 0;
             query_vertex < Q_.num_vertices;
             ++query_vertex) {
            if (query_vertex > 0) row << ", ";
            row << 'q' << query_vertex << '('
                << labelToString(
                       Q_.vertex_labels[
                           static_cast<std::size_t>(query_vertex)])
                << ")->"
                << G_.externalId(
                       state.mapping[
                           static_cast<std::size_t>(query_vertex)]);
        }
        row << " | active="
            << formatIntervals(state.active_intervals) << '\n';
        const std::string text = row.str();
        statistics_.add(
            statistics_.output_buffer_bytes, text.size());
        *match_rows << text;
        if (!*match_rows) {
            throw std::runtime_error("failed while writing match rows");
        }
    };

    auto initialize_root = [&](std::size_t root_index,
                               SearchState& state) {
        state.mapping.assign(
            static_cast<std::size_t>(Q_.num_vertices), -1);

        const int root_candidate = roots[root_index];
        statistics_.add(statistics_.root_tasks);

        if (GraphUtils::hasEdge(
                Q_.adj, plan_.root, plan_.root)) {
            const TemporalSupport root_support =
                executor_.temporalSupport(
                    plan_.root, root_candidate, state);
            if (!root_support.has_constraints ||
                root_support.longest_run < k_threshold_ ||
                root_support.intervals->empty()) {
                statistics_.add(
                    statistics_.pruned_temporal_support);
                return false;
            }
            state.active_intervals = *root_support.intervals;
            state.has_active_intervals = true;
        }

        state.mapping[static_cast<std::size_t>(plan_.root)] =
            root_candidate;
        return true;
    };

    const std::size_t prefix_depth_limit =
        options_.prefix_depth == 0
        ? std::min<std::size_t>(2, plan_.order.size())
        : std::min(options_.prefix_depth, plan_.order.size());

    auto dfs = [&](auto&& self,
                   SearchState& state,
                   std::size_t depth,
                   RootResult& result,
                   std::uint64_t& output_match_index,
                   WorkerMetrics& worker_metrics,
                   DynamicPrefixQueue* prefix_queue) -> void {
                if (cancel.load(std::memory_order_relaxed) ||
                    result.counter.overflowed) {
                    return;
                }
                ++worker_metrics.dfs_states;
                statistics_.add(statistics_.dfs_states);
                if (depth >= plan_.order.size()) {
                    if (state.has_active_intervals &&
                        state.active_intervals.empty()) {
                        return;
                    }
                    const std::uint64_t match_index =
                        output_match_index;
                    if (!result.counter.increment()) {
                        cancel.store(true, std::memory_order_relaxed);
                        return;
                    }
                    ++output_match_index;
                    if (options_.result_digest) {
                        result.digest.add(
                            mappingHash(G_, state.mapping));
                    }
                    statistics_.add(
                        statistics_.completed_matches);
                    write_match(state, match_index);
                    return;
                }

                const int query_vertex = plan_.order[depth];
                const int parent_query_vertex =
                    plan_.parent[
                        static_cast<std::size_t>(query_vertex)];
                if (parent_query_vertex >= 0) {
                    const int parent_data_vertex =
                        state.mapping[
                            static_cast<std::size_t>(
                                parent_query_vertex)];
                    const std::vector<int>* raw_candidates =
                        td_tree_.candidatesForParent(
                            query_vertex, parent_data_vertex);
                    statistics_.add(
                        statistics_.raw_extension_candidates,
                        raw_candidates == nullptr
                            ? 0
                            : raw_candidates->size());
                }
                const DagExecutor::CandidateSetPtr candidates =
                    executor_.structuralCandidates(
                        query_vertex, state);
                statistics_.add(
                    statistics_.generated_candidates,
                    candidates->size());
                if (candidates->empty()) {
                    statistics_.add(
                        statistics_.pruned_structural);
                    return;
                }

                for (int candidate : *candidates) {
                    if (cancel.load(std::memory_order_relaxed)) return;
                    if (std::find(
                            state.mapping.begin(),
                            state.mapping.end(),
                            candidate) != state.mapping.end()) {
                        statistics_.add(
                            statistics_.pruned_injectivity);
                        continue;
                    }

                    const TemporalSupport support =
                        executor_.temporalSupport(
                            query_vertex, candidate, state);
                    if (!support.has_constraints ||
                        support.longest_run < k_threshold_ ||
                        support.intervals->empty()) {
                        statistics_.add(
                            statistics_.pruned_temporal_support);
                        continue;
                    }

                    std::vector<TimeInterval> next_active;
                    if (state.has_active_intervals) {
                        next_active = intersectTimeIntervals(
                            state.active_intervals,
                            *support.intervals,
                            k_threshold_);
                    } else {
                        next_active = *support.intervals;
                    }
                    if (next_active.empty() &&
                        options_.early_prune) {
                        statistics_.add(
                            statistics_.pruned_active_support);
                        continue;
                    }

                    statistics_.add(
                        statistics_.accepted_candidates);
                    statistics_.add(
                        statistics_.accepted_extensions);
                    std::vector<TimeInterval> previous_active =
                        std::move(state.active_intervals);
                    const bool previous_has_active =
                        state.has_active_intervals;
                    state.active_intervals = std::move(next_active);
                    state.has_active_intervals = true;
                    state.mapping[
                        static_cast<std::size_t>(query_vertex)] =
                        candidate;

                    const std::size_t child_depth = depth + 1;
                    bool spawned = false;
                    if (prefix_queue != nullptr &&
                        child_depth <= prefix_depth_limit &&
                        prefix_queue->mayAcceptPrefix()) {
                        SearchTask task;
                        task.state = state;
                        task.depth = child_depth;
                        spawned =
                            prefix_queue->tryPushPrefix(
                                std::move(task));
                    }
                    if (!spawned) {
                        self(
                            self,
                            state,
                            child_depth,
                            result,
                            output_match_index,
                            worker_metrics,
                            prefix_queue);
                    }

                    state.mapping[
                        static_cast<std::size_t>(query_vertex)] = -1;
                    state.active_intervals =
                        std::move(previous_active);
                    state.has_active_intervals =
                        previous_has_active;
                    if (result.counter.overflowed) return;
                }
            };

    auto enumerate_root = [&](std::size_t root_index,
                              RootResult& result,
                              std::uint64_t& output_match_index,
                              WorkerMetrics& worker_metrics,
                              DynamicPrefixQueue* prefix_queue) {
        SearchState state;
        if (!initialize_root(root_index, state)) return;
        dfs(
            dfs,
            state,
            1,
            result,
            output_match_index,
            worker_metrics,
            prefix_queue);
    };

    // Full output is intentionally streamed in deterministic root/DFS order.
    // Count-only mode can either assign complete roots or dynamically expose
    // bounded prefix subtrees to idle workers.
    std::vector<WorkerMetrics> worker_metrics;
    std::vector<RootResult> worker_results;
    if (mode == MatchOutputMode::Full) {
        summary.threads_used = 1;
        summary.parallel_mode_effective = "serial-output";
        worker_metrics.resize(1);
        std::uint64_t output_match_index = 0;
        for (std::size_t root_index = 0;
             root_index < roots.size();
             ++root_index) {
            const auto task_start =
                std::chrono::steady_clock::now();
            ++worker_metrics[0].tasks;
            enumerate_root(
                root_index,
                root_results[root_index],
                output_match_index,
                worker_metrics[0],
                nullptr);
            worker_metrics[0].active_nanoseconds +=
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<
                        std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() -
                        task_start).count());
            if (cancel.load(std::memory_order_relaxed)) break;
        }
        summary.parallel_statistics.root_tasks_claimed =
            static_cast<std::uint64_t>(worker_metrics[0].tasks);
        summary.parallel_statistics.queue_claims =
            static_cast<std::uint64_t>(worker_metrics[0].tasks);
        summary.parallel_statistics.prefix_depth_limit =
            prefix_depth_limit;
    } else if (options_.parallel_mode == ParallelMode::Roots ||
               options_.threads == 1) {
        summary.parallel_mode_effective =
            options_.parallel_mode == ParallelMode::Prefix
            ? "roots-single-worker"
            : "roots";
        const std::size_t worker_count = roots.empty()
            ? 1
            : std::min(options_.threads, roots.size());
        summary.threads_used = worker_count;
        worker_metrics.resize(worker_count);
        std::atomic<std::size_t> next_root{0};
        std::mutex exception_mutex;
        std::exception_ptr worker_exception;
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        for (std::size_t worker = 0;
             worker < worker_count;
             ++worker) {
            workers.emplace_back([&, worker]() {
                try {
                    std::uint64_t unused_output_index = 0;
                    while (!cancel.load(std::memory_order_relaxed)) {
                        const std::size_t root_index =
                            next_root.fetch_add(
                                1, std::memory_order_relaxed);
                        if (root_index >= roots.size()) break;
                        const auto task_start =
                            std::chrono::steady_clock::now();
                        ++worker_metrics[worker].tasks;
                        enumerate_root(
                            root_index,
                            root_results[root_index],
                            unused_output_index,
                            worker_metrics[worker],
                            nullptr);
                        worker_metrics[worker].active_nanoseconds +=
                            static_cast<std::uint64_t>(
                                std::chrono::duration_cast<
                                    std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() -
                                    task_start).count());
                    }
                } catch (...) {
                    {
                        std::lock_guard<std::mutex> lock(
                            exception_mutex);
                        if (!worker_exception) {
                            worker_exception =
                                std::current_exception();
                        }
                    }
                    cancel.store(true, std::memory_order_relaxed);
                }
            });
        }
        for (auto& worker : workers) worker.join();
        if (worker_exception) std::rethrow_exception(worker_exception);
        summary.parallel_statistics.root_tasks_claimed =
            static_cast<std::uint64_t>(
                std::min(next_root.load(std::memory_order_relaxed),
                         roots.size()));
        summary.parallel_statistics.queue_claims =
            summary.parallel_statistics.root_tasks_claimed;
    } else {
        summary.parallel_mode_effective = "prefix";
        const std::size_t worker_count =
            roots.empty() ? 1 : options_.threads;
        summary.threads_used = worker_count;
        worker_metrics.resize(worker_count);
        worker_results.resize(worker_count);

        const std::size_t max_size =
            std::numeric_limits<std::size_t>::max();
        const std::size_t task_queue_capacity =
            options_.tasks_per_thread >
                    max_size / worker_count
            ? max_size
            : std::max<std::size_t>(
                  1, options_.tasks_per_thread * worker_count);
        DynamicPrefixQueue prefix_queue(
            roots.size(), task_queue_capacity);
        std::mutex exception_mutex;
        std::exception_ptr worker_exception;
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        for (std::size_t worker = 0;
             worker < worker_count;
             ++worker) {
            workers.emplace_back([&, worker]() {
                try {
                    std::uint64_t unused_output_index = 0;
                    SearchTask task;
                    while (!cancel.load(std::memory_order_relaxed) &&
                           prefix_queue.take(task)) {
                        const auto task_start =
                            std::chrono::steady_clock::now();
                        ++worker_metrics[worker].tasks;
                        if (task.is_root) {
                            enumerate_root(
                                task.root_index,
                                worker_results[worker],
                                unused_output_index,
                                worker_metrics[worker],
                                &prefix_queue);
                        } else {
                            dfs(
                                dfs,
                                task.state,
                                task.depth,
                                worker_results[worker],
                                unused_output_index,
                                worker_metrics[worker],
                                &prefix_queue);
                        }
                        worker_metrics[worker].active_nanoseconds +=
                            static_cast<std::uint64_t>(
                                std::chrono::duration_cast<
                                    std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() -
                                    task_start).count());
                        if (worker_results[worker].counter.overflowed) {
                            cancel.store(
                                true, std::memory_order_relaxed);
                            prefix_queue.cancel();
                        } else {
                            prefix_queue.complete();
                        }
                    }
                } catch (...) {
                    {
                        std::lock_guard<std::mutex> lock(
                            exception_mutex);
                        if (!worker_exception) {
                            worker_exception =
                                std::current_exception();
                        }
                    }
                    cancel.store(true, std::memory_order_relaxed);
                    prefix_queue.cancel();
                }
            });
        }
        for (auto& worker : workers) worker.join();
        if (worker_exception) std::rethrow_exception(worker_exception);
        summary.parallel_statistics =
            prefix_queue.snapshot(prefix_depth_limit);
    }

    CheckedMatchCounter total;
    DigestAccumulator digest;
    for (const RootResult& root : root_results) {
        if (root.counter.overflowed ||
            !checkedAdd(total, root.counter.value)) {
            total.overflowed = true;
            break;
        }
        digest.merge(root.digest);
    }
    for (const RootResult& worker : worker_results) {
        if (worker.counter.overflowed ||
            !checkedAdd(total, worker.counter.value)) {
            total.overflowed = true;
            break;
        }
        digest.merge(worker.digest);
    }

    if (!worker_metrics.empty()) {
        std::uint64_t tasks_min =
            std::numeric_limits<std::uint64_t>::max();
        std::uint64_t dfs_min =
            std::numeric_limits<std::uint64_t>::max();
        std::uint64_t active_min =
            std::numeric_limits<std::uint64_t>::max();
        long double tasks_squares = 0.0L;
        long double dfs_squares = 0.0L;
        long double active_squares = 0.0L;
        for (const WorkerMetrics& worker : worker_metrics) {
            if (worker.tasks > 0) {
                ++summary.parallel_statistics.workers_with_tasks;
            }
            tasks_min = std::min(tasks_min, worker.tasks);
            dfs_min = std::min(dfs_min, worker.dfs_states);
            active_min =
                std::min(active_min, worker.active_nanoseconds);
            summary.parallel_statistics.worker_tasks_max =
                std::max(
                    summary.parallel_statistics.worker_tasks_max,
                    worker.tasks);
            summary.parallel_statistics.worker_tasks_total +=
                worker.tasks;
            summary.parallel_statistics.worker_dfs_states_max =
                std::max(
                    summary.parallel_statistics.worker_dfs_states_max,
                    worker.dfs_states);
            summary.parallel_statistics.worker_dfs_states_total +=
                worker.dfs_states;
            summary.parallel_statistics.worker_active_nanoseconds_max =
                std::max(
                    summary.parallel_statistics
                        .worker_active_nanoseconds_max,
                    worker.active_nanoseconds);
            summary.parallel_statistics
                .worker_active_nanoseconds_total +=
                worker.active_nanoseconds;
            const long double tasks =
                static_cast<long double>(worker.tasks);
            const long double dfs_count =
                static_cast<long double>(worker.dfs_states);
            const long double active =
                static_cast<long double>(worker.active_nanoseconds);
            tasks_squares += tasks * tasks;
            dfs_squares += dfs_count * dfs_count;
            active_squares += active * active;
        }
        summary.parallel_statistics.worker_tasks_min = tasks_min;
        summary.parallel_statistics.worker_dfs_states_min = dfs_min;
        summary.parallel_statistics.worker_active_nanoseconds_min =
            active_min;

        const long double count =
            static_cast<long double>(worker_metrics.size());
        auto coefficient_of_variation =
            [count](std::uint64_t value_total,
                    long double squares) {
                if (value_total == 0 || count == 0.0L) return 0.0;
                const long double mean =
                    static_cast<long double>(value_total) / count;
                const long double variance = std::max(
                    0.0L,
                    squares / count - mean * mean);
                return static_cast<double>(
                    std::sqrt(variance) / mean);
            };
        summary.parallel_statistics.worker_tasks_cv =
            coefficient_of_variation(
                summary.parallel_statistics.worker_tasks_total,
                tasks_squares);
        summary.parallel_statistics.worker_dfs_states_cv =
            coefficient_of_variation(
                summary.parallel_statistics
                    .worker_dfs_states_total,
                dfs_squares);
        summary.parallel_statistics
            .worker_active_nanoseconds_cv =
            coefficient_of_variation(
                summary.parallel_statistics
                    .worker_active_nanoseconds_total,
                active_squares);
    }

    const auto elapsed =
        std::chrono::steady_clock::now() - search_start;
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            elapsed).count();
    statistics_.set(
        statistics_.search_nanoseconds,
        static_cast<std::uint64_t>(elapsed_ns));

    summary.match_count = total.value;
    summary.count_overflow = total.overflowed;
    summary.enumeration_milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            elapsed).count();
    summary.enumeration_nanoseconds =
        static_cast<std::uint64_t>(elapsed_ns);
    if (summary.threads_used > 0 && elapsed_ns > 0) {
        const long double capacity =
            static_cast<long double>(summary.threads_used) *
            static_cast<long double>(elapsed_ns);
        summary.parallel_statistics.worker_utilization =
            static_cast<double>(
                static_cast<long double>(
                    summary.parallel_statistics
                        .worker_active_nanoseconds_total) /
                capacity);
    }
    summary.result_digest = options_.result_digest
        ? digest.text()
        : "disabled";
    summary.statistics = statistics_.snapshot();
    summary.statistics.dag_nodes_attempted =
        dag_.nodes_attempted;
    summary.statistics.dag_nodes_materialized =
        dag_.nodes.size();
    summary.statistics.dag_nodes_merged =
        dag_.nodes_merged;
    executor_.augmentStatistics(summary.statistics);
    summary.cache_memory_bytes =
        executor_.cacheMemoryUsage();
    return summary;
}

GopherMatchSummary GopherMatcher::save_res(
    const std::string& filename,
    MatchOutputMode mode) {
    GopherMatchSummary summary;
    std::ofstream output(filename, std::ios::binary);
    if (!output.is_open()) return summary;

    output << "[Run]\n"
           << "engine: gopher\n"
           << "mode: " << matchOutputModeName(mode) << '\n'
           << "memo: "
           << memoModeName(options_.executor.memo_mode) << '\n'
           << "order: " << orderModeName(options_.order_mode) << '\n'
           << "selectivity_mode: "
           << selectivityModeName(plan_.selectivity_mode) << '\n'
           << "query_root: " << plan_.root << '\n'
           << "root_selectivity_score: "
           << std::setprecision(17)
           << plan_.root_selectivity_score << '\n'
           << "plan_id: " << plan_.plan_id << '\n'
           << "threads_requested: " << options_.threads << '\n'
           << "parallel_mode_requested: "
           << parallelModeName(options_.parallel_mode) << '\n'
           << "prefix_depth_requested: "
           << options_.prefix_depth << '\n'
           << "tasks_per_thread: "
           << options_.tasks_per_thread << '\n'
           << "early_prune: "
           << (options_.early_prune ? "on" : "off") << '\n'
           << "result_digest: "
           << (options_.result_digest ? "on" : "off") << '\n'
           << "statistics: "
           << (options_.executor.collect_statistics ? "on" : "off")
           << '\n'
           << "cache_budget_bytes: "
           << options_.executor.memo_options.budget_bytes << '\n'
           << "cache_max_entry_bytes: "
           << options_.executor.memo_options.max_entry_bytes << '\n'
           << "cache_minimum_work: "
           << options_.executor.memo_options.minimum_estimated_work << '\n'
           << "plan_order:";
    for (int query_vertex : plan_.order) {
        output << ' ' << query_vertex;
    }
    output << '\n'
           << "verify_dag: "
           << (options_.executor.verify_dag ? "true" : "false")
           << "\n\n[Selectivity Summary]\n";
    td_tree_.writeSelectivitySummary(output);
    output << "\n[Candidate Summary]\n";
    td_tree_.writeCandidateSummary(output);
    output << "\n[Final Matches]\nCount: ";
    const std::streampos count_position = output.tellp();
    output << std::setw(20) << 0 << '\n';

    summary = run(mode, &output);

    const std::streampos end_position = output.tellp();
    output.seekp(count_position);
    if (summary.count_overflow) {
        output << std::setw(20) << "OVERFLOW";
    } else {
        output << std::setw(20) << summary.match_count;
    }
    output.seekp(end_position);
    output << "\n[Statistics]\n"
           << "mode: " << matchOutputModeName(mode) << '\n'
           << "count_overflow: "
           << (summary.count_overflow ? "true" : "false") << '\n'
           << "result_digest: " << summary.result_digest << '\n'
           << "threads_used: " << summary.threads_used << '\n'
           << "parallel_mode_effective: "
           << summary.parallel_mode_effective << '\n'
           << "parallel_root_tasks_claimed: "
           << summary.parallel_statistics.root_tasks_claimed << '\n'
           << "parallel_prefix_tasks_spawned: "
           << summary.parallel_statistics.prefix_tasks_spawned << '\n'
           << "parallel_prefix_split_attempts: "
           << summary.parallel_statistics.prefix_split_attempts << '\n'
           << "parallel_prefix_split_rejected: "
           << summary.parallel_statistics.prefix_split_rejected << '\n'
           << "parallel_queue_claims: "
           << summary.parallel_statistics.queue_claims << '\n'
           << "parallel_queue_peak: "
           << summary.parallel_statistics.queue_peak << '\n'
           << "parallel_queue_state_bytes_peak: "
           << summary.parallel_statistics.queue_state_bytes_peak
           << '\n'
           << "parallel_workers_with_tasks: "
           << summary.parallel_statistics.workers_with_tasks << '\n'
           << "parallel_worker_tasks_min: "
           << summary.parallel_statistics.worker_tasks_min << '\n'
           << "parallel_worker_tasks_max: "
           << summary.parallel_statistics.worker_tasks_max << '\n'
           << "parallel_worker_tasks_total: "
           << summary.parallel_statistics.worker_tasks_total << '\n'
           << "parallel_worker_dfs_states_min: "
           << summary.parallel_statistics.worker_dfs_states_min << '\n'
           << "parallel_worker_dfs_states_max: "
           << summary.parallel_statistics.worker_dfs_states_max << '\n'
           << "parallel_worker_dfs_states_total: "
           << summary.parallel_statistics.worker_dfs_states_total << '\n'
           << "parallel_worker_active_ns_min: "
           << summary.parallel_statistics
                  .worker_active_nanoseconds_min << '\n'
           << "parallel_worker_active_ns_max: "
           << summary.parallel_statistics
                  .worker_active_nanoseconds_max << '\n'
           << "parallel_worker_active_ns_total: "
           << summary.parallel_statistics
                  .worker_active_nanoseconds_total << '\n'
           << "parallel_worker_tasks_cv: "
           << summary.parallel_statistics.worker_tasks_cv << '\n'
           << "parallel_worker_dfs_states_cv: "
           << summary.parallel_statistics.worker_dfs_states_cv << '\n'
           << "parallel_worker_active_ns_cv: "
           << summary.parallel_statistics
                  .worker_active_nanoseconds_cv << '\n'
           << "parallel_worker_utilization: "
           << summary.parallel_statistics.worker_utilization << '\n'
           << "parallel_task_queue_capacity: "
           << summary.parallel_statistics.task_queue_capacity << '\n'
           << "parallel_prefix_depth_limit: "
           << summary.parallel_statistics.prefix_depth_limit << '\n'
           << "root_candidate_count: "
           << td_tree_.rootCandidates().size() << '\n'
           << "candidate_relation_entries: "
           << td_tree_.candidateRelationCount() << '\n'
           << "dag_memory_bytes: " << dag_.getMemoryUsage() << '\n'
           << "cache_memory_bytes: "
           << summary.cache_memory_bytes << '\n'
           << "enumeration_ms: "
           << summary.enumeration_milliseconds << '\n';
    if (options_.executor.collect_statistics) {
        writeSearchStatistics(output, summary.statistics);
    } else {
        output << "statistics_collected: false\n";
    }
    output.flush();
    summary.output_written = output.good();
    return summary;
}
