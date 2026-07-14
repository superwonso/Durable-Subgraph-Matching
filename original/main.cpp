#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#include "TDTree.h"
#include "Utils.h"
#include "query_decomposition.h"

namespace {

std::size_t getPeakRSS() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS info{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info)) == 0) return 0;
    return static_cast<std::size_t>(info.PeakWorkingSetSize);
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
    return static_cast<std::size_t>(usage.ru_maxrss) * 1024;
#endif
}

long long elapsedMilliseconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <Data Graph> <Query Graph> <Minimum Duration k> "
                     "[Label Seed] [--count-only]\n";
        return 1;
    }

    int minimum_duration = 0;
    try {
        const std::string duration_text = argv[3];
        std::size_t parsed_characters = 0;
        minimum_duration = std::stoi(duration_text, &parsed_characters);
        if (parsed_characters != duration_text.size()) {
            throw std::invalid_argument("minimum duration");
        }
    } catch (const std::exception&) {
        std::cerr << "Error: k must be an integer.\n";
        return 1;
    }
    if (minimum_duration < 1) {
        std::cerr << "Error: k must be at least 1.\n";
        return 1;
    }

    std::uint32_t label_seed = kDefaultLabelSeed;
    bool label_seed_seen = false;
    bool count_only = false;
    for (int argument_index = 4; argument_index < argc; ++argument_index) {
        const std::string argument = argv[argument_index];
        if (argument == "--count-only") {
            if (count_only) {
                std::cerr << "Error: --count-only may be specified only once.\n";
                return 1;
            }
            count_only = true;
            continue;
        }
        if (argument.rfind("--", 0) == 0) {
            std::cerr << "Error: Unknown option: " << argument << '\n';
            return 1;
        }
        if (label_seed_seen) {
            std::cerr << "Error: Label seed may be specified only once.\n";
            return 1;
        }
        try {
            const std::string& seed_text = argument;
            if (seed_text.empty() || seed_text.front() == '-') {
                throw std::invalid_argument("label seed");
            }
            std::size_t parsed_characters = 0;
            const unsigned long long parsed_seed =
                std::stoull(seed_text, &parsed_characters);
            if (parsed_characters != seed_text.size()) {
                throw std::invalid_argument("label seed");
            }
            if (parsed_seed > std::numeric_limits<std::uint32_t>::max()) {
                throw std::out_of_range("label seed");
            }
            label_seed = static_cast<std::uint32_t>(parsed_seed);
            label_seed_seen = true;
        } catch (const std::exception&) {
            std::cerr << "Error: label seed must be a 32-bit unsigned integer.\n";
            return 1;
        }
    }
    const MatchOutputMode output_mode = count_only
        ? MatchOutputMode::CountOnly
        : MatchOutputMode::Full;

    const std::string temporal_graph_file = argv[1];
    const std::string query_graph_file = argv[2];
    const std::string dataset_name =
        std::filesystem::path(temporal_graph_file).stem().string();
    const auto total_start = std::chrono::steady_clock::now();
    std::unordered_map<std::string, long long> timings;

    Graph temporal_graph;
    TemporalGraphLoadTimings temporal_load_timings;
    auto stage_start = std::chrono::steady_clock::now();
    if (!readTemporalGraph(
            temporal_graph_file, temporal_graph, label_seed, &temporal_load_timings)) {
        return 2;
    }
    timings["readAndPrepareTemporalGraph"] = elapsedMilliseconds(stage_start);
    timings["readTemporalGraph"] = temporal_load_timings.read_milliseconds;
    timings["preprocessTemporalGraph"] = temporal_load_timings.preprocess_milliseconds;
    std::cout << "Temporal graph: " << temporal_graph.input_occurrence_count
              << " occurrences, " << temporal_graph.input_unique_edge_count
              << " unique directed edges, " << temporal_graph.retained_edge_count
              << " retained edges, " << temporal_graph.num_vertices
              << " active vertices, random label seed=" << label_seed << ".\n";
    std::cout << "Output mode: " << matchOutputModeName(output_mode) << ".\n";
    std::cout << "Temporal preprocessing (ms): read="
              << temporal_load_timings.read_milliseconds
              << ", preprocess=" << temporal_load_timings.preprocess_milliseconds
              << ", total=" << timings["readAndPrepareTemporalGraph"] << '\n';

    Graph query_graph;
    stage_start = std::chrono::steady_clock::now();
    if (!readQueryGraph(query_graph_file, query_graph)) return 2;
    timings["readQueryGraph"] = elapsedMilliseconds(stage_start);

    stage_start = std::chrono::steady_clock::now();
    std::array<std::size_t, kLabelCount> label_vertex_counts{};
    for (std::size_t vertex = 0; vertex < temporal_graph.vertex_labels.size(); ++vertex) {
        const Label label = temporal_graph.vertex_labels[vertex];
        if (label >= kLabelCount) continue;
        ++label_vertex_counts[label];
    }
    timings["labelStatistics"] = elapsedMilliseconds(stage_start);

    stage_start = std::chrono::steady_clock::now();
    const QueryDecomposition decomposition = decomposeQuery(
        query_graph, label_vertex_counts);
    timings["queryDecomposition"] = elapsedMilliseconds(stage_start);
    if (decomposition.root < 0) {
        std::cerr << "Error: Query graph has no edge-connected root.\n";
        return 3;
    }
    if (!decomposition.connected) {
        std::cerr << "Error: Query graph must be connected.\n";
        return 3;
    }

    std::cout << "Query root: q" << decomposition.root
              << " label=" << labelToString(query_graph.vertex_labels[decomposition.root])
              << " S_topo=" << decomposition.topological_selectivity[decomposition.root]
              << " | DFS order:";
    for (int query_vertex : decomposition.dfs_order) std::cout << ' ' << query_vertex;
    std::cout << " | non-tree edges=" << decomposition.non_tree_edges.size() << '\n';

    stage_start = std::chrono::steady_clock::now();
    TDTree td_tree(temporal_graph, query_graph, decomposition, minimum_duration);
    timings["buildTDTree"] = elapsedMilliseconds(stage_start);
    td_tree.print_res();

    const std::string matching_result_file = "matching_results_" + dataset_name + ".txt";
    const MatchSummary match_summary = td_tree.save_res(
        matching_result_file, output_mode);
    if (!match_summary.output_written) {
        std::cerr << "Error: Could not write " << matching_result_file << '\n';
        return 4;
    }
    timings["enumerateMatches"] = match_summary.enumeration_milliseconds;
    timings["endToEnd"] = elapsedMilliseconds(total_start);

    const std::string timing_result_file = "timing_results_" + dataset_name + ".txt";
    std::ofstream timing_output(timing_result_file);
    if (!timing_output.is_open()) {
        std::cerr << "Error: Could not write " << timing_result_file << '\n';
        return 4;
    }
    timing_output << "mode: " << matchOutputModeName(output_mode) << '\n'
                  << "count_overflow: "
                  << (match_summary.count_overflow ? "true" : "false") << '\n';
    const std::array<const char*, 9> timing_order{{
        "readTemporalGraph",
        "preprocessTemporalGraph",
        "readAndPrepareTemporalGraph",
        "readQueryGraph",
        "labelStatistics",
        "queryDecomposition",
        "buildTDTree",
        "enumerateMatches",
        "endToEnd"}};
    for (const char* timing_name : timing_order) {
        const auto timing = timings.find(timing_name);
        if (timing != timings.end()) {
            timing_output << timing->first << ": " << timing->second << " ms\n";
        }
    }

    const std::size_t input_graph_memory = temporal_graph.getMemoryUsage();
    const std::size_t td_tree_memory = td_tree.getMemoryUsage();
    const std::size_t total_peak_memory = getPeakRSS();
    const std::size_t known_memory = input_graph_memory + td_tree_memory;
    const std::size_t other_memory =
        total_peak_memory > known_memory ? total_peak_memory - known_memory : 0;

    if (match_summary.count_overflow) {
        std::cerr << "Error: Match count exceeds uint64_t; enumeration stopped "
                     "without wrapping the count.\n";
        std::cout << "Final durable matches: OVERFLOW\n";
    } else {
        std::cout << "Final durable matches: " << match_summary.match_count << '\n';
    }
    std::cout << "Result file: " << matching_result_file << '\n'
              << "Timing file: " << timing_result_file << '\n'
              << "Memory (KiB): graph=" << input_graph_memory / 1024
              << ", TD-tree=" << td_tree_memory / 1024
              << ", other=" << other_memory / 1024
              << ", peak=" << total_peak_memory / 1024 << '\n';
    return match_summary.count_overflow ? 5 : 0;
}
