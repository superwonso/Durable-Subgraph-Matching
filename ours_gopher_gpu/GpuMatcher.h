#ifndef OURS_GOPHER_GPU_MATCHER_H
#define OURS_GOPHER_GPU_MATCHER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "CandidateDag.h"
#include "TDTree.h"
#include "Utils.h"

enum class GpuExecutionVariant {
    LevelSynchronous,
    ResidentContinuation
};

const char* gpuExecutionVariantName(GpuExecutionVariant variant);
bool parseGpuExecutionVariant(
    const std::string& text,
    GpuExecutionVariant& variant);

struct GpuMatcherOptions {
    int device = 0;
    std::size_t frontier_budget_bytes = 1024ULL * 1024ULL * 1024ULL;
    bool early_prune = true;
    bool result_digest = false;
    bool collect_statistics = true;
    GpuExecutionVariant variant =
        GpuExecutionVariant::LevelSynchronous;
};

struct GpuDepthMetrics {
    std::uint64_t depth = 0;
    std::uint64_t input_states = 0;
    std::uint64_t output_states = 0;
    std::uint64_t generated_candidates = 0;
    std::uint64_t maximum_candidates_per_state = 0;
    std::uint64_t kernel_nanoseconds = 0;
    std::uint64_t host_round_trip_nanoseconds = 0;
    std::uint64_t grid_blocks = 0;
};

struct GpuMatcherMetrics {
    std::string device_name;
    std::string architecture;
    int compute_units = 0;
    int warp_size = 0;
    int block_size = 0;
    int hip_runtime_version = 0;
    int hip_driver_version = 0;
    std::size_t device_global_memory_bytes = 0;
    std::size_t persistent_device_bytes = 0;
    std::size_t peak_frontier_device_bytes = 0;
    std::size_t peak_frontier_states = 0;
    std::uint64_t flatten_nanoseconds = 0;
    std::uint64_t h2d_nanoseconds = 0;
    std::uint64_t d2h_nanoseconds = 0;
    std::uint64_t device_allocation_nanoseconds = 0;
    std::uint64_t kernel_nanoseconds = 0;
    std::uint64_t gpu_ready_to_result_nanoseconds = 0;
    std::uint64_t gpu_search_nanoseconds = 0;
    std::uint64_t kernel_launches = 0;
    std::uint64_t frontier_levels = 0;
    std::uint64_t generated_candidates = 0;
    std::uint64_t accepted_candidates = 0;
    std::uint64_t pruned_injectivity = 0;
    std::uint64_t pruned_structural = 0;
    std::uint64_t pruned_temporal = 0;
    std::uint64_t temporal_interval_steps = 0;
    std::uint64_t directed_edge_lookups = 0;
    std::uint64_t h2d_bytes = 0;
    std::uint64_t d2h_bytes = 0;
    std::uint64_t dag_nodes_lowered = 0;
    std::uint64_t depth_host_round_trips = 0;
    std::uint64_t depth_host_round_trip_nanoseconds = 0;
    std::uint64_t device_synchronizations = 0;
    std::uint64_t depth_count_d2h_bytes = 0;
    std::uint64_t frontier_validation_passes = 0;
    std::uint64_t frontier_materialization_passes = 0;
    std::uint64_t launch_and_sync_overhead_nanoseconds = 0;
    std::uint64_t resident_capacity_states = 0;
    std::uint64_t resident_grid_blocks = 0;
    int occupancy_blocks_per_multiprocessor = 0;
    std::vector<GpuDepthMetrics> depths;
    bool statistics_collected = true;
    bool fallback_used = false;
};

struct GpuMatchSummary {
    std::uint64_t match_count = 0;
    bool count_overflow = false;
    std::string result_digest = "disabled";
    GpuMatcherMetrics metrics;
};

class GpuMatcher {
public:
    GpuMatcher(
        const Graph& temporal_graph,
        const Graph& query_graph,
        const QueryPlan& plan,
        const CandidateDag& dag,
        const TDTree& td_tree,
        int minimum_duration,
        GpuMatcherOptions options);

    GpuMatchSummary run();

private:
    const Graph& temporal_graph_;
    const Graph& query_graph_;
    const QueryPlan& plan_;
    const CandidateDag& dag_;
    const TDTree& td_tree_;
    int minimum_duration_;
    GpuMatcherOptions options_;
};

#endif
