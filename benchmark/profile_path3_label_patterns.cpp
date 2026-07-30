#include "../ours/Utils.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::size_t kPatternCount =
    kLabelCount * kLabelCount * kLabelCount * kLabelCount;

struct PatternProfile {
    std::array<Label, 4> labels{};
    std::uint64_t matches = 0;
    std::array<std::unordered_set<int>, 4> vertices_by_position;
};

std::size_t patternIndex(const std::array<Label, 4>& labels) {
    std::size_t index = 0;
    for (Label label : labels) {
        index = index * kLabelCount + static_cast<std::size_t>(label);
    }
    return index;
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
        throw std::invalid_argument("seed");
    }
    return static_cast<std::uint32_t>(value);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <temporal-data> <minimum-duration-k> [label-seed]\n";
        return 1;
    }

    int minimum_duration = 0;
    std::uint32_t seed = kDefaultLabelSeed;
    try {
        minimum_duration = parsePositiveInt(argv[2], "minimum duration");
        if (argc == 4) seed = parseSeed(argv[3]);
    } catch (const std::exception&) {
        std::cerr << "Error: invalid duration or seed.\n";
        return 1;
    }

    Graph graph;
    if (!readTemporalGraph(argv[1], graph, seed)) return 2;

    std::array<PatternProfile, kPatternCount> profiles;
    for (std::size_t a = 0; a < kLabelCount; ++a) {
        for (std::size_t b = 0; b < kLabelCount; ++b) {
            for (std::size_t c = 0; c < kLabelCount; ++c) {
                for (std::size_t d = 0; d < kLabelCount; ++d) {
                    const std::array<Label, 4> labels{{
                        static_cast<Label>(a),
                        static_cast<Label>(b),
                        static_cast<Label>(c),
                        static_cast<Label>(d)}};
                    profiles[patternIndex(labels)].labels = labels;
                }
            }
        }
    }

    for (int v0 = 0; v0 < graph.num_vertices; ++v0) {
        for (const Edge& ref01 :
             graph.adj[static_cast<std::size_t>(v0)]) {
            const int v1 = ref01.to;
            if (v1 == v0) continue;
            const TemporalEdge& edge01 = graph.temporal_edges[
                static_cast<std::size_t>(ref01.temporal_edge_id)];
            std::vector<TimeInterval> support01;
            for (const TimeInterval& interval : edge01.active_intervals) {
                if (interval.length() >= minimum_duration) {
                    support01.push_back(interval);
                }
            }
            if (support01.empty()) continue;

            for (const Edge& ref12 :
                 graph.adj[static_cast<std::size_t>(v1)]) {
                const int v2 = ref12.to;
                if (v2 == v0 || v2 == v1) continue;
                const TemporalEdge& edge12 = graph.temporal_edges[
                    static_cast<std::size_t>(ref12.temporal_edge_id)];
                const std::vector<TimeInterval> support12 =
                    intersectTimeIntervals(
                        support01,
                        edge12.active_intervals,
                        minimum_duration);
                if (support12.empty()) continue;

                for (const Edge& ref23 :
                     graph.adj[static_cast<std::size_t>(v2)]) {
                    const int v3 = ref23.to;
                    if (v3 == v0 || v3 == v1 || v3 == v2) continue;
                    const TemporalEdge& edge23 = graph.temporal_edges[
                        static_cast<std::size_t>(ref23.temporal_edge_id)];
                    if (intersectTimeIntervals(
                            support12,
                            edge23.active_intervals,
                            minimum_duration)
                            .empty()) {
                        continue;
                    }

                    const std::array<int, 4> vertices{{v0, v1, v2, v3}};
                    const std::array<Label, 4> labels{{
                        graph.vertex_labels[static_cast<std::size_t>(v0)],
                        graph.vertex_labels[static_cast<std::size_t>(v1)],
                        graph.vertex_labels[static_cast<std::size_t>(v2)],
                        graph.vertex_labels[static_cast<std::size_t>(v3)]}};
                    PatternProfile& profile = profiles[patternIndex(labels)];
                    if (profile.matches !=
                        std::numeric_limits<std::uint64_t>::max()) {
                        ++profile.matches;
                    }
                    for (std::size_t position = 0; position < 4; ++position) {
                        profile.vertices_by_position[position].insert(
                            vertices[position]);
                    }
                }
            }
        }
    }

    std::vector<const PatternProfile*> ordered;
    ordered.reserve(profiles.size());
    for (const PatternProfile& profile : profiles) {
        if (profile.matches > 0) ordered.push_back(&profile);
    }
    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const PatternProfile* lhs, const PatternProfile* rhs) {
            if (lhs->matches != rhs->matches) {
                return lhs->matches > rhs->matches;
            }
            return lhs->labels < rhs->labels;
        });

    std::cout << "labels,matches,unique_q0,unique_q1,unique_q2,unique_q3\n";
    for (const PatternProfile* profile : ordered) {
        for (Label label : profile->labels) {
            std::cout << labelToString(label);
        }
        std::cout << ',' << profile->matches;
        for (const auto& vertices : profile->vertices_by_position) {
            std::cout << ',' << vertices.size();
        }
        std::cout << '\n';
    }
    return 0;
}
