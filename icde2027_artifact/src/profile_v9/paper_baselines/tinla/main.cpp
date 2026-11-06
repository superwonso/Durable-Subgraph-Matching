#include "tinla.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;

void usage(const char* executable) {
    std::cerr
        << "Usage: " << executable
        << " DATA QUERY K [seed] [--index tinla|ctinla] [--count-only] "
           "[--duration-mode cardinality|consecutive] "
           "[--label-mode sequential-rng|stable-id] [--threads N] "
           "[--output-dir DIR]\n";
}

bool parsePositiveInt(const std::string& text, int& value) {
    try {
        std::size_t used = 0;
        const long long parsed = std::stoll(text, &used);
        if (used != text.size() || parsed < 1 ||
            parsed > std::numeric_limits<int>::max()) {
            return false;
        }
        value = static_cast<int>(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parseThreads(const std::string& text, std::size_t& value) {
    try {
        if (text.empty() || text.front() == '-') return false;
        std::size_t used = 0;
        const unsigned long long parsed = std::stoull(text, &used);
        if (used != text.size() || parsed < 1 || parsed > 1024) return false;
        value = static_cast<std::size_t>(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string intervalText(const std::vector<tinla::Interval>& intervals) {
    std::string result;
    for (std::size_t i = 0; i < intervals.size(); ++i) {
        if (i != 0) result += ',';
        result += '[' + std::to_string(intervals[i].start) + ',' +
            std::to_string(intervals[i].end) + ']';
    }
    return result;
}

bool writeOutputs(
    const std::string& data_path,
    const std::filesystem::path& output_dir,
    const tinla::Graph& graph,
    const tinla::Query& query,
    const tinla::RunOptions& options,
    const tinla::RunResult& result,
    std::string& error) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(output_dir, filesystem_error);
    if (filesystem_error) {
        error = "cannot create output directory: " + output_dir.string();
        return false;
    }
    std::string stem = std::filesystem::path(data_path).stem().string();
    if (stem.empty()) stem = "data";
    const std::filesystem::path match_path =
        output_dir / ("matching_results_" + stem + ".txt");
    const std::filesystem::path timing_path =
        output_dir / ("timing_results_" + stem + ".txt");

    std::ofstream matches(match_path);
    if (!matches) {
        error = "cannot write matching results: " + match_path.string();
        return false;
    }
    matches << "Count: " << result.match_count << '\n'
            << "mode: " << (options.count_only ? "count-only" : "full") << '\n'
            << "algorithm: tinla_reimpl\n"
            << "adaptation: tinla_adapted_fixed_k\n"
            << "index_mode: " << tinla::indexModeName(options.index_mode) << '\n'
            << "duration_mode: " << tinla::durationModeName(options.duration_mode) << '\n'
            << "minimum_duration_k: " << options.minimum_duration << '\n';
    if (!options.count_only) {
        for (std::size_t row = 0; row < result.matches.size(); ++row) {
            matches << "Match " << row + 1 << ": ";
            const auto& match = result.matches[row];
            for (std::size_t q = 0; q < match.mapping.size(); ++q) {
                if (q != 0) matches << ", ";
                matches << query.names[q] << '(' << tinla::labelToString(query.labels[q])
                        << ")->" << graph.externalId(match.mapping[q]);
            }
            matches << " | active=" << intervalText(match.support) << '\n';
        }
    }

    std::ofstream timing(timing_path);
    if (!timing) {
        error = "cannot write timing results: " + timing_path.string();
        return false;
    }
    timing << std::fixed << std::setprecision(6)
           << "algorithm: tinla_reimpl\n"
           << "adaptation: tinla_adapted_fixed_k\n"
           << "index_mode: " << tinla::indexModeName(options.index_mode) << '\n'
           << "mode: " << (options.count_only ? "count-only" : "full") << '\n'
           << "duration_mode: " << tinla::durationModeName(options.duration_mode) << '\n'
           << "label_mode: " << tinla::labelModeName(options.label_mode) << '\n'
           << "label_seed: " << options.label_seed << '\n'
           << "minimum_duration_k: " << options.minimum_duration << '\n'
           << "threads_requested: " << result.threads_requested << '\n'
           << "threads_used: " << result.threads_used << '\n'
           << "data_vertices: " << graph.labels.size() << '\n'
           << "data_directed_edges: " << graph.edges.size() << '\n'
           << "input_occurrences: " << graph.input_occurrences << '\n'
           << "unique_occurrences: " << graph.unique_occurrences << '\n'
           << "query_vertices: " << query.labels.size() << '\n'
           << "query_directed_edges: " << query.edges.size() << '\n'
           << "matches: " << result.match_count << '\n'
           << "read_graph_ms: " << result.timings.read_graph_ms << '\n'
           << "preprocess_graph_ms: " << result.timings.preprocess_graph_ms << '\n'
           << "read_query_ms: " << result.timings.read_query_ms << '\n'
           << "build_index_ms: " << result.timings.build_index_ms << '\n'
           << "filter_candidates_ms: " << result.timings.filter_candidates_ms << '\n'
           << "refine_candidates_ms: " << result.timings.refine_candidates_ms << '\n'
           << "enumerate_ns: " << result.timings.enumerate_ns << '\n'
           << "enumerate_ms: " << result.timings.enumerate_ms << '\n'
           << "end_to_end_ms: " << result.timings.end_to_end_ms << '\n'
           << "graph_memory_bytes: " << result.graph_memory_bytes << '\n'
           << "index_memory_bytes: " << result.index_memory_bytes << '\n'
           << "peak_rss_bytes: " << result.peak_rss_bytes << '\n'
           << "index_cells: " << result.index_cells << '\n'
           << "index_segments: " << result.index_segments << '\n'
           << "initial_candidates: " << result.counters.initial_candidates << '\n'
           << "post_index_candidates: " << result.counters.post_index_candidates << '\n'
           << "post_refine_candidates: " << result.counters.post_refine_candidates << '\n'
           << "index_pruned: " << result.counters.index_pruned << '\n'
           << "refine_pruned: " << result.counters.refine_pruned << '\n'
           << "refine_rounds: " << result.counters.refine_rounds << '\n'
           << "root_candidates: " << result.counters.root_candidates << '\n'
           << "dfs_states: " << result.counters.dfs_states << '\n'
           << "raw_extension_candidates: " << result.counters.raw_extension_candidates << '\n'
           << "injectivity_pruned: " << result.counters.injectivity_pruned << '\n'
           << "structural_pruned: " << result.counters.structural_pruned << '\n'
           << "temporal_pruned: " << result.counters.temporal_pruned << '\n'
           << "dynamic_refine_calls: " << result.counters.dynamic_refine_calls << '\n'
           << "dynamic_refine_pruned: " << result.counters.dynamic_refine_pruned << '\n'
           << "dynamic_refine_rounds: " << result.counters.dynamic_refine_rounds << '\n';
    dsm::pure_online_v2::writePrimaryMetrics(
        timing, result.primary_metrics);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        usage(argv[0]);
        return 1;
    }
    const auto total_start = Clock::now();
    tinla::RunOptions options;
    if (!parsePositiveInt(argv[3], options.minimum_duration)) {
        std::cerr << "Error: K must be an integer of at least 1.\n";
        return 1;
    }

    bool seed_seen = false;
    bool index_seen = false;
    bool label_mode_seen = false;
    bool duration_mode_seen = false;
    bool threads_seen = false;
    bool output_dir_seen = false;
    std::filesystem::path output_dir = ".";
    for (int i = 4; i < argc; ++i) {
        const std::string argument = argv[i];
        auto valueFor = [&](const std::string& option, std::string& value) {
            if (argument == option) {
                if (i + 1 >= argc) return false;
                value = argv[++i];
                return true;
            }
            const std::string prefix = option + '=';
            if (argument.rfind(prefix, 0) == 0) {
                value = argument.substr(prefix.size());
                return !value.empty();
            }
            return false;
        };
        if (argument == "--count-only") {
            if (options.count_only) {
                std::cerr << "Error: --count-only may be specified only once.\n";
                return 1;
            }
            options.count_only = true;
            continue;
        }
        std::string value;
        if (valueFor("--index", value)) {
            if (index_seen || (value != "tinla" && value != "ctinla")) {
                std::cerr << "Error: --index must be tinla or ctinla and specified once.\n";
                return 1;
            }
            options.index_mode = value == "ctinla"
                ? tinla::IndexMode::Ctinla
                : tinla::IndexMode::Tinla;
            index_seen = true;
            continue;
        }
        if (valueFor("--label-mode", value)) {
            if (label_mode_seen ||
                (value != "sequential-rng" && value != "stable-id")) {
                std::cerr << "Error: --label-mode must be sequential-rng or stable-id and specified once.\n";
                return 1;
            }
            options.label_mode = value == "stable-id"
                ? tinla::LabelMode::StableId
                : tinla::LabelMode::SequentialRng;
            label_mode_seen = true;
            continue;
        }
        if (valueFor("--duration-mode", value)) {
            if (duration_mode_seen ||
                (value != "cardinality" && value != "consecutive")) {
                std::cerr << "Error: --duration-mode must be cardinality or consecutive and specified once.\n";
                return 1;
            }
            options.duration_mode = value == "consecutive"
                ? tinla::DurationMode::Consecutive
                : tinla::DurationMode::Cardinality;
            duration_mode_seen = true;
            continue;
        }
        if (valueFor("--threads", value)) {
            if (threads_seen || !parseThreads(value, options.threads)) {
                std::cerr << "Error: --threads must be an integer from 1 to 1024 and specified once.\n";
                return 1;
            }
            threads_seen = true;
            continue;
        }
        if (valueFor("--output-dir", value)) {
            if (output_dir_seen) {
                std::cerr << "Error: --output-dir may be specified only once.\n";
                return 1;
            }
            output_dir = value;
            output_dir_seen = true;
            continue;
        }
        if (!argument.empty() && argument.front() == '-') {
            std::cerr << "Error: unknown or incomplete option: " << argument << '\n';
            return 1;
        }
        if (seed_seen) {
            std::cerr << "Error: label seed may be specified only once.\n";
            return 1;
        }
        try {
            std::size_t used = 0;
            const unsigned long parsed = std::stoul(argument, &used);
            if (used != argument.size() || parsed > std::numeric_limits<std::uint32_t>::max()) {
                throw std::out_of_range("seed");
            }
            options.label_seed = static_cast<std::uint32_t>(parsed);
            seed_seen = true;
        } catch (const std::exception&) {
            std::cerr << "Error: label seed must be a uint32 integer.\n";
            return 1;
        }
    }

    std::string error;
    tinla::Graph graph;
    double read_graph_ms = 0.0;
    double preprocess_graph_ms = 0.0;
    if (!tinla::readTemporalGraph(
            argv[1],
            options.label_seed,
            options.label_mode,
            graph,
            &read_graph_ms,
            &preprocess_graph_ms,
            error)) {
        std::cerr << "Error: " << error << '\n';
        return 1;
    }
    const auto query_start = Clock::now();
    tinla::Query query;
    if (!tinla::readQuery(argv[2], query, error)) {
        std::cerr << "Error: " << error << '\n';
        return 1;
    }
    const double read_query_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - query_start).count();

    tinla::RunResult result;
    if (!tinla::runMatcher(graph, query, options, result, error)) {
        std::cerr << "Error: " << error << '\n';
        return 1;
    }
    result.timings.read_graph_ms = read_graph_ms;
    result.timings.preprocess_graph_ms = preprocess_graph_ms;
    result.timings.read_query_ms = read_query_ms;
    result.timings.end_to_end_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - total_start).count();
    if (!writeOutputs(argv[1], output_dir, graph, query, options, result, error)) {
        std::cerr << "Error: " << error << '\n';
        return 1;
    }
    std::cout << "algorithm=tinla_reimpl index=" << tinla::indexModeName(options.index_mode)
              << " matches=" << result.match_count
              << " threads=" << result.threads_used
              << " elapsed_ms=" << std::fixed << std::setprecision(3)
              << result.timings.end_to_end_ms << '\n';
    return 0;
}
