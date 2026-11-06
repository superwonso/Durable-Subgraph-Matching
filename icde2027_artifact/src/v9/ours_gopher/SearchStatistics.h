#ifndef SEARCH_STATISTICS_H
#define SEARCH_STATISTICS_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iosfwd>

#include "CandidateDag.h"

inline constexpr const char* kDagOperatorStatisticsSchemaVersion =
    "dag_operator_summary_v1";

// These profile values are defined in SearchStatistics.cpp.  Keeping the
// erased-control switch out of this header is intentional: the statistics-OFF
// kernels include this type from a profile-independent translation unit whose
// preprocessed source and code generation must be identical in product and
// erased builds.
extern const bool kSearchStatisticsHooksErasedAtBuild;
extern const char* const kSearchStatisticsBuildProfile;

#ifndef DSM_GOPHER_SOURCE_IDENTITY
#define DSM_GOPHER_SOURCE_IDENTITY "unbound"
#endif
#ifndef DSM_GOPHER_COMMON_FLAGS_IDENTITY
#define DSM_GOPHER_COMMON_FLAGS_IDENTITY "unbound"
#endif

inline constexpr const char* kGopherSourceIdentity =
    DSM_GOPHER_SOURCE_IDENTITY;
inline constexpr const char* kGopherCommonFlagsIdentity =
    DSM_GOPHER_COMMON_FLAGS_IDENTITY;
#if defined(__VERSION__)
inline constexpr const char* kGopherCompilerIdentity = __VERSION__;
#elif defined(_MSC_VER)
inline constexpr const char* kGopherCompilerIdentity = "msvc";
#else
inline constexpr const char* kGopherCompilerIdentity = "unknown";
#endif
struct SearchStatistics {
    std::uint64_t root_tasks = 0;
    std::uint64_t dfs_states = 0;
    // Candidate counters use an explicit common schema:
    // raw_extension_candidates counts the TD-relation row entries inspected
    // before Gopher's additional structural intersections;
    // generated_candidates retains the historical post-structural meaning;
    // accepted_extensions counts extensions that recurse.
    std::uint64_t raw_extension_candidates = 0;
    std::uint64_t generated_candidates = 0;
    std::uint64_t accepted_candidates = 0;
    std::uint64_t accepted_extensions = 0;
    std::uint64_t completed_matches = 0;
    std::uint64_t pruned_injectivity = 0;
    std::uint64_t pruned_structural = 0;
    std::uint64_t pruned_temporal_support = 0;
    std::uint64_t pruned_active_support = 0;

    std::uint64_t logical_td_block_lookups = 0;
    std::uint64_t physical_td_block_lookups = 0;
    std::uint64_t logical_neighbor_lookups = 0;
    std::uint64_t physical_neighbor_lookups = 0;
    std::uint64_t logical_structural_intersections = 0;
    std::uint64_t physical_structural_intersections = 0;
    std::uint64_t logical_temporal_edge_lookups = 0;
    std::uint64_t physical_temporal_edge_lookups = 0;
    std::uint64_t logical_temporal_intersections = 0;
    std::uint64_t physical_temporal_intersections = 0;
    std::uint64_t logical_longest_run = 0;
    std::uint64_t physical_longest_run = 0;

    std::uint64_t structural_input_elements = 0;
    std::uint64_t temporal_input_intervals = 0;
    std::uint64_t output_buffer_bytes = 0;

