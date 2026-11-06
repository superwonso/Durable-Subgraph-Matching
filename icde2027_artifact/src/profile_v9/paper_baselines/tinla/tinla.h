#ifndef PAPER_BASELINES_TINLA_TINLA_H
#define PAPER_BASELINES_TINLA_TINLA_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../../benchmark/e3_e4_adapter_v1/E3E4RuntimeHooksV1.h"
#include "../../original/PrimaryMetricsV2.h"

namespace tinla {

using Label = std::uint8_t;
constexpr std::size_t kLabelCount = 5;
constexpr Label kInvalidLabel = static_cast<Label>(kLabelCount);
constexpr std::uint32_t kDefaultLabelSeed = 42U;

enum class LabelMode { SequentialRng, StableId };
enum class DurationMode { Cardinality, Consecutive };
enum class IndexMode { Tinla, Ctinla };

const char* labelModeName(LabelMode mode);
const char* durationModeName(DurationMode mode);
const char* indexModeName(IndexMode mode);
Label stableIdLabel(int external_id, std::uint32_t label_seed);
Label labelFromString(const std::string& text);
std::string labelToString(Label label);

struct Interval {
    int start = 0;
    int end = -1;

    std::int64_t length() const;
    bool operator==(const Interval& other) const;
};

struct TemporalEdge {
    int from = -1;
    int to = -1;
    std::vector<Interval> active;
};

struct AdjEdge {
    int vertex = -1;
    int edge_id = -1;
};

struct Graph {
    std::vector<int> external_ids;
    std::vector<Label> labels;
    std::vector<TemporalEdge> edges;
    std::vector<std::vector<AdjEdge>> out;
    std::vector<std::vector<AdjEdge>> in;
    std::size_t input_occurrences = 0;
    std::size_t unique_occurrences = 0;
    int min_time = 0;
    int max_time = -1;

    const TemporalEdge* findEdge(int from, int to) const;
    int externalId(int internal_id) const;
    std::size_t memoryBytes() const;
};

// This narrow input surface is for callers that already authenticated and
// strictly parsed every row.  It deliberately has no path, seed, or ambient
// label-mode parameter: the complete explicit runtime-label vector is the
// only source of Graph::labels on this path.
struct ValidatedTemporalOccurrence {
    int from = -1;
    int to = -1;
    int time = 0;
};

struct ValidatedRuntimeLabel {
    int external_id = -1;
    Label runtime_label = kInvalidLabel;
};

struct Query {
    std::vector<std::string> names;
    std::vector<Label> labels;
    std::vector<std::pair<int, int>> edges;
    std::vector<std::vector<int>> out;
    std::vector<std::vector<int>> in;
};

struct CountSegment {
    int start = 0;
    int end = -1;
    int count = 0;
};

struct PresenceCell {
    std::uint64_t key = 0;
    std::vector<Interval> active;
};

struct CounterCell {
    std::uint64_t key = 0;
    std::vector<CountSegment> active;
};

class NeighborLabelIndex {
public:
    explicit NeighborLabelIndex(IndexMode mode = IndexMode::Tinla);
    bool build(const Graph& graph, std::string& error);
    bool supports(
        int vertex,
        bool incoming,
        Label neighbor_label,
        int required_count,
        int minimum_duration,
        DurationMode duration_mode) const;
    std::size_t memoryBytes() const;
    std::size_t cellCount() const;
    std::size_t segmentCount() const;
    IndexMode mode() const;

private:
    IndexMode mode_;
    std::vector<PresenceCell> presence_;
    std::vector<CounterCell> counters_;
};

struct RunOptions {
    int minimum_duration = 1;
    std::uint32_t label_seed = kDefaultLabelSeed;
    LabelMode label_mode = LabelMode::SequentialRng;
    DurationMode duration_mode = DurationMode::Cardinality;
    IndexMode index_mode = IndexMode::Tinla;
    std::size_t threads = 1;
    bool count_only = false;
};

// Deliberately excludes label seed/mode and a full-output switch.  Labels for
// this path have already been injected explicitly into Graph.
struct CountOnlyOptions {
    int minimum_duration = 1;
    DurationMode duration_mode = DurationMode::Consecutive;
    IndexMode index_mode = IndexMode::Tinla;
    std::size_t threads = 1;
};

struct Counters {
    std::uint64_t initial_candidates = 0;
    std::uint64_t post_index_candidates = 0;
    std::uint64_t post_refine_candidates = 0;
    std::uint64_t index_pruned = 0;
    std::uint64_t refine_pruned = 0;
    std::uint64_t refine_rounds = 0;
    std::uint64_t root_candidates = 0;
    std::uint64_t dfs_states = 0;
    std::uint64_t raw_extension_candidates = 0;
    std::uint64_t injectivity_pruned = 0;
    std::uint64_t structural_pruned = 0;
    std::uint64_t temporal_pruned = 0;
    std::uint64_t dynamic_refine_calls = 0;
    std::uint64_t dynamic_refine_pruned = 0;
    std::uint64_t dynamic_refine_rounds = 0;

