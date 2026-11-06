#include "GopherMatcher.h"
#include "DirectP4Evaluator.h"
#include "OpKernels.h"
#include "WindowProgram.h"
#include "WindowStore.h"
#include "JIndexExecution.h"
#include "BIndexExecution.h"
#include "UnionBlockGuard.h"
#include "RawBitset.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
namespace W = window_algebra;
using Clock = std::chrono::steady_clock;
std::uint64_t nanos(Clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - start).count());
}
std::uint64_t mix(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}
std::uint64_t mappingHash(const Graph& graph, const std::vector<int>& mapping) {
    std::uint64_t hash = 0x6a09e667f3bcc909ULL;
    for (std::size_t q = 0; q < mapping.size(); ++q) {
        const auto external = static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(graph.externalId(mapping[q])));
        hash = mix(hash ^ mix(external + q * 0x9e3779b97f4a7c15ULL));
    }
    return hash;
}
struct Frame {
    W::WindowBuffer left, right;
    raw_bitset::Scratch raw_run_scratch{};
    // Bulk, frame-owned storage: a batch grows one contiguous allocation,
    // never one owning vector per candidate. Capacity is reused across roots.
    std::vector<std::uint64_t> dense;
    std::vector<W::Interval> intervals;
    std::vector<std::size_t> counts;
    std::vector<unsigned char> valid;
    std::size_t interval_stride = 0;
    void prepare(std::size_t count, const W::Domain& domain) {
        valid.assign(count, 0);
        if (domain.representation() == W::Representation::Dense) {
            const auto words = static_cast<std::size_t>(domain.wordCount());
            if (words && count > dense.max_size() / words)
                throw std::length_error("W batch dense size overflow");
            dense.resize(count * words);
        } else {
            if (interval_stride && count > intervals.max_size() / interval_stride)
                throw std::length_error("W batch interval size overflow");
            intervals.resize(count * interval_stride);
            counts.resize(count);
        }
    }
    void save(std::size_t index, W::WindowView result) {
        if (result.isEmpty()) return;
        if (result.isUniversal()) throw std::logic_error("constrained W batch remained universal");
        valid[index] = 1;
        if (result.domain().representation() == W::Representation::Dense) {
            std::copy_n(result.words(), result.wordCount(),
                        dense.data() + index * result.wordCount());
        } else {
            const auto starts = result.starts();
            if (starts.size > interval_stride) throw std::logic_error("W batch capacity proof failed");
            counts[index] = starts.size;
            std::copy_n(starts.data, starts.size, intervals.data() + index * interval_stride);
        }
    }
    W::WindowView get(std::size_t index, const W::Domain& domain) const {
        if (!valid[index]) return W::WindowView::empty(domain);
        if (domain.representation() == W::Representation::Dense)
            return W::WindowView::dense(domain,
                dense.data() + index * domain.wordCount(), domain.wordCount());
        return W::WindowView::intervals(domain,
            {intervals.data() + index * interval_stride, counts[index]});
    }
};
struct Worker {
    std::vector<Frame> frames;
    BIndexExecution::WorkerScratch bindex_scratch;
    W::IntervalBuffer restored;
    CheckedMatchCounter matches;
    std::uint64_t hash_xor = 0, hash_sum = 0, claimed = 0;
    std::uint64_t union_checked = 0, union_skipped = 0, union_candidates_skipped = 0;
};
}

