// Stage-0 input metadata only: no query, DFS, timing comparison or arm.
#include "../Utils.h"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("usage: audit_time_domain INPUT");
        Graph graph;
        if (!readTemporalGraph(argv[1], graph, 42, nullptr, LabelMode::StableId, nullptr, 2))
            throw std::runtime_error("cannot load frozen graph");
        std::map<std::size_t, std::uint64_t> counts, lengths, longest;
        std::int64_t minimum = std::numeric_limits<std::int64_t>::max();
        std::int64_t maximum = std::numeric_limits<std::int64_t>::min();
        std::uint64_t intervals = 0, occurrences = 0;
        for (const auto& edge : graph.temporal_edges) {
            ++counts[edge.activeIntervals().size()];
            ++longest[static_cast<std::size_t>(edge.maximumConsecutiveRun())];
            intervals += edge.activeIntervals().size();
            for (const auto& run : edge.activeIntervals()) {
                const auto size = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(run.end) - run.start + 1);
                ++lengths[static_cast<std::size_t>(size)];
                occurrences += size;
                minimum = std::min(minimum, static_cast<std::int64_t>(run.start));
                maximum = std::max(maximum, static_cast<std::int64_t>(run.end));
            }
        }
        if (!intervals) { minimum = 0; maximum = -1; }
        std::cout << "{\"status\":\"PASS\",\"matcher_invocations\":0,"
                  << "\"label_mode\":\"stable-id\",\"label_seed\":42,\"filter_minimum_run\":2,"
                  << "\"time_min\":" << minimum << ",\"time_max\":" << maximum
                  << ",\"integer_span\":" << (maximum - minimum + 1)
                  << ",\"edges\":" << graph.temporal_edges.size()
                  << ",\"intervals\":" << intervals << ",\"occurrences\":" << occurrences;
        auto histogram = [](const char* name, const auto& values) {
            std::cout << ",\"" << name << "\":{";
            bool first = true;
            for (const auto& [key, value] : values) {
                if (!first) std::cout << ',';
                first = false;
                std::cout << '"' << key << "\":" << value;
            }
            std::cout << '}';
        };
        histogram("edge_interval_count_histogram", counts);
        histogram("interval_length_histogram", lengths);
        histogram("edge_lmax_histogram", longest);
        std::cout << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
