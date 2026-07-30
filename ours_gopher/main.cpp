#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#include "CandidateDag.h"
#include "DagExecutor.h"
#include "GopherMatcher.h"
#include "SearchStatistics.h"
#include "TDTree.h"
#include "Utils.h"
#include "query_decomposition.h"

namespace {

using Clock = std::chrono::steady_clock;

enum class Engine {
    Gopher,
    Legacy
};

const char* engineName(Engine engine) {
    return engine == Engine::Legacy ? "legacy" : "gopher";
}

struct CliOptions {
    std::string temporal_graph_file;
    std::string query_graph_file;
    int minimum_duration = 0;
    std::uint32_t label_seed = kDefaultLabelSeed;
    LabelMode label_mode = LabelMode::SequentialRng;
    Engine engine = Engine::Gopher;
    OrderMode order_mode = OrderMode::Paper;
    SelectivityMode selectivity_mode = SelectivityMode::Paper;
    MemoMode memo_mode = MemoMode::Dag;
    MemoOptions memo_options;
    std::size_t threads = 1;
    ParallelMode parallel_mode = ParallelMode::Roots;
    std::size_t prefix_depth = 0;
    std::size_t tasks_per_thread = 8;
    MatchOutputMode output_mode = MatchOutputMode::Full;
    bool verify_dag = false;
    bool early_prune = true;
    bool result_digest = false;
    bool collect_statistics = true;
    std::filesystem::path output_directory = ".";
    std::filesystem::path metrics_file;
    bool metrics_file_explicit = false;
};

void printUsage(std::ostream& output, const char* program) {
    output
        << "Usage: " << program
        << " <Data Graph> <Query Graph> <Minimum Duration k> [Label Seed]"
           " [options]\n\n"
        << "Options:\n"
        << "  --label-mode sequential-rng|stable-id\n"
        << "                               Vertex label assignment (default: sequential-rng)\n"
        << "  --engine gopher|legacy       Search engine (default: gopher)\n"
        << "  --selectivity tdtree|paper|intended\n"
        << "                               Root-score interpretation (default: paper)\n"
        << "  --order paper|reuse          Fixed query plan (default: paper)\n"
        << "  --memo none|structural|temporal|dag\n"
        << "                               DAG memoization ablation (default: dag)\n"
        << "  --cache-mb N                 Total bounded-cache budget (default: 256)\n"
        << "  --cache-entry-mb N           Maximum admitted entry (default: 8)\n"
        << "  --cache-min-work N           Cost-aware admission threshold (default: 128)\n"
        << "  --no-cost-aware              Admit without the work threshold\n"
        << "  --threads N                  Count-only worker threads (default: 1)\n"
        << "  --parallel roots|prefix      Count-only scheduler (default: roots)\n"
        << "  --prefix-depth N             Bound vertices in queued prefixes; 0=auto depth 2 (default: 0)\n"
        << "  --tasks-per-thread N         Bounded prefix-queue slots per worker (default: 8)\n"
        << "  --early-prune on|off         Cumulative active-support branch prune (default: on)\n"
        << "  --result-digest on|off       Order-independent result hash (default: off)\n"
        << "  --statistics on|off          Hot-path detailed counters (default: on)\n"
        << "  --count-only                 Suppress individual match rows\n"
        << "  --verify-dag                 Cross-check each DAG result with direct logic\n"
        << "  --output-dir DIR             Result/timing output directory\n"
        << "  --metrics PATH               Metrics JSON path\n"
        << "  --no-memo                    Alias for --memo none\n"
        << "  --reuse-aware                Alias for --order reuse\n"
        << "  --help                       Show this help\n\n"
        << "Full match rows are streamed deterministically with one worker. "
           "--threads/--parallel apply to --count-only runs.\n";
}

bool parseUnsigned(
    const std::string& text,
    std::uint64_t& value) {
    if (text.empty() || text.front() == '-') return false;
    try {
        std::size_t parsed = 0;
        const unsigned long long candidate =
            std::stoull(text, &parsed);
        if (parsed != text.size()) return false;
        value = static_cast<std::uint64_t>(candidate);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool megabytesToBytes(
    const std::string& text,
    std::size_t& bytes) {
    std::uint64_t megabytes = 0;
    if (!parseUnsigned(text, megabytes)) return false;
    constexpr std::uint64_t scale = 1024ULL * 1024ULL;
    if (megabytes >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max()) / scale) {
        return false;
    }
    bytes = static_cast<std::size_t>(megabytes * scale);
    return true;
}

bool parseCli(
    int argc,
    char* argv[],
    CliOptions& options) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        printUsage(std::cout, argv[0]);
        return false;
    }
    if (argc < 4) {
        printUsage(std::cerr, argv[0]);
        return false;
    }

    options.temporal_graph_file = argv[1];
    options.query_graph_file = argv[2];
    try {
        const std::string duration_text = argv[3];
        std::size_t parsed = 0;
        options.minimum_duration =
            std::stoi(duration_text, &parsed);
        if (parsed != duration_text.size()) {
            throw std::invalid_argument("minimum duration");
        }
    } catch (const std::exception&) {
        std::cerr << "Error: k must be an integer.\n";
        return false;
    }
    if (options.minimum_duration < 2) {
        std::cerr
            << "Error: k must be at least 2 because preprocessing removes "
               "edges without a consecutive snapshot pair.\n";
        return false;
    }

    bool seed_seen = false;
    bool label_mode_seen = false;
    bool count_only_seen = false;
    bool engine_seen = false;
    bool selectivity_seen = false;
    bool order_seen = false;
    bool memo_seen = false;
    bool cache_seen = false;
    bool cache_entry_seen = false;
    bool cache_work_seen = false;
    bool threads_seen = false;
    bool parallel_seen = false;
    bool prefix_depth_seen = false;
    bool tasks_per_thread_seen = false;
    bool verify_seen = false;
    bool early_prune_seen = false;
    bool result_digest_seen = false;
    bool statistics_seen = false;
    bool output_seen = false;
    bool metrics_seen = false;
    bool cost_seen = false;

