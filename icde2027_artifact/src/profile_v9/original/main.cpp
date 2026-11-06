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
#include <vector>

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

void writeVertexOrderJson(
    std::ostream& output,
    const std::vector<int>& order) {
    output << '[';
    for (std::size_t index = 0; index < order.size(); ++index) {
        if (index > 0) output << ',';
        output << order[index];
    }
    output << ']';
}

void writeQueryPathOrderJson(
    std::ostream& output,
    const std::vector<std::vector<int>>& paths) {
    output << '[';
    for (std::size_t path_index = 0;
         path_index < paths.size();
         ++path_index) {
        if (path_index > 0) output << ',';
        writeVertexOrderJson(output, paths[path_index]);
    }
    output << ']';
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <Data Graph> <Query Graph> <Minimum Duration k> "
                     "[Label Seed] [--count-only] "
                     "[--label-mode sequential-rng|stable-id] "
                     "[--duration-mode cardinality|consecutive] "
                     "[--prefilter on|off] "
                     "[--threads N] [--query-tree dfs|bfs] "
                     "[--path-order sorted|random] "
                     "[--path-order-seed N] "
                     "[--time-compaction on|off]\n";
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
    LabelMode label_mode = LabelMode::SequentialRng;
    bool label_mode_seen = false;
    bool count_only = false;
    DurationMode duration_mode = DurationMode::Cardinality;
    bool duration_mode_seen = false;
    bool apply_consecutive_pair_filter = false;
    bool prefilter_seen = false;
    std::size_t threads = 1;
    bool threads_seen = false;
    QueryTreeMode query_tree_mode = QueryTreeMode::Dfs;
    bool query_tree_mode_seen = false;
    PathOrderMode path_order_mode = PathOrderMode::Sorted;
    bool path_order_mode_seen = false;
    std::uint64_t path_order_seed = UINT64_C(2022);
    bool path_order_seed_seen = false;
    bool time_compaction = true;
    bool time_compaction_seen = false;
    auto set_duration_mode = [&](const std::string& value) {
        if (duration_mode_seen) {
            std::cerr << "Error: --duration-mode may be specified only once.\n";
            return false;
        }
        if (value == "cardinality") {
            duration_mode = DurationMode::Cardinality;
        } else if (value == "consecutive") {
            duration_mode = DurationMode::Consecutive;
        } else {
            std::cerr << "Error: duration mode must be cardinality or consecutive.\n";
            return false;
        }
        duration_mode_seen = true;
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
    auto set_prefilter = [&](const std::string& value) {
        if (prefilter_seen) {
            std::cerr << "Error: --prefilter may be specified only once.\n";
            return false;
        }
        if (value == "on") {
            apply_consecutive_pair_filter = true;
        } else if (value == "off") {
            apply_consecutive_pair_filter = false;
        } else {
            std::cerr << "Error: prefilter mode must be on or off.\n";
            return false;
        }
        prefilter_seen = true;
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
    auto set_query_tree_mode = [&](const std::string& value) {
        if (query_tree_mode_seen) {
            std::cerr << "Error: --query-tree may be specified only once.\n";
            return false;
        }
        if (value == "dfs") {
            query_tree_mode = QueryTreeMode::Dfs;
        } else if (value == "bfs") {
            query_tree_mode = QueryTreeMode::Bfs;
        } else {
            std::cerr << "Error: query tree must be dfs or bfs.\n";
            return false;
        }
        query_tree_mode_seen = true;
        return true;
    };
    auto set_path_order_mode = [&](const std::string& value) {
        if (path_order_mode_seen) {
            std::cerr << "Error: --path-order may be specified only once.\n";
            return false;
        }
        if (value == "sorted") {
            path_order_mode = PathOrderMode::Sorted;
        } else if (value == "random") {
            path_order_mode = PathOrderMode::Random;
        } else {
            std::cerr << "Error: path order must be sorted or random.\n";
            return false;
        }
        path_order_mode_seen = true;
        return true;
    };
    auto set_path_order_seed = [&](const std::string& value) {
        if (path_order_seed_seen) {
            std::cerr << "Error: --path-order-seed may be specified only once.\n";
            return false;
        }
        try {
            if (value.empty() || value.front() == '-') {
                throw std::invalid_argument("path order seed");
            }
            std::size_t parsed_characters = 0;
            path_order_seed = std::stoull(value, &parsed_characters);
            if (parsed_characters != value.size()) {
                throw std::invalid_argument("path order seed");
            }
            path_order_seed_seen = true;
            return true;
        } catch (const std::exception&) {
            std::cerr << "Error: --path-order-seed must be an unsigned integer.\n";
            return false;
        }
    };
    auto set_time_compaction = [&](const std::string& value) {
        if (time_compaction_seen) {
            std::cerr << "Error: --time-compaction may be specified only once.\n";
            return false;
        }
        if (value == "on") {
            time_compaction = true;
        } else if (value == "off") {
            time_compaction = false;
        } else {
            std::cerr << "Error: time compaction must be on or off.\n";
            return false;
        }
        time_compaction_seen = true;
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
        if (argument == "--duration-mode") {
            if (argument_index + 1 >= argc) {
                std::cerr << "Error: --duration-mode requires a value.\n";
                return 1;
            }
            if (!set_duration_mode(argv[++argument_index])) {
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
        if (argument == "--prefilter") {
            if (argument_index + 1 >= argc ||
                !set_prefilter(argv[++argument_index])) {
                return 1;
            }
            continue;
        }
        constexpr const char* prefilter_prefix = "--prefilter=";
        if (argument.rfind(prefilter_prefix, 0) == 0) {
            if (!set_prefilter(argument.substr(
                    std::char_traits<char>::length(prefilter_prefix)))) {
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
        constexpr const char* duration_mode_prefix = "--duration-mode=";
        if (argument.rfind(duration_mode_prefix, 0) == 0) {
            if (!set_duration_mode(
                    argument.substr(std::char_traits<char>::length(
                        duration_mode_prefix)))) {
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
        if (argument == "--query-tree") {
            if (argument_index + 1 >= argc ||
                !set_query_tree_mode(argv[++argument_index])) return 1;
            continue;
        }
        constexpr const char* query_tree_prefix = "--query-tree=";
        if (argument.rfind(query_tree_prefix, 0) == 0) {
            if (!set_query_tree_mode(argument.substr(
                    std::char_traits<char>::length(query_tree_prefix)))) return 1;
            continue;
        }
        if (argument == "--path-order") {
            if (argument_index + 1 >= argc ||
                !set_path_order_mode(argv[++argument_index])) return 1;
            continue;
        }
        constexpr const char* path_order_prefix = "--path-order=";
        if (argument.rfind(path_order_prefix, 0) == 0) {
            if (!set_path_order_mode(argument.substr(
                    std::char_traits<char>::length(path_order_prefix)))) return 1;
            continue;
        }
        if (argument == "--path-order-seed") {
            if (argument_index + 1 >= argc ||
                !set_path_order_seed(argv[++argument_index])) return 1;
            continue;
        }
        constexpr const char* path_order_seed_prefix = "--path-order-seed=";
        if (argument.rfind(path_order_seed_prefix, 0) == 0) {
            if (!set_path_order_seed(argument.substr(
                    std::char_traits<char>::length(path_order_seed_prefix)))) return 1;
            continue;
        }
        if (argument == "--time-compaction") {
            if (argument_index + 1 >= argc ||
                !set_time_compaction(argv[++argument_index])) return 1;
            continue;
        }
        constexpr const char* time_compaction_prefix = "--time-compaction=";
        if (argument.rfind(time_compaction_prefix, 0) == 0) {
            if (!set_time_compaction(argument.substr(
                    std::char_traits<char>::length(time_compaction_prefix)))) return 1;
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
    if (apply_consecutive_pair_filter &&
        (duration_mode != DurationMode::Consecutive || minimum_duration < 2)) {
        std::cerr
            << "Error: --prefilter on is safe only for consecutive duration "
               "mode with k at least 2.\n";
        return 1;
    }

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
            label_mode,
            apply_consecutive_pair_filter)) {
        return 2;
    }
    timings["readAndPrepareTemporalGraph"] = elapsedMilliseconds(stage_start);
    timings["readTemporalGraph"] = temporal_load_timings.read_milliseconds;
    timings["preprocessTemporalGraph"] = temporal_load_timings.preprocess_milliseconds;
    if (temporal_load_timings.filter_milliseconds >= 0) {
        timings["filterTemporalGraph"] =
            temporal_load_timings.filter_milliseconds;
    }
    const std::uint64_t temporal_graph_fingerprint =
        temporalGraphContentFingerprint(temporal_graph);
    std::cout << "Temporal graph: " << temporal_graph.input_occurrence_count
              << " occurrences, " << temporal_graph.input_unique_edge_count
              << " unique directed edges, " << temporal_graph.retained_edge_count
              << " retained edges, " << temporal_graph.num_vertices
              << " active vertices, label mode="
              << labelModeName(label_mode)
              << ", label seed=" << label_seed << ".\n";
    std::cout << "Temporal graph content fingerprint (FNV-1a-64): "
              << temporal_graph_fingerprint << ".\n";
    std::cout << "Prefilter: "
              << (apply_consecutive_pair_filter ? "on" : "off") << ".\n";
    std::cout << "Output mode: " << matchOutputModeName(output_mode) << ".\n";
    std::cout << "Threads requested: " << threads
              << " (count-only matching only).\n";
    std::cout << "Duration mode: " << durationModeName(duration_mode)
              << " (k=" << minimum_duration << ").\n";
    std::cout << "Temporal preprocessing (ms): read="
              << temporal_load_timings.read_milliseconds
              << ", preprocess=" << temporal_load_timings.preprocess_milliseconds;
    if (temporal_load_timings.filter_milliseconds >= 0) {
        std::cout << ", filter="
                  << temporal_load_timings.filter_milliseconds;
    } else {
        std::cout << ", filter=not-applied";
    }
    std::cout << ", total=" << timings["readAndPrepareTemporalGraph"] << '\n';

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

    const auto controllable_query_start =
        std::chrono::steady_clock::now();
    stage_start = controllable_query_start;
    const QueryDecomposition decomposition = decomposeQuery(
        query_graph, label_vertex_counts, query_tree_mode);
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
              << " | " << queryTreeModeName(query_tree_mode) << " order:";
    for (int query_vertex : decomposition.dfs_order) std::cout << ' ' << query_vertex;
    std::cout << " | non-tree edges=" << decomposition.non_tree_edges.size() << '\n';

    stage_start = std::chrono::steady_clock::now();
    TDTree td_tree(
        temporal_graph,
        query_graph,
        decomposition,
        minimum_duration,
        duration_mode,
        path_order_mode,
        path_order_seed,
        time_compaction);
    const auto build_tdtree_elapsed =
        std::chrono::steady_clock::now() - stage_start;
    const std::uint64_t build_tdtree_nanoseconds =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                build_tdtree_elapsed)
                .count());
    timings["buildTDTree"] =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            build_tdtree_elapsed)
            .count();
    td_tree.print_res();

    const std::string matching_result_file = "matching_results_" + dataset_name + ".txt";
    const MatchSummary match_summary = td_tree.save_res(
        matching_result_file, output_mode, threads);
    if (!match_summary.output_written) {
        std::cerr << "Error: Could not write " << matching_result_file << '\n';
        return 4;
    }
    timings["enumerateMatches"] = match_summary.enumeration_milliseconds;
    const std::uint64_t tdtree_query_processing_nanoseconds =
        build_tdtree_nanoseconds + match_summary.enumeration_nanoseconds;
    const std::uint64_t controllable_query_processing_nanoseconds =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - controllable_query_start)
                .count());
    timings["endToEnd"] = elapsedMilliseconds(total_start);

    const std::string timing_result_file = "timing_results_" + dataset_name + ".txt";
    std::ofstream timing_output(timing_result_file);
    if (!timing_output.is_open()) {
        std::cerr << "Error: Could not write " << timing_result_file << '\n';
        return 4;
    }
    timing_output << "algorithm: tdtree_repo_corrected\n"
                  << "implementation: repository-paper-derived-corrected\n"
                  << "official_author_artifact: false\n"
                  << "mode: " << matchOutputModeName(output_mode) << '\n'
                  << "label_mode: " << labelModeName(label_mode) << '\n'
                  << "label_seed: " << label_seed << '\n'
                  << "selectivity_mode: tdtree\n"
                  << "selectivity_formula_id: label_population_over_query_degree\n"
                  << "query_root: " << decomposition.root << '\n'
                  << "root_selectivity_score: "
                  << std::setprecision(17)
                  << decomposition.topological_selectivity[
                         static_cast<std::size_t>(
                             decomposition.root)]
                  << '\n'
                  << "duration_mode: " << durationModeName(duration_mode) << '\n'
                  << "minimum_duration_k: " << minimum_duration << '\n'
                  << "prefilter: "
                  << (apply_consecutive_pair_filter ? "on" : "off") << '\n'
                  << "temporal_graph_content_fingerprint_fnv1a64: "
                  << temporal_graph_fingerprint << '\n'
                  << "query_tree_mode: " << queryTreeModeName(query_tree_mode) << '\n'
                  << "path_order_mode: " << pathOrderModeName(path_order_mode) << '\n'
                  << "path_order_seed: " << path_order_seed << '\n'
                  << "time_compaction: " << (time_compaction ? "on" : "off") << '\n'
                  << "threads_requested: "
                  << match_summary.threads_requested << '\n'
                  << "threads_used: "
                  << match_summary.threads_used << '\n'
                  << "workers_ever_with_tasks: "
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
                  << "leaf_block_precheck_attempts: "
                  << match_summary.leaf_block_precheck_attempts << '\n'
                  << "leaf_blocks_pruned: "
                  << match_summary.leaf_blocks_pruned << '\n'
                  << "leaf_block_candidates_pruned: "
                  << match_summary.leaf_block_candidates_pruned << '\n'
                  << "count_overflow: "
                  << (match_summary.count_overflow ? "true" : "false") << '\n';
    timing_output << "plan_order:";
    for (int query_vertex : decomposition.dfs_order) {
        timing_output << ' ' << query_vertex;
    }
    timing_output << "\ntdtree_instrumentation_schema_version: "
                     "tdtree_internal_ablation_v2\n"
                  << "generated_matching_order: ";
    writeVertexOrderJson(
        timing_output, match_summary.generated_matching_order);
    timing_output << "\nquery_path_order: ";
    writeQueryPathOrderJson(
        timing_output, match_summary.query_path_order);
    timing_output << '\n';
    const std::array<const char*, 10> timing_order{{
        "readTemporalGraph",
        "preprocessTemporalGraph",
        "filterTemporalGraph",
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
    const std::size_t td_tree_logical_memory =
        td_tree.getLogicalMemoryUsageBytes();
    const std::size_t total_peak_memory = getPeakRSS();
    const std::size_t known_memory = input_graph_memory + td_tree_memory;
    const std::size_t other_memory =
        total_peak_memory > known_memory ? total_peak_memory - known_memory : 0;
    timing_output << "tdtree_logical_bytes: "
                  << td_tree_logical_memory << '\n'
                  << "tdtree_capacity_bytes: "
                  << td_tree_memory << '\n'
                  << "buildTDTree_ns: "
                  << build_tdtree_nanoseconds << '\n'
                  << "matching_plan_ns: "
                  << match_summary.matching_plan_nanoseconds << '\n'
                  << "enumerateMatches_ns: "
                  << match_summary.enumeration_nanoseconds << '\n'
                  << "tdtreeQueryProcessing_ns: "
                  << tdtree_query_processing_nanoseconds << '\n'
                  << "controllableQueryProcessing_ns: "
                  << controllable_query_processing_nanoseconds << '\n'
                  << "leaf_block_precheck_attempts: "
                  << match_summary.leaf_block_precheck_attempts << '\n'
                  << "leaf_blocks_pruned: "
                  << match_summary.leaf_blocks_pruned << '\n'
                  << "leaf_block_candidates_pruned: "
                  << match_summary.leaf_block_candidates_pruned << '\n';
    dsm::pure_online_v2::writePrimaryMetrics(
        timing_output, match_summary.primary_metrics);

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
