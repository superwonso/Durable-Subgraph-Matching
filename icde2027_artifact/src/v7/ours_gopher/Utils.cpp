#include "Utils.h"

#include "LabelProjection.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#if !defined(DSM_E3E4_CORE_ONLY)
namespace {

struct RawTemporalEdge {
    int u;
    int v;
    int timestamp;
};

struct FilteredExternalEdge {
    int u;
    int v;
    std::vector<TimeInterval> active_intervals;
    std::int64_t active_snapshot_count;
};

bool rawEdgeLess(const RawTemporalEdge& lhs, const RawTemporalEdge& rhs) {
    if (lhs.u != rhs.u) return lhs.u < rhs.u;
    if (lhs.v != rhs.v) return lhs.v < rhs.v;
    return lhs.timestamp < rhs.timestamp;
}

enum class TemporalLineStatus {
    empty,
    valid,
    invalid
};

TemporalLineStatus parseTemporalLine(
    const std::string& line,
    int& u,
    int& v,
    int& timestamp) {
    const char* cursor = line.data();
    const char* const end = cursor + line.size();
    auto skip_whitespace = [&]() {
        while (cursor != end &&
               std::isspace(static_cast<unsigned char>(*cursor)) != 0) {
            ++cursor;
        }
    };
    auto parse_integer = [&](int& value) {
        skip_whitespace();
        if (cursor == end || *cursor == '#') return false;
        const auto parsed = std::from_chars(cursor, end, value);
        if (parsed.ec != std::errc{} || parsed.ptr == cursor) return false;
        cursor = parsed.ptr;
        return cursor == end || *cursor == '#' ||
            std::isspace(static_cast<unsigned char>(*cursor)) != 0;
    };

    skip_whitespace();
    if (cursor == end || *cursor == '#') return TemporalLineStatus::empty;
    if (!parse_integer(u) || !parse_integer(v) || !parse_integer(timestamp)) {
        return TemporalLineStatus::invalid;
    }
    skip_whitespace();
    return cursor == end || *cursor == '#'
        ? TemporalLineStatus::valid
        : TemporalLineStatus::invalid;
}

int compactId(const std::vector<int>& external_ids, int external_id) {
    const auto it = std::lower_bound(external_ids.begin(), external_ids.end(), external_id);
    if (it == external_ids.end() || *it != external_id) return -1;
    return static_cast<int>(it - external_ids.begin());
}

int unionDuration(std::vector<TimeInterval>& intervals) {
    if (intervals.empty()) return 0;

    std::sort(intervals.begin(), intervals.end(), [](const TimeInterval& lhs, const TimeInterval& rhs) {
        if (lhs.start != rhs.start) return lhs.start < rhs.start;
        return lhs.end < rhs.end;
    });

    long long total = 0;
    int current_start = intervals.front().start;
    int current_end = intervals.front().end;
    for (std::size_t i = 1; i < intervals.size(); ++i) {
        const auto& interval = intervals[i];
        if (interval.start <= current_end ||
            (current_end != std::numeric_limits<int>::max() && interval.start == current_end + 1)) {
            current_end = std::max(current_end, interval.end);
        } else {
            total += static_cast<long long>(current_end) - current_start + 1;
            current_start = interval.start;
            current_end = interval.end;
        }
    }
    total += static_cast<long long>(current_end) - current_start + 1;

    return total > std::numeric_limits<int>::max()
        ? std::numeric_limits<int>::max()
        : static_cast<int>(total);
}

} // namespace
#endif

const char* labelModeName(LabelMode mode) {
    switch (mode) {
        case LabelMode::SequentialRng: return "sequential-rng";
        case LabelMode::StableId: return "stable-id";
        case LabelMode::ExplicitProjection: return "explicit-projection";
    }
    return "unknown";
}

Label stableIdLabel(int external_id, std::uint32_t label_seed) {
    if (external_id < 0) return kInvalidLabel;
    std::uint64_t mixed =
        (static_cast<std::uint64_t>(label_seed) << 32U) |
        static_cast<std::uint32_t>(external_id);
    mixed += UINT64_C(0x9e3779b97f4a7c15);
    mixed =
        (mixed ^ (mixed >> 30U)) *
        UINT64_C(0xbf58476d1ce4e5b9);
    mixed =
        (mixed ^ (mixed >> 27U)) *
        UINT64_C(0x94d049bb133111eb);
    mixed ^= mixed >> 31U;
    return static_cast<Label>(
        mixed % static_cast<std::uint64_t>(kLabelCount));
}

