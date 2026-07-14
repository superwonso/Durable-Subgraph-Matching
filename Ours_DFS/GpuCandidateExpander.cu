#include "GpuCandidateExpander.h"

#ifdef BUILD_WITH_CUDA

#include <cuda_runtime.h>

#include <vector>

namespace {

constexpr int kThreadsPerBlock = 256;

__global__ void filterRootCandidatesKernel(
    const int* vertex_label_ids,
    const int* vertex_degrees,
    const int* vertex_duration_counts,
    const int* vertex_distinct_neighbor_label_counts,
    int num_vertices,
    int query_label_id,
    int required_degree,
    int k_threshold,
    int* flags) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= num_vertices) {
        return;
    }

    const bool matches =
        vertex_label_ids[idx] == query_label_id &&
        vertex_degrees[idx] >= required_degree &&
        vertex_distinct_neighbor_label_counts[idx] >= required_degree &&
        vertex_duration_counts[idx] >= k_threshold;

    flags[idx] = matches ? 1 : 0;
}

__global__ void filterChildCandidatesKernel(
    const int* adjacency_offsets,
    const int* adjacency_neighbors,
    const int* vertex_label_ids,
    const int* vertex_degrees,
    const int* vertex_duration_counts,
    const int* vertex_distinct_neighbor_label_counts,
    int parent_vertex,
    int query_label_id,
    int required_degree,
    int k_threshold,
    int* flags) {
    const int start = adjacency_offsets[parent_vertex];
    const int end = adjacency_offsets[parent_vertex + 1];
    const int degree = end - start;
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= degree) {
        return;
    }

    const int cand = adjacency_neighbors[start + idx];
    const bool matches =
        vertex_label_ids[cand] == query_label_id &&
        vertex_degrees[cand] >= required_degree &&
        vertex_distinct_neighbor_label_counts[cand] >= required_degree &&
        vertex_duration_counts[cand] >= k_threshold;

    flags[idx] = matches ? cand : -1;
}

template <typename T>
bool copyVectorToDevice(const std::vector<T>& host, T** device_ptr) {
    if (host.empty()) {
        *device_ptr = nullptr;
        return true;
    }

    const size_t bytes = sizeof(T) * host.size();
    if (cudaMalloc(reinterpret_cast<void**>(device_ptr), bytes) != cudaSuccess) {
        *device_ptr = nullptr;
        return false;
    }
    if (cudaMemcpy(*device_ptr, host.data(), bytes, cudaMemcpyHostToDevice) != cudaSuccess) {
        cudaFree(*device_ptr);
        *device_ptr = nullptr;
        return false;
    }
    return true;
}

template <typename T>
void safeCudaFree(T* ptr) {
    if (ptr != nullptr) {
        cudaFree(ptr);
    }
}

} // namespace