void GopherMatcher::prepareV5() {
    if ((options_.window_pad_raw_words || options_.window_block_union) &&
        (dag_ || !options_.direct_p4 || !options_.window_algebra || options_.raw_bitset ||
         options_.window_lazy || options_.wstore_path.empty() ||
         options_.bindex_mode != BIndexMode::None || options_.jindex_mode != JIndexMode::None))
        throw std::invalid_argument("E1/E6 controls require loaded eroded Direct P4 W without an index");
    if (options_.raw_bitset &&
        (dag_ || !options_.direct_p4 || !options_.window_algebra || options_.window_lazy ||
         options_.wstore_path.empty() || options_.bindex_mode != BIndexMode::None ||
         options_.jindex_mode != JIndexMode::None))
        throw std::invalid_argument("raw bitset requires Direct P4, a loaded raw store, and no index");
    if ((!options_.wstore_path.empty() || options_.bindex_mode != BIndexMode::None) &&
        (dag_ || !options_.window_algebra || !options_.direct_p4 || options_.window_lazy))
        throw std::invalid_argument("v6 store/B-index requires Direct P4 with eager W");
    if (options_.bindex_mode != BIndexMode::None && options_.jindex_mode != JIndexMode::None)
        throw std::invalid_argument("B-index and adjacency-index are separate controls");
    if (options_.jindex_mode != JIndexMode::None && !options_.window_algebra)
        throw std::invalid_argument("P8 requires W and a P4 producer");
    if (!options_.window_algebra && !options_.direct_p4) return;
    if (options_.parallel_mode != ParallelMode::Roots || !options_.early_prune ||
        options_.executor.memo_mode != MemoMode::None || statistics_.enabled() ||
        options_.order_mode != OrderMode::Paper ||
        options_.order_simulation != OrderSimulationMode::Disabled ||
        options_.executor.adaptive || options_.executor.verify_dag ||
        options_.executor.runtime_key_diagnostics != RuntimeKeyDiagnosticsMode::Off)
        throw std::invalid_argument("v5 requires static paper/roots/no memo/no legacy diagnostics");
    if (options_.jindex_simulation)
        throw std::invalid_argument("J-index shadow follows original S1, never the v5 W/control search");
    if (dag_) {
        if (options_.direct_p4 || !options_.window_algebra || !dag_->passes.p4 ||
            dag_->passes.temporalRewriteAny())
            throw std::invalid_argument("v5 DAG W requires P4 and P1-P3 off");
        window_program_ = std::make_unique<WindowProgram>(
            *dag_, plan_, options_.jindex_mode != JIndexMode::None);
    } else {
        if (!options_.direct_p4 || options_.executor.direct_passes.any())
            throw std::invalid_argument("v5 Direct requires its explicit P4 control, no DAG passes");
        direct_p4_ = std::make_unique<DirectP4Evaluator>(
            G_, Q_, plan_, td_tree_, k_threshold_, options_.executor.ops, options_.window_algebra);
    }
    if (options_.window_algebra) {
        if (options_.wstore_path.empty())
            window_store_ = std::make_unique<WindowStore>(
                G_, Q_, td_tree_, k_threshold_, options_.window_lazy);
        else
            window_store_ = std::make_unique<WindowStore>(
                G_, options_.wstore_path, options_.graph_file_sha256,
                options_.raw_bitset ? 1 : k_threshold_);
        if (options_.raw_bitset && !window_store_->rawOnlySection())
            throw std::invalid_argument("raw bitset store must contain exactly one k=1 section");
        if (options_.window_pad_raw_words) window_store_->padLoadedToRawWords();
        if (options_.window_block_union)
            union_guard_=std::make_unique<UnionBlockGuard>(G_,Q_,plan_,td_tree_,*window_store_);
    }
    if (options_.bindex_mode != BIndexMode::None) {
        bindex_execution_ = std::make_unique<BIndexExecution>(
            G_, Q_, plan_, td_tree_, *window_store_, k_threshold_,
            options_.bindex_block_size, options_.bindex_min_size,
            options_.bindex_reuse_threshold, options_.bindex_alpha,
            options_.bindex_mode == BIndexMode::Always ? BIndexExecution::Mode::Always :
                BIndexExecution::Mode::Adaptive,
            options_.executor.ops != nullptr, options_.bindex_verify_superset,
            options_.graph_adjacency_temporal_capacity_bytes);
    }
    if (options_.jindex_mode != JIndexMode::None) {
        if (!dag_ && options_.jindex_mode != JIndexMode::Always)
            throw std::invalid_argument("Direct P8 is the fixed always-index control");
        jindex_execution_ = std::make_unique<JIndexExecution>(
            G_, window_store_->domain(), k_threshold_, options_.jindex_block_size,
            options_.jindex_mode == JIndexMode::Adaptive ? JIndexExecution::Mode::Adaptive :
                JIndexExecution::Mode::Always,
            options_.jindex_alpha, static_cast<std::size_t>(Q_.num_vertices),
            options_.executor.ops != nullptr, options_.jindex_verify_superset,
            options_.wstore_path.empty() ? nullptr : window_store_.get());
        if (dag_) executor_->registerIndexDrivers(*jindex_execution_);
        else direct_p4_->registerIndexDrivers(*jindex_execution_);
        jindex_execution_->freezeDrivers();
    }
}