    auto requireValue = [&](int& index, const std::string& option)
        -> const char* {
        if (index + 1 >= argc) {
            std::cerr << "Error: " << option
                      << " requires a value.\n";
            return nullptr;
        }
        return argv[++index];
    };
    auto rejectDuplicate = [](bool& seen, const std::string& option) {
        if (seen) {
            std::cerr << "Error: " << option
                      << " may be specified only once.\n";
            return false;
        }
        seen = true;
        return true;
    };

    for (int index = 4; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help") {
            printUsage(std::cout, argv[0]);
            return false;
        }
        if (argument == "--count-only") {
            if (!rejectDuplicate(
                    count_only_seen, "--count-only")) {
                return false;
            }
            options.output_mode = MatchOutputMode::CountOnly;
            continue;
        }
        if (argument == "--verify-dag") {
            if (!rejectDuplicate(verify_seen, "--verify-dag")) {
                return false;
            }
            options.verify_dag = true;
            continue;
        }
        if (argument == "--no-cost-aware") {
            if (!rejectDuplicate(
                    cost_seen, "--no-cost-aware")) {
                return false;
            }
            options.memo_options.cost_aware_admission = false;
            continue;
        }
        if (argument == "--no-memo") {
            if (!rejectDuplicate(memo_seen, "--memo/--no-memo")) {
                return false;
            }
            options.memo_mode = MemoMode::None;
            continue;
        }
        if (argument == "--reuse-aware") {
            if (!rejectDuplicate(
                    order_seen, "--order/--reuse-aware")) {
                return false;
            }
            options.order_mode = OrderMode::ReuseAware;
            continue;
        }
        if (argument == "--label-mode") {
            if (!rejectDuplicate(
                    label_mode_seen, "--label-mode")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr) return false;
            const std::string text = value;
            if (text == "sequential-rng") {
                options.label_mode = LabelMode::SequentialRng;
            } else if (text == "stable-id") {
                options.label_mode = LabelMode::StableId;
            } else {
                std::cerr
                    << "Error: --label-mode must be sequential-rng "
                       "or stable-id.\n";
                return false;
            }
            continue;
        }
        if (argument == "--engine") {
            if (!rejectDuplicate(engine_seen, "--engine")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr) return false;
            const std::string text = value;
            if (text == "gopher") {
                options.engine = Engine::Gopher;
            } else if (text == "legacy") {
                options.engine = Engine::Legacy;
            } else {
                std::cerr
                    << "Error: --engine must be gopher or legacy.\n";
                return false;
            }
            continue;
        }
        if (argument == "--selectivity") {
            if (!rejectDuplicate(
                    selectivity_seen, "--selectivity")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr ||
                !parseSelectivityMode(
                    value, options.selectivity_mode)) {
                std::cerr
                    << "Error: --selectivity must be tdtree, paper, "
                       "or intended.\n";
                return false;
            }
            continue;
        }
        if (argument == "--order") {
            if (!rejectDuplicate(order_seen, "--order")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr) return false;
            const std::string text = value;
            if (text == "paper") {
                options.order_mode = OrderMode::Paper;
            } else if (text == "reuse") {
                options.order_mode = OrderMode::ReuseAware;
            } else {
                std::cerr
                    << "Error: --order must be paper or reuse.\n";
                return false;
            }
            continue;
        }
        if (argument == "--memo") {
            if (!rejectDuplicate(memo_seen, "--memo")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr) return false;
            if (!parseMemoMode(value, options.memo_mode)) {
                std::cerr
                    << "Error: --memo must be none, structural, temporal, "
                       "or dag.\n";
                return false;
            }
            continue;
        }
        if (argument == "--cache-mb") {
            if (!rejectDuplicate(cache_seen, "--cache-mb")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr ||
                !megabytesToBytes(
                    value, options.memo_options.budget_bytes)) {
                std::cerr
                    << "Error: --cache-mb must be a non-negative integer.\n";
                return false;
            }
            continue;
        }
        if (argument == "--cache-entry-mb") {
            if (!rejectDuplicate(
                    cache_entry_seen, "--cache-entry-mb")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr ||
                !megabytesToBytes(
                    value, options.memo_options.max_entry_bytes)) {
                std::cerr
                    << "Error: --cache-entry-mb must be a non-negative "
                       "integer.\n";
                return false;
            }
            continue;
        }
        if (argument == "--cache-min-work") {
            if (!rejectDuplicate(
                    cache_work_seen, "--cache-min-work")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            std::uint64_t parsed = 0;
            if (value == nullptr ||
                !parseUnsigned(value, parsed) ||
                parsed >
                    std::numeric_limits<std::size_t>::max()) {
                std::cerr
                    << "Error: --cache-min-work must be a non-negative "
                       "integer.\n";
                return false;
            }
            options.memo_options.minimum_estimated_work =
                static_cast<std::size_t>(parsed);
            continue;
        }
        if (argument == "--threads") {
            if (!rejectDuplicate(threads_seen, "--threads")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            std::uint64_t parsed = 0;
            if (value == nullptr ||
                !parseUnsigned(value, parsed) ||
                parsed == 0 ||
                parsed >
                    std::numeric_limits<std::size_t>::max()) {
                std::cerr
                    << "Error: --threads must be a positive integer.\n";
                return false;
            }
            options.threads = static_cast<std::size_t>(parsed);
            continue;
        }
        if (argument == "--parallel") {
            if (!rejectDuplicate(parallel_seen, "--parallel")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr ||
                !parseParallelMode(value, options.parallel_mode)) {
                std::cerr
                    << "Error: --parallel must be roots or prefix.\n";
                return false;
            }
            continue;
        }
        if (argument == "--prefix-depth") {
            if (!rejectDuplicate(
                    prefix_depth_seen, "--prefix-depth")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            std::uint64_t parsed = 0;
            if (value == nullptr ||
                !parseUnsigned(value, parsed) ||
                parsed >
                    std::numeric_limits<std::size_t>::max()) {
                std::cerr
                    << "Error: --prefix-depth must be a non-negative "
                       "integer.\n";
                return false;
            }
            options.prefix_depth =
                static_cast<std::size_t>(parsed);
            continue;
        }
        if (argument == "--tasks-per-thread") {
            if (!rejectDuplicate(
                    tasks_per_thread_seen,
                    "--tasks-per-thread")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            std::uint64_t parsed = 0;
            if (value == nullptr ||
                !parseUnsigned(value, parsed) ||
                parsed == 0 ||
                parsed >
                    std::numeric_limits<std::size_t>::max()) {
                std::cerr
                    << "Error: --tasks-per-thread must be a positive "
                       "integer.\n";
                return false;
            }
            options.tasks_per_thread =
                static_cast<std::size_t>(parsed);
            continue;
        }
        if (argument == "--early-prune") {
            if (!rejectDuplicate(
                    early_prune_seen, "--early-prune")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr) return false;
            const std::string text = value;
            if (text == "on") {
                options.early_prune = true;
            } else if (text == "off") {
                options.early_prune = false;
            } else {
                std::cerr
                    << "Error: --early-prune must be on or off.\n";
                return false;
            }
            continue;
        }
        if (argument == "--result-digest") {
            if (!rejectDuplicate(
                    result_digest_seen, "--result-digest")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr) return false;
            const std::string text = value;
            if (text == "on") {
                options.result_digest = true;
            } else if (text == "off") {
                options.result_digest = false;
            } else {
                std::cerr
                    << "Error: --result-digest must be on or off.\n";
                return false;
            }
            continue;
        }
        if (argument == "--statistics") {
            if (!rejectDuplicate(
                    statistics_seen, "--statistics")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr) return false;
            const std::string text = value;
            if (text == "on") {
                options.collect_statistics = true;
            } else if (text == "off") {
                options.collect_statistics = false;
            } else {
                std::cerr
                    << "Error: --statistics must be on or off.\n";
                return false;
            }
            continue;
        }
        if (argument == "--output-dir") {
            if (!rejectDuplicate(output_seen, "--output-dir")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr || std::string(value).empty()) {
                std::cerr << "Error: --output-dir cannot be empty.\n";
                return false;
            }
            options.output_directory = value;
            continue;
        }
        if (argument == "--metrics") {
            if (!rejectDuplicate(metrics_seen, "--metrics")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr || std::string(value).empty()) {
                std::cerr << "Error: --metrics cannot be empty.\n";
                return false;
            }
            options.metrics_file = value;
            options.metrics_file_explicit = true;
            continue;
        }
        if (argument.rfind("--", 0) == 0) {
            std::cerr << "Error: Unknown option: "
                      << argument << '\n';
            return false;
        }

        if (seed_seen) {
            std::cerr
                << "Error: Label seed may be specified only once.\n";
            return false;
        }
        std::uint64_t seed = 0;
        if (!parseUnsigned(argument, seed) ||
            seed > std::numeric_limits<std::uint32_t>::max()) {
            std::cerr
                << "Error: label seed must be a 32-bit unsigned integer.\n";
            return false;
        }
        options.label_seed = static_cast<std::uint32_t>(seed);
        seed_seen = true;
    }
    return true;
}

std::size_t getPeakRSS() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS info{};
    if (GetProcessMemoryInfo(
            GetCurrentProcess(), &info, sizeof(info)) == 0) {
        return 0;
    }
    return static_cast<std::size_t>(info.PeakWorkingSetSize);
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<std::size_t>(usage.ru_maxrss);
#else
    return static_cast<std::size_t>(usage.ru_maxrss) * 1024;
#endif
#endif
}

std::uint64_t elapsedNanoseconds(Clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start).count());
}

