#include "SearchStatistics.h"

#include <ostream>

#if defined(DSM_ERASE_SEARCH_STATISTICS)
const bool kSearchStatisticsHooksErasedAtBuild = true;
const char* const kSearchStatisticsBuildProfile = "erased-control";
#else
const bool kSearchStatisticsHooksErasedAtBuild = false;
const char* const kSearchStatisticsBuildProfile = "product";
#endif

bool SearchStatisticsCollector::enabled() const {
#if defined(DSM_ERASE_SEARCH_STATISTICS)
    return false;
#else
    return enabled_;
#endif
}

void SearchStatisticsCollector::add(
    std::atomic<std::uint64_t>& counter,
    std::uint64_t value) {
#if defined(DSM_ERASE_SEARCH_STATISTICS)
    (void)counter;
    (void)value;
#else
    if (enabled_) {
        counter.fetch_add(value, std::memory_order_relaxed);
    }
#endif
}

void SearchStatisticsCollector::set(
    std::atomic<std::uint64_t>& counter,
    std::uint64_t value) {
#if defined(DSM_ERASE_SEARCH_STATISTICS)
    (void)counter;
    (void)value;
#else
    if (enabled_) {
        counter.store(value, std::memory_order_relaxed);
    }
#endif
}

SearchStatistics SearchStatisticsCollector::snapshot() const {
    SearchStatistics result;
    if (!enabled()) return result;
#define COPY_FIELD(name) result.name = name.load(std::memory_order_relaxed)
    COPY_FIELD(root_tasks);
    COPY_FIELD(dfs_states);
    COPY_FIELD(raw_extension_candidates);
    COPY_FIELD(generated_candidates);
    COPY_FIELD(accepted_candidates);
    COPY_FIELD(accepted_extensions);
    COPY_FIELD(completed_matches);
    COPY_FIELD(pruned_injectivity);
    COPY_FIELD(pruned_structural);
    COPY_FIELD(pruned_temporal_support);
    COPY_FIELD(pruned_active_support);
    COPY_FIELD(logical_td_block_lookups);
    COPY_FIELD(physical_td_block_lookups);
    COPY_FIELD(logical_neighbor_lookups);
    COPY_FIELD(physical_neighbor_lookups);
    COPY_FIELD(logical_structural_intersections);
    COPY_FIELD(physical_structural_intersections);
    COPY_FIELD(logical_temporal_edge_lookups);
    COPY_FIELD(physical_temporal_edge_lookups);
    COPY_FIELD(logical_temporal_intersections);
    COPY_FIELD(physical_temporal_intersections);
    COPY_FIELD(logical_longest_run);
    COPY_FIELD(physical_longest_run);
    COPY_FIELD(structural_input_elements);
    COPY_FIELD(temporal_input_intervals);
    COPY_FIELD(output_buffer_bytes);
    COPY_FIELD(td_block_cache_hits);
    COPY_FIELD(neighbor_lookup_cache_hits);
    COPY_FIELD(structural_intersection_cache_hits);
    COPY_FIELD(edge_time_cache_hits);
    COPY_FIELD(temporal_intersection_cache_hits);
    COPY_FIELD(longest_run_cache_hits);
    COPY_FIELD(structural_parent_subtrees_skipped);
    COPY_FIELD(structural_v2_eligible_probes);
    COPY_FIELD(structural_v2_projection_bypasses);
    COPY_FIELD(runtime_key_construction_nanoseconds);
    COPY_FIELD(cache_lookup_nanoseconds);
    COPY_FIELD(structural_compute_nanoseconds);
    COPY_FIELD(temporal_compute_nanoseconds);
    COPY_FIELD(longest_compute_nanoseconds);
    COPY_FIELD(search_nanoseconds);
#undef COPY_FIELD
    for (std::size_t operation = 0;
         operation < kDagOpCount;
         ++operation) {
        result.runtime_key_construction_nanoseconds_by_op[operation] =
            runtime_key_construction_nanoseconds_by_op[operation].load(
                std::memory_order_relaxed);
        result.cache_lookup_nanoseconds_by_op[operation] =
            cache_lookup_nanoseconds_by_op[operation].load(
                std::memory_order_relaxed);
        result.dag_operator_invocations_by_op[operation] =
            dag_operator_invocations_by_op[operation].load(
                std::memory_order_relaxed);
        result.dag_operator_cache_hits_by_op[operation] =
            dag_operator_cache_hits_by_op[operation].load(
                std::memory_order_relaxed);
        result.dag_operator_cache_misses_by_op[operation] =
            dag_operator_cache_misses_by_op[operation].load(
                std::memory_order_relaxed);
        result
            .dag_operator_exclusive_compute_nanoseconds_by_op[operation] =
            dag_operator_exclusive_compute_nanoseconds_by_op[operation]
                .load(std::memory_order_relaxed);
        result
            .dag_operator_exclusive_miss_compute_nanoseconds_by_op[
                operation] =
            dag_operator_exclusive_miss_compute_nanoseconds_by_op[
                operation]
                .load(std::memory_order_relaxed);
        result.dag_operator_result_units_sum_by_op[operation] =
            dag_operator_result_units_sum_by_op[operation].load(
                std::memory_order_relaxed);
        result.dag_operator_result_observations_by_op[operation] =
            dag_operator_result_observations_by_op[operation].load(
                std::memory_order_relaxed);
        result.dag_operator_cache_admissions_by_op[operation] =
            dag_operator_cache_admissions_by_op[operation].load(
                std::memory_order_relaxed);
        result.dag_operator_cache_admitted_bytes_by_op[operation] =
            dag_operator_cache_admitted_bytes_by_op[operation].load(
                std::memory_order_relaxed);
    }
    result.longest_run_returned_length_sum =
        longest_run_returned_length_sum.load(
            std::memory_order_relaxed);
    result.longest_run_returned_length_observations =
        longest_run_returned_length_observations.load(
            std::memory_order_relaxed);
    return result;
}

