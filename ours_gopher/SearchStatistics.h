#ifndef SEARCH_STATISTICS_H
#define SEARCH_STATISTICS_H

#include <atomic>
#include <cstdint>
#include <iosfwd>

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

    std::uint64_t dag_nodes_attempted = 0;
    std::uint64_t dag_nodes_materialized = 0;
    std::uint64_t dag_nodes_merged = 0;

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

    bool enabled() const { return enabled_; }
    void add(
        std::atomic<std::uint64_t>& counter,
        std::uint64_t value = 1) {
        if (enabled_) {
            counter.fetch_add(value, std::memory_order_relaxed);
        }
    }
    void set(
        std::atomic<std::uint64_t>& counter,
        std::uint64_t value) {
        if (enabled_) {
            counter.store(value, std::memory_order_relaxed);
        }
    }

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
    GOPHER_STAT_FIELD(cache_lookup_nanoseconds);
    GOPHER_STAT_FIELD(structural_compute_nanoseconds);
    GOPHER_STAT_FIELD(temporal_compute_nanoseconds);
    GOPHER_STAT_FIELD(longest_compute_nanoseconds);
    GOPHER_STAT_FIELD(search_nanoseconds);
#undef GOPHER_STAT_FIELD

    SearchStatistics snapshot() const;

private:
    bool enabled_ = true;
};

void writeSearchStatistics(std::ostream& output, const SearchStatistics& statistics);

#endif
