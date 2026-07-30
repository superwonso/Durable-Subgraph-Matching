#include "SearchStatistics.h"

#include <ostream>

SearchStatistics SearchStatisticsCollector::snapshot() const {
    SearchStatistics result;
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
    COPY_FIELD(cache_lookup_nanoseconds);
    COPY_FIELD(structural_compute_nanoseconds);
    COPY_FIELD(temporal_compute_nanoseconds);
    COPY_FIELD(longest_compute_nanoseconds);
    COPY_FIELD(search_nanoseconds);
#undef COPY_FIELD
    return result;
}

void writeSearchStatistics(
    std::ostream& output,
    const SearchStatistics& s) {
    output << "root_tasks: " << s.root_tasks << '\n'
           << "dfs_states: " << s.dfs_states << '\n'
           << "metric_schema_version: logical_extensions_v1\n"
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
           << "dag_nodes_attempted: " << s.dag_nodes_attempted << '\n'
           << "dag_nodes_materialized: " << s.dag_nodes_materialized << '\n'
           << "dag_nodes_merged: " << s.dag_nodes_merged << '\n'
           << "cache_lookup_ns: " << s.cache_lookup_nanoseconds << '\n'
           << "structural_compute_ns: " << s.structural_compute_nanoseconds << '\n'
           << "temporal_compute_ns: " << s.temporal_compute_nanoseconds << '\n'
           << "longest_compute_ns: " << s.longest_compute_nanoseconds << '\n'
           << "search_ns: " << s.search_nanoseconds << '\n'
           << "output_buffer_bytes: " << s.output_buffer_bytes << '\n';
}