    std::uint64_t structural_cache_hits = 0;
    std::uint64_t structural_cache_misses = 0;
    std::uint64_t structural_cache_admitted = 0;
    std::uint64_t structural_cache_bypassed = 0;
    std::uint64_t structural_cache_bytes = 0;
    std::uint64_t structural_cache_entries = 0;
    std::uint64_t structural_cache_duplicate_inserts = 0;
    std::uint64_t structural_cache_rejected_budget = 0;
    std::uint64_t structural_cache_rejected_entry_size = 0;
    std::uint64_t structural_cache_rejected_cost = 0;
    std::uint64_t structural_cache_hit_result_elements = 0;
    std::uint64_t structural_cache_hit_saved_work = 0;
    std::uint64_t structural_cache_max_hit_result_elements = 0;
    std::uint64_t structural_cache_admitted_result_elements = 0;
    std::uint64_t structural_cache_admitted_estimated_work = 0;
    std::uint64_t temporal_cache_hits = 0;
    std::uint64_t temporal_cache_misses = 0;
    std::uint64_t temporal_cache_admitted = 0;
    std::uint64_t temporal_cache_bypassed = 0;
    std::uint64_t temporal_cache_bytes = 0;
    std::uint64_t temporal_cache_entries = 0;
    std::uint64_t temporal_cache_duplicate_inserts = 0;
    std::uint64_t temporal_cache_rejected_budget = 0;
    std::uint64_t temporal_cache_rejected_entry_size = 0;
    std::uint64_t temporal_cache_rejected_cost = 0;
    std::uint64_t temporal_cache_hit_result_elements = 0;
    std::uint64_t temporal_cache_hit_saved_work = 0;
    std::uint64_t temporal_cache_max_hit_result_elements = 0;
    std::uint64_t temporal_cache_admitted_result_elements = 0;
    std::uint64_t temporal_cache_admitted_estimated_work = 0;
    std::uint64_t longest_cache_hits = 0;
    std::uint64_t longest_cache_misses = 0;

    // Per-operation hits make aggregate cache hits interpretable. The
    // subtree counter is exact at the boundary we can observe: each counted
    // parent root (and therefore its entire recursive evaluation) was not
    // invoked because a structural-v2 node hit before parent evaluation.
    std::uint64_t td_block_cache_hits = 0;
    std::uint64_t neighbor_lookup_cache_hits = 0;
    std::uint64_t structural_intersection_cache_hits = 0;
    std::uint64_t edge_time_cache_hits = 0;
    std::uint64_t temporal_intersection_cache_hits = 0;
    std::uint64_t longest_run_cache_hits = 0;
    std::uint64_t structural_parent_subtrees_skipped = 0;
    std::uint64_t structural_v2_intersection_nodes = 0;
    std::uint64_t structural_v2_eligible_nodes = 0;
    std::uint64_t structural_v2_no_projection_reuse_nodes = 0;
    std::uint64_t structural_v2_eligible_probes = 0;
    std::uint64_t structural_v2_projection_bypasses = 0;

    std::uint64_t dag_nodes_attempted = 0;
    std::uint64_t dag_nodes_materialized = 0;
    std::uint64_t dag_nodes_merged = 0;

    // Exact runtime-key diagnostics are collected only in the explicitly
    // enabled, untimed diagnostic mode.  A RuntimeKey includes its node id,
    // so keys from different operation types cannot alias.  Consequently,
    // exact_runtime_key_distinct is also the sum of the per-operation
    // distinct counts.  The reuse opportunity count is I - U and its ratio
    // is derived downstream as (I - U) / I.
    bool exact_runtime_key_diagnostics = false;
    std::uint64_t exact_runtime_key_cap = 0;
    std::uint64_t exact_runtime_key_invocations = 0;
    std::uint64_t exact_runtime_key_distinct = 0;
    std::array<std::uint64_t, kDagOpCount>
        exact_runtime_key_invocations_by_op{};
    std::array<std::uint64_t, kDagOpCount>
        exact_runtime_key_distinct_by_op{};
    std::array<std::uint64_t, kDagOpCount>
        runtime_key_construction_nanoseconds_by_op{};
    std::array<std::uint64_t, kDagOpCount>
        cache_lookup_nanoseconds_by_op{};

