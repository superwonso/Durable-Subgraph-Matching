#include "GpuMatcher.h"

#include <hip/hip_runtime.h>
#include <hipcub/block/block_reduce.hpp>
#include <hipcub/block/block_scan.hpp>
#include <hipcub/device/device_scan.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using DeviceOffset = unsigned long long;

constexpr int kBlockSize = 256;
constexpr int kResidentBlockSize = 128;
constexpr int kMaximumQueryArcs = 32;
constexpr unsigned int kMaximumGridBlocks = 65535;

void checkHip(hipError_t status, const char* operation) {
    if (status == hipSuccess) return;
    throw std::runtime_error(
        std::string(operation) + ": " + hipGetErrorString(status));
}

std::uint64_t elapsedNanoseconds(Clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start).count());
}

std::size_t checkedBytes(std::size_t count, std::size_t element_size) {
    if (element_size != 0 &&
        count > std::numeric_limits<std::size_t>::max() / element_size) {
        throw std::overflow_error("GPU allocation size overflow");
    }
    return count * element_size;
}

template <typename T>
class DeviceBuffer {
public:
    DeviceBuffer() = default;

    explicit DeviceBuffer(std::size_t count) {
        allocate(count);
    }

    ~DeviceBuffer() {
        if (data_ != nullptr) {
            (void)hipFree(data_);
        }
    }

    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;

    DeviceBuffer(DeviceBuffer&& other) noexcept
        : data_(other.data_), count_(other.count_) {
        other.data_ = nullptr;
        other.count_ = 0;
    }

    DeviceBuffer& operator=(DeviceBuffer&& other) noexcept {
        if (this == &other) return *this;
        if (data_ != nullptr) {
            (void)hipFree(data_);
        }
        data_ = other.data_;
        count_ = other.count_;
        other.data_ = nullptr;
        other.count_ = 0;
        return *this;
    }

    void allocate(std::size_t count) {
        if (data_ != nullptr) {
            checkHip(hipFree(data_), "hipFree");
            data_ = nullptr;
            count_ = 0;
        }
        if (count == 0) return;
        const std::size_t bytes = checkedBytes(count, sizeof(T));
        checkHip(
            hipMalloc(reinterpret_cast<void**>(&data_), bytes),
            "hipMalloc");
        count_ = count;
    }

    T* get() { return data_; }
    const T* get() const { return data_; }
    std::size_t size() const { return count_; }
    std::size_t bytes() const {
        return checkedBytes(count_, sizeof(T));
    }

private:
    T* data_ = nullptr;
    std::size_t count_ = 0;
};

struct GpuInterval {
    int start;
    int end;
};

struct DeviceGraphView {
    const DeviceOffset* adjacency_offsets;
    const int* adjacency_targets;
    const int* adjacency_edge_ids;
    const DeviceOffset* interval_offsets;
    const GpuInterval* intervals;
};

struct DeviceQueryView {
    const int* arc_sources;
    const int* arc_targets;
    int arc_count;
    int query_vertex_count;
};

struct DeviceRelationView {
    const DeviceOffset* offsets;
    const int* values;
    DeviceOffset row_stride;
};

struct DeviceCounters {
    DeviceOffset generated_candidates;
    DeviceOffset accepted_candidates;
    DeviceOffset pruned_injectivity;
    DeviceOffset pruned_structural;
    DeviceOffset pruned_temporal;
    DeviceOffset temporal_interval_steps;
    DeviceOffset directed_edge_lookups;
};

struct DeviceDepthCounters {
    DeviceOffset generated_candidates;
    DeviceOffset maximum_candidates_per_state;
};

enum class ValidationResult : unsigned char {
    Valid,
    Injectivity,
    Structural,
    Temporal
};

__device__ int resolveMappedVertex(
    const int* mapping,
    int query_vertex,
    int overlay_query_vertex,
    int overlay_data_vertex) {
    return query_vertex == overlay_query_vertex
        ? overlay_data_vertex
        : mapping[query_vertex];
}