Label labelFromString(const std::string& value) {
    if (value.size() != 1 || value[0] < 'A' || value[0] > 'E') {
        return kInvalidLabel;
    }
    return static_cast<Label>(value[0] - 'A');
}

std::string labelToString(Label label) {
    if (label >= kLabelCount) return "?";
    return std::string(1, static_cast<char>('A' + label));
}

const std::vector<TimeInterval>&
TemporalEdge::activeIntervals() const noexcept {
    return active_intervals_;
}

const TemporalEdge* Graph::findTemporalEdge(int u, int v) const {
    if (u < 0 || v < 0 || u >= num_vertices || v >= num_vertices) return nullptr;
    const auto& neighbors = adj[static_cast<std::size_t>(u)];
    const auto it = std::lower_bound(
        neighbors.begin(), neighbors.end(), v,
        [](const Edge& edge, int target) { return edge.to < target; });
    if (it == neighbors.end() || it->to != v || it->temporal_edge_id < 0) return nullptr;
    const std::size_t edge_id = static_cast<std::size_t>(it->temporal_edge_id);
    return edge_id < temporal_edges.size() ? &temporal_edges[edge_id] : nullptr;
}

int Graph::externalId(int internal_id) const {
    if (internal_id < 0 || static_cast<std::size_t>(internal_id) >= external_ids.size()) {
        return internal_id;
    }
    return external_ids[static_cast<std::size_t>(internal_id)];
}

std::size_t Graph::getMemoryUsage() const {
    std::size_t total = sizeof(Graph);
    total += adj.capacity() * sizeof(std::vector<Edge>);
    for (const auto& neighbors : adj) total += neighbors.capacity() * sizeof(Edge);
    total += in_adj.capacity() * sizeof(std::vector<Edge>);
    for (const auto& neighbors : in_adj) total += neighbors.capacity() * sizeof(Edge);
    total += vertex_labels.capacity() * sizeof(Label);
    total += external_ids.capacity() * sizeof(int);
    total += vertex_active_durations.capacity() * sizeof(int);
    total += neighbor_label_counts.capacity() * sizeof(std::array<int, kLabelCount>);
    total += incoming_neighbor_label_counts.capacity() *
        sizeof(std::array<int, kLabelCount>);
    total += temporal_edges.capacity() * sizeof(TemporalEdge);
    for (const auto& edge : temporal_edges) {
        total += edge.activeIntervals().capacity() * sizeof(TimeInterval);
    }
    return total;
}

std::uint64_t temporalGraphContentFingerprint(const Graph& graph) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    auto append_u64 = [&hash](std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            hash ^= (value >> shift) & UINT64_C(0xff);
            hash *= UINT64_C(1099511628211);
        }
    };
    append_u64(UINT64_C(0x44534d2d47524150));
    append_u64(static_cast<std::uint64_t>(graph.external_ids.size()));
    for (std::size_t vertex = 0; vertex < graph.external_ids.size(); ++vertex) {
        append_u64(static_cast<std::uint32_t>(graph.external_ids[vertex]));
        append_u64(
            vertex < graph.vertex_labels.size()
            ? static_cast<std::uint64_t>(graph.vertex_labels[vertex])
            : static_cast<std::uint64_t>(kInvalidLabel));
    }
    append_u64(static_cast<std::uint64_t>(graph.temporal_edges.size()));
    for (const TemporalEdge& edge : graph.temporal_edges) {
        append_u64(static_cast<std::uint32_t>(graph.externalId(edge.u)));
        append_u64(static_cast<std::uint32_t>(graph.externalId(edge.v)));
        append_u64(static_cast<std::uint64_t>(edge.active_snapshot_count));
        append_u64(static_cast<std::uint64_t>(edge.activeIntervals().size()));
        for (const TimeInterval& interval : edge.activeIntervals()) {
            append_u64(static_cast<std::uint32_t>(interval.start));
            append_u64(static_cast<std::uint32_t>(interval.end));
        }
    }
    return hash;
}