void writeSearchStatistics(
    std::ostream& output,
    const SearchStatistics& s) {
    output << "root_tasks: " << s.root_tasks << '\n'
           << "dfs_states: " << s.dfs_states << '\n'
           << "metric_schema_version: logical_extensions_v1\n"
           << "dag_operator_statistics_schema_version: "
           << kDagOperatorStatisticsSchemaVersion << '\n'
           << "search_statistics_hooks_erased_at_build: "
           << (kSearchStatisticsHooksErasedAtBuild ? "true" : "false")
           << '\n'
           << "search_statistics_build_profile: "
           << kSearchStatisticsBuildProfile << '\n'
           << "gopher_source_identity: "
           << kGopherSourceIdentity << '\n'
           << "gopher_common_flags_identity: "
           << kGopherCommonFlagsIdentity << '\n'
           << "gopher_compiler_identity: "
           << kGopherCompilerIdentity << '\n'
           << "dag_operator_reconciliation_passed: "
           << (s.dag_operator_reconciliation_passed ? "true" : "false")
           << '\n'
           << "raw_extension_candidates: "
           << s.raw_extension_candidates << '\n'
           << "post_structural_candidates: "
           << s.generated_candidates << '\n'
           << "generated_candidates: " << s.generated_candidates << '\n'
           << "accepted_candidates: " << s.accepted_candidates << '\n'
           << "accepted_extensions: " << s.accepted_extensions << '\n'
           << "completed_matches: " << s.completed_matches << '\n'
           << "pruned_injectivity: " << s.pruned_injectivity << '\n'
           << "pruned_structural: " << s.pruned_structural << '\n'
           << "pruned_temporal_support: " << s.pruned_temporal_support << '\n'
           << "pruned_active_support: " << s.pruned_active_support << '\n'
           << "logical_td_block_lookups: " << s.logical_td_block_lookups << '\n'
           << "physical_td_block_lookups: " << s.physical_td_block_lookups << '\n'
           << "logical_neighbor_lookups: " << s.logical_neighbor_lookups << '\n'
           << "physical_neighbor_lookups: " << s.physical_neighbor_lookups << '\n'
           << "logical_structural_intersections: "
           << s.logical_structural_intersections << '\n'
           << "physical_structural_intersections: "
           << s.physical_structural_intersections << '\n'
           << "logical_temporal_edge_lookups: "
           << s.logical_temporal_edge_lookups << '\n'
           << "physical_temporal_edge_lookups: "
           << s.physical_temporal_edge_lookups << '\n'
           << "logical_temporal_intersections: "
           << s.logical_temporal_intersections << '\n'
           << "physical_temporal_intersections: "
           << s.physical_temporal_intersections << '\n'
           << "logical_longest_run: " << s.logical_longest_run << '\n'
           << "physical_longest_run: " << s.physical_longest_run << '\n'
           << "structural_input_elements: " << s.structural_input_elements << '\n'
           << "temporal_input_intervals: " << s.temporal_input_intervals << '\n'
           << "structural_cache_hits: " << s.structural_cache_hits << '\n'
           << "structural_cache_misses: " << s.structural_cache_misses << '\n'
           << "structural_cache_admitted: " << s.structural_cache_admitted << '\n'
           << "structural_cache_bypassed: " << s.structural_cache_bypassed << '\n'
           << "structural_cache_bytes: " << s.structural_cache_bytes << '\n'
           << "structural_cache_entries: " << s.structural_cache_entries << '\n'
           << "structural_cache_duplicate_inserts: "
           << s.structural_cache_duplicate_inserts << '\n'
           << "structural_cache_rejected_budget: "
           << s.structural_cache_rejected_budget << '\n'
           << "structural_cache_rejected_entry_size: "
           << s.structural_cache_rejected_entry_size << '\n'
           << "structural_cache_rejected_cost: "
           << s.structural_cache_rejected_cost << '\n'
           << "structural_cache_hit_result_elements: "
           << s.structural_cache_hit_result_elements << '\n'
           << "structural_cache_hit_saved_work: "
           << s.structural_cache_hit_saved_work << '\n'
           << "structural_cache_max_hit_result_elements: "
           << s.structural_cache_max_hit_result_elements << '\n'
           << "structural_cache_admitted_result_elements: "
           << s.structural_cache_admitted_result_elements << '\n'
           << "structural_cache_admitted_estimated_work: "
           << s.structural_cache_admitted_estimated_work << '\n'
           << "temporal_cache_hits: " << s.temporal_cache_hits << '\n'
           << "temporal_cache_misses: " << s.temporal_cache_misses << '\n'
           << "temporal_cache_admitted: " << s.temporal_cache_admitted << '\n'
           << "temporal_cache_bypassed: " << s.temporal_cache_bypassed << '\n'
           << "temporal_cache_bytes: " << s.temporal_cache_bytes << '\n'
           << "temporal_cache_entries: " << s.temporal_cache_entries << '\n'
           << "temporal_cache_duplicate_inserts: "
           << s.temporal_cache_duplicate_inserts << '\n'
           << "temporal_cache_rejected_budget: "
           << s.temporal_cache_rejected_budget << '\n'
           << "temporal_cache_rejected_entry_size: "
           << s.temporal_cache_rejected_entry_size << '\n'
           << "temporal_cache_rejected_cost: "
           << s.temporal_cache_rejected_cost << '\n'
           << "temporal_cache_hit_result_elements: "
           << s.temporal_cache_hit_result_elements << '\n'
           << "temporal_cache_hit_saved_work: "
           << s.temporal_cache_hit_saved_work << '\n'
           << "temporal_cache_max_hit_result_elements: "
           << s.temporal_cache_max_hit_result_elements << '\n'
           << "temporal_cache_admitted_result_elements: "
           << s.temporal_cache_admitted_result_elements << '\n'
           << "temporal_cache_admitted_estimated_work: "
           << s.temporal_cache_admitted_estimated_work << '\n'
           << "longest_cache_hits: " << s.longest_cache_hits << '\n'
           << "longest_cache_misses: " << s.longest_cache_misses << '\n'
           << "td_block_cache_hits: " << s.td_block_cache_hits << '\n'
           << "neighbor_lookup_cache_hits: "
           << s.neighbor_lookup_cache_hits << '\n'
           << "structural_intersection_cache_hits: "
           << s.structural_intersection_cache_hits << '\n'
           << "edge_time_cache_hits: " << s.edge_time_cache_hits << '\n'
           << "temporal_intersection_cache_hits: "
           << s.temporal_intersection_cache_hits << '\n'
           << "longest_run_cache_hits: "
           << s.longest_run_cache_hits << '\n'
           << "structural_parent_subtrees_skipped: "
           << s.structural_parent_subtrees_skipped << '\n'
           << "structural_v2_intersection_nodes: "
           << s.structural_v2_intersection_nodes << '\n'
           << "structural_v2_eligible_nodes: "
           << s.structural_v2_eligible_nodes << '\n'
           << "structural_v2_no_projection_reuse_nodes: "
           << s.structural_v2_no_projection_reuse_nodes << '\n'
           << "structural_v2_eligible_probes: "
           << s.structural_v2_eligible_probes << '\n'
           << "structural_v2_projection_bypasses: "
           << s.structural_v2_projection_bypasses << '\n'
           << "dag_nodes_attempted: " << s.dag_nodes_attempted << '\n'
           << "dag_nodes_materialized: " << s.dag_nodes_materialized << '\n'
           << "dag_nodes_merged: " << s.dag_nodes_merged << '\n'
           << "exact_runtime_key_diagnostics: "
           << (s.exact_runtime_key_diagnostics ? "on" : "off") << '\n'
           << "exact_runtime_key_cap: "
           << s.exact_runtime_key_cap << '\n'
           << "exact_runtime_key_invocations: "
           << s.exact_runtime_key_invocations << '\n'
           << "exact_runtime_key_distinct: "
           << s.exact_runtime_key_distinct << '\n'
           << "exact_runtime_key_reuse_opportunities: "
           << (s.exact_runtime_key_invocations -
               s.exact_runtime_key_distinct) << '\n'
           << "runtime_key_construction_ns: "
           << s.runtime_key_construction_nanoseconds << '\n'
           << "cache_lookup_ns: " << s.cache_lookup_nanoseconds << '\n'
           << "structural_compute_ns: " << s.structural_compute_nanoseconds << '\n'
           << "temporal_compute_ns: " << s.temporal_compute_nanoseconds << '\n'
           << "longest_compute_ns: " << s.longest_compute_nanoseconds << '\n'
           << "search_ns: " << s.search_nanoseconds << '\n'
           << "output_buffer_bytes: " << s.output_buffer_bytes << '\n';
    for (std::size_t operation = 0;
         operation < kDagOpCount;
         ++operation) {
        const DagOp op = static_cast<DagOp>(operation);
        output << "exact_runtime_key_invocations_"
               << dagOpMetricName(op) << ": "
               << s.exact_runtime_key_invocations_by_op[operation]
               << '\n'
               << "exact_runtime_key_distinct_"
               << dagOpMetricName(op) << ": "
               << s.exact_runtime_key_distinct_by_op[operation]
               << '\n'
               << "runtime_key_construction_ns_"
               << dagOpMetricName(op) << ": "
               << s.runtime_key_construction_nanoseconds_by_op[operation]
               << '\n'
               << "cache_lookup_ns_"
               << dagOpMetricName(op) << ": "
               << s.cache_lookup_nanoseconds_by_op[operation]
               << '\n'
               << "dag_operator_invocations_"
               << dagOpMetricName(op) << ": "
               << s.dag_operator_invocations_by_op[operation]
               << '\n'
               << "dag_operator_cache_hits_"
               << dagOpMetricName(op) << ": "
               << s.dag_operator_cache_hits_by_op[operation]
               << '\n'
               << "dag_operator_cache_misses_"
               << dagOpMetricName(op) << ": "
               << s.dag_operator_cache_misses_by_op[operation]
               << '\n'
               << "dag_operator_exclusive_compute_ns_"
               << dagOpMetricName(op) << ": "
               << s
                      .dag_operator_exclusive_compute_nanoseconds_by_op[
                          operation]
               << '\n'
               << "dag_operator_exclusive_miss_compute_ns_"
               << dagOpMetricName(op) << ": "
               << s
                      .dag_operator_exclusive_miss_compute_nanoseconds_by_op[
                          operation]
               << '\n'
               << "dag_operator_result_units_sum_"
               << dagOpMetricName(op) << ": "
               << s.dag_operator_result_units_sum_by_op[operation]
               << '\n'
               << "dag_operator_result_observations_"
               << dagOpMetricName(op) << ": "
               << s
                      .dag_operator_result_observations_by_op[operation]
               << '\n'
               << "dag_operator_result_units_mean_"
               << dagOpMetricName(op) << ": ";
        const std::uint64_t observations =
            s.dag_operator_result_observations_by_op[operation];
        if (observations == 0) {
            output << "N/A";
        } else {
            output << static_cast<long double>(
                          s.dag_operator_result_units_sum_by_op[operation]) /
                          static_cast<long double>(observations);
        }
        output << '\n'
               << "dag_operator_cache_admissions_"
               << dagOpMetricName(op) << ": "
               << s.dag_operator_cache_admissions_by_op[operation]
               << '\n'
               << "dag_operator_cache_admitted_bytes_"
               << dagOpMetricName(op) << ": "
               << s
                      .dag_operator_cache_admitted_bytes_by_op[operation]
               << '\n';
    }
    output << "longest_run_returned_length_sum: "
           << s.longest_run_returned_length_sum << '\n'
           << "longest_run_returned_length_observations: "
           << s.longest_run_returned_length_observations << '\n'
           << "longest_run_returned_length_mean: ";
    if (s.longest_run_returned_length_observations == 0) {
        output << "N/A\n";
    } else {
        output << static_cast<long double>(
                      s.longest_run_returned_length_sum) /
                      static_cast<long double>(
                          s.longest_run_returned_length_observations)
               << '\n';
    }
}
