#ifndef UTILS_H
#define UTILS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using Label = std::uint8_t;

constexpr std::size_t kLabelCount = 5;
constexpr Label kInvalidLabel = static_cast<Label>(kLabelCount);
constexpr std::uint32_t kDefaultLabelSeed = 42U;

Label labelFromString(const std::string& value);
std::string labelToString(Label label);

struct TimeInterval {
    int start = 0;
    int end = -1;

    int length() const {
        return end >= start ? end - start + 1 : 0;
    }
};

struct TemporalEdge {
    int u = -1;
    int v = -1;
    std::vector<TimeInterval> active_intervals;
    int active_snapshot_count = 0;
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

// Intersect two sorted, disjoint interval lists. Intervals shorter than
// minimum_length are discarded because later intersections cannot lengthen them.
std::vector<TimeInterval> intersectTimeIntervals(
    const std::vector<TimeInterval>& lhs,
    const std::vector<TimeInterval>& rhs,
    int minimum_length = 1);

bool hasMinimumConsecutiveDuration(
    const std::vector<TimeInterval>& intervals,
    int minimum_duration);

std::string formatIntervals(const std::vector<TimeInterval>& intervals);

namespace GraphUtils {
bool hasEdge(const std::vector<std::vector<Edge>>& adj, int u, int v);
}

struct TemporalGraphLoadTimings {
    long long read_milliseconds = 0;
    // Sorting, deduplication, Algorithm 3.1 filtering, random labels, compact
    // graph construction, and directed vertex statistics after file parsing.
    long long filter_milliseconds = 0;
};

// Temporal and query inputs are directed. Parallel timestamps for the same
// ordered pair are merged, external IDs are compacted, and Algorithm 3.1's
// consecutive-pair filter is applied. Label-free data vertices receive a
// reproducible random A-E label before temporal filtering.
bool readTemporalGraph(
    const std::string& filename,
    Graph& graph,
    std::uint32_t label_seed = kDefaultLabelSeed,
    TemporalGraphLoadTimings* load_timings = nullptr);
bool readQueryGraph(const std::string& filename, Graph& query_graph);

#endif // UTILS_H