std::vector<TimeInterval> intersectTimeIntervals(
    const std::vector<TimeInterval>& lhs,
    const std::vector<TimeInterval>& rhs,
    int minimum_length) {
    const int required = std::max(1, minimum_length);
    std::vector<TimeInterval> result;
    result.reserve(std::min(lhs.size(), rhs.size()));

    std::size_t i = 0;
    std::size_t j = 0;
    while (i < lhs.size() && j < rhs.size()) {
        const int start = std::max(lhs[i].start, rhs[j].start);
        const int end = std::min(lhs[i].end, rhs[j].end);
        const long long intersection_length =
            static_cast<long long>(end) - static_cast<long long>(start) + 1;
        if (end >= start && intersection_length >= required) {
            result.push_back({start, end});
        }

        if (lhs[i].end < rhs[j].end) {
            ++i;
        } else {
            ++j;
        }
    }
    return result;
}

bool hasMinimumConsecutiveDuration(
    const std::vector<TimeInterval>& intervals,
    int minimum_duration) {
    if (minimum_duration <= 0) return true;
    return maximumConsecutiveRun(intervals) >= minimum_duration;
}

int maximumConsecutiveRun(
    const std::vector<TimeInterval>& intervals) {
    int maximum_run = 0;
    for (const TimeInterval& interval : intervals) {
        maximum_run = std::max(maximum_run, interval.length());
    }
    return maximum_run;
}

TemporalEdge::TemporalEdge(
    int source,
    int target,
    std::vector<TimeInterval> intervals,
    std::int64_t snapshot_count)
    : u(source),
      v(target),
      active_snapshot_count(snapshot_count),
      active_intervals_(std::move(intervals)) {
    if (snapshot_count < 0) {
        throw std::invalid_argument(
            "temporal edge snapshot count cannot be negative");
    }

    std::int64_t exact_snapshot_count = 0;
    int previous_end = 0;
    bool have_previous = false;
    for (const TimeInterval& interval : active_intervals_) {
        if (interval.end < interval.start) {
            throw std::invalid_argument(
                "temporal edge interval has negative length");
        }
        if (have_previous &&
            (interval.start <= previous_end ||
             (previous_end != std::numeric_limits<int>::max() &&
              interval.start == previous_end + 1))) {
            throw std::invalid_argument(
                "temporal edge intervals must be sorted, disjoint, "
                "and maximally coalesced");
        }
        const std::int64_t span =
            static_cast<std::int64_t>(interval.end) -
            static_cast<std::int64_t>(interval.start) + 1;
        if (span > std::numeric_limits<std::int64_t>::max() -
                       exact_snapshot_count) {
            throw std::overflow_error(
                "temporal edge snapshot count overflow");
        }
        exact_snapshot_count += span;
        cached_max_consecutive_run_ = std::max(
            cached_max_consecutive_run_, interval.length());
        previous_end = interval.end;
        have_previous = true;
    }
    if (exact_snapshot_count != active_snapshot_count) {
        throw std::invalid_argument(
            "temporal edge snapshot count does not match intervals");
    }
}

int TemporalEdge::maximumConsecutiveRun() const {
    return cached_max_consecutive_run_;
}

bool TemporalEdge::hasCachedMaximumConsecutiveRun() const noexcept {
    return true;
}

bool hasMinimumConsecutiveDuration(
    const TemporalEdge& edge,
    int minimum_duration) {
    return minimum_duration <= 0 ||
        edge.maximumConsecutiveRun() >= minimum_duration;
}

std::string formatIntervals(const std::vector<TimeInterval>& intervals) {
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < intervals.size(); ++i) {
        if (i > 0) out << ", ";
        out << intervals[i].start;
        if (intervals[i].end != intervals[i].start) out << '-' << intervals[i].end;
    }
    out << ']';
    return out.str();
}

