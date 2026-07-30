#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
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

constexpr std::size_t kMaxMatchingThreads = 1024;

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
                     "[Label Seed] [--count-only] "
                     "[--label-mode sequential-rng|stable-id] "
                     "[--selectivity tdtree|paper|intended] "
                     "[--prefilter on|off] "
                     "[--threads N]\n";
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
    if (minimum_duration < 2) {
        std::cerr << "Error: k must be at least 2 because the PDF's preprocessing "
                     "removes edges without a consecutive snapshot pair.\n";
        return 1;
    }

    std::uint32_t label_seed = kDefaultLabelSeed;
    bool label_seed_seen = false;
    LabelMode label_mode = LabelMode::SequentialRng;
    bool label_mode_seen = false;
    bool count_only = false;
    SelectivityMode selectivity_mode = SelectivityMode::Paper;
    bool selectivity_mode_seen = false;
    bool apply_consecutive_pair_filter = true;
    bool prefilter_seen = false;
    std::size_t threads = 1;
    bool threads_seen = false;
    auto set_selectivity_mode = [&](const std::string& value) {
        if (selectivity_mode_seen) {
            std::cerr << "Error: --selectivity may be specified only once.\n";
            return false;
        }
        if (!parseSelectivityMode(value, selectivity_mode)) {
            std::cerr
                << "Error: selectivity mode must be tdtree, paper, "
                   "or intended.\n";
            return false;
        }
        selectivity_mode_seen = true;
        return true;
    };
    auto set_label_mode = [&](const std::string& value) {
        if (label_mode_seen) {
            std::cerr
                << "Error: --label-mode may be specified only once.\n";
            return false;
        }
        if (value == "sequential-rng") {
            label_mode = LabelMode::SequentialRng;
        } else if (value == "stable-id") {
            label_mode = LabelMode::StableId;
        } else {
            std::cerr
                << "Error: label mode must be sequential-rng or stable-id.\n";
            return false;
        }
        label_mode_seen = true;
        return true;
    };
    auto set_threads = [&](const std::string& value) {
        if (threads_seen) {
            std::cerr
                << "Error: --threads may be specified only once.\n";
            return false;
        }
        try {
            if (value.empty() || value.front() == '-') {
                throw std::invalid_argument("threads");
            }
            std::size_t parsed_characters = 0;
            const unsigned long long parsed_threads =
                std::stoull(value, &parsed_characters);
            if (parsed_characters != value.size() ||
                parsed_threads == 0 ||
                parsed_threads >
                    std::numeric_limits<std::size_t>::max() ||
                parsed_threads > kMaxMatchingThreads) {
                throw std::out_of_range("threads");
            }
            threads = static_cast<std::size_t>(parsed_threads);
            threads_seen = true;
            return true;
        } catch (const std::exception&) {
            std::cerr
                << "Error: --threads must be an integer from 1 to "
                << kMaxMatchingThreads << ".\n";
            return false;
        }
    };
    auto set_prefilter = [&](const std::string& value) {
        if (prefilter_seen) {
            std::cerr
                << "Error: --prefilter may be specified only once.\n";
            return false;
        }
        if (value == "on") {
            apply_consecutive_pair_filter = true;
        } else if (value == "off") {
            apply_consecutive_pair_filter = false;
        } else {
            std::cerr
                << "Error: prefilter mode must be on or off.\n";
            return false;
        }
        prefilter_seen = true;
        return true;
    };
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
        if (argument == "--selectivity") {
            if (argument_index + 1 >= argc) {
                std::cerr << "Error: --selectivity requires a value.\n";
                return 1;
            }
            if (!set_selectivity_mode(argv[++argument_index])) {
                return 1;
            }
            continue;
        }
        if (argument == "--label-mode") {
            if (argument_index + 1 >= argc) {
                std::cerr << "Error: --label-mode requires a value.\n";
                return 1;
            }
            if (!set_label_mode(argv[++argument_index])) {
                return 1;
            }
            continue;
        }
        constexpr const char* label_mode_prefix = "--label-mode=";
        if (argument.rfind(label_mode_prefix, 0) == 0) {
            if (!set_label_mode(
                    argument.substr(
                        std::char_traits<char>::length(
                            label_mode_prefix)))) {
                return 1;
            }
            continue;
        }
        constexpr const char* selectivity_prefix = "--selectivity=";
        if (argument.rfind(selectivity_prefix, 0) == 0) {
            if (!set_selectivity_mode(
                    argument.substr(std::char_traits<char>::length(
                        selectivity_prefix)))) {
                return 1;
            }
            continue;
        }
        if (argument == "--threads") {
            if (argument_index + 1 >= argc) {
                std::cerr << "Error: --threads requires a value.\n";
                return 1;
            }
            if (!set_threads(argv[++argument_index])) {
                return 1;
            }
            continue;
        }
        if (argument == "--prefilter") {
            if (argument_index + 1 >= argc) {
                std::cerr
                    << "Error: --prefilter requires a value.\n";
                return 1;
            }
            if (!set_prefilter(argv[++argument_index])) {
                return 1;
            }
            continue;
        }
        constexpr const char* prefilter_prefix =
            "--prefilter=";
        if (argument.rfind(prefilter_prefix, 0) == 0) {
            if (!set_prefilter(
                    argument.substr(
                        std::char_traits<char>::length(
                            prefilter_prefix)))) {
                return 1;
            }
            continue;
        }
        constexpr const char* threads_prefix = "--threads=";
        if (argument.rfind(threads_prefix, 0) == 0) {
            if (!set_threads(
                    argument.substr(
                        std::char_traits<char>::length(
                            threads_prefix)))) {
                return 1;
            }
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
            temporal_graph_file,
            temporal_graph,
            label_seed,
            &temporal_load_timings,
            apply_consecutive_pair_filter,
            label_mode)) {
        return 2;
    }
    timings["readAndFilterTemporalGraph"] = elapsedMilliseconds(stage_start);
    timings["readTemporalGraph"] = temporal_load_timings.read_milliseconds;
    timings["filterTemporalGraph"] = temporal_load_timings.filter_milliseconds;
    std::cout << "Temporal graph: " << temporal_graph.input_occurrence_count
              << " occurrences, " << temporal_graph.input_unique_edge_count
              << " unique directed edges, " << temporal_graph.filtered_edge_count
              << " retained edges, " << temporal_graph.num_vertices
              << " active vertices, label mode="
              << labelModeName(label_mode)
              << ", label seed=" << label_seed << ".\n";
    std::cout << "Output mode: " << matchOutputModeName(output_mode) << ".\n";
    std::cout << "Prefilter: "
              << (apply_consecutive_pair_filter ? "on" : "off")
              << ".\n";
    std::cout << "Threads requested: " << threads
              << " (count-only matching only).\n";
    std::cout << "Temporal preprocessing (ms): read="
              << temporal_load_timings.read_milliseconds
              << ", filter=" << temporal_load_timings.filter_milliseconds
              << ", total=" << timings["readAndFilterTemporalGraph"] << '\n';

    Graph query_graph;
    stage_start = std::chrono::steady_clock::now();
    if (!readQueryGraph(query_graph_file, query_graph)) return 2;
    timings["readQueryGraph"] = elapsedMilliseconds(stage_start);

    stage_start = std::chrono::steady_clock::now();
    std::array<std::size_t, kLabelCount> label_vertex_counts{};
    std::array<long long, kLabelCount> label_duration_sums{};
    for (std::size_t vertex = 0; vertex < temporal_graph.vertex_labels.size(); ++vertex) {
        const Label label = temporal_graph.vertex_labels[vertex];
        if (label >= kLabelCount) continue;
        ++label_vertex_counts[label];
        label_duration_sums[label] += temporal_graph.vertex_active_durations[vertex];
    }

    std::array<double, kLabelCount> label_average_lifespans{};
    for (std::size_t label = 0; label < kLabelCount; ++label) {
        if (label_vertex_counts[label] > 0) {
            label_average_lifespans[label] =
                static_cast<double>(label_duration_sums[label]) /
                static_cast<double>(label_vertex_counts[label]);
        }
    }
    timings["labelStatistics"] = elapsedMilliseconds(stage_start);

    stage_start = std::chrono::steady_clock::now();
    const QueryDecomposition decomposition = decomposeQuery(
        query_graph,
        label_vertex_counts,
        label_average_lifespans,
        selectivity_mode);
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
              << " selectivity_mode="
              << selectivityModeName(decomposition.selectivity_mode)
              << " S_temp=" << std::setprecision(17)
              << decomposition.temporal_selectivity[decomposition.root]
              << " | DFS order:";
    for (int query_vertex : decomposition.dfs_order) std::cout << ' ' << query_vertex;
    std::cout << " | non-tree edges=" << decomposition.non_tree_edges.size() << '\n';

    stage_start = std::chrono::steady_clock::now();
    TDTree td_tree(temporal_graph, query_graph, decomposition, minimum_duration);
    timings["buildTDTree"] = elapsedMilliseconds(stage_start);
    td_tree.print_res();

    const std::string matching_result_file = "matching_results_" + dataset_name + ".txt";
    const MatchSummary match_summary = td_tree.save_res(
        matching_result_file, output_mode, threads);
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
                  << "label_mode: " << labelModeName(label_mode) << '\n'
                  << "label_seed: " << label_seed << '\n'
                  << "prefilter: "
                  << (apply_consecutive_pair_filter ? "on" : "off")
                  << '\n'
                  << "selectivity_mode: "
                  << selectivityModeName(decomposition.selectivity_mode) << '\n'
                  << "query_root: " << decomposition.root << '\n'
                  << "root_selectivity_score: "
                  << std::setprecision(17)
                  << decomposition.temporal_selectivity[
                         static_cast<std::size_t>(decomposition.root)]
                  << '\n'
                  << "threads_requested: "
                  << match_summary.threads_requested << '\n'
                  << "threads_used: "
                  << match_summary.threads_used << '\n'
                  << "workers_with_tasks: "
                  << match_summary.workers_with_tasks << '\n'
                  << "root_tasks_claimed: "
                  << match_summary.root_tasks_claimed << '\n'
                  << "worker_root_tasks_min: "
                  << match_summary.worker_root_tasks_min << '\n'
                  << "worker_root_tasks_max: "
                  << match_summary.worker_root_tasks_max << '\n'
                  << "root_candidate_count: "
                  << match_summary.root_candidate_count << '\n'
                  << "candidate_relation_entries: "
                  << td_tree.candidateRelationCount() << '\n'
                  << "dfs_states: "
                  << match_summary.dfs_states << '\n'
                  << "metric_schema_version: logical_extensions_v1\n"
                  << "raw_extension_candidates: "
                  << match_summary.generated_candidates << '\n'
                  << "post_structural_candidates: "
                  << match_summary.generated_candidates << '\n'
                  << "generated_candidates: "
                  << match_summary.generated_candidates << '\n'
                  << "accepted_candidates: "
                  << match_summary.accepted_candidates << '\n'
                  << "accepted_extensions: "
                  << match_summary.accepted_candidates << '\n'
                  << "completed_matches: "
                  << match_summary.completed_matches << '\n'
                  << "pruned_injectivity: "
                  << match_summary.pruned_injectivity << '\n'
                  << "pruned_temporal_support: "
                  << match_summary.pruned_temporal_support << '\n'
                  << "count_overflow: "
                  << (match_summary.count_overflow ? "true" : "false") << '\n';
    timing_output << "plan_order:";
    for (int query_vertex : decomposition.dfs_order) {
        timing_output << ' ' << query_vertex;
    }
    timing_output << '\n';
    const std::array<const char*, 9> timing_order{{
        "readTemporalGraph",
        "filterTemporalGraph",
        "readAndFilterTemporalGraph",
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
              << "threads_requested: "
              << match_summary.threads_requested << '\n'
              << "threads_used: "
              << match_summary.threads_used << '\n'
              << "workers_with_tasks: "
              << match_summary.workers_with_tasks << '\n'
              << "root_tasks_claimed: "
              << match_summary.root_tasks_claimed << '\n'
              << "worker_root_tasks_min: "
              << match_summary.worker_root_tasks_min << '\n'
              << "worker_root_tasks_max: "
              << match_summary.worker_root_tasks_max << '\n'
              << "root_candidate_count: "
              << match_summary.root_candidate_count << '\n'
              << "candidate_relation_entries: "
              << td_tree.candidateRelationCount() << '\n'
              << "dfs_states: "
              << match_summary.dfs_states << '\n'
              << "metric_schema_version: logical_extensions_v1\n"
              << "raw_extension_candidates: "
              << match_summary.generated_candidates << '\n'
              << "post_structural_candidates: "
              << match_summary.generated_candidates << '\n'
              << "generated_candidates: "
              << match_summary.generated_candidates << '\n'
              << "accepted_candidates: "
              << match_summary.accepted_candidates << '\n'
              << "accepted_extensions: "
              << match_summary.accepted_candidates << '\n'
              << "completed_matches: "
              << match_summary.completed_matches << '\n'
              << "pruned_injectivity: "
              << match_summary.pruned_injectivity << '\n'
              << "pruned_temporal_support: "
              << match_summary.pruned_temporal_support << '\n'
              << "Memory (KiB): graph=" << input_graph_memory / 1024
              << ", TD-tree=" << td_tree_memory / 1024
              << ", other=" << other_memory / 1024
              << ", peak=" << total_peak_memory / 1024 << '\n';
    return match_summary.count_overflow ? 5 : 0;
}