    // Diagnostic summary for the seven stable DagOp values.  Cache hits and
    // misses count actual BoundedMemo::find probes only; a NoMemo execution or
    // an admission-policy bypass is neither.  result_observations counts every
    // successful logical return, including hits, empty values, borrowed
    // immutable views, and duplicate-insert returns.  Candidate/interval
    // result units are element counts; the scalar longest-run result uses one
    // result unit and reports its returned length separately below.
    // exclusive_compute is the local physical operator work actually
    // incurred: source/binding resolution, result materialization, local
    // intersection, or the longest-run scan. It excludes child recursion,
    // runtime-key construction, cache lookup, shared_ptr wrapping, and cache
    // insertion. If policy requires source resolution before a probe, that
    // work is included for both hit and miss invocations.
    // exclusive_miss_compute is the same local-work boundary, restricted to
    // invocations whose actual BoundedMemo::find probe missed; pre-probe local
    // work is attributed to that invocation after its outcome is known.
    std::array<std::uint64_t, kDagOpCount>
        dag_operator_invocations_by_op{};
    std::array<std::uint64_t, kDagOpCount>
        dag_operator_cache_hits_by_op{};
    std::array<std::uint64_t, kDagOpCount>
        dag_operator_cache_misses_by_op{};
    std::array<std::uint64_t, kDagOpCount>
        dag_operator_exclusive_compute_nanoseconds_by_op{};
    std::array<std::uint64_t, kDagOpCount>
        dag_operator_exclusive_miss_compute_nanoseconds_by_op{};
    std::array<std::uint64_t, kDagOpCount>
        dag_operator_result_units_sum_by_op{};
    std::array<std::uint64_t, kDagOpCount>
        dag_operator_result_observations_by_op{};
    std::array<std::uint64_t, kDagOpCount>
        dag_operator_cache_admissions_by_op{};
    std::array<std::uint64_t, kDagOpCount>
        dag_operator_cache_admitted_bytes_by_op{};
    std::uint64_t longest_run_returned_length_sum = 0;
    std::uint64_t longest_run_returned_length_observations = 0;
    bool dag_operator_reconciliation_passed = false;

    std::uint64_t runtime_key_construction_nanoseconds = 0;
    std::uint64_t cache_lookup_nanoseconds = 0;
    std::uint64_t structural_compute_nanoseconds = 0;
    std::uint64_t temporal_compute_nanoseconds = 0;
    std::uint64_t longest_compute_nanoseconds = 0;
    std::uint64_t search_nanoseconds = 0;
};

class SearchStatisticsCollector {
public:
    explicit SearchStatisticsCollector(bool enabled = true)
        : enabled_(enabled) {}