    Counters& operator+=(const Counters& other);
};

struct Match {
    std::vector<int> mapping;
    std::vector<Interval> support;
};

struct Timings {
    std::uint64_t build_index_ns = 0;
    std::uint64_t filter_candidates_ns = 0;
    std::uint64_t refine_candidates_ns = 0;
    std::uint64_t order_ns = 0;
    double read_graph_ms = 0.0;
    double preprocess_graph_ms = 0.0;
    double read_query_ms = 0.0;
    double build_index_ms = 0.0;
    double filter_candidates_ms = 0.0;
    double refine_candidates_ms = 0.0;
    std::uint64_t enumerate_ns = 0;
    double enumerate_ms = 0.0;
    double end_to_end_ms = 0.0;
};

struct V8LoadTimings {
    std::uint64_t read_parse_ns = 0;
    std::uint64_t label_ns = 0;
    std::uint64_t filter_ns = 0;
    std::uint64_t graph_build_ns = 0;
};

struct RunResult {
    std::uint64_t match_count = 0;
    // Saturating authority counter for the observed count-only path.  An
    // overflow is retained as typed evidence instead of being collapsed into
    // a diagnostic string and losing the child-ledger finalization boundary.
    bool count_overflow = false;
    std::vector<Match> matches;
    Counters counters;
    Timings timings;
    dsm::pure_online_v2::PrimaryMetrics primary_metrics;
    std::size_t graph_memory_bytes = 0;
    std::size_t index_memory_bytes = 0;
    std::size_t index_cells = 0;
    std::size_t index_segments = 0;
    std::size_t peak_rss_bytes = 0;
    std::size_t threads_requested = 1;
    std::size_t threads_used = 1;
    // Populated only by runCountOnlyObserved().  Entry i is the single CPU
    // to which worker i was successfully bound and then re-observed.
    std::vector<std::uint32_t> affinity_cpu_set;
    // Provenance-carrying counterpart used by the closed adapter.  Scripted
    // observations remain useful for source-only tests but cannot become an
    // authority ThreadObservation.
    std::vector<dsm::e3e4::adapter_v1::runtime_hooks_v1::
        WorkerAffinityObservation> worker_affinity_observations;
};

enum class NativeCountPhase {
    Requirements,
    IndexConstruction,
    CandidateFiltering,
    CandidateRefinement,
    OrderConstruction,
    Enumeration,
    CountReduction,
};

using NativePhaseObserver = std::function<bool(
    NativeCountPhase phase,
    std::uint64_t steady_clock_ns,
    std::string& error)>;

using RuntimeHookedNativePhaseObserver = std::function<bool(
    NativeCountPhase phase,
    const dsm::e3e4::adapter_v1::runtime_hooks_v1::
        MonotonicClockReading& reading,
    std::string& error)>;

namespace e3e4_adapter_v1 {
class AdapterProductionAccess;
namespace test_support {
#if defined(DSM_E3E4_TEST_HOOKS)
class TestOnlyCoreAccess;
#endif
}  // namespace test_support
}  // namespace e3e4_adapter_v1

class RuntimeHookedCountOnlyCore final {
private:
    static bool runAdapterOwned(
        const Graph& graph,
        const Query& query,
        const CountOnlyOptions& options,
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::MonotonicNowSource&
            clock,
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::WorkerAffinityBinder&
            affinity_binder,
        const RuntimeHookedNativePhaseObserver& observer,
        RunResult& result,
        std::string& error);
    friend class e3e4_adapter_v1::AdapterProductionAccess;
#if defined(DSM_E3E4_TEST_HOOKS)
    friend class e3e4_adapter_v1::test_support::TestOnlyCoreAccess;
#endif
};

#if defined(DSM_E3E4_TEST_HOOKS)
namespace e3e4_adapter_v1::test_support {

class TestOnlyCoreAccess final {
public:
    static bool run(
        const Graph& graph,
        const Query& query,
        const CountOnlyOptions& options,
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::MonotonicNowSource&
            clock,
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::WorkerAffinityBinder&
            affinity_binder,
        const RuntimeHookedNativePhaseObserver& observer,
        RunResult& result,
        std::string& error);
};

}  // namespace e3e4_adapter_v1::test_support
#endif

std::vector<Interval> intersectIntervals(
    const std::vector<Interval>& lhs,
    const std::vector<Interval>& rhs);
std::int64_t duration(
    const std::vector<Interval>& intervals,
    DurationMode mode);

#if !defined(DSM_E3E4_CORE_ONLY)
bool readTemporalGraph(
    const std::string& path,
    std::uint32_t seed,
    LabelMode label_mode,
    Graph& graph,
    double* read_ms,
    double* preprocess_ms,
    std::string& error,
    const std::vector<ValidatedRuntimeLabel>* explicit_labels = nullptr,
    bool apply_consecutive_pair_filter = false,
    V8LoadTimings* v8_timings = nullptr);
#endif  // !defined(DSM_E3E4_CORE_ONLY)

// Constructs the historical native graph representation from an already
// validated occurrence stream and injects an exact, strictly increasing,
// full-coverage runtime-label vector.  It performs no filesystem I/O and
// never derives labels from a seed.
bool buildTemporalGraphFromValidatedInput(
    const std::vector<ValidatedTemporalOccurrence>& occurrences,
    const std::vector<ValidatedRuntimeLabel>& labels_by_external_id,
    Graph& graph,
    std::string& error);
#if !defined(DSM_E3E4_CORE_ONLY)
bool readQuery(const std::string& path, Query& query, std::string& error);

bool runMatcher(
    const Graph& graph,
    const Query& query,
    const RunOptions& options,
    RunResult& result,
    std::string& error);
#endif  // !defined(DSM_E3E4_CORE_ONLY)

// Dedicated E3/E4 count-only core.  Unlike the historical CLI, this entry
// point cannot collect matches or emit legacy result/timing files.  Every
// native boundary is reported with the core's steady-clock uint64 timestamp;
// order construction has its own boundary.  Its workers are bound to exact
// CPUs 0..threads-1 and the observed singleton affinity set is returned.
#if !defined(DSM_E3E4_CORE_ONLY)
bool runCountOnlyObserved(
    const Graph& graph,
    const Query& query,
    const CountOnlyOptions& options,
    const NativePhaseObserver& observer,
    RunResult& result,
    std::string& error);
#endif  // !defined(DSM_E3E4_CORE_ONLY)

}  // namespace tinla

#endif
