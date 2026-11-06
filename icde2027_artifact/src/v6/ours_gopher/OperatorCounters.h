#ifndef DSM_OPERATOR_COUNTERS_H
#define DSM_OPERATOR_COUNTERS_H

#include <atomic>
#include <cstdint>
#include <ostream>
#include "StructuralProfile.h"
#include "PhysicalExecutionProfile.h"

// These counters are orthogonal to the original statistics/timers. They are
// disabled in primary E6 timing, enabled uniformly for separate operation
// census runs of EVERY arm. Null is unmeasured, never a measured zero.
#define DSM_OPS_FIELDS(X) \
    X(td_block_lookups_executed) X(td_block_lookups_skipped) \
    X(neighbor_lookups_executed) X(neighbor_lookups_skipped) \
    X(structural_intersections_executed) X(structural_intersections_skipped) \
    X(history_lookups_executed) X(history_lookups_skipped) \
    X(history_guard_checks) X(history_guard_failures) \
    X(time_intersections_executed) X(time_intersections_skipped) \
    X(kappa_executed) X(kappa_skipped) \
    X(time_elements_read) X(time_input_elements) X(intersection_output_elements) \
    X(candidates) X(logical_extensions) X(galloping_intersections) \
    X(physical_neighbor_copies) X(physical_neighbor_copy_elements) X(physical_neighbor_copy_bytes) \
    X(physical_seed_copy_elements) X(physical_seed_copy_bytes) \
    X(physical_projected_views) X(physical_private_descriptors) \
    X(physical_probe_selections) X(physical_merge_selections) \
    X(physical_structural_probes) X(physical_handle_reprobes) \
    X(physical_lmax_checks) X(physical_lmax_rejections) \
    X(physical_history_handle_reads) X(physical_history_payload_reads) \
    X(physical_input_candidates) X(physical_output_candidates) \
    X(physical_raw_candidates) X(physical_structure_rejections) \
    X(physical_injectivity_rejections) X(physical_temporal_rejections) X(physical_extensions) \
    X(physical_output_elements_materialized) X(physical_handle_writes) \
    X(physical_handle_copy_bytes) \
    X(window_batch_calls) X(window_candidate_checks) \
    X(window_edge_reads) X(window_word_ands) X(window_empty_rejections)

struct OperatorCounters {
    StructuralProfile physical_profile;
    PhysicalExecutionProfile physical_execution;
#define DSM_DECLARE_OP(name) std::atomic<std::uint64_t> ops_##name{0};
    DSM_OPS_FIELDS(DSM_DECLARE_OP)
#undef DSM_DECLARE_OP
    void writeJsonMembers(std::ostream& out, bool enabled) const {
        out << "  \"ops_enabled\": " << (enabled ? "true" : "false") << ",\n";
#define DSM_WRITE_OP(name) \
        out << "  \"ops_" #name "\": "; \
        if (enabled) out << ops_##name.load(std::memory_order_relaxed); \
        else out << "null"; \
        out << ",\n";
        DSM_OPS_FIELDS(DSM_WRITE_OP)
#undef DSM_WRITE_OP
        out << "  \"ops_physical_profile\": ";
        if (enabled) physical_profile.writeJson(out);
        else out << "null";
        out << ",\n";
        out << "  \"ops_physical_execution\": ";
        if (enabled) physical_execution.writeJson(out);
        else out << "null";
        out << ",\n";
    }
};

#define OPS_ADD(pointer, name, amount) do { \
    if ((pointer) != nullptr) { \
        (pointer)->ops_##name.fetch_add(static_cast<std::uint64_t>(amount), \
                                       std::memory_order_relaxed); \
    } \
} while (false)

#endif