namespace GraphUtils {

bool hasEdge(const std::vector<std::vector<Edge>>& adj, int u, int v) {
    if (u < 0 || v < 0 || static_cast<std::size_t>(u) >= adj.size()) return false;
    const auto& neighbors = adj[static_cast<std::size_t>(u)];
    const auto it = std::lower_bound(
        neighbors.begin(), neighbors.end(), v,
        [](const Edge& edge, int target) { return edge.to < target; });
    return it != neighbors.end() && it->to == v;
}

} // namespace GraphUtils

#if !defined(DSM_E3E4_CORE_ONLY)
bool readTemporalGraph(
    const std::string& filename,
    Graph& graph,
    std::uint32_t label_seed,
    TemporalGraphLoadTimings* load_timings,
    LabelMode label_mode,
    const ExplicitRuntimeLabelMap* explicit_label_map,
    int filter_minimum_run) {
    if (load_timings != nullptr) *load_timings = {};
    const auto read_start = std::chrono::steady_clock::now();
    std::ifstream input(filename);
    if (!input.is_open()) {
        std::cerr << "Error: Cannot open temporal graph file " << filename << '\n';
        return false;
    }

    graph = Graph{};
    std::vector<RawTemporalEdge> raw_edges;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        int u = 0;
        int v = 0;
        int timestamp = 0;
        const TemporalLineStatus status = parseTemporalLine(line, u, v, timestamp);
        if (status == TemporalLineStatus::empty) continue;
        if (status == TemporalLineStatus::invalid) {
            std::cerr << "Error: Temporal graph line " << line_number
                      << " must contain exactly three integers.\n";
            return false;
        }
        ++graph.input_occurrence_count;
        if (u < 0 || v < 0) {
            std::cerr << "Error: Negative vertex ID on temporal graph line "
                      << line_number << ".\n";
            return false;
        }
        raw_edges.push_back({u, v, timestamp});
    }
    if (input.bad()) {
        std::cerr << "Error: Failed while reading temporal graph file " << filename << '\n';
        return false;
    }
    input.close();
    if (load_timings != nullptr) {
        load_timings->read_milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - read_start).count();
    }

    const auto filter_start = std::chrono::steady_clock::now();
    std::sort(raw_edges.begin(), raw_edges.end(), rawEdgeLess);
    std::vector<FilteredExternalEdge> filtered_edges;
    filtered_edges.reserve(std::min<std::size_t>(raw_edges.size() / 4 + 1, 4'000'000));
    std::vector<int> all_vertex_ids;
    all_vertex_ids.reserve(std::min<std::size_t>(raw_edges.size(), 8'000'000));

    std::size_t index = 0;
    while (index < raw_edges.size()) {
        const int edge_u = raw_edges[index].u;
        const int edge_v = raw_edges[index].v;
        ++graph.input_unique_edge_count;
        all_vertex_ids.push_back(edge_u);
        all_vertex_ids.push_back(edge_v);

        std::vector<TimeInterval> intervals;
        std::int64_t active_count = 0;
        bool has_consecutive_pair = false;
        bool have_timestamp = false;
        int run_start = 0;
        int previous = 0;

        while (index < raw_edges.size() &&
               raw_edges[index].u == edge_u && raw_edges[index].v == edge_v) {
            const int current = raw_edges[index].timestamp;
            ++index;
            if (have_timestamp && current == previous) continue;

            ++active_count;
            if (!have_timestamp) {
                run_start = current;
                previous = current;
                have_timestamp = true;
            } else if (previous != std::numeric_limits<int>::max() && current == previous + 1) {
                has_consecutive_pair = true;
                previous = current;
            } else {
                intervals.push_back({run_start, previous});
                run_start = previous = current;
            }
        }
        if (have_timestamp) intervals.push_back({run_start, previous});

        // Algorithm 3.1: retain the entire edge history iff at least one
        // back-to-back snapshot pair exists.
        // Optional P2 filter variant: Lmax >= k is necessary for every
        // edge of a durable match. Preserve full histories of surviving
        // edges. The default F2 branch is exactly the original predicate.
        if (filter_minimum_run == 2 ? has_consecutive_pair :
            maximumConsecutiveRun(intervals) >= filter_minimum_run) {
            filtered_edges.push_back({edge_u, edge_v, std::move(intervals), active_count});
        }
    }
    graph.filtered_edge_count = filtered_edges.size();

    std::vector<RawTemporalEdge>().swap(raw_edges);

    // The legacy mode assigns RNG labels to the complete raw vertex set before
    // filtering. Sorting preserves that stream under input-line permutations;
    // StableId additionally makes labels invariant to vertex-set subsets.
    std::sort(all_vertex_ids.begin(), all_vertex_ids.end());
    all_vertex_ids.erase(
        std::unique(all_vertex_ids.begin(), all_vertex_ids.end()),
        all_vertex_ids.end());
    std::vector<Label> all_vertex_labels(all_vertex_ids.size(), kInvalidLabel);
    if (label_mode == LabelMode::ExplicitProjection) {
        if (explicit_label_map == nullptr) {
            std::cerr
                << "Error: explicit-projection label mode requires a validated label map.\n";
            return false;
        }
        if (explicit_label_map->labels_by_external_id.size() !=
            all_vertex_ids.size()) {
            std::cerr
                << "Error: Explicit global label map does not cover the raw temporal "
                   "graph exactly (vertex count differs).\n";
            return false;
        }
        for (std::size_t i = 0; i < all_vertex_ids.size(); ++i) {
            const auto& label_row =
                explicit_label_map->labels_by_external_id[i];
            if (label_row.first != all_vertex_ids[i]) {
                std::cerr
                    << "Error: Explicit global label map does not cover the raw "
                       "temporal graph exactly (first differing vertex ID: graph="
                    << all_vertex_ids[i] << ", map=" << label_row.first << ").\n";
                return false;
            }
            all_vertex_labels[i] = label_row.second;
        }
    } else if (label_mode == LabelMode::StableId) {
        for (std::size_t i = 0; i < all_vertex_ids.size(); ++i) {
            all_vertex_labels[i] =
                stableIdLabel(all_vertex_ids[i], label_seed);
        }
    } else {
        std::mt19937 label_generator(label_seed);
        std::uniform_int_distribution<int> label_distribution(
            0, static_cast<int>(kLabelCount) - 1);
        for (auto& label : all_vertex_labels) {
            label =
                static_cast<Label>(
                    label_distribution(label_generator));
        }
    }

    graph.external_ids.reserve(filtered_edges.size() * 2);
    for (const auto& edge : filtered_edges) {
        graph.external_ids.push_back(edge.u);
        graph.external_ids.push_back(edge.v);
    }
    std::sort(graph.external_ids.begin(), graph.external_ids.end());
    graph.external_ids.erase(
        std::unique(graph.external_ids.begin(), graph.external_ids.end()),
        graph.external_ids.end());

    graph.num_vertices = static_cast<int>(graph.external_ids.size());
    graph.adj.resize(graph.external_ids.size());
    graph.in_adj.resize(graph.external_ids.size());
    graph.vertex_labels.resize(graph.external_ids.size(), kInvalidLabel);
    std::size_t label_index = 0;
    for (std::size_t i = 0; i < graph.external_ids.size(); ++i) {
        while (label_index < all_vertex_ids.size() &&
               all_vertex_ids[label_index] < graph.external_ids[i]) {
            ++label_index;
        }
        if (label_index >= all_vertex_ids.size() ||
            all_vertex_ids[label_index] != graph.external_ids[i]) {
            std::cerr << "Error: Failed to assign a data vertex label.\n";
            return false;
        }
        graph.vertex_labels[i] = all_vertex_labels[label_index];
    }

    std::vector<std::size_t> out_degrees(graph.external_ids.size(), 0);
    std::vector<std::size_t> in_degrees(graph.external_ids.size(), 0);
    for (const auto& edge : filtered_edges) {
        const int compact_u = compactId(graph.external_ids, edge.u);
        const int compact_v = compactId(graph.external_ids, edge.v);
        if (compact_u >= 0 && compact_v >= 0) {
            ++out_degrees[static_cast<std::size_t>(compact_u)];
            ++in_degrees[static_cast<std::size_t>(compact_v)];
        }
    }
    for (std::size_t i = 0; i < graph.adj.size(); ++i) {
        graph.adj[i].reserve(out_degrees[i]);
        graph.in_adj[i].reserve(in_degrees[i]);
    }

    graph.temporal_edges.reserve(filtered_edges.size());
    // F1: constant work per element in existing graph-finalization loops, for
    // every arm. No extra graph pass is moved outside query preparation.
    std::uint64_t finalized_capacity_bytes = 0;
    auto add_capacity = [&](std::size_t count, std::size_t element_bytes) {
        if (element_bytes && count > std::numeric_limits<std::uint64_t>::max() / element_bytes)
            throw std::overflow_error("graph finalized capacity size overflow");
        const auto bytes = static_cast<std::uint64_t>(count) * element_bytes;
        if (bytes > std::numeric_limits<std::uint64_t>::max() - finalized_capacity_bytes)
            throw std::overflow_error("graph finalized capacity sum overflow");
        finalized_capacity_bytes += bytes;
    };
    for (auto& edge : filtered_edges) {
        const int compact_u = compactId(graph.external_ids, edge.u);
        const int compact_v = compactId(graph.external_ids, edge.v);
        if (compact_u < 0 || compact_v < 0) continue;

        const int edge_id = static_cast<int>(graph.temporal_edges.size());
        graph.temporal_edges.push_back({
            compact_u,
            compact_v,
            std::move(edge.active_intervals),
            edge.active_snapshot_count});
        add_capacity(graph.temporal_edges.back().activeIntervals().capacity(), sizeof(TimeInterval));
        graph.adj[static_cast<std::size_t>(compact_u)].push_back({compact_v, edge_id});
        graph.in_adj[static_cast<std::size_t>(compact_v)].push_back({compact_u, edge_id});
    }
    std::vector<FilteredExternalEdge>().swap(filtered_edges);

    for (auto& neighbors : graph.adj) {
        add_capacity(neighbors.capacity(), sizeof(Edge));
        std::sort(neighbors.begin(), neighbors.end(), [](const Edge& lhs, const Edge& rhs) {
            return lhs.to < rhs.to;
        });
    }
    for (auto& neighbors : graph.in_adj) {
        add_capacity(neighbors.capacity(), sizeof(Edge));
        std::sort(neighbors.begin(), neighbors.end(), [](const Edge& lhs, const Edge& rhs) {
            return lhs.to < rhs.to;
        });
    }

    add_capacity(graph.adj.capacity(), sizeof(std::vector<Edge>));
    add_capacity(graph.in_adj.capacity(), sizeof(std::vector<Edge>));
    add_capacity(graph.temporal_edges.capacity(), sizeof(TemporalEdge));

    graph.vertex_active_durations.assign(graph.external_ids.size(), 0);
    graph.neighbor_label_counts.assign(graph.external_ids.size(), {});
    graph.incoming_neighbor_label_counts.assign(graph.external_ids.size(), {});
    std::vector<TimeInterval> incident_intervals;
    for (std::size_t vertex = 0; vertex < graph.adj.size(); ++vertex) {
        incident_intervals.clear();
        for (const auto& edge_ref : graph.adj[vertex]) {
            const auto& temporal_edge = graph.temporal_edges[static_cast<std::size_t>(edge_ref.temporal_edge_id)];
            incident_intervals.insert(
                incident_intervals.end(),
                temporal_edge.activeIntervals().begin(),
                temporal_edge.activeIntervals().end());

            const Label neighbor_label = graph.vertex_labels[static_cast<std::size_t>(edge_ref.to)];
            if (neighbor_label < kLabelCount) {
                ++graph.neighbor_label_counts[vertex][neighbor_label];
            }
        }
        for (const auto& edge_ref : graph.in_adj[vertex]) {
            const auto& temporal_edge =
                graph.temporal_edges[static_cast<std::size_t>(edge_ref.temporal_edge_id)];
            incident_intervals.insert(
                incident_intervals.end(),
                temporal_edge.activeIntervals().begin(),
                temporal_edge.activeIntervals().end());

            const Label neighbor_label =
                graph.vertex_labels[static_cast<std::size_t>(edge_ref.to)];
            if (neighbor_label < kLabelCount) {
                ++graph.incoming_neighbor_label_counts[vertex][neighbor_label];
            }
        }
        graph.vertex_active_durations[vertex] = unionDuration(incident_intervals);
    }

    if (load_timings != nullptr) {
        load_timings->adjacency_temporal_capacity_bytes = finalized_capacity_bytes;
        load_timings->filter_milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - filter_start).count();
    }

    return true;
}