__device__ int findDirectedEdge(
    const DeviceGraphView& graph,
    int source,
    int target) {
    DeviceOffset low = graph.adjacency_offsets[source];
    DeviceOffset high = graph.adjacency_offsets[source + 1];
    while (low < high) {
        const DeviceOffset middle = low + (high - low) / 2;
        const int value = graph.adjacency_targets[middle];
        if (value < target) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    const DeviceOffset end = graph.adjacency_offsets[source + 1];
    if (low >= end || graph.adjacency_targets[low] != target) return -1;
    return graph.adjacency_edge_ids[low];
}

__device__ ValidationResult validateExtension(
    const DeviceGraphView& graph,
    const DeviceQueryView& query,
    const int* mapping,
    int overlay_query_vertex,
    int overlay_data_vertex,
    int minimum_duration,
    bool cumulative_prune,
    bool final_depth,
    DeviceOffset& interval_steps,
    DeviceOffset& edge_lookups) {
    for (int query_vertex = 0;
         query_vertex < query.query_vertex_count;
         ++query_vertex) {
        const int mapped = mapping[query_vertex];
        if (mapped >= 0 && mapped == overlay_data_vertex) {
            return ValidationResult::Injectivity;
        }
    }

    DeviceOffset cursors[kMaximumQueryArcs];
    DeviceOffset limits[kMaximumQueryArcs];
    int active_arc_count = 0;

    for (int arc = 0; arc < query.arc_count; ++arc) {
        const int query_source = query.arc_sources[arc];
        const int query_target = query.arc_targets[arc];
        const int data_source = resolveMappedVertex(
            mapping,
            query_source,
            overlay_query_vertex,
            overlay_data_vertex);
        const int data_target = resolveMappedVertex(
            mapping,
            query_target,
            overlay_query_vertex,
            overlay_data_vertex);
        if (data_source < 0 || data_target < 0) continue;

        ++edge_lookups;
        const int edge_id =
            findDirectedEdge(graph, data_source, data_target);
        if (edge_id < 0) return ValidationResult::Structural;

        const bool include_temporal =
            cumulative_prune ||
            final_depth ||
            query_source == overlay_query_vertex ||
            query_target == overlay_query_vertex;
        if (!include_temporal) continue;

        const DeviceOffset begin = graph.interval_offsets[edge_id];
        const DeviceOffset end = graph.interval_offsets[edge_id + 1];
        if (begin == end) return ValidationResult::Temporal;
        cursors[active_arc_count] = begin;
        limits[active_arc_count] = end;
        ++active_arc_count;
    }

    if (active_arc_count == 0) return ValidationResult::Valid;

    while (true) {
        int maximum_start = std::numeric_limits<int>::min();
        int minimum_end = std::numeric_limits<int>::max();
        for (int arc = 0; arc < active_arc_count; ++arc) {
            const GpuInterval interval =
                graph.intervals[cursors[arc]];
            maximum_start =
                maximum_start > interval.start
                ? maximum_start
                : interval.start;
            minimum_end =
                minimum_end < interval.end
                ? minimum_end
                : interval.end;
        }
        ++interval_steps;

        const long long run =
            static_cast<long long>(minimum_end) -
            static_cast<long long>(maximum_start) + 1LL;
        if (run >= static_cast<long long>(minimum_duration)) {
            return ValidationResult::Valid;
        }

        for (int arc = 0; arc < active_arc_count; ++arc) {
            if (graph.intervals[cursors[arc]].end != minimum_end) {
                continue;
            }
            ++cursors[arc];
            if (cursors[arc] >= limits[arc]) {
                return ValidationResult::Temporal;
            }
        }
    }
}

template <int BlockSize>
__global__ void countExtensionsKernel(
    DeviceGraphView graph,
    DeviceQueryView query,
    DeviceRelationView relations,
    const int* current_mappings,
    DeviceOffset state_count,
    int next_query_vertex,
    int parent_query_vertex,
    int minimum_duration,
    bool cumulative_prune,
    bool final_depth,
    bool collect_statistics,
    DeviceOffset* valid_counts,
    DeviceCounters* counters,
    DeviceDepthCounters* depth_counters) {
    using BlockReduce = hipcub::BlockReduce<DeviceOffset, BlockSize>;
    __shared__ typename BlockReduce::TempStorage reduce_storage;

    for (DeviceOffset state_index = blockIdx.x;
         state_index < state_count;
         state_index += gridDim.x) {
        const int* mapping =
            current_mappings +
            state_index *
                static_cast<DeviceOffset>(
                    query.query_vertex_count);
        const int parent_data_vertex =
            mapping[parent_query_vertex];
        const DeviceOffset row =
            static_cast<DeviceOffset>(next_query_vertex) *
            relations.row_stride;
        const DeviceOffset begin =
            relations.offsets[
                row +
                static_cast<DeviceOffset>(
                    parent_data_vertex)];
        const DeviceOffset end =
            relations.offsets[
                row +
                static_cast<DeviceOffset>(
                    parent_data_vertex) +
                1ULL];

        DeviceOffset local_valid = 0;
        DeviceOffset local_injectivity = 0;
        DeviceOffset local_structural = 0;
        DeviceOffset local_temporal = 0;
        DeviceOffset local_interval_steps = 0;
        DeviceOffset local_edge_lookups = 0;

        for (DeviceOffset relation_index =
                 begin + threadIdx.x;
             relation_index < end;
             relation_index += BlockSize) {
            const int candidate =
                relations.values[relation_index];
            DeviceOffset candidate_interval_steps = 0;
            DeviceOffset candidate_edge_lookups = 0;
            const ValidationResult result = validateExtension(
                graph,
                query,
                mapping,
                next_query_vertex,
                candidate,
                minimum_duration,
                cumulative_prune,
                final_depth,
                candidate_interval_steps,
                candidate_edge_lookups);
            local_interval_steps += candidate_interval_steps;
            local_edge_lookups += candidate_edge_lookups;
            if (result == ValidationResult::Valid) {
                ++local_valid;
            } else if (result == ValidationResult::Injectivity) {
                ++local_injectivity;
            } else if (result == ValidationResult::Structural) {
                ++local_structural;
            } else {
                ++local_temporal;
            }
        }

        const DeviceOffset block_valid =
            BlockReduce(reduce_storage).Sum(local_valid);
        __syncthreads();
        if (threadIdx.x == 0) {
            valid_counts[state_index] = block_valid;
        }

        if (collect_statistics) {
            const DeviceOffset block_injectivity =
                BlockReduce(reduce_storage).Sum(local_injectivity);
            __syncthreads();
            const DeviceOffset block_structural =
                BlockReduce(reduce_storage).Sum(local_structural);
            __syncthreads();
            const DeviceOffset block_temporal =
                BlockReduce(reduce_storage).Sum(local_temporal);
            __syncthreads();
            const DeviceOffset block_interval_steps =
                BlockReduce(reduce_storage).Sum(local_interval_steps);
            __syncthreads();
            const DeviceOffset block_edge_lookups =
                BlockReduce(reduce_storage).Sum(local_edge_lookups);
            __syncthreads();
            if (threadIdx.x == 0) {
                const DeviceOffset generated =
                    end - begin;
                atomicAdd(
                    &counters->generated_candidates,
                    generated);
                atomicAdd(
                    &counters->accepted_candidates,
                    block_valid);
                atomicAdd(
                    &counters->pruned_injectivity,
                    block_injectivity);
                atomicAdd(
                    &counters->pruned_structural,
                    block_structural);
                atomicAdd(
                    &counters->pruned_temporal,
                    block_temporal);
                atomicAdd(
                    &counters->temporal_interval_steps,
                    block_interval_steps);
                atomicAdd(
                    &counters->directed_edge_lookups,
                    block_edge_lookups);
                atomicAdd(
                    &depth_counters
                         ->generated_candidates,
                    generated);
                atomicMax(
                    &depth_counters
                         ->maximum_candidates_per_state,
                    generated);
            }
        }
        __syncthreads();
    }
}

template <int BlockSize>
__global__ void residentContinuationKernel(
    DeviceGraphView graph,
    DeviceQueryView query,
    DeviceRelationView relations,
    const int* current_mappings,
    const DeviceOffset* current_state_count,
    int* next_mappings,
    DeviceOffset frontier_capacity,
    DeviceOffset* next_state_count,
    DeviceOffset* work_index,
    int next_query_vertex,
    int parent_query_vertex,
    int minimum_duration,
    bool cumulative_prune,
    bool final_depth,
    bool collect_statistics,
    DeviceCounters* counters,
    DeviceDepthCounters* depth_counters,
    int* overflow_flag) {
    using BlockScan = hipcub::BlockScan<int, BlockSize>;
    using BlockReduce = hipcub::BlockReduce<DeviceOffset, BlockSize>;
    __shared__ typename BlockScan::TempStorage scan_storage;
    __shared__ typename BlockReduce::TempStorage reduce_storage;
    __shared__ DeviceOffset state_index;
    __shared__ DeviceOffset output_base;

    while (true) {
        if (threadIdx.x == 0) {
            state_index =
                atomicAdd(work_index, 1ULL);
        }
        __syncthreads();

        const DeviceOffset states = *current_state_count;
        if (state_index >= states ||
            state_index >= frontier_capacity ||
            *overflow_flag != 0) {
            break;
        }

        const int* mapping =
            current_mappings +
            state_index *
                static_cast<DeviceOffset>(
                    query.query_vertex_count);
        const int parent_data_vertex =
            mapping[parent_query_vertex];
        const DeviceOffset row =
            static_cast<DeviceOffset>(next_query_vertex) *
            relations.row_stride;
        const DeviceOffset begin =
            relations.offsets[
                row +
                static_cast<DeviceOffset>(
                    parent_data_vertex)];
        const DeviceOffset end =
            relations.offsets[
                row +
                static_cast<DeviceOffset>(
                    parent_data_vertex) +
                1ULL];

        DeviceOffset local_valid = 0;
        DeviceOffset local_injectivity = 0;
        DeviceOffset local_structural = 0;
        DeviceOffset local_temporal = 0;
        DeviceOffset local_interval_steps = 0;
        DeviceOffset local_edge_lookups = 0;

        for (DeviceOffset tile = begin;
             tile < end;
             tile += BlockSize) {
            const DeviceOffset relation_index =
                tile +
                static_cast<DeviceOffset>(threadIdx.x);
            int valid = 0;
            int candidate = -1;
            if (relation_index < end) {
                candidate =
                    relations.values[relation_index];
                DeviceOffset candidate_interval_steps = 0;
                DeviceOffset candidate_edge_lookups = 0;
                const ValidationResult result =
                    validateExtension(
                        graph,
                        query,
                        mapping,
                        next_query_vertex,
                        candidate,
                        minimum_duration,
                        cumulative_prune,
                        final_depth,
                        candidate_interval_steps,
                        candidate_edge_lookups);
                local_interval_steps +=
                    candidate_interval_steps;
                local_edge_lookups +=
                    candidate_edge_lookups;
                if (result == ValidationResult::Valid) {
                    valid = 1;
                    ++local_valid;
                } else if (
                    result ==
                    ValidationResult::Injectivity) {
                    ++local_injectivity;
                } else if (
                    result ==
                    ValidationResult::Structural) {
                    ++local_structural;
                } else {
                    ++local_temporal;
                }
            }

            int local_output = 0;
            int tile_valid = 0;
            BlockScan(scan_storage).ExclusiveSum(
                valid,
                local_output,
                tile_valid);
            __syncthreads();
            if (threadIdx.x == 0) {
                output_base =
                    atomicAdd(
                        next_state_count,
                        static_cast<DeviceOffset>(
                            tile_valid));
            }
            __syncthreads();

            if (valid != 0) {
                const DeviceOffset output_index =
                    output_base +
                    static_cast<DeviceOffset>(
                        local_output);
                if (output_index >= frontier_capacity) {
                    atomicExch(overflow_flag, 1);
                } else {
                    int* output_mapping =
                        next_mappings +
                        output_index *
                            static_cast<DeviceOffset>(
                                query.query_vertex_count);
                    for (int query_vertex = 0;
                         query_vertex <
                             query.query_vertex_count;
                         ++query_vertex) {
                        output_mapping[query_vertex] =
                            mapping[query_vertex];
                    }
                    output_mapping[next_query_vertex] =
                        candidate;
                }
            }
            __syncthreads();
        }

        if (collect_statistics) {
            const DeviceOffset block_valid =
                BlockReduce(reduce_storage).Sum(
                    local_valid);
            __syncthreads();
            const DeviceOffset block_injectivity =
                BlockReduce(reduce_storage).Sum(
                    local_injectivity);
            __syncthreads();
            const DeviceOffset block_structural =
                BlockReduce(reduce_storage).Sum(
                    local_structural);
            __syncthreads();
            const DeviceOffset block_temporal =
                BlockReduce(reduce_storage).Sum(
                    local_temporal);
            __syncthreads();
            const DeviceOffset block_interval_steps =
                BlockReduce(reduce_storage).Sum(
                    local_interval_steps);
            __syncthreads();
            const DeviceOffset block_edge_lookups =
                BlockReduce(reduce_storage).Sum(
                    local_edge_lookups);
            __syncthreads();
            if (threadIdx.x == 0) {
                const DeviceOffset generated =
                    end - begin;
                atomicAdd(
                    &counters->generated_candidates,
                    generated);
                atomicAdd(
                    &counters->accepted_candidates,
                    block_valid);
                atomicAdd(
                    &counters->pruned_injectivity,
                    block_injectivity);
                atomicAdd(
                    &counters->pruned_structural,
                    block_structural);
                atomicAdd(
                    &counters->pruned_temporal,
                    block_temporal);
                atomicAdd(
                    &counters->temporal_interval_steps,
                    block_interval_steps);
                atomicAdd(
                    &counters->directed_edge_lookups,
                    block_edge_lookups);
                atomicAdd(
                    &depth_counters
                         ->generated_candidates,
                    generated);
                atomicMax(
                    &depth_counters
                         ->maximum_candidates_per_state,
                    generated);
            }
        }
        __syncthreads();
    }
}

__global__ void setLastOffsetKernel(
    const DeviceOffset* counts,
    DeviceOffset* offsets,
    DeviceOffset count) {
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        offsets[count] =
            offsets[count - 1] + counts[count - 1];
    }
}