GopherMatchSummary GopherMatcher::runV5(MatchOutputMode mode, std::ostream* match_rows) {
    GopherMatchSummary summary;
    summary.parallel_mode_requested = options_.parallel_mode;
    summary.parallel_mode_effective = mode == MatchOutputMode::Full ? "serial-output" : "roots";
    summary.primary_metrics.scope = "final_dag_search_and_count_reduction";
    const auto& roots = td_tree_.rootCandidates();
    const auto workers_count = mode == MatchOutputMode::Full ? 1 :
        (roots.empty() ? 1 : std::min(options_.threads, roots.size()));
    summary.threads_used = workers_count;
    const bool window = window_store_ != nullptr;
    const auto prep_start = Clock::now();
    std::vector<Worker> workers_data;
    if (window) {
        workers_data.resize(workers_count);
        std::size_t edges = 0;
        for (const auto& neighbors : Q_.adj) edges += neighbors.size();
        const auto capacity = window_store_->scratchIntervalBound(edges);
        for (auto& worker : workers_data) {
            worker.frames.resize(plan_.order.size());
            for (auto& frame : worker.frames) {
                frame.left.reserveIntervals(capacity);
                frame.right.reserveIntervals(capacity);
                frame.interval_stride = capacity;
            }
            // A dense window bitmap may restore more than eight disjoint runs.
            // Reserve before enumeration, even though count-only never restores.
            if (options_.exact_match_observer || mode == MatchOutputMode::Full)
                worker.restored.reserveIntervals(
                    window_store_->domain().representation() == W::Representation::Dense
                    ? static_cast<std::size_t>((window_store_->domain().startCount() + 1) / 2)
                    : capacity);
        }
        summary.v5_build_nanoseconds = window_store_->preparationNanoseconds() + nanos(prep_start);
        if (jindex_execution_)
            summary.v5_build_nanoseconds += jindex_execution_->setupNanoseconds();
        if (bindex_execution_)
            summary.v5_build_nanoseconds += bindex_execution_->setupNanoseconds();
        if (union_guard_)
            summary.v5_build_nanoseconds += union_guard_->setupNanoseconds();
    }
    std::atomic<bool> cancel{false};
    std::atomic<std::size_t> next_root{0}, live{0}, peak_live{0}, productive{0}, peak_productive{0};
    std::mutex failure_mutex;
    std::exception_ptr failure;
    auto* ops = options_.executor.ops;
    auto used = [](const SearchState& state, int candidate) {
        return std::find(state.mapping.begin(), state.mapping.end(), candidate) != state.mapping.end();
    };
    auto window_candidate = [&](const PhysicalCandidateBatch& batch, std::size_t index,
                                W::WindowView prior, Frame& frame, int q) {
        OPS_ADD(ops, window_candidate_checks, 1);
        if (options_.raw_bitset) OPS_ADD(ops, bitset_time_checks, 1);
        const auto row = batch.historyRow(index);
        if (window_program_) {
            const auto& node = window_program_->node(q);
            if (row.producer != node.producer || row.size != node.history_slots.size())
                throw std::logic_error("W compiled producer binding differs from batch");
        }
        W::WindowView active = prior;
        bool left = true;
        for (std::size_t slot = 0; slot < row.size; ++slot) {
            const auto selected = window_program_ ?
                window_program_->node(q).operand_order[slot] : slot;
            const auto edge = window_store_->get(row.temporal_edge_ids[selected]);
            // Logical edge payload width consumed per lookup, excluding the
            // common WindowView validation scan and branch scratch reads.
            if (options_.raw_bitset)
                OPS_ADD(ops, bitset_edge_words_read, window_store_->domain().wordCount());
            OPS_ADD(ops, physical_history_handle_reads, 1);
            OPS_ADD(ops, history_lookups_executed, 1);
            OPS_ADD(ops, window_edge_reads, 1);
            OPS_ADD(ops, time_intersections_executed, 1);
            if (active.kind() == W::Kind::Dense && edge.kind() == W::Kind::Dense)
                OPS_ADD(ops, window_word_ands, window_store_->domain().wordCount());
            auto& output = left ? frame.left : frame.right;
            W::intersectInto(window_store_->domain(), active, edge, output);
            active = output.view();
            left = !left;
            if (active.isEmpty()) {
                OPS_ADD(ops, window_empty_rejections, 1);
                break;
            }
        }
        if (options_.raw_bitset) {
            raw_bitset::RunWork counted;
            const bool pass = raw_bitset::hasRun(active, k_threshold_, frame.raw_run_scratch,
                                                ops ? &counted : nullptr);
            OPS_ADD(ops, bitset_shift_and_steps, counted.shift_and_steps);
            OPS_ADD(ops, bitset_shift_word_ops, counted.shift_word_ops);
            if (!pass) return W::WindowView::empty(window_store_->domain());
            OPS_ADD(ops, bitset_time_passes, 1);
        }
        return active;
    };
    auto emit = [&](SearchState& state, Worker& worker, W::WindowView active) {
        if (!worker.matches.increment()) { cancel.store(true); return; }
        if (options_.result_digest) {
            const auto hash = mappingHash(G_, state.mapping);
            worker.hash_xor ^= hash;
            worker.hash_sum += hash;
        }
        if (window && (options_.exact_match_observer || mode == MatchOutputMode::Full)) {
            state.has_active_intervals = !active.isUniversal();
            state.active_intervals.clear();
            if (state.has_active_intervals) {
                W::restoreInto(window_store_->domain(), active, worker.restored);
                for (std::size_t i = 0; i < worker.restored.size(); ++i) {
                    const auto& interval = worker.restored.data()[i];
                    // Raw J remains unnormalized in DFS. Only output/observer
                    // drops maximal runs shorter than actual query k.
                    if (options_.raw_bitset && interval.end - interval.start + 1 < k_threshold_)
                        continue;
                    state.active_intervals.push_back(
                        {static_cast<int>(interval.start), static_cast<int>(interval.end)});
                }
            }
        }
        if (options_.exact_match_observer)
            options_.exact_match_observer(state, options_.exact_match_observer_context);
        if (mode == MatchOutputMode::Full && match_rows) {
            *match_rows << "Match " << worker.matches.value - 1 << ": ";
            for (int q = 0; q < Q_.num_vertices; ++q) {
                if (q) *match_rows << ", ";
                *match_rows << 'q' << q << '(' << labelToString(Q_.vertex_labels[q])
                            << ")->" << G_.externalId(state.mapping[q]);
            }
            *match_rows << " | active=" << formatIntervals(state.active_intervals) << '\n';
            if (!*match_rows) throw std::runtime_error("v5 match output failed");
        }
    };
    auto dfs = [&](auto&& self, SearchState& state, std::size_t depth,
                   Worker& worker, W::WindowView active) -> void {
        if (cancel.load(std::memory_order_relaxed)) return;
        if (depth >= plan_.order.size()) { emit(state, worker, active); return; }
        const int q = plan_.order[depth];
        if (union_guard_) {
            const auto checked=union_guard_->check(q,state.mapping[static_cast<std::size_t>(plan_.parent[q])],active);
            // Per-worker integer counters only, no atomic operation on a probe.
            if (checked.tested) ++worker.union_checked;
            if (checked.rejected) {
                if (checked.tested) ++worker.union_skipped;
                worker.union_candidates_skipped+=checked.candidates;
                return;
            }
        }
        auto batch = bindex_execution_ ?
            direct_p4_->physicalCandidates(q, state, *bindex_execution_, worker.bindex_scratch, active) :
            jindex_execution_ ?
            (dag_ ? executor_->physicalCandidates(q, state, *jindex_execution_, active) :
                    direct_p4_->physicalCandidates(q, state, *jindex_execution_, active)) :
            (dag_ ? executor_->physicalCandidates(q, state) :
                    direct_p4_->physicalCandidates(q, state));
        OPS_ADD(ops, candidates, batch.size());
        Frame* frame = window ? &worker.frames[depth] : nullptr;
        if (window_program_) {
            // One compiled node invocation per candidate batch. Injectivity
            // precedes every time read; DFS recurses only after the full batch
            // WindowAnd has been evaluated and owns its resulting supports.
            OPS_ADD(ops, window_batch_calls, 1);
            frame->prepare(batch.size(), window_store_->domain());
            for (std::size_t i = 0; i < batch.size(); ++i) {
                if (used(state, batch.candidate(i))) continue;
                frame->save(i, window_candidate(batch, i, active, *frame, q));
            }
        }
        for (std::size_t i = 0; i < batch.size(); ++i) {
            if (cancel.load(std::memory_order_relaxed)) return;
            const int candidate = batch.candidate(i);
            if (used(state, candidate)) {
                OPS_ADD(ops, physical_injectivity_rejections, 1);
                continue;
            }
            W::WindowView next_window;
            std::vector<TimeInterval> next_active;
            if (window) {
                if (bindex_execution_) bindex_execution_->recordTimeCheck(q);
                next_window = window_program_ ? frame->get(i, window_store_->domain()) :
                    window_candidate(batch, i, active, *frame, q);
                if (next_window.isEmpty()) {
                    OPS_ADD(ops, physical_temporal_rejections, 1);
                    continue;
                }
                if (bindex_execution_) bindex_execution_->recordTimePass(q);
            } else {
                const auto row = batch.historyRow(i);
                const auto support = direct_p4_->temporalSupport(q, candidate, state, &row);
                if (!support.has_constraints || support.intervals->empty()) {
                    OPS_ADD(ops, physical_temporal_rejections, 1);
                    continue;
                }
                next_active = state.has_active_intervals ?
                    (ops ? OperatorKernels::intersectIntervals(
                        state.active_intervals, *support.intervals, k_threshold_, false, ops) :
                        intersectTimeIntervals(state.active_intervals, *support.intervals, k_threshold_)) :
                    *support.intervals;
                if (next_active.empty()) {
                    OPS_ADD(ops, physical_temporal_rejections, 1);
                    continue;
                }
            }
            OPS_ADD(ops, logical_extensions, 1);
            OPS_ADD(ops, physical_extensions, 1);
            const bool had_active = state.has_active_intervals;
            auto previous = std::move(state.active_intervals);
            if (!window) {
                state.active_intervals = std::move(next_active);
                state.has_active_intervals = true;
            }
            state.mapping[q] = candidate;
            self(self, state, depth + 1, worker, next_window);
            state.mapping[q] = -1;
            state.active_intervals = std::move(previous);
            state.has_active_intervals = had_active;
        }
    };
    const bool root_loop = GraphUtils::hasEdge(Q_.adj, plan_.root, plan_.root);
    dsm::pure_online_v2::captureEnvironmentStart(summary.primary_metrics);
    const auto start = dsm::pure_online_v2::beginInterval();
    // Direct+P4 has no k-dependent W setup: its worker allocation stays in the
    // native enumeration boundary, just as in the original Direct matcher.
    if (!window) workers_data.resize(workers_count);
    auto work = [&](std::size_t worker_id) {
        dsm::pure_online_v2::updateAtomicPeak(peak_live, live.fetch_add(1) + 1);
        auto& worker = workers_data[worker_id];
        bool busy = false;
        try {
            while (!cancel.load(std::memory_order_relaxed)) {
                const auto root_index = next_root.fetch_add(1, std::memory_order_relaxed);
                if (root_index >= roots.size()) break;
                ++worker.claimed;
                busy = true;
                dsm::pure_online_v2::updateAtomicPeak(peak_productive, productive.fetch_add(1) + 1);
                SearchState state;
                state.mapping.assign(Q_.num_vertices, -1);
                W::WindowView active;
                if (window) active = W::WindowView::universal(window_store_->domain());
                bool valid = true;
                const int root = roots[root_index];
                if (root_loop) {
                    if (window) {
                        const auto& neighbors = G_.adj[root];
                        const auto found = std::lower_bound(neighbors.begin(), neighbors.end(), root,
                            [](const Edge& edge, int v) { return edge.to < v; });
                        valid = found != neighbors.end() && found->to == root;
                        if (valid) {
                            OPS_ADD(ops, history_lookups_executed, 1);
                            OPS_ADD(ops, window_edge_reads, 1);
                            active = window_store_->get(found->temporal_edge_id);
                            valid = !active.isEmpty();
                            if (options_.raw_bitset) {
                                OPS_ADD(ops, bitset_time_checks, 1);
                                OPS_ADD(ops, bitset_edge_words_read, window_store_->domain().wordCount());
                                raw_bitset::RunWork counted;
                                valid = raw_bitset::hasRun(active, k_threshold_,
                                    worker.frames.front().raw_run_scratch, ops ? &counted : nullptr);
                                OPS_ADD(ops, bitset_shift_and_steps, counted.shift_and_steps);
                                OPS_ADD(ops, bitset_shift_word_ops, counted.shift_word_ops);
                                if (valid) OPS_ADD(ops, bitset_time_passes, 1);
                            }
                        }
                    } else {
                        const auto support = direct_p4_->temporalSupport(plan_.root, root, state, nullptr);
                        valid = support.has_constraints && !support.intervals->empty();
                        if (valid) { state.active_intervals = *support.intervals; state.has_active_intervals = true; }
                    }
                }
                if (valid) {
                    state.mapping[plan_.root] = root;
                    dfs(dfs, state, 1, worker, active);
                }
                productive.fetch_sub(1);
                busy = false;
            }
        } catch (...) {
            if (busy) productive.fetch_sub(1);
            std::lock_guard<std::mutex> lock(failure_mutex);
            if (!failure) failure = std::current_exception();
            cancel.store(true);
        }
        live.fetch_sub(1);
    };
    std::vector<std::thread> threads;
    if (mode == MatchOutputMode::Full) work(0);
    else {
        try {
            for (std::size_t id = 0; id < workers_count; ++id) threads.emplace_back(work, id);
        } catch (...) {
            cancel.store(true);
            for (auto& thread : threads) thread.join();
            throw;
        }
        for (auto& thread : threads) thread.join();
    }
    if (failure) std::rethrow_exception(failure);
    std::uint64_t hash_xor = 0, hash_sum = 0;
    for (const auto& worker : workers_data) {
        if (worker.matches.overflowed || worker.matches.value >
            std::numeric_limits<std::uint64_t>::max() - summary.match_count) {
            summary.count_overflow = true;
            break;
        }
        summary.match_count += worker.matches.value;
        hash_xor ^= worker.hash_xor;
        hash_sum += worker.hash_sum;
    }
    const auto delta = dsm::pure_online_v2::endInterval(start);
    summary.enum_start_monotonic_ns = start.wall_ns;
    summary.enum_end_monotonic_ns = start.wall_ns + delta.wall_ns;
    dsm::pure_online_v2::captureEnvironmentEnd(summary.primary_metrics);
    dsm::pure_online_v2::addInterval(summary.primary_metrics, delta);
    summary.enumeration_nanoseconds = delta.wall_ns;
    summary.enumeration_milliseconds = static_cast<long long>(delta.wall_ns / 1000000);
    auto& primary = summary.primary_metrics;
    primary.worker_threads_created = threads.size();
    primary.peak_live_worker_threads = peak_live.load();
    primary.workers_with_tasks = peak_productive.load();
    primary.parallel_interval_count = 1;
    primary.parallel_work_units_min = primary.parallel_work_units_max =
        primary.parallel_work_units_total = roots.size();
    for (const auto& worker : workers_data) primary.parallel_work_units_claimed += worker.claimed;
    primary.parallel_intervals_with_at_least_requested_work = roots.size() >= options_.threads ? 1 : 0;
    primary.parallel_workers_with_tasks_min = primary.parallel_workers_with_tasks_max =
        primary.parallel_workers_with_tasks_sum = primary.workers_with_tasks;
    primary.parallelism_eligible = primary.parallel_intervals_with_at_least_requested_work == 1;
    if (options_.result_digest) {
        std::ostringstream digest;
        digest << std::hex << std::setfill('0') << std::setw(16) << hash_xor << ':'
               << std::setw(16) << hash_sum;
        summary.result_digest = digest.str();
    } else summary.result_digest = "disabled";
    if (window) {
        summary.v9_window_words=window_store_->domain().wordCount();
        summary.v9_padding_nanoseconds=window_store_->paddingNanoseconds();
        summary.v5_lazy_build_thread_nanoseconds = window_store_->lazyBuildThreadNanoseconds();
        summary.v5_window_bytes = window_store_->storageBytes();
        summary.v5_window_relevant_edges = window_store_->relevantEdges();
        summary.v5_window_built_edges = window_store_->builtEdges();
        summary.v5_window_forced_intervals = window_store_->forcedIntervals();
        std::ostringstream loading;
        window_store_->writeLoadJson(loading);
        summary.wstore_loading_json = loading.str();
    }
    if (union_guard_) {
        for (const auto& worker : workers_data) {
            summary.v9_union_blocks_checked+=worker.union_checked;
            summary.v9_union_blocks_skipped+=worker.union_skipped;
            summary.v9_union_candidates_skipped+=worker.union_candidates_skipped;
        }
        summary.v9_union_blocks_built=union_guard_->builtBlocks();
        summary.v9_union_bytes=union_guard_->storageBytes();
    }
    if (jindex_execution_) {
        // Lazy index construction already lies INSIDE enumeration. Its
        // all-thread CPU sum is auxiliary and MUST NOT be added to the primary
        // enum+build time. Only setupNanoseconds() above is outside enum.
        summary.v5_lazy_build_thread_nanoseconds += jindex_execution_->lazyBuildThreadNanoseconds();
        std::ostringstream json;
        jindex_execution_->writeJson(json);
        summary.jindex_execution_json = json.str();
    }
    if (bindex_execution_) {
        // Lazy build is already inside enumeration, never add it twice.
        summary.v5_lazy_build_thread_nanoseconds += bindex_execution_->buildThreadNanoseconds();
        std::ostringstream json;
        bindex_execution_->writeJson(json);
        summary.bindex_execution_json = json.str();
    }
    return summary;
}
