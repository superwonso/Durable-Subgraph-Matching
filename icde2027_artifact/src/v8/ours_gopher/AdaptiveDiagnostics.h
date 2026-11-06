#ifndef DSM_ADAPTIVE_DIAGNOSTICS_H
#define DSM_ADAPTIVE_DIAGNOSTICS_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <ostream>
#include <tuple>
#include <vector>

struct Edge;

// Stage-A observation only. In particular this class never selects a search
// strategy, skips a candidate, or changes a plan. State profiling and the
// optional copy-loop timer are independent so timer overhead can be measured
// with all counters/statistics disabled. No gate decision is made in C++.
class AdaptiveDiagnostics {
public:
    enum class Outcome {
        InjectivityDrop, LmaxDrop, LocalTemporalDrop,
        CumulativeSupportDrop, Extension
    };
    explicit AdaptiveDiagnostics(bool state_counters_enabled = false,
                                 bool neighbor_copy_timer_enabled = false)
        : state_enabled_(state_counters_enabled), timer_enabled_(neighbor_copy_timer_enabled) {}

    bool stateCountersEnabled() const noexcept { return state_enabled_; }
    bool neighborCopyTimerEnabled() const noexcept { return timer_enabled_; }

    // Explicit opt-in execution path, defined in its own translation unit.
    // The caller reserves destination capacity BEFORE entering this function.
    // Unlike a renamed clock wrapper, this function owns the entire timed
    // materialization loop. The default path keeps its original untimed loop.
    void copyNeighborIdsTimed(const std::vector<Edge>& source,
                              std::vector<int>& destination,
                              int query_vertex, std::int64_t node_id);

    static std::uint64_t ceilLog2(std::uint64_t value) {
        if (value <= 1) return 0;
        std::uint64_t result = 0;
        for (--value; value != 0; value >>= 1) ++result;
        return result;
    }