__global__ void materializeExtensionsKernel(
    DeviceGraphView graph,
    DeviceQueryView query,
    DeviceRelationView relations,
    const int* current_mappings,
    DeviceOffset state_count,
    int next_query_vertex,
    int parent_query_vertex,
    int minimum_duration,
    bool cumulative_prune,
    bool final_depth,
    const DeviceOffset* output_offsets,
    int* next_mappings,
    int* mismatch_flag) {
    __shared__ DeviceOffset local_cursor;

    for (DeviceOffset state_index = blockIdx.x;
         state_index < state_count;
         state_index += gridDim.x) {
        if (threadIdx.x == 0) local_cursor = 0;
        __syncthreads();

        const int* mapping =
            current_mappings +
            state_index *
                static_cast<DeviceOffset>(
                    query.query_vertex_count);
        const int parent_data_vertex =
            mapping[parent_query_vertex];
        const DeviceOffset row =
            static_cast<DeviceOffset>(next_query_vertex) *
            relations.row_stride;
        const DeviceOffset begin =
            relations.offsets[
                row +
                static_cast<DeviceOffset>(
                    parent_data_vertex)];
        const DeviceOffset end =
            relations.offsets[
                row +
                static_cast<DeviceOffset>(
                    parent_data_vertex) +
                1ULL];

        for (DeviceOffset relation_index =
                 begin + threadIdx.x;
             relation_index < end;
             relation_index += blockDim.x) {
            const int candidate =
                relations.values[relation_index];
            DeviceOffset unused_interval_steps = 0;
            DeviceOffset unused_edge_lookups = 0;
            if (validateExtension(
                    graph,
                    query,
                    mapping,
                    next_query_vertex,
                    candidate,
                    minimum_duration,
                    cumulative_prune,
                    final_depth,
                    unused_interval_steps,
                    unused_edge_lookups) !=
                ValidationResult::Valid) {
                continue;
            }

            const DeviceOffset local_output =
                atomicAdd(&local_cursor, 1ULL);
            const DeviceOffset output_index =
                output_offsets[state_index] + local_output;
            int* output_mapping =
                next_mappings +
                output_index *
                    static_cast<DeviceOffset>(
                        query.query_vertex_count);
            for (int query_vertex = 0;
                 query_vertex < query.query_vertex_count;
                 ++query_vertex) {
                output_mapping[query_vertex] =
                    mapping[query_vertex];
            }
            output_mapping[next_query_vertex] = candidate;
        }
        __syncthreads();
        if (threadIdx.x == 0) {
            const DeviceOffset expected =
                output_offsets[state_index + 1] -
                output_offsets[state_index];
            if (local_cursor != expected) {
                atomicExch(mismatch_flag, 1);
            }
        }
        __syncthreads();
    }
}