std::string jsonEscape(const std::string& text) {
    std::ostringstream output;
    for (unsigned char character : text) {
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20) {
                    output << "\\u"
                           << std::hex << std::setw(4)
                           << std::setfill('0')
                           << static_cast<int>(character)
                           << std::dec << std::setfill(' ');
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    return output.str();
}

void writeStatisticsJson(
    std::ostream& output,
    const SearchStatistics& s,
    bool enabled) {
    bool first = true;
    auto field = [&](const char* name, std::uint64_t value) {
        if (!first) output << ',';
        first = false;
        output << "\n    \"" << name << "\": ";
        if (enabled) {
            output << value;
        } else {
            output << "null";
        }
    };
#define STAT_JSON(name) field(#name, s.name)
    STAT_JSON(root_tasks);
    STAT_JSON(dfs_states);
    STAT_JSON(raw_extension_candidates);
    STAT_JSON(generated_candidates);
    STAT_JSON(accepted_candidates);
    STAT_JSON(accepted_extensions);
    STAT_JSON(completed_matches);
    STAT_JSON(pruned_injectivity);
    STAT_JSON(pruned_structural);
    STAT_JSON(pruned_temporal_support);
    STAT_JSON(pruned_active_support);
    STAT_JSON(logical_td_block_lookups);
    STAT_JSON(physical_td_block_lookups);
    STAT_JSON(logical_neighbor_lookups);
    STAT_JSON(physical_neighbor_lookups);
    STAT_JSON(logical_structural_intersections);
    STAT_JSON(physical_structural_intersections);
    STAT_JSON(logical_temporal_edge_lookups);
    STAT_JSON(physical_temporal_edge_lookups);
    STAT_JSON(logical_temporal_intersections);
    STAT_JSON(physical_temporal_intersections);
    STAT_JSON(logical_longest_run);
    STAT_JSON(physical_longest_run);
    STAT_JSON(structural_input_elements);
    STAT_JSON(temporal_input_intervals);
    STAT_JSON(output_buffer_bytes);
    STAT_JSON(structural_cache_hits);
    STAT_JSON(structural_cache_misses);
    STAT_JSON(structural_cache_admitted);
    STAT_JSON(structural_cache_bypassed);
    STAT_JSON(structural_cache_bytes);
    STAT_JSON(structural_cache_entries);
    STAT_JSON(structural_cache_duplicate_inserts);
    STAT_JSON(structural_cache_rejected_budget);
    STAT_JSON(structural_cache_rejected_entry_size);
    STAT_JSON(structural_cache_rejected_cost);
    STAT_JSON(structural_cache_hit_result_elements);
    STAT_JSON(structural_cache_hit_saved_work);
    STAT_JSON(structural_cache_max_hit_result_elements);
    STAT_JSON(structural_cache_admitted_result_elements);
    STAT_JSON(structural_cache_admitted_estimated_work);
    STAT_JSON(temporal_cache_hits);
    STAT_JSON(temporal_cache_misses);
    STAT_JSON(temporal_cache_admitted);
    STAT_JSON(temporal_cache_bypassed);
    STAT_JSON(temporal_cache_bytes);
    STAT_JSON(temporal_cache_entries);
    STAT_JSON(temporal_cache_duplicate_inserts);
    STAT_JSON(temporal_cache_rejected_budget);
    STAT_JSON(temporal_cache_rejected_entry_size);
    STAT_JSON(temporal_cache_rejected_cost);
    STAT_JSON(temporal_cache_hit_result_elements);
    STAT_JSON(temporal_cache_hit_saved_work);
    STAT_JSON(temporal_cache_max_hit_result_elements);
    STAT_JSON(temporal_cache_admitted_result_elements);
    STAT_JSON(temporal_cache_admitted_estimated_work);
    STAT_JSON(longest_cache_hits);
    STAT_JSON(longest_cache_misses);
    STAT_JSON(dag_nodes_attempted);
    STAT_JSON(dag_nodes_materialized);
    STAT_JSON(dag_nodes_merged);
    STAT_JSON(cache_lookup_nanoseconds);
    STAT_JSON(structural_compute_nanoseconds);
    STAT_JSON(temporal_compute_nanoseconds);
    STAT_JSON(longest_compute_nanoseconds);
    STAT_JSON(search_nanoseconds);
#undef STAT_JSON
    output << '\n';
}

bool ensureParentDirectory(const std::filesystem::path& path) {
    const std::filesystem::path parent = path.parent_path();
    if (parent.empty()) return true;
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
        std::cerr << "Error: Cannot create directory "
                  << parent.string() << ": "
                  << error.message() << '\n';
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    CliOptions options;
    if (!parseCli(argc, argv, options)) {
        return argc == 2 && std::string(argv[1]) == "--help" ? 0 : 1;
    }

    std::error_code directory_error;
    std::filesystem::create_directories(
        options.output_directory, directory_error);
    if (directory_error) {
        std::cerr << "Error: Cannot create output directory "
                  << options.output_directory.string() << ": "
                  << directory_error.message() << '\n';
        return 1;
    }

    const std::string dataset_name =
        std::filesystem::path(
            options.temporal_graph_file).stem().string();
    const std::filesystem::path result_file =
        options.output_directory /
        ("matching_results_" + dataset_name + ".txt");
    const std::filesystem::path timing_file =
        options.output_directory /
        ("timing_results_" + dataset_name + ".txt");
    if (!options.metrics_file_explicit) {
        options.metrics_file =
            options.output_directory /
            ("metrics_results_" + dataset_name + ".json");
    } else if (!ensureParentDirectory(options.metrics_file)) {
        return 1;
    }

    const auto total_start = Clock::now();
    std::map<std::string, std::uint64_t> timings_ns;

    Graph temporal_graph;
    TemporalGraphLoadTimings load_timings;
    auto stage_start = Clock::now();
    if (!readTemporalGraph(
            options.temporal_graph_file,
            temporal_graph,
            options.label_seed,
            &load_timings,
            options.label_mode)) {
        return 2;
    }
    timings_ns["readAndFilterTemporalGraph"] =
        elapsedNanoseconds(stage_start);
    timings_ns["readTemporalGraph"] =
        static_cast<std::uint64_t>(
            std::max(0LL, load_timings.read_milliseconds)) *
        1'000'000ULL;
    timings_ns["filterTemporalGraph"] =
        static_cast<std::uint64_t>(
            std::max(0LL, load_timings.filter_milliseconds)) *
        1'000'000ULL;

    Graph query_graph;
    stage_start = Clock::now();
    if (!readQueryGraph(
            options.query_graph_file, query_graph)) {
        return 2;
    }
    timings_ns["readQueryGraph"] =
        elapsedNanoseconds(stage_start);

    stage_start = Clock::now();
    std::array<std::size_t, kLabelCount>
        label_vertex_counts{};
    std::array<long long, kLabelCount>
        label_duration_sums{};
    for (std::size_t vertex = 0;
         vertex < temporal_graph.vertex_labels.size();
         ++vertex) {
        const Label label =
            temporal_graph.vertex_labels[vertex];
        if (label >= kLabelCount) continue;
        ++label_vertex_counts[label];
        label_duration_sums[label] +=
            temporal_graph.vertex_active_durations[vertex];
    }
    std::array<double, kLabelCount>
        label_average_lifespans{};
    for (std::size_t label = 0;
         label < kLabelCount;
         ++label) {
        if (label_vertex_counts[label] != 0) {
            label_average_lifespans[label] =
                static_cast<double>(
                    label_duration_sums[label]) /
                static_cast<double>(
                    label_vertex_counts[label]);
        }
    }
    timings_ns["labelStatistics"] =
        elapsedNanoseconds(stage_start);

    stage_start = Clock::now();
    const QueryDecomposition decomposition =
        decomposeQuery(
            query_graph,
            label_vertex_counts,
            label_average_lifespans,
            options.selectivity_mode);
    timings_ns["queryDecomposition"] =
        elapsedNanoseconds(stage_start);
    if (decomposition.root < 0) {
        std::cerr
            << "Error: Query graph has no edge-connected root.\n";
        return 3;
    }
    if (!decomposition.connected) {
        std::cerr
            << "Error: Query graph must be connected.\n";
        return 3;
    }

    stage_start = Clock::now();
    TDTree td_tree(
        temporal_graph,
        query_graph,
        decomposition,
        options.minimum_duration);
    timings_ns["buildTDTree"] =
        elapsedNanoseconds(stage_start);

    QueryPlan plan;
    CandidateDag dag;
    GopherMatchSummary gopher_summary;
    MatchSummary legacy_summary;
    SearchStatistics search_statistics;
    std::string result_digest = "disabled";
    std::size_t threads_used = 1;
    std::size_t cache_memory_bytes = 0;
    std::string parallel_mode_effective =
        engineName(options.engine);
    ParallelExecutionStatistics parallel_statistics;

    if (options.engine == Engine::Gopher) {
        stage_start = Clock::now();
        plan = makeQueryPlan(
            query_graph,
            decomposition,
            td_tree,
            options.order_mode);
        timings_ns["buildQueryPlan"] =
            elapsedNanoseconds(stage_start);

        stage_start = Clock::now();
        CandidateDagCompiler compiler;
        dag = compiler.compile(
            query_graph, plan, decomposition);
        timings_ns["compileCandidateDag"] =
            elapsedNanoseconds(stage_start);

        GopherMatcherOptions matcher_options;
        matcher_options.threads = options.threads;
        matcher_options.parallel_mode = options.parallel_mode;
        matcher_options.prefix_depth = options.prefix_depth;
        matcher_options.tasks_per_thread =
            options.tasks_per_thread;
        matcher_options.order_mode = options.order_mode;
        matcher_options.early_prune = options.early_prune;
        matcher_options.result_digest = options.result_digest;
        matcher_options.executor.memo_mode =
            options.memo_mode;
        matcher_options.executor.memo_options =
            options.memo_options;
        matcher_options.executor.verify_dag =
            options.verify_dag;
        matcher_options.executor.collect_statistics =
            options.collect_statistics;
        GopherMatcher matcher(
            temporal_graph,
            query_graph,
            plan,
            dag,
            td_tree,
            options.minimum_duration,
            matcher_options);
        gopher_summary = matcher.save_res(
            result_file.string(), options.output_mode);
        if (!gopher_summary.output_written) {
            std::cerr << "Error: Could not write "
                      << result_file.string() << '\n';
            return 4;
        }
        timings_ns["enumerateMatches"] =
            gopher_summary.enumeration_nanoseconds;
        search_statistics = gopher_summary.statistics;
        result_digest = gopher_summary.result_digest;
        threads_used = gopher_summary.threads_used;
        parallel_mode_effective =
            gopher_summary.parallel_mode_effective;
        parallel_statistics =
            gopher_summary.parallel_statistics;
        cache_memory_bytes =
            gopher_summary.cache_memory_bytes;
    } else {
        legacy_summary = td_tree.save_res(
            result_file.string(), options.output_mode);
        if (!legacy_summary.output_written) {
            std::cerr << "Error: Could not write "
                      << result_file.string() << '\n';
            return 4;
        }
        timings_ns["enumerateMatches"] =
            static_cast<std::uint64_t>(
                std::max(
                    0LL,
                    legacy_summary.enumeration_milliseconds)) *
            1'000'000ULL;
    }
    timings_ns["endToEnd"] =
        elapsedNanoseconds(total_start);

    const std::uint64_t match_count =
        options.engine == Engine::Gopher
        ? gopher_summary.match_count
        : legacy_summary.match_count;
    const bool count_overflow =
        options.engine == Engine::Gopher
        ? gopher_summary.count_overflow
        : legacy_summary.count_overflow;

    const std::size_t graph_memory =
        temporal_graph.getMemoryUsage();
    const std::size_t td_tree_memory =
        td_tree.getMemoryUsage();
    const std::size_t dag_memory =
        options.engine == Engine::Gopher
        ? dag.getMemoryUsage()
        : 0;
    const std::size_t peak_memory = getPeakRSS();
    const std::size_t known_memory =
        graph_memory + td_tree_memory +
        dag_memory + cache_memory_bytes;
    const std::size_t other_memory =
        peak_memory > known_memory
        ? peak_memory - known_memory
        : 0;

    std::ofstream timing_output(timing_file);
    if (!timing_output.is_open()) {
        std::cerr << "Error: Could not write "
                  << timing_file.string() << '\n';
        return 4;
    }
    timing_output
        << "engine: " << engineName(options.engine) << '\n'
        << "mode: "
        << matchOutputModeName(options.output_mode) << '\n'
        << "label_mode: "
        << labelModeName(options.label_mode) << '\n'
        << "label_seed: " << options.label_seed << '\n'
        << "selectivity_mode: "
        << selectivityModeName(decomposition.selectivity_mode) << '\n'
        << "query_root: " << decomposition.root << '\n'
        << "root_selectivity_score: "
        << std::setprecision(17)
        << decomposition.temporal_selectivity[
               static_cast<std::size_t>(decomposition.root)]
        << '\n'
        << "metric_schema_version: logical_extensions_v1\n"
        << "root_candidate_count: "
        << td_tree.rootCandidates().size() << '\n'
        << "candidate_relation_entries: "
        << td_tree.candidateRelationCount() << '\n'
        << "order: " << orderModeName(options.order_mode) << '\n'
        << "memo: " << memoModeName(options.memo_mode) << '\n'
        << "threads_requested: " << options.threads << '\n'
        << "threads_used: " << threads_used << '\n'
        << "parallel_mode_requested: "
        << parallelModeName(options.parallel_mode) << '\n'
        << "parallel_mode_effective: "
        << parallel_mode_effective << '\n'
        << "prefix_depth_requested: "
        << options.prefix_depth << '\n'
        << "tasks_per_thread: "
        << options.tasks_per_thread << '\n'
        << "early_prune: "
        << (options.early_prune ? "on" : "off") << '\n'
        << "result_digest_enabled: "
        << (options.result_digest ? "on" : "off") << '\n'
        << "statistics_enabled: "
        << (options.collect_statistics ? "on" : "off") << '\n'
        << "cache_budget_bytes: "
        << options.memo_options.budget_bytes << '\n'
        << "count_overflow: "
        << (count_overflow ? "true" : "false") << '\n';
    timing_output << "plan_order:";
    const std::vector<int>& timing_order =
        options.engine == Engine::Gopher
        ? plan.order
        : decomposition.dfs_order;
    for (int query_vertex : timing_order) {
        timing_output << ' ' << query_vertex;
    }
    timing_output << '\n';
    for (const auto& timing : timings_ns) {
        timing_output << timing.first
                      << "_ns: " << timing.second << '\n'
                      << timing.first
                      << "_ms: " << std::fixed
                      << std::setprecision(6)
                      << static_cast<double>(timing.second) /
                             1'000'000.0
                      << '\n';
    }
    if (options.engine == Engine::Gopher) {
        timing_output << "result_digest: "
                      << result_digest << '\n';
        timing_output
            << "parallel_root_tasks_claimed: "
            << parallel_statistics.root_tasks_claimed << '\n'
            << "parallel_prefix_tasks_spawned: "
            << parallel_statistics.prefix_tasks_spawned << '\n'
            << "parallel_prefix_split_attempts: "
            << parallel_statistics.prefix_split_attempts << '\n'
            << "parallel_prefix_split_rejected: "
            << parallel_statistics.prefix_split_rejected << '\n'
            << "parallel_queue_claims: "
            << parallel_statistics.queue_claims << '\n'
            << "parallel_queue_peak: "
            << parallel_statistics.queue_peak << '\n'
            << "parallel_queue_state_bytes_peak: "
            << parallel_statistics.queue_state_bytes_peak << '\n'
            << "parallel_workers_with_tasks: "
            << parallel_statistics.workers_with_tasks << '\n'
            << "parallel_worker_tasks_min: "
            << parallel_statistics.worker_tasks_min << '\n'
            << "parallel_worker_tasks_max: "
            << parallel_statistics.worker_tasks_max << '\n'
            << "parallel_worker_tasks_total: "
            << parallel_statistics.worker_tasks_total << '\n'
            << "parallel_worker_dfs_states_min: "
            << parallel_statistics.worker_dfs_states_min << '\n'
            << "parallel_worker_dfs_states_max: "
            << parallel_statistics.worker_dfs_states_max << '\n'
            << "parallel_worker_dfs_states_total: "
            << parallel_statistics.worker_dfs_states_total << '\n'
            << "parallel_worker_active_ns_min: "
            << parallel_statistics.worker_active_nanoseconds_min
            << '\n'
            << "parallel_worker_active_ns_max: "
            << parallel_statistics.worker_active_nanoseconds_max
            << '\n'
            << "parallel_worker_active_ns_total: "
            << parallel_statistics.worker_active_nanoseconds_total
            << '\n'
            << "parallel_worker_tasks_cv: "
            << parallel_statistics.worker_tasks_cv << '\n'
            << "parallel_worker_dfs_states_cv: "
            << parallel_statistics.worker_dfs_states_cv << '\n'
            << "parallel_worker_active_ns_cv: "
            << parallel_statistics.worker_active_nanoseconds_cv
            << '\n'
            << "parallel_worker_utilization: "
            << parallel_statistics.worker_utilization << '\n'
            << "parallel_task_queue_capacity: "
            << parallel_statistics.task_queue_capacity << '\n'
            << "parallel_prefix_depth_limit: "
            << parallel_statistics.prefix_depth_limit << '\n';
        if (options.collect_statistics) {
            writeSearchStatistics(
                timing_output, search_statistics);
        } else {
            timing_output
                << "statistics_collected: false\n";
        }
    }

    std::ofstream metrics_output(options.metrics_file);
    if (!metrics_output.is_open()) {
        std::cerr << "Error: Could not write "
                  << options.metrics_file.string() << '\n';
        return 4;
    }
    metrics_output
        << "{\n"
        << "  \"schema_version\": 2,\n"
        << "  \"metric_schema_version\": "
           "\"logical_extensions_v1\",\n"
        << "  \"engine\": \"" << engineName(options.engine)
        << "\",\n"
        << "  \"mode\": \""
        << matchOutputModeName(options.output_mode) << "\",\n"
        << "  \"label_mode\": \""
        << labelModeName(options.label_mode) << "\",\n"
        << "  \"selectivity_mode\": \""
        << selectivityModeName(decomposition.selectivity_mode)
        << "\",\n"
        << "  \"query_root\": " << decomposition.root << ",\n"
        << "  \"root_selectivity_score\": "
        << std::setprecision(17)
        << decomposition.temporal_selectivity[
               static_cast<std::size_t>(decomposition.root)]
        << ",\n"
        << "  \"root_candidate_count\": "
        << td_tree.rootCandidates().size() << ",\n"
        << "  \"candidate_relation_entries\": "
        << td_tree.candidateRelationCount() << ",\n"
        << "  \"order\": \"" << orderModeName(options.order_mode)
        << "\",\n"
        << "  \"memo\": \"" << memoModeName(options.memo_mode)
        << "\",\n"
        << "  \"verify_dag\": "
        << (options.verify_dag ? "true" : "false") << ",\n"
        << "  \"threads_requested\": " << options.threads << ",\n"
        << "  \"threads_used\": " << threads_used << ",\n"
        << "  \"parallel_mode_requested\": \""
        << parallelModeName(options.parallel_mode) << "\",\n"
        << "  \"parallel_mode_effective\": \""
        << jsonEscape(parallel_mode_effective) << "\",\n"
        << "  \"prefix_depth_requested\": "
        << options.prefix_depth << ",\n"
        << "  \"tasks_per_thread\": "
        << options.tasks_per_thread << ",\n"
        << "  \"early_prune\": "
        << (options.early_prune ? "true" : "false") << ",\n"
        << "  \"result_digest_enabled\": "
        << (options.result_digest ? "true" : "false") << ",\n"
        << "  \"statistics_enabled\": "
        << (options.collect_statistics ? "true" : "false") << ",\n"
        << "  \"cache_budget_bytes\": "
        << options.memo_options.budget_bytes << ",\n"
        << "  \"cache_max_entry_bytes\": "
        << options.memo_options.max_entry_bytes << ",\n"
        << "  \"cache_minimum_work\": "
        << options.memo_options.minimum_estimated_work << ",\n"
        << "  \"cost_aware_admission\": "
        << (options.memo_options.cost_aware_admission
            ? "true" : "false")
        << ",\n"
        << "  \"minimum_duration\": "
        << options.minimum_duration << ",\n"
        << "  \"label_seed\": " << options.label_seed << ",\n"
        << "  \"data_file\": \""
        << jsonEscape(options.temporal_graph_file) << "\",\n"
        << "  \"query_file\": \""
        << jsonEscape(options.query_graph_file) << "\",\n"
        << "  \"match_count\": " << match_count << ",\n"
        << "  \"count_overflow\": "
        << (count_overflow ? "true" : "false") << ",\n"
        << "  \"result_digest\": \""
        << jsonEscape(result_digest) << "\",\n"
        << "  \"parallel_statistics\": {\n"
        << "    \"root_tasks_claimed\": "
        << parallel_statistics.root_tasks_claimed << ",\n"
        << "    \"prefix_tasks_spawned\": "
        << parallel_statistics.prefix_tasks_spawned << ",\n"
        << "    \"prefix_split_attempts\": "
        << parallel_statistics.prefix_split_attempts << ",\n"
        << "    \"prefix_split_rejected\": "
        << parallel_statistics.prefix_split_rejected << ",\n"
        << "    \"queue_claims\": "
        << parallel_statistics.queue_claims << ",\n"
        << "    \"queue_peak\": "
        << parallel_statistics.queue_peak << ",\n"
        << "    \"queue_state_bytes_peak\": "
        << parallel_statistics.queue_state_bytes_peak << ",\n"
        << "    \"workers_with_tasks\": "
        << parallel_statistics.workers_with_tasks << ",\n"
        << "    \"worker_tasks_min\": "
        << parallel_statistics.worker_tasks_min << ",\n"
        << "    \"worker_tasks_max\": "
        << parallel_statistics.worker_tasks_max << ",\n"
        << "    \"worker_tasks_total\": "
        << parallel_statistics.worker_tasks_total << ",\n"
        << "    \"worker_dfs_states_min\": "
        << parallel_statistics.worker_dfs_states_min << ",\n"
        << "    \"worker_dfs_states_max\": "
        << parallel_statistics.worker_dfs_states_max << ",\n"
        << "    \"worker_dfs_states_total\": "
        << parallel_statistics.worker_dfs_states_total << ",\n"
        << "    \"worker_active_nanoseconds_min\": "
        << parallel_statistics.worker_active_nanoseconds_min << ",\n"
        << "    \"worker_active_nanoseconds_max\": "
        << parallel_statistics.worker_active_nanoseconds_max << ",\n"
        << "    \"worker_active_nanoseconds_total\": "
        << parallel_statistics.worker_active_nanoseconds_total
        << ",\n"
        << "    \"worker_tasks_cv\": "
        << parallel_statistics.worker_tasks_cv << ",\n"
        << "    \"worker_dfs_states_cv\": "
        << parallel_statistics.worker_dfs_states_cv << ",\n"
        << "    \"worker_active_nanoseconds_cv\": "
        << parallel_statistics.worker_active_nanoseconds_cv
        << ",\n"
        << "    \"worker_utilization\": "
        << parallel_statistics.worker_utilization << ",\n"
        << "    \"task_queue_capacity\": "
        << parallel_statistics.task_queue_capacity << ",\n"
        << "    \"prefix_depth_limit\": "
        << parallel_statistics.prefix_depth_limit << "\n"
        << "  },\n"
        << "  \"input\": {\n"
        << "    \"occurrences\": "
        << temporal_graph.input_occurrence_count << ",\n"
        << "    \"unique_directed_edges\": "
        << temporal_graph.input_unique_edge_count << ",\n"
        << "    \"retained_edges\": "
        << temporal_graph.filtered_edge_count << ",\n"
        << "    \"active_vertices\": "
        << temporal_graph.num_vertices << ",\n"
        << "    \"query_vertices\": "
        << query_graph.num_vertices << "\n"
        << "  },\n"
        << "  \"timings_nanoseconds\": {";
    bool first_timing = true;
    for (const auto& timing : timings_ns) {
        if (!first_timing) metrics_output << ',';
        first_timing = false;
        metrics_output << "\n    \""
                       << jsonEscape(timing.first) << "\": "
                       << timing.second;
    }
    if (!timings_ns.empty()) metrics_output << '\n';
    metrics_output
        << "  },\n"
        << "  \"memory_bytes\": {\n"
        << "    \"graph\": " << graph_memory << ",\n"
        << "    \"td_tree\": " << td_tree_memory << ",\n"
        << "    \"dag\": " << dag_memory << ",\n"
        << "    \"cache\": " << cache_memory_bytes << ",\n"
        << "    \"other\": " << other_memory << ",\n"
        << "    \"peak_rss\": " << peak_memory << "\n"
        << "  },\n"
        << "  \"plan\": {\n"
        << "    \"id\": " << plan.plan_id << ",\n"
        << "    \"root\": " << decomposition.root << ",\n"
        << "    \"order\": [";
    const std::vector<int>& reported_order =
        options.engine == Engine::Gopher
        ? plan.order
        : decomposition.dfs_order;
    for (std::size_t index = 0;
         index < reported_order.size();
         ++index) {
        if (index > 0) metrics_output << ", ";
        metrics_output << reported_order[index];
    }
    metrics_output
        << "]\n"
        << "  },\n"
        << "  \"statistics\": {";
    writeStatisticsJson(
        metrics_output,
        search_statistics,
        options.collect_statistics);
    metrics_output
        << "  },\n"
        << "  \"result_file\": \""
        << jsonEscape(result_file.string()) << "\",\n"
        << "  \"timing_file\": \""
        << jsonEscape(timing_file.string()) << "\"\n"
        << "}\n";
    metrics_output.flush();
    if (!metrics_output.good() || !timing_output.good()) {
        std::cerr
            << "Error: Failed while writing metrics/timing output.\n";
        return 4;
    }

    std::cout
        << "engine: " << engineName(options.engine) << '\n'
        << "mode: " << matchOutputModeName(options.output_mode) << '\n'
        << "label_mode: "
        << labelModeName(options.label_mode) << '\n'
        << "label_seed: " << options.label_seed << '\n'
        << "selectivity_mode: "
        << selectivityModeName(decomposition.selectivity_mode) << '\n'
        << "query_root: " << decomposition.root << '\n'
        << "root_selectivity_score: "
        << std::setprecision(17)
        << decomposition.temporal_selectivity[
               static_cast<std::size_t>(decomposition.root)]
        << '\n'
        << "metric_schema_version: logical_extensions_v1\n"
        << "root_candidate_count: "
        << td_tree.rootCandidates().size() << '\n'
        << "candidate_relation_entries: "
        << td_tree.candidateRelationCount() << '\n'
        << "order: " << orderModeName(options.order_mode) << '\n'
        << "memo: " << memoModeName(options.memo_mode) << '\n'
        << "threads_requested: " << options.threads << '\n'
        << "threads_used: " << threads_used << '\n'
        << "parallel_mode_requested: "
        << parallelModeName(options.parallel_mode) << '\n'
        << "parallel_mode_effective: "
        << parallel_mode_effective << '\n'
        << "prefix_depth_requested: "
        << options.prefix_depth << '\n'
        << "tasks_per_thread: "
        << options.tasks_per_thread << '\n'
        << "early_prune: "
        << (options.early_prune ? "on" : "off") << '\n'
        << "result_digest_enabled: "
        << (options.result_digest ? "on" : "off") << '\n'
        << "statistics_enabled: "
        << (options.collect_statistics ? "on" : "off") << '\n'
        << "cache_budget_bytes: "
        << options.memo_options.budget_bytes << '\n'
        << "match_count: ";
    if (count_overflow) {
        std::cout << "OVERFLOW\n";
    } else {
        std::cout << match_count << '\n';
    }
    std::cout
        << "count_overflow: "
        << (count_overflow ? "true" : "false") << '\n'
        << "result_digest: " << result_digest << '\n'
        << "enumerate_matches_ns: "
        << timings_ns["enumerateMatches"] << '\n'
        << "end_to_end_ns: "
        << timings_ns["endToEnd"] << '\n'
        << "peak_rss_bytes: " << peak_memory << '\n'
        << "result_file: " << result_file.string() << '\n'
        << "timing_file: " << timing_file.string() << '\n'
        << "metrics_file: "
        << options.metrics_file.string() << '\n';
    if (options.engine == Engine::Gopher) {
        std::cout
            << "parallel_prefix_tasks_spawned: "
            << parallel_statistics.prefix_tasks_spawned << '\n'
            << "parallel_queue_peak: "
            << parallel_statistics.queue_peak << '\n'
            << "parallel_queue_state_bytes_peak: "
            << parallel_statistics.queue_state_bytes_peak << '\n'
            << "parallel_workers_with_tasks: "
            << parallel_statistics.workers_with_tasks << '\n'
            << "parallel_worker_tasks_min: "
            << parallel_statistics.worker_tasks_min << '\n'
            << "parallel_worker_tasks_max: "
            << parallel_statistics.worker_tasks_max << '\n'
            << "parallel_worker_dfs_states_min: "
            << parallel_statistics.worker_dfs_states_min << '\n'
            << "parallel_worker_dfs_states_max: "
            << parallel_statistics.worker_dfs_states_max << '\n'
            << "parallel_worker_utilization: "
            << parallel_statistics.worker_utilization << '\n';
        if (options.collect_statistics) {
            writeSearchStatistics(
                std::cout, search_statistics);
        } else {
            std::cout
                << "statistics_collected: false\n";
        }
    }

    if (count_overflow) {
        std::cerr
            << "Error: Match count exceeds uint64_t; search stopped "
               "without wrapping the count.\n";
        return 5;
    }
    return 0;
}
