#ifndef UTILS_H
#define UTILS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <limits>
#include <string>
#include <vector>

using Label = std::uint8_t;

constexpr std::size_t kLabelCount = 5;
constexpr Label kInvalidLabel = static_cast<Label>(kLabelCount);
constexpr std::uint32_t kDefaultLabelSeed = 42U;

enum class LabelMode {
    SequentialRng,
    StableId,
    ExplicitProjection
};

const char* labelModeName(LabelMode mode);
Label stableIdLabel(int external_id, std::uint32_t label_seed);

Label labelFromString(const std::string& value);
std::string labelToString(Label label);

struct TimeInterval {
    int start = 0;
    int end = -1;

    int length() const {
        if (end < start) return 0;
        const long long span =
            static_cast<long long>(end) - static_cast<long long>(start) + 1;
        return span > std::numeric_limits<int>::max()
            ? std::numeric_limits<int>::max()
            : static_cast<int>(span);
    }
};

struct TemporalEdge {
    int u = -1;
    int v = -1;
    std::int64_t active_snapshot_count = 0;

    TemporalEdge() = default;
    TemporalEdge(
        int source,
        int target,
        std::vector<TimeInterval> intervals,
        std::int64_t snapshot_count);

    const std::vector<TimeInterval>& activeIntervals() const noexcept;
    int maximumConsecutiveRun() const;
    bool hasCachedMaximumConsecutiveRun() const noexcept;

private:
    std::vector<TimeInterval> active_intervals_;
    int cached_max_consecutive_run_ = 0;
};

struct Edge {
    int to = -1;
    int temporal_edge_id = -1;
};

struct Graph {
    int num_vertices = 0;
    // adj stores outgoing edges. in_adj stores incoming edges and uses Edge::to
    // for the source vertex of each incoming edge.
    std::vector<std::vector<Edge>> adj;
    std::vector<std::vector<Edge>> in_adj;
    std::vector<Label> vertex_labels;

    // Data graphs are remapped to compact internal IDs. Query graphs leave this
    // as 0..num_vertices-1.
    std::vector<int> external_ids;

    // Populated only for temporal data graphs.
    std::vector<TemporalEdge> temporal_edges;
    std::vector<int> vertex_active_durations;
    // Outgoing and incoming neighbor-label multiplicities are kept separately
    // so directed degree/NLF filtering cannot accept a reversed edge.
    std::vector<std::array<int, kLabelCount>> neighbor_label_counts;
    std::vector<std::array<int, kLabelCount>> incoming_neighbor_label_counts;

    std::size_t input_occurrence_count = 0;
    std::size_t input_unique_edge_count = 0;
    std::size_t filtered_edge_count = 0;

    const TemporalEdge* findTemporalEdge(int u, int v) const;
    int externalId(int internal_id) const;
    std::size_t getMemoryUsage() const;
};

std::uint64_t temporalGraphContentFingerprint(const Graph& graph);

// Intersect two sorted, disjoint interval lists. Intervals shorter than
// minimum_length are discarded because later intersections cannot lengthen them.
std::vector<TimeInterval> intersectTimeIntervals(
    const std::vector<TimeInterval>& lhs,
    const std::vector<TimeInterval>& rhs,
    int minimum_length = 1);

bool hasMinimumConsecutiveDuration(
    const std::vector<TimeInterval>& intervals,
    int minimum_duration);

int maximumConsecutiveRun(
    const std::vector<TimeInterval>& intervals);

bool hasMinimumConsecutiveDuration(
    const TemporalEdge& edge,
    int minimum_duration);

std::string formatIntervals(const std::vector<TimeInterval>& intervals);

namespace GraphUtils {
bool hasEdge(const std::vector<std::vector<Edge>>& adj, int u, int v);
}

struct TemporalGraphLoadTimings {
    long long read_milliseconds = 0;
    // Sorting, deduplication, Algorithm 3.1 filtering, label assignment, compact
    // graph construction, and directed vertex statistics after file parsing.
    long long filter_milliseconds = 0;
    // Exact finalized capacity sum accumulated in the existing load loops.
    // This receipt belongs to this load, not to a copyable/mutable Graph cache.
    std::optional<std::uint64_t> adjacency_temporal_capacity_bytes;
};

struct ExplicitRuntimeLabelMap;

// Temporal and query inputs are directed. Parallel timestamps for the same
// ordered pair are merged, external IDs are compacted, and Algorithm 3.1's
// consecutive-pair filter is applied. Label-free data vertices receive a
// reproducible A-E label before temporal filtering.
#if !defined(DSM_E3E4_CORE_ONLY)
bool readTemporalGraph(
    const std::string& filename,
    Graph& graph,
    std::uint32_t label_seed = kDefaultLabelSeed,
    TemporalGraphLoadTimings* load_timings = nullptr,
    LabelMode label_mode = LabelMode::SequentialRng,
    const ExplicitRuntimeLabelMap* explicit_label_map = nullptr,
    int filter_minimum_run = 2);
bool readQueryGraph(const std::string& filename, Graph& query_graph);
#endif

#endif // UTILS_H