__device__ DeviceOffset mix64Device(DeviceOffset value) {
    value += 0x9e3779b97f4a7c15ULL;
    value =
        (value ^ (value >> 30U)) *
        0xbf58476d1ce4e5b9ULL;
    value =
        (value ^ (value >> 27U)) *
        0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

__global__ void digestMappingsKernel(
    const int* mappings,
    DeviceOffset state_count,
    int query_vertex_count,
    const int* external_ids,
    DeviceOffset* xor_value,
    DeviceOffset* sum_value) {
    const DeviceOffset stride =
        static_cast<DeviceOffset>(blockDim.x) * gridDim.x;
    DeviceOffset state_index =
        static_cast<DeviceOffset>(blockIdx.x) * blockDim.x +
        threadIdx.x;
    for (; state_index < state_count; state_index += stride) {
        const int* mapping =
            mappings +
            state_index *
                static_cast<DeviceOffset>(
                    query_vertex_count);
        DeviceOffset hash = 0x6a09e667f3bcc909ULL;
        for (int query_vertex = 0;
             query_vertex < query_vertex_count;
             ++query_vertex) {
            const DeviceOffset external =
                static_cast<DeviceOffset>(
                    static_cast<unsigned int>(
                        external_ids[mapping[query_vertex]]));
            hash = mix64Device(
                hash ^
                mix64Device(
                    external +
                    static_cast<DeviceOffset>(query_vertex) *
                        0x9e3779b97f4a7c15ULL));
        }
        atomicXor(xor_value, hash);
        atomicAdd(sum_value, hash);
    }
}

struct HostFlatData {
    std::vector<DeviceOffset> adjacency_offsets;
    std::vector<int> adjacency_targets;
    std::vector<int> adjacency_edge_ids;
    std::vector<DeviceOffset> interval_offsets;
    std::vector<GpuInterval> intervals;
    std::vector<DeviceOffset> relation_offsets;
    std::vector<int> relation_values;
    std::vector<int> arc_sources;
    std::vector<int> arc_targets;
    std::vector<int> external_ids;
    std::vector<int> root_mappings;
};

bool hasQuerySelfLoop(const Graph& query, int query_vertex) {
    return GraphUtils::hasEdge(
        query.adj, query_vertex, query_vertex);
}

HostFlatData flattenForGpu(
    const Graph& temporal_graph,
    const Graph& query_graph,
    const QueryPlan& plan,
    const TDTree& td_tree,
    int minimum_duration) {
    if (query_graph.num_vertices <= 0 ||
        plan.root < 0 ||
        plan.order.empty()) {
        throw std::runtime_error("GPU matcher received an empty query plan");
    }

    HostFlatData flat;
    const std::size_t data_vertices =
        static_cast<std::size_t>(temporal_graph.num_vertices);
    const std::size_t query_vertices =
        static_cast<std::size_t>(query_graph.num_vertices);

    flat.adjacency_offsets.resize(data_vertices + 1, 0);
    for (std::size_t source = 0;
         source < data_vertices;
         ++source) {
        flat.adjacency_offsets[source] =
            static_cast<DeviceOffset>(
                flat.adjacency_targets.size());
        for (const Edge& edge : temporal_graph.adj[source]) {
            if (edge.temporal_edge_id < 0) {
                throw std::runtime_error(
                    "temporal adjacency contains a non-temporal edge");
            }
            flat.adjacency_targets.push_back(edge.to);
            flat.adjacency_edge_ids.push_back(edge.temporal_edge_id);
        }
    }
    flat.adjacency_offsets[data_vertices] =
        static_cast<DeviceOffset>(
            flat.adjacency_targets.size());

    flat.interval_offsets.resize(
        temporal_graph.temporal_edges.size() + 1, 0);
    for (std::size_t edge_id = 0;
         edge_id < temporal_graph.temporal_edges.size();
         ++edge_id) {
        flat.interval_offsets[edge_id] =
            static_cast<DeviceOffset>(flat.intervals.size());
        for (const TimeInterval& interval :
             temporal_graph.temporal_edges[edge_id]
                 .active_intervals) {
            flat.intervals.push_back(
                {interval.start, interval.end});
        }
    }
    flat.interval_offsets[
        temporal_graph.temporal_edges.size()] =
        static_cast<DeviceOffset>(flat.intervals.size());

    const std::size_t relation_stride =
        data_vertices + 1;
    if (relation_stride != 0 &&
        query_vertices >
            std::numeric_limits<std::size_t>::max() /
                relation_stride) {
        throw std::overflow_error(
            "GPU relation-offset element count overflow");
    }
    const std::size_t relation_offset_count =
        query_vertices * relation_stride;
    (void)checkedBytes(
        relation_offset_count, sizeof(DeviceOffset));
    flat.relation_offsets.resize(
        relation_offset_count, 0);
    for (int query_vertex = 0;
         query_vertex < query_graph.num_vertices;
         ++query_vertex) {
        const std::size_t row =
            static_cast<std::size_t>(query_vertex) *
            relation_stride;
        for (int parent_data_vertex = 0;
             parent_data_vertex < temporal_graph.num_vertices;
             ++parent_data_vertex) {
            flat.relation_offsets[
                row +
                static_cast<std::size_t>(
                    parent_data_vertex)] =
                static_cast<DeviceOffset>(
                    flat.relation_values.size());
            const std::vector<int>* candidates =
                td_tree.candidatesForParent(
                    query_vertex, parent_data_vertex);
            if (candidates != nullptr) {
                flat.relation_values.insert(
                    flat.relation_values.end(),
                    candidates->begin(),
                    candidates->end());
            }
        }
        flat.relation_offsets[row + data_vertices] =
            static_cast<DeviceOffset>(
                flat.relation_values.size());
    }

    for (int query_source = 0;
         query_source < query_graph.num_vertices;
         ++query_source) {
        for (const Edge& edge :
             query_graph.adj[
                 static_cast<std::size_t>(query_source)]) {
            flat.arc_sources.push_back(query_source);
            flat.arc_targets.push_back(edge.to);
        }
    }
    if (flat.arc_sources.size() >
        static_cast<std::size_t>(kMaximumQueryArcs)) {
        throw std::runtime_error(
            "GPU backend supports at most " +
            std::to_string(kMaximumQueryArcs) +
            " directed query arcs");
    }

    flat.external_ids = temporal_graph.external_ids;
    if (flat.external_ids.size() != data_vertices) {
        flat.external_ids.resize(data_vertices);
        for (std::size_t vertex = 0;
             vertex < data_vertices;
             ++vertex) {
            flat.external_ids[vertex] =
                temporal_graph.externalId(
                    static_cast<int>(vertex));
        }
    }

    const bool root_self_loop =
        hasQuerySelfLoop(query_graph, plan.root);
    for (int root_candidate : td_tree.rootCandidates()) {
        if (root_self_loop) {
            const TemporalEdge* self_edge =
                temporal_graph.findTemporalEdge(
                    root_candidate, root_candidate);
            if (self_edge == nullptr ||
                !hasMinimumConsecutiveDuration(
                    self_edge->active_intervals,
                    minimum_duration)) {
                continue;
            }
        }
        const std::size_t base =
            flat.root_mappings.size();
        flat.root_mappings.resize(
            base + query_vertices, -1);
        flat.root_mappings[
            base +
            static_cast<std::size_t>(plan.root)] =
            root_candidate;
    }
    return flat;
}

unsigned int gridBlocks(DeviceOffset state_count) {
    return static_cast<unsigned int>(
        std::min<DeviceOffset>(
            state_count,
            static_cast<DeviceOffset>(kMaximumGridBlocks)));
}

template <typename T>
void addBufferBytes(std::size_t& total, const DeviceBuffer<T>& buffer) {
    if (buffer.bytes() >
        std::numeric_limits<std::size_t>::max() - total) {
        throw std::overflow_error("GPU memory accounting overflow");
    }
    total += buffer.bytes();
}

std::string formatDigest(
    DeviceOffset xor_value,
    DeviceOffset sum_value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0')
           << std::setw(16) << xor_value << ':'
           << std::setw(16) << sum_value;
    return output.str();
}

} // namespace

const char* gpuExecutionVariantName(
    GpuExecutionVariant variant) {
    return variant ==
            GpuExecutionVariant::ResidentContinuation
        ? "resident_continuation"
        : "level_synchronous";
}

bool parseGpuExecutionVariant(
    const std::string& text,
    GpuExecutionVariant& variant) {
    if (text == "level" ||
        text == "level_synchronous" ||
        text == "baseline") {
        variant =
            GpuExecutionVariant::LevelSynchronous;
        return true;
    }
    if (text == "resident" ||
        text == "resident_continuation" ||
        text == "continuation") {
        variant =
            GpuExecutionVariant::ResidentContinuation;
        return true;
    }
    return false;
}

GpuMatcher::GpuMatcher(
    const Graph& temporal_graph,
    const Graph& query_graph,
    const QueryPlan& plan,
    const CandidateDag& dag,
    const TDTree& td_tree,
    int minimum_duration,
    GpuMatcherOptions options)
    : temporal_graph_(temporal_graph),
      query_graph_(query_graph),
      plan_(plan),
      dag_(dag),
      td_tree_(td_tree),
      minimum_duration_(minimum_duration),
      options_(std::move(options)) {}

