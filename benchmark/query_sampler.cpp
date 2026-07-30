#include "../ours/Utils.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

struct Pattern {
    std::vector<int> vertices;
    std::vector<std::pair<int, int>> edges;
    std::vector<TimeInterval> support;
};

struct Options {
    std::string data_path;
    std::string output_path;
    std::string shape;
    int minimum_duration = 3;
    std::uint32_t label_seed = kDefaultLabelSeed;
    LabelMode label_mode = LabelMode::SequentialRng;
};

void usage(const char* executable) {
    std::cerr
        << "Usage: " << executable
        << " <data> <output-query>"
           " <shape:path3|path4|path5|cycle3|tailed4|diamond5>"
           " <minimum-duration> [label-seed]"
           " [--label-mode sequential-rng|stable-id]\n";
}

int parsePositiveInt(const std::string& text, const char* name) {
    std::size_t consumed = 0;
    const long long value = std::stoll(text, &consumed);
    if (consumed != text.size() || value <= 0 ||
        value > std::numeric_limits<int>::max()) {
        throw std::invalid_argument(name);
    }
    return static_cast<int>(value);
}

std::uint32_t parseSeed(const std::string& text) {
    std::size_t consumed = 0;
    const unsigned long long value = std::stoull(text, &consumed);
    if (consumed != text.size() ||
        value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("label seed");
    }
    return static_cast<std::uint32_t>(value);
}

bool extendSupport(
    const std::vector<TimeInterval>& current,
    const TemporalEdge& edge,
    int minimum_duration,
    std::vector<TimeInterval>& next) {
    if (current.empty()) {
        next.clear();
        for (const auto& interval : edge.active_intervals) {
            if (interval.length() >= minimum_duration) next.push_back(interval);
        }
    } else {
        next = intersectTimeIntervals(
            current, edge.active_intervals, minimum_duration);
    }
    return !next.empty();
}

bool containsVertex(const Pattern& pattern, int vertex) {
    return std::find(pattern.vertices.begin(), pattern.vertices.end(), vertex) !=
        pattern.vertices.end();
}

bool findDirectedPath(
    const Graph& graph,
    int edge_count,
    int minimum_duration,
    Pattern& result) {
    std::function<bool(int, Pattern&)> search =
        [&](int current, Pattern& pattern) -> bool {
        if (static_cast<int>(pattern.edges.size()) == edge_count) return true;
        for (const auto& ref : graph.adj[static_cast<std::size_t>(current)]) {
            if (containsVertex(pattern, ref.to)) continue;
            const auto& temporal =
                graph.temporal_edges[static_cast<std::size_t>(ref.temporal_edge_id)];
            std::vector<TimeInterval> support;
            if (!extendSupport(
                    pattern.support, temporal, minimum_duration, support)) {
                continue;
            }
            pattern.vertices.push_back(ref.to);
            pattern.edges.emplace_back(current, ref.to);
            auto old_support = std::move(pattern.support);
            pattern.support = std::move(support);
            if (search(ref.to, pattern)) return true;
            pattern.support = std::move(old_support);
            pattern.edges.pop_back();
            pattern.vertices.pop_back();
        }
        return false;
    };

    for (int start = 0; start < graph.num_vertices; ++start) {
        if (graph.adj[static_cast<std::size_t>(start)].empty()) continue;
        Pattern candidate;
        candidate.vertices.push_back(start);
        if (search(start, candidate)) {
            result = std::move(candidate);
            return true;
        }
    }
    return false;
}

bool findDirectedTriangle(
    const Graph& graph,
    int minimum_duration,
    Pattern& result) {
    for (int u = 0; u < graph.num_vertices; ++u) {
        for (const auto& uv_ref : graph.adj[static_cast<std::size_t>(u)]) {
            const int v = uv_ref.to;
            if (v == u) continue;
            const auto& uv =
                graph.temporal_edges[static_cast<std::size_t>(uv_ref.temporal_edge_id)];
            std::vector<TimeInterval> support_uv;
            if (!extendSupport({}, uv, minimum_duration, support_uv)) continue;

            for (const auto& vw_ref : graph.adj[static_cast<std::size_t>(v)]) {
                const int w = vw_ref.to;
                if (w == u || w == v) continue;
                const auto& vw = graph.temporal_edges[
                    static_cast<std::size_t>(vw_ref.temporal_edge_id)];
                const TemporalEdge* wu = graph.findTemporalEdge(w, u);
                if (wu == nullptr) continue;

                auto support =
                    intersectTimeIntervals(support_uv, vw.active_intervals, minimum_duration);
                if (support.empty()) continue;
                support = intersectTimeIntervals(
                    support, wu->active_intervals, minimum_duration);
                if (support.empty()) continue;

                result.vertices = {u, v, w};
                result.edges = {{u, v}, {v, w}, {w, u}};
                result.support = std::move(support);
                return true;
            }
        }
    }
    return false;
}

bool addTail(
    const Graph& graph,
    int minimum_duration,
    Pattern& pattern) {
    for (int anchor : pattern.vertices) {
        for (const auto& ref : graph.adj[static_cast<std::size_t>(anchor)]) {
            if (containsVertex(pattern, ref.to)) continue;
            const auto& edge =
                graph.temporal_edges[static_cast<std::size_t>(ref.temporal_edge_id)];
            std::vector<TimeInterval> support;
            if (!extendSupport(
                    pattern.support, edge, minimum_duration, support)) {
                continue;
            }
            pattern.vertices.push_back(ref.to);
            pattern.edges.emplace_back(anchor, ref.to);
            pattern.support = std::move(support);
            return true;
        }
    }
    return false;
}

