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

enum class LabelMode {
    SequentialRng,
    StableId
};

const char* labelModeName(LabelMode mode);
Label stableIdLabel(int external_id, std::uint32_t label_seed);

enum class DurationMode {
    // Paper-faithful semantics: a matching is k-durable when it occurs at
    // at least k (not necessarily consecutive) time instances.
    Cardinality,
    // Compatibility mode for comparing against methods that require one
    // consecutive run of at least k snapshots.
    Consecutive
};

const char* durationModeName(DurationMode mode);

Label labelFromString(const std::string& value);
std::string labelToString(Label label);

struct TimeInterval {
    int start = 0;
    int end = -1;

    std::int64_t length() const {
        return end >= start
            ? static_cast<std::int64_t>(end) - static_cast<std::int64_t>(start) + 1
            : 0;
    }
};

struct TemporalEdge {
    int u = -1;
    int v = -1;
    std::vector<TimeInterval> active_intervals;
    std::int64_t active_snapshot_count = 0;
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
    std::vector<std::int64_t> vertex_active_durations;
    // Outgoing and incoming neighbor-label multiplicities are kept separately;
    // TD-tree growth derives the paper's distinct-neighbor-label condition
    // from their union while exact enumeration preserves arc direction.
    std::vector<std::array<int, kLabelCount>> neighbor_label_counts;
    std::vector<std::array<int, kLabelCount>> incoming_neighbor_label_counts;

    std::size_t input_occurrence_count = 0;
    std::size_t input_unique_edge_count = 0;
    std::size_t retained_edge_count = 0;

    const TemporalEdge* findTemporalEdge(int u, int v) const;
    int externalId(int internal_id) const;
    std::size_t getMemoryUsage() const;
};

// Intersect two sorted, disjoint interval lists without discarding short runs.
// Under the paper's cardinality semantics, several short runs can jointly
// satisfy k, so pruning each run independently would be incorrect.
std::vector<TimeInterval> intersectTimeIntervals(
    const std::vector<TimeInterval>& lhs,
    const std::vector<TimeInterval>& rhs);

std::vector<TimeInterval> unionTimeIntervals(
    const std::vector<TimeInterval>& lhs,
    const std::vector<TimeInterval>& rhs);

// Return lhs \ rhs for sorted, disjoint interval lists.
std::vector<TimeInterval> subtractTimeIntervals(
    const std::vector<TimeInterval>& lhs,
    const std::vector<TimeInterval>& rhs);

std::int64_t timeInstanceCardinality(
    const std::vector<TimeInterval>& intervals);

bool hasMinimumDuration(
    const std::vector<TimeInterval>& intervals,
    int minimum_duration,
    DurationMode mode = DurationMode::Cardinality);

std::string formatIntervals(const std::vector<TimeInterval>& intervals);

namespace GraphUtils {
bool hasEdge(const std::vector<std::vector<Edge>>& adj, int u, int v);
}

struct TemporalGraphLoadTimings {
    long long read_milliseconds = 0;
    // Sorting, timestamp deduplication, interval compression, seeded label
    // assignment, compact graph construction, and directed vertex statistics.
    // Original deliberately does not apply Algorithm 3.1's edge filter.
    long long preprocess_milliseconds = 0;
};

// Temporal and query inputs are directed. Parallel timestamps for the same
// ordered pair are merged and external IDs are compacted. Every ordered edge
// history is retained; unlike Ours, Original has no Algorithm 3.1 prefilter.
// Label-free data vertices receive a reproducible A-E label. SequentialRng
// preserves the legacy sorted-ID RNG stream; StableId is vertex-subset
// invariant.
bool readTemporalGraph(
    const std::string& filename,
    Graph& graph,
    std::uint32_t label_seed = kDefaultLabelSeed,
    TemporalGraphLoadTimings* load_timings = nullptr,
    LabelMode label_mode = LabelMode::SequentialRng);
bool readQueryGraph(const std::string& filename, Graph& query_graph);

#endif // UTILS_H