    bool enabled() const;
    // The matcher dispatches once at run entry and uses these specializations
    // throughout the recursive search.  In the false specialization the
    // counter expression has no observable use, so optimized builds erase the
    // atomic operation and its enabled check entirely.
    template <bool CollectStatistics>
    void addSpecialized(
        std::atomic<std::uint64_t>& counter,
        std::uint64_t value = 1) {
        if constexpr (CollectStatistics) {
            counter.fetch_add(value, std::memory_order_relaxed);
        } else {
            (void)counter;
            (void)value;
        }
    }
    template <bool CollectStatistics>
    void setSpecialized(
        std::atomic<std::uint64_t>& counter,
        std::uint64_t value) {
        if constexpr (CollectStatistics) {
            counter.store(value, std::memory_order_relaxed);
        } else {
            (void)counter;
            (void)value;
        }
    }
    void add(
        std::atomic<std::uint64_t>& counter,
        std::uint64_t value = 1);
    void set(
        std::atomic<std::uint64_t>& counter,
        std::uint64_t value);

#define GOPHER_STAT_FIELD(name) std::atomic<std::uint64_t> name{0}
    GOPHER_STAT_FIELD(root_tasks);
    GOPHER_STAT_FIELD(dfs_states);
    GOPHER_STAT_FIELD(raw_extension_candidates);
    GOPHER_STAT_FIELD(generated_candidates);
    GOPHER_STAT_FIELD(accepted_candidates);
    GOPHER_STAT_FIELD(accepted_extensions);
    GOPHER_STAT_FIELD(completed_matches);
    GOPHER_STAT_FIELD(pruned_injectivity);
    GOPHER_STAT_FIELD(pruned_structural);
    GOPHER_STAT_FIELD(pruned_temporal_support);
    GOPHER_STAT_FIELD(pruned_active_support);
    GOPHER_STAT_FIELD(logical_td_block_lookups);
    GOPHER_STAT_FIELD(physical_td_block_lookups);
    GOPHER_STAT_FIELD(logical_neighbor_lookups);
    GOPHER_STAT_FIELD(physical_neighbor_lookups);
    GOPHER_STAT_FIELD(logical_structural_intersections);
    GOPHER_STAT_FIELD(physical_structural_intersections);
    GOPHER_STAT_FIELD(logical_temporal_edge_lookups);
    GOPHER_STAT_FIELD(physical_temporal_edge_lookups);
    GOPHER_STAT_FIELD(logical_temporal_intersections);
    GOPHER_STAT_FIELD(physical_temporal_intersections);
    GOPHER_STAT_FIELD(logical_longest_run);
    GOPHER_STAT_FIELD(physical_longest_run);
    GOPHER_STAT_FIELD(structural_input_elements);
    GOPHER_STAT_FIELD(temporal_input_intervals);
    GOPHER_STAT_FIELD(output_buffer_bytes);
    GOPHER_STAT_FIELD(td_block_cache_hits);
    GOPHER_STAT_FIELD(neighbor_lookup_cache_hits);
    GOPHER_STAT_FIELD(structural_intersection_cache_hits);
    GOPHER_STAT_FIELD(edge_time_cache_hits);
    GOPHER_STAT_FIELD(temporal_intersection_cache_hits);
    GOPHER_STAT_FIELD(longest_run_cache_hits);
    GOPHER_STAT_FIELD(structural_parent_subtrees_skipped);
    GOPHER_STAT_FIELD(structural_v2_eligible_probes);
    GOPHER_STAT_FIELD(structural_v2_projection_bypasses);
    GOPHER_STAT_FIELD(runtime_key_construction_nanoseconds);
    GOPHER_STAT_FIELD(cache_lookup_nanoseconds);
    GOPHER_STAT_FIELD(structural_compute_nanoseconds);
    GOPHER_STAT_FIELD(temporal_compute_nanoseconds);
    GOPHER_STAT_FIELD(longest_compute_nanoseconds);
    GOPHER_STAT_FIELD(search_nanoseconds);
#undef GOPHER_STAT_FIELD

    std::array<std::atomic<std::uint64_t>, kDagOpCount>
        runtime_key_construction_nanoseconds_by_op{};
    std::array<std::atomic<std::uint64_t>, kDagOpCount>
        cache_lookup_nanoseconds_by_op{};
    std::array<std::atomic<std::uint64_t>, kDagOpCount>
        dag_operator_invocations_by_op{};
    std::array<std::atomic<std::uint64_t>, kDagOpCount>
        dag_operator_cache_hits_by_op{};
    std::array<std::atomic<std::uint64_t>, kDagOpCount>
        dag_operator_cache_misses_by_op{};
    std::array<std::atomic<std::uint64_t>, kDagOpCount>
        dag_operator_exclusive_compute_nanoseconds_by_op{};
    std::array<std::atomic<std::uint64_t>, kDagOpCount>
        dag_operator_exclusive_miss_compute_nanoseconds_by_op{};
    std::array<std::atomic<std::uint64_t>, kDagOpCount>
        dag_operator_result_units_sum_by_op{};
    std::array<std::atomic<std::uint64_t>, kDagOpCount>
        dag_operator_result_observations_by_op{};
    std::array<std::atomic<std::uint64_t>, kDagOpCount>
        dag_operator_cache_admissions_by_op{};
    std::array<std::atomic<std::uint64_t>, kDagOpCount>
        dag_operator_cache_admitted_bytes_by_op{};
    std::atomic<std::uint64_t> longest_run_returned_length_sum{0};
    std::atomic<std::uint64_t>
        longest_run_returned_length_observations{0};

    SearchStatistics snapshot() const;

private:
    bool enabled_ = true;
};

void writeSearchStatistics(std::ostream& output, const SearchStatistics& statistics);

#endif