GpuMatchSummary GpuMatcher::run() {
    GpuMatchSummary summary;
    summary.metrics.statistics_collected =
        options_.collect_statistics;
    summary.metrics.dag_nodes_lowered =
        static_cast<std::uint64_t>(dag_.nodes.size());
    summary.metrics.block_size =
        options_.variant ==
                GpuExecutionVariant::ResidentContinuation
        ? kResidentBlockSize
        : kBlockSize;

    int device_count = 0;
    checkHip(hipGetDeviceCount(&device_count), "hipGetDeviceCount");
    if (device_count <= 0) {
        throw std::runtime_error("no HIP devices are available");
    }
    if (options_.device < 0 || options_.device >= device_count) {
        throw std::runtime_error(
            "GPU device index is outside the available range");
    }
    checkHip(hipSetDevice(options_.device), "hipSetDevice");
    checkHip(
        hipRuntimeGetVersion(
            &summary.metrics.hip_runtime_version),
        "hipRuntimeGetVersion");
    checkHip(
        hipDriverGetVersion(
            &summary.metrics.hip_driver_version),
        "hipDriverGetVersion");

    hipDeviceProp_t properties{};
    checkHip(
        hipGetDeviceProperties(
            &properties, options_.device),
        "hipGetDeviceProperties");
    summary.metrics.device_name = properties.name;
    summary.metrics.architecture = properties.gcnArchName;
    summary.metrics.compute_units =
        properties.multiProcessorCount;
    summary.metrics.warp_size = properties.warpSize;
    summary.metrics.device_global_memory_bytes =
        properties.totalGlobalMem;
    if (properties.maxThreadsPerBlock <
        summary.metrics.block_size) {
        throw std::runtime_error(
            "selected HIP device cannot launch the required block size");
    }

    for (const DagNode& node : dag_.nodes) {
        switch (node.op) {
            case DagOp::TdBlockLookup:
            case DagOp::OutNeighborLookup:
            case DagOp::InNeighborLookup:
            case DagOp::StructuralIntersection:
            case DagOp::EdgeTimeLookup:
            case DagOp::TemporalIntersection:
            case DagOp::LongestConsecutiveRun:
                break;
            default:
                throw std::runtime_error(
                    "Candidate DAG contains an unsupported GPU operation");
        }
    }

    const auto flatten_start = Clock::now();
    HostFlatData flat = flattenForGpu(
        temporal_graph_,
        query_graph_,
        plan_,
        td_tree_,
        minimum_duration_);
    summary.metrics.flatten_nanoseconds =
        elapsedNanoseconds(flatten_start);

    const std::size_t query_vertex_count =
        static_cast<std::size_t>(
            query_graph_.num_vertices);
    const std::size_t root_count =
        flat.root_mappings.size() /
        query_vertex_count;
    const std::size_t root_bytes = checkedBytes(
        flat.root_mappings.size(), sizeof(int));
    if (root_bytes > options_.frontier_budget_bytes) {
        throw std::runtime_error(
            "root frontier exceeds --gpu-memory-mb; "
            "implicit CPU fallback is disabled");
    }

    const auto gpu_ready_start = Clock::now();

    const auto persistent_allocation_start =
        Clock::now();
    DeviceBuffer<DeviceOffset> d_adjacency_offsets(
        flat.adjacency_offsets.size());
    DeviceBuffer<int> d_adjacency_targets(
        flat.adjacency_targets.size());
    DeviceBuffer<int> d_adjacency_edge_ids(
        flat.adjacency_edge_ids.size());
    DeviceBuffer<DeviceOffset> d_interval_offsets(
        flat.interval_offsets.size());
    DeviceBuffer<GpuInterval> d_intervals(
        flat.intervals.size());
    DeviceBuffer<DeviceOffset> d_relation_offsets(
        flat.relation_offsets.size());
    DeviceBuffer<int> d_relation_values(
        flat.relation_values.size());
    DeviceBuffer<int> d_arc_sources(
        flat.arc_sources.size());
    DeviceBuffer<int> d_arc_targets(
        flat.arc_targets.size());
    DeviceBuffer<int> d_external_ids(
        flat.external_ids.size());
    DeviceBuffer<DeviceCounters> d_counters(1);
    DeviceBuffer<DeviceDepthCounters>
        d_level_depth_counters(plan_.order.size());
    DeviceBuffer<int> d_mismatch(1);
    summary.metrics.device_allocation_nanoseconds +=
        elapsedNanoseconds(
            persistent_allocation_start);

    std::size_t persistent_bytes = 0;
    addBufferBytes(persistent_bytes, d_adjacency_offsets);
    addBufferBytes(persistent_bytes, d_adjacency_targets);
    addBufferBytes(persistent_bytes, d_adjacency_edge_ids);
    addBufferBytes(persistent_bytes, d_interval_offsets);
    addBufferBytes(persistent_bytes, d_intervals);
    addBufferBytes(persistent_bytes, d_relation_offsets);
    addBufferBytes(persistent_bytes, d_relation_values);
    addBufferBytes(persistent_bytes, d_arc_sources);
    addBufferBytes(persistent_bytes, d_arc_targets);
    addBufferBytes(persistent_bytes, d_external_ids);
    addBufferBytes(persistent_bytes, d_counters);
    addBufferBytes(
        persistent_bytes,
        d_level_depth_counters);
    addBufferBytes(persistent_bytes, d_mismatch);
    summary.metrics.persistent_device_bytes = persistent_bytes;

    auto h2d = [&](void* destination,
                   const void* source,
                   std::size_t bytes) {
        if (bytes == 0) return;
        const auto start = Clock::now();
        checkHip(
            hipMemcpy(
                destination,
                source,
                bytes,
                hipMemcpyHostToDevice),
            "hipMemcpy H2D");
        summary.metrics.h2d_nanoseconds +=
            elapsedNanoseconds(start);
        summary.metrics.h2d_bytes += bytes;
    };
    auto d2h = [&](void* destination,
                   const void* source,
                   std::size_t bytes) {
        if (bytes == 0) return;
        const auto start = Clock::now();
        checkHip(
            hipMemcpy(
                destination,
                source,
                bytes,
                hipMemcpyDeviceToHost),
            "hipMemcpy D2H");
        summary.metrics.d2h_nanoseconds +=
            elapsedNanoseconds(start);
        summary.metrics.d2h_bytes += bytes;
    };

    h2d(
        d_adjacency_offsets.get(),
        flat.adjacency_offsets.data(),
        d_adjacency_offsets.bytes());
    h2d(
        d_adjacency_targets.get(),
        flat.adjacency_targets.data(),
        d_adjacency_targets.bytes());
    h2d(
        d_adjacency_edge_ids.get(),
        flat.adjacency_edge_ids.data(),
        d_adjacency_edge_ids.bytes());
    h2d(
        d_interval_offsets.get(),
        flat.interval_offsets.data(),
        d_interval_offsets.bytes());
    h2d(
        d_intervals.get(),
        flat.intervals.data(),
        d_intervals.bytes());
    h2d(
        d_relation_offsets.get(),
        flat.relation_offsets.data(),
        d_relation_offsets.bytes());
    h2d(
        d_relation_values.get(),
        flat.relation_values.data(),
        d_relation_values.bytes());
    h2d(
        d_arc_sources.get(),
        flat.arc_sources.data(),
        d_arc_sources.bytes());
    h2d(
        d_arc_targets.get(),
        flat.arc_targets.data(),
        d_arc_targets.bytes());
    h2d(
        d_external_ids.get(),
        flat.external_ids.data(),
        d_external_ids.bytes());
    checkHip(
        hipMemset(
            d_counters.get(), 0, d_counters.bytes()),
        "hipMemset counters");
    checkHip(
        hipMemset(
            d_level_depth_counters.get(),
            0,
            d_level_depth_counters.bytes()),
        "hipMemset level depth counters");
    checkHip(
        hipMemset(
            d_mismatch.get(), 0, d_mismatch.bytes()),
        "hipMemset mismatch");

    DeviceBuffer<int> current_mappings;
    DeviceBuffer<int> resident_frontier_a;
    DeviceBuffer<int> resident_frontier_b;
    DeviceBuffer<DeviceOffset> resident_depth_counts;
    DeviceBuffer<DeviceOffset> resident_work_indices;
    DeviceBuffer<DeviceDepthCounters>
        resident_depth_counters;
    DeviceBuffer<int> resident_overflow;

    const auto root_allocation_start = Clock::now();
    if (options_.variant ==
        GpuExecutionVariant::LevelSynchronous) {
        current_mappings.allocate(
            flat.root_mappings.size());
        summary.metrics.peak_frontier_device_bytes =
            current_mappings.bytes();
    } else {
        const std::size_t depth_count =
            plan_.order.size();
        std::size_t control_bytes = checkedBytes(
            depth_count,
            sizeof(DeviceOffset));
        const std::size_t work_bytes = checkedBytes(
            depth_count,
            sizeof(DeviceOffset));
        const std::size_t depth_counter_bytes =
            checkedBytes(
                depth_count,
                sizeof(DeviceDepthCounters));
        if (work_bytes >
                std::numeric_limits<std::size_t>::max() -
                    control_bytes ||
            depth_counter_bytes >
                std::numeric_limits<std::size_t>::max() -
                    control_bytes -
                    work_bytes ||
            sizeof(int) >
                std::numeric_limits<std::size_t>::max() -
                    control_bytes -
                    work_bytes -
                    depth_counter_bytes) {
            throw std::overflow_error(
                "resident GPU control storage overflow");
        }
        control_bytes +=
            work_bytes +
            depth_counter_bytes +
            sizeof(int);
        const std::size_t state_bytes = checkedBytes(
            query_vertex_count,
            sizeof(int));
        const bool root_workspace_overflow =
            state_bytes != 0 &&
            root_count >
                std::numeric_limits<std::size_t>::max() /
                    state_bytes /
                    2ULL;
        const std::size_t root_workspace_bytes =
            root_workspace_overflow
            ? std::numeric_limits<std::size_t>::max()
            : root_count * state_bytes * 2ULL;
        if (state_bytes == 0 ||
            control_bytes >
                options_.frontier_budget_bytes ||
            options_.frontier_budget_bytes -
                    control_bytes <
                root_workspace_bytes) {
            throw std::runtime_error(
                "resident frontier root/control storage exceeds "
                "--gpu-memory-mb; implicit CPU fallback is disabled");
        }
        const std::size_t maximum_capacity =
            (options_.frontier_budget_bytes -
             control_bytes) /
            (2ULL * state_bytes);

        std::size_t upper_bound = root_count;
        std::size_t peak_upper_bound = root_count;
        const std::size_t relation_stride =
            static_cast<std::size_t>(
                temporal_graph_.num_vertices) +
            1ULL;
        for (std::size_t depth = 1;
             depth < plan_.order.size();
             ++depth) {
            const int next_query_vertex =
                plan_.order[depth];
            const std::size_t row =
                static_cast<std::size_t>(
                    next_query_vertex) *
                relation_stride;
            std::size_t maximum_row = 0;
            for (int parent_data_vertex = 0;
                 parent_data_vertex <
                     temporal_graph_.num_vertices;
                 ++parent_data_vertex) {
                const std::size_t column =
                    static_cast<std::size_t>(
                        parent_data_vertex);
                const DeviceOffset begin =
                    flat.relation_offsets[
                        row + column];
                const DeviceOffset end =
                    flat.relation_offsets[
                        row + column + 1ULL];
                maximum_row =
                    std::max<std::size_t>(
                        maximum_row,
                        static_cast<std::size_t>(
                            end - begin));
            }
            if (upper_bound == 0 ||
                maximum_row == 0) {
                upper_bound = 0;
            } else if (
                upper_bound >
                maximum_capacity / maximum_row) {
                upper_bound = maximum_capacity;
                peak_upper_bound = maximum_capacity;
                break;
            } else {
                upper_bound *= maximum_row;
                peak_upper_bound =
                    std::max(
                        peak_upper_bound,
                        upper_bound);
            }
        }
        const std::size_t resident_capacity =
            std::max<std::size_t>(
                root_count,
                std::min(
                    maximum_capacity,
                    peak_upper_bound));
        summary.metrics.resident_capacity_states =
            resident_capacity;
        const std::size_t resident_elements =
            checkedBytes(
                resident_capacity,
                query_vertex_count);
        resident_frontier_a.allocate(
            resident_elements);
        resident_frontier_b.allocate(
            resident_elements);
        resident_depth_counts.allocate(depth_count);
        resident_work_indices.allocate(depth_count);
        resident_depth_counters.allocate(depth_count);
        resident_overflow.allocate(1);
        summary.metrics.peak_frontier_device_bytes =
            resident_frontier_a.bytes() +
            resident_frontier_b.bytes() +
            resident_depth_counts.bytes() +
            resident_work_indices.bytes() +
            resident_depth_counters.bytes() +
            resident_overflow.bytes();
    }
    summary.metrics.device_allocation_nanoseconds +=
        elapsedNanoseconds(root_allocation_start);
    if (options_.variant ==
        GpuExecutionVariant::LevelSynchronous) {
        h2d(
            current_mappings.get(),
            flat.root_mappings.data(),
            current_mappings.bytes());
    } else {
        h2d(
            resident_frontier_a.get(),
            flat.root_mappings.data(),
            root_bytes);
        const DeviceOffset root_device_count =
            static_cast<DeviceOffset>(root_count);
        checkHip(
            hipMemset(
                resident_depth_counts.get(),
                0,
                resident_depth_counts.bytes()),
            "hipMemset resident depth counts");
        h2d(
            resident_depth_counts.get(),
            &root_device_count,
            sizeof(root_device_count));
        checkHip(
            hipMemset(
                resident_work_indices.get(),
                0,
                resident_work_indices.bytes()),
            "hipMemset resident work indices");
        checkHip(
            hipMemset(
                resident_depth_counters.get(),
                0,
                resident_depth_counters.bytes()),
            "hipMemset resident depth counters");
        checkHip(
            hipMemset(
                resident_overflow.get(),
                0,
                resident_overflow.bytes()),
            "hipMemset resident overflow");
    }
    summary.metrics.peak_frontier_states = root_count;

    flat = HostFlatData{};

    const DeviceGraphView device_graph{
        d_adjacency_offsets.get(),
        d_adjacency_targets.get(),
        d_adjacency_edge_ids.get(),
        d_interval_offsets.get(),
        d_intervals.get()};
    const DeviceQueryView device_query{
        d_arc_sources.get(),
        d_arc_targets.get(),
        static_cast<int>(d_arc_sources.size()),
        query_graph_.num_vertices};
    const DeviceRelationView device_relations{
        d_relation_offsets.get(),
        d_relation_values.get(),
        static_cast<DeviceOffset>(
            temporal_graph_.num_vertices) + 1ULL};

    hipEvent_t event_start = nullptr;
    hipEvent_t event_stop = nullptr;
    checkHip(
        hipEventCreate(&event_start), "hipEventCreate start");
    try {
        checkHip(
            hipEventCreate(&event_stop), "hipEventCreate stop");
    } catch (...) {
        (void)hipEventDestroy(event_start);
        throw;
    }
    auto timeDeviceSequence = [&](auto&& launch) {
        const auto host_start = Clock::now();
        checkHip(
            hipEventRecord(event_start, nullptr),
            "hipEventRecord start");
        launch();
        checkHip(
            hipEventRecord(event_stop, nullptr),
            "hipEventRecord stop");
        checkHip(
            hipEventSynchronize(event_stop),
            "hipEventSynchronize");
        ++summary.metrics.device_synchronizations;
        float milliseconds = 0.0F;
        checkHip(
            hipEventElapsedTime(
                &milliseconds,
                event_start,
                event_stop),
            "hipEventElapsedTime");
        const std::uint64_t device_nanoseconds =
            static_cast<std::uint64_t>(
                static_cast<double>(milliseconds) *
                1'000'000.0);
        const std::uint64_t host_nanoseconds =
            elapsedNanoseconds(host_start);
        summary.metrics.kernel_nanoseconds +=
            device_nanoseconds;
        if (host_nanoseconds > device_nanoseconds) {
            summary.metrics
                .launch_and_sync_overhead_nanoseconds +=
                host_nanoseconds -
                device_nanoseconds;
        }
        return device_nanoseconds;
    };

    DeviceOffset state_count =
        static_cast<DeviceOffset>(root_count);
    const auto gpu_search_start = Clock::now();

    try {
        const int* final_mappings =
            current_mappings.get();
        if (options_.variant ==
            GpuExecutionVariant::ResidentContinuation) {
            int active_blocks_per_multiprocessor = 0;
            checkHip(
                hipOccupancyMaxActiveBlocksPerMultiprocessor(
                    &active_blocks_per_multiprocessor,
                    residentContinuationKernel<
                        kResidentBlockSize>,
                    kResidentBlockSize,
                    0),
                "hipOccupancyMaxActiveBlocksPerMultiprocessor");
            if (active_blocks_per_multiprocessor <= 0) {
                throw std::runtime_error(
                    "resident continuation kernel has zero occupancy");
            }
            summary.metrics
                .occupancy_blocks_per_multiprocessor =
                active_blocks_per_multiprocessor;
            const unsigned long long requested_blocks =
                static_cast<unsigned long long>(
                    active_blocks_per_multiprocessor) *
                static_cast<unsigned long long>(
                    std::max(
                        1,
                        properties.multiProcessorCount));
            const unsigned int resident_blocks =
                static_cast<unsigned int>(
                    std::min<unsigned long long>(
                        requested_blocks,
                        kMaximumGridBlocks));
            summary.metrics.resident_grid_blocks =
                resident_blocks;

            int* resident_current =
                resident_frontier_a.get();
            int* resident_next =
                resident_frontier_b.get();
            const DeviceOffset resident_capacity =
                static_cast<DeviceOffset>(
                    summary.metrics
                        .resident_capacity_states);

            const std::uint64_t resident_device_ns =
                timeDeviceSequence([&]() {
                    for (std::size_t depth = 1;
                         depth < plan_.order.size();
                         ++depth) {
                        const int next_query_vertex =
                            plan_.order[depth];
                        const int parent_query_vertex =
                            plan_.parent[
                                static_cast<std::size_t>(
                                    next_query_vertex)];
                        if (parent_query_vertex < 0) {
                            throw std::runtime_error(
                                "GPU query plan violates "
                                "parent-before-child order");
                        }
                        checkHip(
                            hipMemsetAsync(
                                resident_depth_counts.get() +
                                    depth,
                                0,
                                sizeof(DeviceOffset),
                                nullptr),
                            "hipMemsetAsync resident next count");
                        checkHip(
                            hipMemsetAsync(
                                resident_work_indices.get() +
                                    depth,
                                0,
                                sizeof(DeviceOffset),
                                nullptr),
                            "hipMemsetAsync resident work index");
                        const bool final_depth =
                            depth + 1 ==
                            plan_.order.size();
                        hipLaunchKernelGGL(
                            HIP_KERNEL_NAME(
                                residentContinuationKernel<
                                    kResidentBlockSize>),
                            dim3(resident_blocks),
                            dim3(kResidentBlockSize),
                            0,
                            0,
                            device_graph,
                            device_query,
                            device_relations,
                            resident_current,
                            resident_depth_counts.get() +
                                depth -
                                1ULL,
                            resident_next,
                            resident_capacity,
                            resident_depth_counts.get() +
                                depth,
                            resident_work_indices.get() +
                                depth,
                            next_query_vertex,
                            parent_query_vertex,
                            minimum_duration_,
                            options_.early_prune,
                            final_depth,
                            options_.collect_statistics,
                            d_counters.get(),
                            resident_depth_counters.get() +
                                depth,
                            resident_overflow.get());
                        checkHip(
                            hipGetLastError(),
                            "residentContinuationKernel launch");
                        ++summary.metrics.kernel_launches;
                        ++summary.metrics.frontier_levels;
                        ++summary.metrics
                              .frontier_validation_passes;
                        ++summary.metrics
                              .frontier_materialization_passes;
                        std::swap(
                            resident_current,
                            resident_next);
                    }
                });
            (void)resident_device_ns;

            std::vector<DeviceOffset> depth_counts(
                plan_.order.size(),
                0);
            d2h(
                depth_counts.data(),
                resident_depth_counts.get(),
                resident_depth_counts.bytes());
            int resident_overflow_flag = 0;
            d2h(
                &resident_overflow_flag,
                resident_overflow.get(),
                sizeof(resident_overflow_flag));
            if (resident_overflow_flag != 0) {
                throw std::runtime_error(
                    "resident continuation frontier exceeded "
                    "--gpu-memory-mb; implicit CPU fallback is disabled");
            }

            std::vector<DeviceDepthCounters>
                depth_counters(plan_.order.size());
            if (options_.collect_statistics) {
                d2h(
                    depth_counters.data(),
                    resident_depth_counters.get(),
                    resident_depth_counters.bytes());
            }
            for (std::size_t depth = 1;
                 depth < plan_.order.size();
                 ++depth) {
                GpuDepthMetrics metrics;
                metrics.depth = depth;
                metrics.input_states =
                    depth_counts[depth - 1ULL];
                metrics.output_states =
                    depth_counts[depth];
                metrics.grid_blocks =
                    resident_blocks;
                if (options_.collect_statistics) {
                    metrics.generated_candidates =
                        depth_counters[depth]
                            .generated_candidates;
                    metrics
                        .maximum_candidates_per_state =
                        depth_counters[depth]
                            .maximum_candidates_per_state;
                }
                summary.metrics.depths.push_back(
                    metrics);
                summary.metrics.peak_frontier_states =
                    std::max<std::size_t>(
                        summary.metrics
                            .peak_frontier_states,
                        static_cast<std::size_t>(
                            depth_counts[depth]));
            }
            state_count =
                depth_counts.empty()
                ? 0
                : depth_counts.back();
            final_mappings = resident_current;
        } else {
        for (std::size_t depth = 1;
             depth < plan_.order.size() &&
             state_count != 0;
             ++depth) {
            ++summary.metrics.frontier_levels;
            const int next_query_vertex =
                plan_.order[depth];
            const int parent_query_vertex =
                plan_.parent[
                    static_cast<std::size_t>(
                        next_query_vertex)];
            if (parent_query_vertex < 0) {
                throw std::runtime_error(
                    "GPU query plan violates parent-before-child order");
            }

            if (state_count >
                static_cast<DeviceOffset>(
                    std::numeric_limits<std::size_t>::max())) {
                throw std::overflow_error(
                    "GPU frontier state count exceeds host size_t");
            }
            const std::size_t states =
                static_cast<std::size_t>(state_count);
            if (states ==
                std::numeric_limits<std::size_t>::max()) {
                throw std::overflow_error(
                    "GPU frontier scan element count overflow");
            }
            std::size_t minimum_auxiliary_bytes =
                current_mappings.bytes();
            const std::size_t counts_bytes =
                checkedBytes(states, sizeof(DeviceOffset));
            const std::size_t offsets_bytes =
                checkedBytes(states + 1, sizeof(DeviceOffset));
            if (counts_bytes >
                    std::numeric_limits<std::size_t>::max() -
                        minimum_auxiliary_bytes ||
                offsets_bytes >
                    std::numeric_limits<std::size_t>::max() -
                        minimum_auxiliary_bytes -
                        counts_bytes ||
                minimum_auxiliary_bytes +
                        counts_bytes +
                        offsets_bytes >
                    options_.frontier_budget_bytes) {
                throw std::runtime_error(
                    "GPU frontier count/scan storage exceeds "
                    "--gpu-memory-mb; implicit CPU fallback is disabled");
            }
            const auto auxiliary_allocation_start =
                Clock::now();
            DeviceBuffer<DeviceOffset> valid_counts(states);
            DeviceBuffer<DeviceOffset> output_offsets(states + 1);

            std::size_t scan_temp_bytes = 0;
            checkHip(
                hipcub::DeviceScan::ExclusiveSum(
                    nullptr,
                    scan_temp_bytes,
                    valid_counts.get(),
                    output_offsets.get(),
                    states),
                "hipcub scan size query");
            DeviceBuffer<unsigned char> scan_temp(
                scan_temp_bytes);
            summary.metrics.device_allocation_nanoseconds +=
                elapsedNanoseconds(
                    auxiliary_allocation_start);

            std::size_t pre_output_bytes =
                current_mappings.bytes();
            addBufferBytes(
                pre_output_bytes, valid_counts);
            addBufferBytes(
                pre_output_bytes, output_offsets);
            addBufferBytes(
                pre_output_bytes, scan_temp);
            if (pre_output_bytes >
                options_.frontier_budget_bytes) {
                throw std::runtime_error(
                    "GPU frontier auxiliary storage exceeds "
                    "--gpu-memory-mb; implicit CPU fallback is disabled");
            }

            const unsigned int blocks =
                gridBlocks(state_count);
            const bool final_depth =
                depth + 1 == plan_.order.size();
            GpuDepthMetrics depth_metrics;
            depth_metrics.depth = depth;
            depth_metrics.input_states = state_count;
            depth_metrics.grid_blocks = blocks;
            const std::uint64_t count_device_ns =
                timeDeviceSequence([&]() {
                hipLaunchKernelGGL(
                    HIP_KERNEL_NAME(
                        countExtensionsKernel<kBlockSize>),
                    dim3(blocks),
                    dim3(kBlockSize),
                    0,
                    0,
                    device_graph,
                    device_query,
                    device_relations,
                    current_mappings.get(),
                    state_count,
                    next_query_vertex,
                    parent_query_vertex,
                    minimum_duration_,
                    options_.early_prune,
                    final_depth,
                    options_.collect_statistics,
                    valid_counts.get(),
                    d_counters.get(),
                    d_level_depth_counters.get() +
                        depth);
                checkHip(
                    hipGetLastError(),
                    "countExtensionsKernel launch");
                ++summary.metrics.kernel_launches;

                checkHip(
                    hipcub::DeviceScan::ExclusiveSum(
                        scan_temp.get(),
                        scan_temp_bytes,
                        valid_counts.get(),
                        output_offsets.get(),
                        states),
                    "hipcub exclusive scan");
                ++summary.metrics.kernel_launches;

                hipLaunchKernelGGL(
                    setLastOffsetKernel,
                    dim3(1),
                    dim3(1),
                    0,
                    0,
                    valid_counts.get(),
                    output_offsets.get(),
                    state_count);
                checkHip(
                    hipGetLastError(),
                    "setLastOffsetKernel launch");
                ++summary.metrics.kernel_launches;
            });
            summary.metrics.frontier_validation_passes += 2;
            ++summary.metrics.frontier_materialization_passes;

            DeviceOffset next_state_count = 0;
            const auto round_trip_start = Clock::now();
            d2h(
                &next_state_count,
                output_offsets.get() + states,
                sizeof(next_state_count));
            const std::uint64_t round_trip_nanoseconds =
                elapsedNanoseconds(round_trip_start);
            ++summary.metrics.depth_host_round_trips;
            summary.metrics
                .depth_host_round_trip_nanoseconds +=
                round_trip_nanoseconds;
            summary.metrics.depth_count_d2h_bytes +=
                sizeof(next_state_count);

            if (next_state_count >
                static_cast<DeviceOffset>(
                    std::numeric_limits<std::size_t>::max() /
                    query_vertex_count)) {
                throw std::overflow_error(
                    "GPU output frontier element count overflow");
            }
            const std::size_t next_elements =
                static_cast<std::size_t>(
                    next_state_count) *
                query_vertex_count;
            const std::size_t next_bytes =
                checkedBytes(next_elements, sizeof(int));
            if (next_bytes >
                    options_.frontier_budget_bytes -
                        pre_output_bytes) {
                throw std::runtime_error(
                    "GPU output frontier exceeds --gpu-memory-mb; "
                    "increase the budget or use the CPU backend. "
                    "Implicit CPU fallback is disabled");
            }

            const auto frontier_allocation_start =
                Clock::now();
            DeviceBuffer<int> next_mappings(next_elements);
            summary.metrics.device_allocation_nanoseconds +=
                elapsedNanoseconds(
                    frontier_allocation_start);
            std::uint64_t materialize_device_ns = 0;
            if (next_state_count != 0) {
                materialize_device_ns =
                    timeDeviceSequence([&]() {
                    hipLaunchKernelGGL(
                        materializeExtensionsKernel,
                        dim3(blocks),
                        dim3(kBlockSize),
                        0,
                        0,
                        device_graph,
                        device_query,
                        device_relations,
                        current_mappings.get(),
                        state_count,
                        next_query_vertex,
                        parent_query_vertex,
                        minimum_duration_,
                        options_.early_prune,
                        final_depth,
                        output_offsets.get(),
                        next_mappings.get(),
                        d_mismatch.get());
                    checkHip(
                        hipGetLastError(),
                        "materializeExtensionsKernel launch");
                    ++summary.metrics.kernel_launches;
                });
            }
            depth_metrics.output_states =
                next_state_count;
            depth_metrics.kernel_nanoseconds =
                count_device_ns +
                materialize_device_ns;
            depth_metrics.host_round_trip_nanoseconds =
                round_trip_nanoseconds;
            summary.metrics.depths.push_back(
                depth_metrics);

            summary.metrics.peak_frontier_device_bytes =
                std::max(
                    summary.metrics.peak_frontier_device_bytes,
                    pre_output_bytes + next_bytes);
            summary.metrics.peak_frontier_states =
                std::max<std::size_t>(
                    summary.metrics.peak_frontier_states,
                    static_cast<std::size_t>(
                        next_state_count));
            current_mappings = std::move(next_mappings);
            state_count = next_state_count;
        }

        int mismatch = 0;
        d2h(
            &mismatch,
            d_mismatch.get(),
            sizeof(mismatch));
        if (mismatch != 0) {
            throw std::runtime_error(
                "GPU count/materialization passes disagreed");
        }
        if (options_.collect_statistics) {
            std::vector<DeviceDepthCounters>
                level_depth_counters(
                    plan_.order.size());
            d2h(
                level_depth_counters.data(),
                d_level_depth_counters.get(),
                d_level_depth_counters.bytes());
            for (GpuDepthMetrics& metrics :
                 summary.metrics.depths) {
                if (metrics.depth >=
                    level_depth_counters.size()) {
                    continue;
                }
                metrics.generated_candidates =
                    level_depth_counters[
                        static_cast<std::size_t>(
                            metrics.depth)]
                        .generated_candidates;
                metrics
                    .maximum_candidates_per_state =
                    level_depth_counters[
                        static_cast<std::size_t>(
                            metrics.depth)]
                        .maximum_candidates_per_state;
            }
        }
        final_mappings = current_mappings.get();
        }

        summary.match_count =
            static_cast<std::uint64_t>(state_count);
        summary.count_overflow = false;

        if (options_.result_digest) {
            const auto digest_allocation_start =
                Clock::now();
            DeviceBuffer<DeviceOffset> d_xor(1);
            DeviceBuffer<DeviceOffset> d_sum(1);
            summary.metrics.device_allocation_nanoseconds +=
                elapsedNanoseconds(
                    digest_allocation_start);
            checkHip(
                hipMemset(d_xor.get(), 0, d_xor.bytes()),
                "hipMemset digest xor");
            checkHip(
                hipMemset(d_sum.get(), 0, d_sum.bytes()),
                "hipMemset digest sum");
            if (state_count != 0) {
                const DeviceOffset needed_blocks =
                    (state_count +
                     static_cast<DeviceOffset>(kBlockSize) - 1ULL) /
                    static_cast<DeviceOffset>(kBlockSize);
                const unsigned int blocks =
                    gridBlocks(needed_blocks);
                timeDeviceSequence([&]() {
                    hipLaunchKernelGGL(
                        digestMappingsKernel,
                        dim3(blocks),
                        dim3(kBlockSize),
                        0,
                        0,
                        final_mappings,
                        state_count,
                        query_graph_.num_vertices,
                        d_external_ids.get(),
                        d_xor.get(),
                        d_sum.get());
                    checkHip(
                        hipGetLastError(),
                        "digestMappingsKernel launch");
                    ++summary.metrics.kernel_launches;
                });
            }
            DeviceOffset xor_value = 0;
            DeviceOffset sum_value = 0;
            d2h(
                &xor_value,
                d_xor.get(),
                sizeof(xor_value));
            d2h(
                &sum_value,
                d_sum.get(),
                sizeof(sum_value));
            summary.result_digest =
                formatDigest(xor_value, sum_value);
        }

        if (options_.collect_statistics) {
            DeviceCounters counters{};
            d2h(
                &counters,
                d_counters.get(),
                sizeof(counters));
            summary.metrics.generated_candidates =
                counters.generated_candidates;
            summary.metrics.accepted_candidates =
                counters.accepted_candidates;
            summary.metrics.pruned_injectivity =
                counters.pruned_injectivity;
            summary.metrics.pruned_structural =
                counters.pruned_structural;
            summary.metrics.pruned_temporal =
                counters.pruned_temporal;
            summary.metrics.temporal_interval_steps =
                counters.temporal_interval_steps;
            summary.metrics.directed_edge_lookups =
                counters.directed_edge_lookups;
        }
    } catch (...) {
        (void)hipEventDestroy(event_stop);
        (void)hipEventDestroy(event_start);
        throw;
    }

    const std::uint64_t search_to_result_nanoseconds =
        elapsedNanoseconds(gpu_search_start);
    summary.metrics.gpu_search_nanoseconds =
        search_to_result_nanoseconds >=
                summary.metrics.d2h_nanoseconds
        ? search_to_result_nanoseconds -
              summary.metrics.d2h_nanoseconds
        : search_to_result_nanoseconds;
    summary.metrics.gpu_ready_to_result_nanoseconds =
        elapsedNanoseconds(gpu_ready_start);
    checkHip(
        hipEventDestroy(event_stop), "hipEventDestroy stop");
    checkHip(
        hipEventDestroy(event_start), "hipEventDestroy start");
    return summary;
}