bool readQueryGraph(const std::string& filename, Graph& query_graph) {
    std::ifstream input(filename);
    if (!input.is_open()) {
        std::cerr << "Error: Cannot open query graph file " << filename << '\n';
        return false;
    }

    query_graph = Graph{};
    std::unordered_map<std::string, int> vertex_by_key;
    std::vector<std::pair<int, int>> query_edges;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        std::istringstream parser(line);
        std::string lhs_token;
        std::string rhs_token;
        std::string extra_token;
        if (!(parser >> lhs_token)) continue;
        if (!(parser >> rhs_token) || (parser >> extra_token)) {
            std::cerr << "Error: Query graph line " << line_number
                      << " must contain exactly two vertex tokens.\n";
            return false;
        }

        auto parse_vertex = [](const std::string& token, std::string& key, Label& label) {
            const std::size_t separator = token.rfind(':');
            const std::string label_text = separator == std::string::npos
                ? token
                : token.substr(separator + 1);
            key = separator == std::string::npos ? token : token.substr(0, separator);
            label = labelFromString(label_text);
            return !key.empty() && label != kInvalidLabel;
        };

        std::string lhs_key;
        std::string rhs_key;
        Label lhs_label = kInvalidLabel;
        Label rhs_label = kInvalidLabel;
        if (!parse_vertex(lhs_token, lhs_key, lhs_label) ||
            !parse_vertex(rhs_token, rhs_key, rhs_label)) {
            std::cerr << "Error: Query labels must be one of A, B, C, D, E.\n";
            return false;
        }

        auto get_vertex = [&](const std::string& key, Label label) {
            const auto found = vertex_by_key.find(key);
            if (found != vertex_by_key.end()) {
                if (query_graph.vertex_labels[static_cast<std::size_t>(found->second)] != label) {
                    return -1;
                }
                return found->second;
            }
            const int id = static_cast<int>(query_graph.vertex_labels.size());
            vertex_by_key.emplace(key, id);
            query_graph.vertex_labels.push_back(label);
            return id;
        };

        int query_u = get_vertex(lhs_key, lhs_label);
        int query_v = get_vertex(rhs_key, rhs_label);
        if (query_u < 0 || query_v < 0) {
            std::cerr << "Error: A query vertex ID was used with conflicting labels.\n";
            return false;
        }
        query_edges.emplace_back(query_u, query_v);
    }
    if (input.bad()) {
        std::cerr << "Error: Failed while reading query graph file "
                  << filename << '\n';
        return false;
    }

    std::sort(query_edges.begin(), query_edges.end());
    query_edges.erase(std::unique(query_edges.begin(), query_edges.end()), query_edges.end());
    query_graph.num_vertices = static_cast<int>(query_graph.vertex_labels.size());
    query_graph.adj.resize(query_graph.vertex_labels.size());
    query_graph.in_adj.resize(query_graph.vertex_labels.size());
    query_graph.external_ids.resize(query_graph.vertex_labels.size());
    std::iota(query_graph.external_ids.begin(), query_graph.external_ids.end(), 0);

    for (const auto& edge : query_edges) {
        query_graph.adj[static_cast<std::size_t>(edge.first)].push_back({edge.second, -1});
        query_graph.in_adj[static_cast<std::size_t>(edge.second)].push_back({edge.first, -1});
    }
    for (auto& neighbors : query_graph.adj) {
        std::sort(neighbors.begin(), neighbors.end(), [](const Edge& lhs, const Edge& rhs) {
            return lhs.to < rhs.to;
        });
    }
    for (auto& neighbors : query_graph.in_adj) {
        std::sort(neighbors.begin(), neighbors.end(), [](const Edge& lhs, const Edge& rhs) {
            return lhs.to < rhs.to;
        });
    }

    return query_graph.num_vertices > 0;
}
#endif