    void recordState(int query_vertex, std::int64_t node_id, std::size_t raw_size,
                     const std::vector<std::size_t>& neighbor_sizes) {
        if (!state_enabled_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        observeState(totals_, raw_size, neighbor_sizes);
        observeState(contexts_[{query_vertex, node_id}], raw_size, neighbor_sizes);
    }
    void recordStructureOutput(int query_vertex, std::int64_t node_id, std::size_t count) {
        if (!state_enabled_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto apply = [&](Stats& s) {
            add(s.structure_output_candidates, count);
            observe(s.output_sizes, count);
        };
        apply(totals_); apply(contexts_[{query_vertex, node_id}]);
    }
    void recordNeighborCopy(int query_vertex, std::int64_t node_id,
                            std::size_t elements, std::size_t element_bytes) {
        if (!state_enabled_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto apply = [&](Stats& s) {
            add(s.neighbor_copy_calls, 1);
            add(s.neighbor_copy_elements, elements);
            add(s.neighbor_copy_bytes, multiply(elements, element_bytes));
        };
        apply(totals_); apply(contexts_[{query_vertex, node_id}]);
    }
    void recordDirectProbe(int query_vertex) {
        if (!state_enabled_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        add(totals_.actual_structural_probes, 1);
        add(contexts_[{query_vertex, -1}].actual_structural_probes, 1);
    }
    // A diagnostics-only metadata lookup is NOT an algorithmic structure
    // probe and must never enter actual_structural_probes or copied elements.
    void recordShadowLmaxProbe(bool edge_exists, bool durable) {
        if (!state_enabled_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto apply = [&](Stats& s) {
            add(s.diagnostic_metadata_probes, 1);
            if (!edge_exists) add(s.diagnostic_missing_edges, 1);
            else if (!durable) add(s.diagnostic_short_edges, 1);
        };
        apply(totals_); apply(contexts_[{-1, -1}]);
    }
    // Record exactly one first-drop outcome for each STRUCTURE-OUTPUT
    // candidate. The caller still performs the original search, without
    // introducing pruning merely because the shadow Lmax check failed.
    void recordCandidateOutcome(int query_vertex, Outcome outcome) {
        if (!state_enabled_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto apply = [&](Stats& s) {
            add(s.candidate_outcomes, 1);
            if (outcome == Outcome::InjectivityDrop) { add(s.drop_injectivity, 1); return; }
            add(s.injectivity_pass, 1);
            if (outcome == Outcome::LmaxDrop) { add(s.drop_lmax, 1); return; }
            add(s.lmax_pass, 1);
            if (outcome == Outcome::LocalTemporalDrop) { add(s.drop_local_temporal, 1); return; }
            add(s.local_temporal_pass, 1);
            if (outcome == Outcome::CumulativeSupportDrop) { add(s.drop_cumulative_support, 1); return; }
            add(s.temporal_pass, 1);
            add(s.extensions, 1);
        };
        apply(totals_); apply(contexts_[{query_vertex, -1}]);
    }
    // Read Clock::now after the copy loop BEFORE calling this method. Mutex
    // acquisition and counter updates therefore remain outside the region.
    void recordNeighborCopyNanoseconds(int query_vertex, std::int64_t node_id,
                                      std::uint64_t elapsed) {
        if (!timer_enabled_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        timerAdd(timer_.samples, 1); timerAdd(timer_.nanoseconds, elapsed);
        auto& context = timer_contexts_[{query_vertex, node_id}];
        timerAdd(context.samples, 1); timerAdd(context.nanoseconds, elapsed);
    }

    void writeJsonMembers(std::ostream& out, bool enabled) const {
        out << "  \"adaptive_diagnostics\": ";
        if (!enabled) out << "null";
        else writeJson(out);
        out << ",\n";
    }
    void writeJson(std::ostream& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        out << "{\"schema_version\":1,\"state_counters_enabled\":" << (state_enabled_ ? "true" : "false")
            << ",\"neighbor_copy_timer_enabled\":" << (timer_enabled_ ? "true" : "false")
            << ",\"overflow\":" << (overflow_ ? "true" : "false")
            << ",\"costs_valid\":" << (state_enabled_ && !overflow_ ? "true" : "false")
            << ",\"definitions\":{"
            << "\"needed_elements\":\"sum over actual structural states: |R| * sum_i ceil(log2 |L_i|); sizes 0/1 contribute 0\","
            << "\"model_status\":\"frozen cost proxy, not a strict comparison-count upper bound; zero aggregate denominator is indeterminate\","
            << "\"generated_elements\":\"actual neighbor ID materialization elements; excludes seed copies, TD copies, and diagnostic metadata lookups\","
            << "\"funnel\":\"raw R -> structure -> injectivity -> shadow Lmax -> local temporal kappa -> cumulative support -> extension; mutually exclusive first-drop attribution\","
            << "\"structural_probes\":\"actual Direct membership calls only; merge comparisons and hypothetical closed-edge counts are not probes\","
            << "\"context\":\"q is extension vertex for states/outcomes, lookup anchor for neighbor copy; node=-1 denotes non-DAG metadata/funnel\","
            << "\"histogram\":\"index0=size0; index i=1..64 covers [2^(i-1),2^i)\","
            << "\"timer_region\":\"vector ID push_back materialization loop only, after reserve; mutex/counter updates outside region; clock overhead remains possible and is assessed separately\"},"
            << "\"state_profile\":";
        if (!state_enabled_) out << "null";
        else {
            out << "{\"totals\":"; writeStats(out, totals_);
            out << ",\"funnel_complete\":" <<
                (!overflow_ && totals_.candidate_outcomes == totals_.structure_output_candidates &&
                 totals_.raw_candidate_elements >= totals_.structure_output_candidates ? "true" : "false");
            out << ",\"contexts\":[";
            bool first = true;
            for (const auto& item : contexts_) {
                if (!first) out << ',';
                first = false;
                out << "{\"query_vertex\":" << item.first.first << ",\"node_id\":" << item.first.second << ",\"stats\":";
                writeStats(out, item.second); out << '}';
            }
            out << "]}";
        }
        out << ",\"neighbor_copy_timer\":";
        if (!timer_enabled_) out << "null";
        else {
            out << "{\"overflow\":" << (timer_overflow_ ? "true" : "false") << ",\"samples\":" << timer_.samples
                << ",\"nanoseconds\":" << timer_.nanoseconds << ",\"contexts\":[";
            bool first = true;
            for (const auto& item : timer_contexts_) {
                if (!first) out << ',';
                first = false;
                out << "{\"query_vertex\":" << item.first.first << ",\"node_id\":" << item.first.second
                    << ",\"samples\":" << item.second.samples << ",\"nanoseconds\":" << item.second.nanoseconds << '}';
            }
            out << "]}";
        }
        out << '}';
    }

private:
    using U64 = std::uint64_t;
    using Context = std::pair<int, std::int64_t>;
    struct Sizes {
        U64 count = 0, sum = 0, minimum = std::numeric_limits<U64>::max(), maximum = 0;
        std::array<U64, 65> histogram{};
    };
#define DSM_ADAPTIVE_COUNTS(X) \
    X(states) X(raw_candidate_elements) X(neighbor_lists) X(needed_elements) \
    X(state_merge_proxy_elements) X(empty_R_states) X(empty_neighbor_lists) X(singleton_neighbor_lists) \
    X(structure_output_candidates) X(neighbor_copy_calls) X(neighbor_copy_elements) X(neighbor_copy_bytes) \
    X(actual_structural_probes) X(diagnostic_metadata_probes) X(diagnostic_missing_edges) X(diagnostic_short_edges) \
    X(candidate_outcomes) X(injectivity_pass) X(lmax_pass) X(local_temporal_pass) X(temporal_pass) X(extensions) \
    X(drop_injectivity) X(drop_lmax) X(drop_local_temporal) X(drop_cumulative_support)
    struct Stats {
#define DSM_ADAPTIVE_DECLARE(name) U64 name = 0;
        DSM_ADAPTIVE_COUNTS(DSM_ADAPTIVE_DECLARE)
#undef DSM_ADAPTIVE_DECLARE
        Sizes raw_sizes, neighbor_sizes, output_sizes;
    };
    struct Timer { U64 samples = 0, nanoseconds = 0; };
    const bool state_enabled_;
    const bool timer_enabled_;
    mutable std::mutex mutex_;
    bool overflow_ = false, timer_overflow_ = false;
    Stats totals_;
    std::map<Context, Stats> contexts_;
    Timer timer_;
    std::map<Context, Timer> timer_contexts_;

    static void checkedAdd(U64& target, U64 value, bool& overflow) {
        if (value > std::numeric_limits<U64>::max() - target) {
            overflow = true; target = std::numeric_limits<U64>::max();
        } else target += value;
    }
    void add(U64& target, U64 value) { checkedAdd(target, value, overflow_); }
    void timerAdd(U64& target, U64 value) { checkedAdd(target, value, timer_overflow_); }
    U64 multiply(U64 a, U64 b) {
        if (b != 0 && a > std::numeric_limits<U64>::max() / b) {
            overflow_ = true; return std::numeric_limits<U64>::max();
        }
        return a * b;
    }
    void observe(Sizes& s, U64 value) {
        add(s.count, 1); add(s.sum, value);
        s.minimum = std::min(s.minimum, value); s.maximum = std::max(s.maximum, value);
        std::size_t index = 0;
        for (U64 n = value; n != 0; n >>= 1) ++index;
        add(s.histogram[index], 1);
    }
    void observeState(Stats& s, U64 raw, const std::vector<std::size_t>& lists) {
        add(s.states, 1); add(s.raw_candidate_elements, raw); add(s.neighbor_lists, lists.size());
        observe(s.raw_sizes, raw);
        if (raw == 0) add(s.empty_R_states, 1);
        U64 logs = 0, merge = raw;
        for (const auto size : lists) {
            observe(s.neighbor_sizes, size); add(logs, ceilLog2(size)); add(merge, size);
            if (size == 0) add(s.empty_neighbor_lists, 1);
            if (size == 1) add(s.singleton_neighbor_lists, 1);
        }
        add(s.needed_elements, multiply(raw, logs)); add(s.state_merge_proxy_elements, merge);
    }
    static void writeSizes(std::ostream& out, const Sizes& s) {
        out << "{\"count\":" << s.count << ",\"sum\":" << s.sum << ",\"min\":";
        if (s.count == 0) out << "null"; else out << s.minimum;
        out << ",\"max\":";
        if (s.count == 0) out << "null"; else out << s.maximum;
        out << ",\"histogram_log2\":[";
        for (std::size_t i = 0; i < s.histogram.size(); ++i) { if (i != 0) out << ','; out << s.histogram[i]; }
        out << "]}";
    }
    static void writeStats(std::ostream& out, const Stats& s) {
        out << '{';
#define DSM_ADAPTIVE_WRITE(name) out << "\"" #name "\":" << s.name << ',';
        DSM_ADAPTIVE_COUNTS(DSM_ADAPTIVE_WRITE)
#undef DSM_ADAPTIVE_WRITE
        out << "\"raw_sizes\":"; writeSizes(out, s.raw_sizes);
        out << ",\"neighbor_sizes\":"; writeSizes(out, s.neighbor_sizes);
        out << ",\"output_sizes\":"; writeSizes(out, s.output_sizes);
        out << '}';
    }
#undef DSM_ADAPTIVE_COUNTS
};

#endif
