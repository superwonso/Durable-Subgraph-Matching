#include "Utils.h"

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
#include <unordered_map>
#include <utility>

namespace {

struct RawTemporalEdge {
    int u;
    int v;
    int timestamp;
};

struct RetainedExternalEdge {
    int u;
    int v;
    std::vector<TimeInterval> active_intervals;
    int active_snapshot_count;
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
        total += edge.active_intervals.capacity() * sizeof(TimeInterval);
    }
    return total;
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
        if (end >= start && end - start + 1 >= required) {
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
    return std::any_of(intervals.begin(), intervals.end(), [minimum_duration](const TimeInterval& interval) {
        return interval.length() >= minimum_duration;
    });
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

bool readTemporalGraph(
    const std::string& filename,
    Graph& graph,
    std::uint32_t label_seed,
    TemporalGraphLoadTimings* load_timings) {
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

    const auto preprocess_start = std::chrono::steady_clock::now();
    std::sort(raw_edges.begin(), raw_edges.end(), rawEdgeLess);
    std::vector<RetainedExternalEdge> retained_edges;
    retained_edges.reserve(std::min<std::size_t>(raw_edges.size() / 4 + 1, 4'000'000));
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
        int active_count = 0;
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
                previous = current;
            } else {
                intervals.push_back({run_start, previous});
                run_start = previous = current;
            }
        }
        if (have_timestamp) intervals.push_back({run_start, previous});

        // Original keeps every directed edge history. Singleton intervals,
        // nonconsecutive histories, reciprocal arcs, and self-loops are not
        // removed during preprocessing.
        retained_edges.push_back({edge_u, edge_v, std::move(intervals), active_count});
    }
    graph.retained_edge_count = retained_edges.size();

    std::vector<RawTemporalEdge>().swap(raw_edges);

    // Sorting external IDs makes seeded random labels stable under input-line
    // permutations and avoids max-ID-sized allocations for sparse datasets.
    std::sort(all_vertex_ids.begin(), all_vertex_ids.end());
    all_vertex_ids.erase(
        std::unique(all_vertex_ids.begin(), all_vertex_ids.end()),
        all_vertex_ids.end());
    std::vector<Label> all_vertex_labels(all_vertex_ids.size(), kInvalidLabel);
    std::mt19937 label_generator(label_seed);
    std::uniform_int_distribution<int> label_distribution(
        0, static_cast<int>(kLabelCount) - 1);
    for (auto& label : all_vertex_labels) {
        label = static_cast<Label>(label_distribution(label_generator));
    }

    graph.external_ids.reserve(retained_edges.size() * 2);
    for (const auto& edge : retained_edges) {
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
    for (const auto& edge : retained_edges) {
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

    graph.temporal_edges.reserve(retained_edges.size());
    for (auto& edge : retained_edges) {
        const int compact_u = compactId(graph.external_ids, edge.u);
        const int compact_v = compactId(graph.external_ids, edge.v);
        if (compact_u < 0 || compact_v < 0) continue;

        const int edge_id = static_cast<int>(graph.temporal_edges.size());
        graph.temporal_edges.push_back({
            compact_u,
            compact_v,
            std::move(edge.active_intervals),
            edge.active_snapshot_count});
        graph.adj[static_cast<std::size_t>(compact_u)].push_back({compact_v, edge_id});
        graph.in_adj[static_cast<std::size_t>(compact_v)].push_back({compact_u, edge_id});
    }
    std::vector<RetainedExternalEdge>().swap(retained_edges);

    for (auto& neighbors : graph.adj) {
        std::sort(neighbors.begin(), neighbors.end(), [](const Edge& lhs, const Edge& rhs) {
            return lhs.to < rhs.to;
        });
    }
    for (auto& neighbors : graph.in_adj) {
        std::sort(neighbors.begin(), neighbors.end(), [](const Edge& lhs, const Edge& rhs) {
            return lhs.to < rhs.to;
        });
    }

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
                temporal_edge.active_intervals.begin(),
                temporal_edge.active_intervals.end());

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
                temporal_edge.active_intervals.begin(),
                temporal_edge.active_intervals.end());

            const Label neighbor_label =
                graph.vertex_labels[static_cast<std::size_t>(edge_ref.to)];
            if (neighbor_label < kLabelCount) {
                ++graph.incoming_neighbor_label_counts[vertex][neighbor_label];
            }
        }
        graph.vertex_active_durations[vertex] = unionDuration(incident_intervals);
    }

    if (load_timings != nullptr) {
        load_timings->preprocess_milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - preprocess_start).count();
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