namespace cuda_backend {

bool isRuntimeAvailable() {
    int device_count = 0;
    const cudaError_t err = cudaGetDeviceCount(&device_count);
    return err == cudaSuccess && device_count > 0;
}

std::vector<int> filterRootCandidates(
    const std::vector<int>& vertex_label_ids,
    const std::vector<int>& vertex_degrees,
    const std::vector<int>& vertex_duration_counts,
    const std::vector<int>& vertex_distinct_neighbor_label_counts,
    int query_label_id,
    int required_degree,
    int k_threshold) {
    std::vector<int> result;
    const int num_vertices = static_cast<int>(vertex_label_ids.size());
    if (num_vertices == 0) {
        return result;
    }

    int* d_vertex_label_ids = nullptr;
    int* d_vertex_degrees = nullptr;
    int* d_vertex_duration_counts = nullptr;
    int* d_vertex_distinct_neighbor_label_counts = nullptr;
    int* d_flags = nullptr;

    if (!copyVectorToDevice(vertex_label_ids, &d_vertex_label_ids) ||
        !copyVectorToDevice(vertex_degrees, &d_vertex_degrees) ||
        !copyVectorToDevice(vertex_duration_counts, &d_vertex_duration_counts) ||
        !copyVectorToDevice(vertex_distinct_neighbor_label_counts, &d_vertex_distinct_neighbor_label_counts) ||
        cudaMalloc(reinterpret_cast<void**>(&d_flags), sizeof(int) * num_vertices) != cudaSuccess) {
        safeCudaFree(d_vertex_label_ids);
        safeCudaFree(d_vertex_degrees);
        safeCudaFree(d_vertex_duration_counts);
        safeCudaFree(d_vertex_distinct_neighbor_label_counts);
        safeCudaFree(d_flags);
        return result;
    }

    const int blocks = (num_vertices + kThreadsPerBlock - 1) / kThreadsPerBlock;
    filterRootCandidatesKernel<<<blocks, kThreadsPerBlock>>>(
        d_vertex_label_ids,
        d_vertex_degrees,
        d_vertex_duration_counts,
        d_vertex_distinct_neighbor_label_counts,
        num_vertices,
        query_label_id,
        required_degree,
        k_threshold,
        d_flags);

    std::vector<int> flags(num_vertices, 0);
    if (cudaDeviceSynchronize() == cudaSuccess &&
        cudaMemcpy(flags.data(), d_flags, sizeof(int) * num_vertices, cudaMemcpyDeviceToHost) == cudaSuccess) {
        for (int i = 0; i < num_vertices; ++i) {
            if (flags[i] == 1) {
                result.push_back(i);
            }
        }
    }

    safeCudaFree(d_vertex_label_ids);
    safeCudaFree(d_vertex_degrees);
    safeCudaFree(d_vertex_duration_counts);
    safeCudaFree(d_vertex_distinct_neighbor_label_counts);
    safeCudaFree(d_flags);

    return result;
}

std::vector<int> filterChildCandidates(
    const std::vector<int>& adjacency_offsets,
    const std::vector<int>& adjacency_neighbors,
    const std::vector<int>& vertex_label_ids,
    const std::vector<int>& vertex_degrees,
    const std::vector<int>& vertex_duration_counts,
    const std::vector<int>& vertex_distinct_neighbor_label_counts,
    int parent_vertex,
    int query_label_id,
    int required_degree,
    int k_threshold) {
    std::vector<int> result;
    if (parent_vertex < 0 || parent_vertex + 1 >= static_cast<int>(adjacency_offsets.size())) {
        return result;
    }

    const int start = adjacency_offsets[parent_vertex];
    const int end = adjacency_offsets[parent_vertex + 1];
    const int degree = end - start;
    if (degree <= 0) {
        return result;
    }

    int* d_adjacency_offsets = nullptr;
    int* d_adjacency_neighbors = nullptr;
    int* d_vertex_label_ids = nullptr;
    int* d_vertex_degrees = nullptr;
    int* d_vertex_duration_counts = nullptr;
    int* d_vertex_distinct_neighbor_label_counts = nullptr;
    int* d_flags = nullptr;

    if (!copyVectorToDevice(adjacency_offsets, &d_adjacency_offsets) ||
        !copyVectorToDevice(adjacency_neighbors, &d_adjacency_neighbors) ||
        !copyVectorToDevice(vertex_label_ids, &d_vertex_label_ids) ||
        !copyVectorToDevice(vertex_degrees, &d_vertex_degrees) ||
        !copyVectorToDevice(vertex_duration_counts, &d_vertex_duration_counts) ||
        !copyVectorToDevice(vertex_distinct_neighbor_label_counts, &d_vertex_distinct_neighbor_label_counts) ||
        cudaMalloc(reinterpret_cast<void**>(&d_flags), sizeof(int) * degree) != cudaSuccess) {
        safeCudaFree(d_adjacency_offsets);
        safeCudaFree(d_adjacency_neighbors);
        safeCudaFree(d_vertex_label_ids);
        safeCudaFree(d_vertex_degrees);
        safeCudaFree(d_vertex_duration_counts);
        safeCudaFree(d_vertex_distinct_neighbor_label_counts);
        safeCudaFree(d_flags);
        return result;
    }

    const int blocks = (degree + kThreadsPerBlock - 1) / kThreadsPerBlock;
    filterChildCandidatesKernel<<<blocks, kThreadsPerBlock>>>(
        d_adjacency_offsets,
        d_adjacency_neighbors,
        d_vertex_label_ids,
        d_vertex_degrees,
        d_vertex_duration_counts,
        d_vertex_distinct_neighbor_label_counts,
        parent_vertex,
        query_label_id,
        required_degree,
        k_threshold,
        d_flags);

    std::vector<int> flags(degree, -1);
    if (cudaDeviceSynchronize() == cudaSuccess &&
        cudaMemcpy(flags.data(), d_flags, sizeof(int) * degree, cudaMemcpyDeviceToHost) == cudaSuccess) {
        for (const int cand : flags) {
            if (cand >= 0) {
                result.push_back(cand);
            }
        }
    }

    safeCudaFree(d_adjacency_offsets);
    safeCudaFree(d_adjacency_neighbors);
    safeCudaFree(d_vertex_label_ids);
    safeCudaFree(d_vertex_degrees);
    safeCudaFree(d_vertex_duration_counts);
    safeCudaFree(d_vertex_distinct_neighbor_label_counts);
    safeCudaFree(d_flags);

    return result;
}

} // namespace cuda_backend

#endif