bool addDiamondVertex(
    const Graph& graph,
    int minimum_duration,
    Pattern& pattern) {
    const auto base_vertices = pattern.vertices;
    for (std::size_t first = 0; first < base_vertices.size(); ++first) {
        const int source = base_vertices[first];
        for (const auto& ref : graph.adj[static_cast<std::size_t>(source)]) {
            const int candidate = ref.to;
            if (containsVertex(pattern, candidate)) continue;
            const auto& first_edge =
                graph.temporal_edges[static_cast<std::size_t>(ref.temporal_edge_id)];
            auto support = intersectTimeIntervals(
                pattern.support, first_edge.active_intervals, minimum_duration);
            if (support.empty()) continue;

            for (std::size_t second = 0; second < base_vertices.size(); ++second) {
                if (second == first) continue;
                const int other_source = base_vertices[second];
                const TemporalEdge* second_edge =
                    graph.findTemporalEdge(other_source, candidate);
                if (second_edge == nullptr) continue;
                auto final_support = intersectTimeIntervals(
                    support, second_edge->active_intervals, minimum_duration);
                if (final_support.empty()) continue;

                pattern.vertices.push_back(candidate);
                pattern.edges.emplace_back(source, candidate);
                pattern.edges.emplace_back(other_source, candidate);
                pattern.support = std::move(final_support);
                return true;
            }
        }
    }
    return false;
}

bool buildPattern(
    const Graph& graph,
    const std::string& shape,
    int minimum_duration,
    Pattern& pattern) {
    if (shape == "path3") {
        return findDirectedPath(graph, 3, minimum_duration, pattern);
    }
    if (shape == "path4") {
        return findDirectedPath(graph, 4, minimum_duration, pattern);
    }
    if (shape == "path5") {
        return findDirectedPath(graph, 5, minimum_duration, pattern);
    }
    if (!findDirectedTriangle(graph, minimum_duration, pattern)) return false;
    if (shape == "cycle3") return true;
    if (shape == "tailed4") return addTail(graph, minimum_duration, pattern);
    if (shape == "diamond5") {
        return addDiamondVertex(graph, minimum_duration, pattern);
    }
    return false;
}

bool writeQuery(
    const Graph& graph,
    const Pattern& pattern,
    const std::string& output_path) {
    std::ofstream output(output_path);
    if (!output.is_open()) return false;

    std::unordered_map<int, int> query_id;
    query_id.reserve(pattern.vertices.size());
    for (std::size_t i = 0; i < pattern.vertices.size(); ++i) {
        query_id.emplace(pattern.vertices[i], static_cast<int>(i));
    }
    for (const auto& [source, target] : pattern.edges) {
        const int query_source = query_id.at(source);
        const int query_target = query_id.at(target);
        output << 'q' << query_source << ':'
               << labelToString(graph.vertex_labels[static_cast<std::size_t>(source)])
               << ' ' << 'q' << query_target << ':'
               << labelToString(graph.vertex_labels[static_cast<std::size_t>(target)])
               << '\n';
    }
    return output.good();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        usage(argv[0]);
        return 1;
    }

    Options options;
    options.data_path = argv[1];
    options.output_path = argv[2];
    options.shape = argv[3];
    try {
        options.minimum_duration = parsePositiveInt(argv[4], "minimum duration");
        bool seed_seen = false;
        bool label_mode_seen = false;
        for (int index = 5; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--label-mode") {
                if (label_mode_seen || index + 1 >= argc) {
                    throw std::invalid_argument("label mode");
                }
                const std::string mode = argv[++index];
                if (mode == "sequential-rng") {
                    options.label_mode = LabelMode::SequentialRng;
                } else if (mode == "stable-id") {
                    options.label_mode = LabelMode::StableId;
                } else {
                    throw std::invalid_argument("label mode");
                }
                label_mode_seen = true;
                continue;
            }
            constexpr const char* prefix = "--label-mode=";
            if (argument.rfind(prefix, 0) == 0) {
                if (label_mode_seen) {
                    throw std::invalid_argument("label mode");
                }
                const std::string mode =
                    argument.substr(std::char_traits<char>::length(prefix));
                if (mode == "sequential-rng") {
                    options.label_mode = LabelMode::SequentialRng;
                } else if (mode == "stable-id") {
                    options.label_mode = LabelMode::StableId;
                } else {
                    throw std::invalid_argument("label mode");
                }
                label_mode_seen = true;
                continue;
            }
            if (seed_seen || argument.rfind("--", 0) == 0) {
                throw std::invalid_argument("argument");
            }
            options.label_seed = parseSeed(argument);
            seed_seen = true;
        }
    } catch (const std::exception&) {
        usage(argv[0]);
        return 1;
    }

    Graph graph;
    TemporalGraphLoadTimings timings;
    if (!readTemporalGraph(
            options.data_path,
            graph,
            options.label_seed,
            &timings,
            true,
            options.label_mode)) {
        return 2;
    }

    Pattern pattern;
    if (!buildPattern(
            graph, options.shape, options.minimum_duration, pattern)) {
        std::cerr << "No " << options.shape << " with common consecutive duration "
                  << options.minimum_duration << " was found.\n";
        return 3;
    }
    if (!writeQuery(graph, pattern, options.output_path)) {
        std::cerr << "Could not write query file " << options.output_path << '\n';
        return 4;
    }

    std::cout << "Wrote " << options.shape << " query with "
              << pattern.vertices.size() << " vertices and "
              << pattern.edges.size() << " edges to " << options.output_path
              << "; witness support=" << formatIntervals(pattern.support)
              << "; label mode=" << labelModeName(options.label_mode) << '\n';
    return 0;
}
