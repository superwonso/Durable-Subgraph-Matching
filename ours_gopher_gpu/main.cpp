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
#include <vector>

#include "CandidateDag.h"
#include "DagExecutor.h"
#include "GopherMatcher.h"
#include "GpuMatcher.h"
#include "TDTree.h"
#include "Utils.h"
#include "query_decomposition.h"

namespace {

using Clock = std::chrono::steady_clock;

struct CliOptions {
    std::string temporal_graph_file;
    std::string query_graph_file;
    int minimum_duration = 0;
    std::uint32_t label_seed = kDefaultLabelSeed;
    LabelMode label_mode = LabelMode::SequentialRng;
    OrderMode order_mode = OrderMode::Paper;
    SelectivityMode selectivity_mode = SelectivityMode::Paper;
    MemoMode requested_memo = MemoMode::Dag;
    std::size_t threads_requested = 1;
    bool early_prune = true;
    bool result_digest = false;
    bool collect_statistics = true;
    bool verify_cpu = false;
    int gpu_device = 0;
    GpuExecutionVariant gpu_variant =
        GpuExecutionVariant::LevelSynchronous;
    std::size_t gpu_memory_bytes =
        1024ULL * 1024ULL * 1024ULL;
    std::filesystem::path output_directory = ".";
    std::filesystem::path metrics_file;
    bool metrics_file_explicit = false;
};

void printUsage(std::ostream& output, const char* program) {
    output
        << "Usage: " << program
        << " DATA QUERY K [LABEL_SEED] [options]\n\n"
        << "GPU backend options:\n"
        << "  --gpu-device N              HIP device index (default: 0)\n"
        << "  --gpu-variant level|resident\n"
        << "                              Level-sync baseline or device-resident\n"
        << "                              continuation (default: level)\n"
        << "  --gpu-memory-mb N           Frontier workspace cap (default: 1024)\n"
        << "  --verify-cpu                Compare count and two-part digest with CPU\n"
        << "  --backend gpu               Explicit backend selector\n\n"
        << "Common ours_gopher options:\n"
        << "  --count-only                Accepted; GPU output is always count-only\n"
        << "  --label-mode sequential-rng|stable-id\n"
        << "                              Vertex label assignment (default: sequential-rng)\n"
        << "  --order paper|reuse         Fixed query plan (default: paper)\n"
        << "  --selectivity tdtree|paper|intended\n"
        << "                              Selectivity definition (default: paper)\n"
        << "  --memo MODE                 Recorded for compatibility; runtime GPU\n"
        << "                              memo is replaced by fused device execution\n"
        << "  --threads N                 Recorded; CPU preprocessing remains 1 thread\n"
        << "  --early-prune on|off        Cumulative temporal pruning (default: on)\n"
        << "  --result-digest on|off      Mapping XOR+sum digest (default: off)\n"
        << "  --statistics on|off         Detailed GPU counters (default: on)\n"
        << "  --output-dir DIR            Result/timing output directory\n"
        << "  --metrics PATH              Metrics JSON path\n"
        << "  --verify-dag                Compatibility alias for --verify-cpu\n"
        << "  --reuse-aware               Alias for --order reuse\n"
        << "  --no-memo                   Recorded alias for --memo none\n"
        << "  --help                      Show this help\n\n"
        << "The GPU backend has no silent CPU fallback. If a frontier exceeds the\n"
        << "workspace cap, the run fails and asks for a larger cap or CPU backend.\n";
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

bool parsePositiveInt(const std::string& text, int& value) {
    std::uint64_t parsed = 0;
    if (!parseUnsigned(text, parsed) ||
        parsed > static_cast<std::uint64_t>(
            std::numeric_limits<int>::max())) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool megabytesToBytes(
    const std::string& text,
    std::size_t& value) {
    std::uint64_t megabytes = 0;
    if (!parseUnsigned(text, megabytes)) return false;
    constexpr std::uint64_t scale = 1024ULL * 1024ULL;
    if (megabytes >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max()) /
            scale) {
        return false;
    }
    value = static_cast<std::size_t>(
        megabytes * scale);
    return true;
}

bool parseOnOff(const std::string& text, bool& value) {
    if (text == "on") {
        value = true;
        return true;
    }
    if (text == "off") {
        value = false;
        return true;
    }
    return false;
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
        const std::string duration = argv[3];
        std::size_t parsed = 0;
        options.minimum_duration =
            std::stoi(duration, &parsed);
        if (parsed != duration.size()) {
            throw std::invalid_argument("duration");
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
    for (int index = 4; index < argc; ++index) {
        const std::string argument = argv[index];
        auto requireValue = [&](const char* option) -> const char* {
            if (index + 1 >= argc) {
                std::cerr << "Error: " << option
                          << " requires a value.\n";
                return nullptr;
            }
            return argv[++index];
        };

        if (argument == "--help") {
            printUsage(std::cout, argv[0]);
            return false;
        }
        if (argument == "--count-only") continue;
        if (argument == "--verify-cpu" ||
            argument == "--verify-dag") {
            options.verify_cpu = true;
            continue;
        }
        if (argument == "--reuse-aware") {
            options.order_mode = OrderMode::ReuseAware;
            continue;
        }
        if (argument == "--no-memo") {
            options.requested_memo = MemoMode::None;
            continue;
        }
        if (argument == "--no-cost-aware") {
            // CPU cache admission is intentionally absent on this backend.
            continue;
        }
        if (argument == "--label-mode") {
            const char* value =
                requireValue("--label-mode");
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
        if (argument == "--backend" ||
            argument == "--engine") {
            const char* value =
                requireValue(argument.c_str());
            if (value == nullptr) return false;
            const std::string text = value;
            if (text != "gpu" &&
                text != "gopher_gpu" &&
                text != "ours_gopher_gpu") {
                std::cerr
                    << "Error: this executable supports only the GPU backend.\n";
                return false;
            }
            continue;
        }
        if (argument == "--order") {
            const char* value = requireValue("--order");
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
        if (argument == "--selectivity") {
            const char* value =
                requireValue("--selectivity");
            if (value == nullptr) return false;
            const std::string text = value;
            if (text == "tdtree") {
                options.selectivity_mode =
                    SelectivityMode::TDTree;
            } else if (text == "paper") {
                options.selectivity_mode =
                    SelectivityMode::Paper;
            } else if (text == "intended") {
                options.selectivity_mode =
                    SelectivityMode::Intended;
            } else {
                std::cerr
                    << "Error: --selectivity must be tdtree, paper, "
                       "or intended.\n";
                return false;
            }
            continue;
        }
        if (argument == "--memo") {
            const char* value = requireValue("--memo");
            if (value == nullptr ||
                !parseMemoMode(
                    value, options.requested_memo)) {
                std::cerr
                    << "Error: --memo must be none, structural, temporal, "
                       "or dag.\n";
                return false;
            }
            continue;
        }
        if (argument == "--threads") {
            const char* value =
                requireValue("--threads");
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
            options.threads_requested =
                static_cast<std::size_t>(parsed);
            continue;
        }
        if (argument == "--early-prune") {
            const char* value =
                requireValue("--early-prune");
            if (value == nullptr ||
                !parseOnOff(value, options.early_prune)) {
                std::cerr
                    << "Error: --early-prune must be on or off.\n";
                return false;
            }
            continue;
        }
        if (argument == "--result-digest") {
            const char* value =
                requireValue("--result-digest");
            if (value == nullptr ||
                !parseOnOff(
                    value, options.result_digest)) {
                std::cerr
                    << "Error: --result-digest must be on or off.\n";
                return false;
            }
            continue;
        }
        if (argument == "--statistics") {
            const char* value =
                requireValue("--statistics");
            if (value == nullptr ||
                !parseOnOff(
                    value, options.collect_statistics)) {
                std::cerr
                    << "Error: --statistics must be on or off.\n";
                return false;
            }
            continue;
        }
        if (argument == "--gpu-device") {
            const char* value =
                requireValue("--gpu-device");
            if (value == nullptr ||
                !parsePositiveInt(
                    value, options.gpu_device)) {
                std::cerr
                    << "Error: --gpu-device must be a non-negative integer.\n";
                return false;
            }
            continue;
        }
        if (argument == "--gpu-variant") {
            const char* value =
                requireValue("--gpu-variant");
            if (value == nullptr ||
                !parseGpuExecutionVariant(
                    value,
                    options.gpu_variant)) {
                std::cerr
                    << "Error: --gpu-variant must be "
                       "level or resident.\n";
                return false;
            }
            continue;
        }
        if (argument == "--gpu-memory-mb") {
            const char* value =
                requireValue("--gpu-memory-mb");
            if (value == nullptr ||
                !megabytesToBytes(
                    value, options.gpu_memory_bytes) ||
                options.gpu_memory_bytes == 0) {
                std::cerr
                    << "Error: --gpu-memory-mb must be a positive integer.\n";
                return false;
            }
            continue;
        }
        if (argument == "--output-dir") {
            const char* value =
                requireValue("--output-dir");
            if (value == nullptr ||
                std::string(value).empty()) {
                std::cerr
                    << "Error: --output-dir cannot be empty.\n";
                return false;
            }
            options.output_directory = value;
            continue;
        }
        if (argument == "--metrics") {
            const char* value =
                requireValue("--metrics");
            if (value == nullptr ||
                std::string(value).empty()) {
                std::cerr
                    << "Error: --metrics cannot be empty.\n";
                return false;
            }
            options.metrics_file = value;
            options.metrics_file_explicit = true;
            continue;
        }
        if (argument == "--cache-mb" ||
            argument == "--cache-entry-mb" ||
            argument == "--cache-min-work") {
            if (requireValue(argument.c_str()) == nullptr) {
                return false;
            }
            continue;
        }
        if (argument.rfind("--", 0) == 0) {
            std::cerr
                << "Error: Unknown option: "
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
            seed >
                std::numeric_limits<std::uint32_t>::max()) {
            std::cerr
                << "Error: label seed must be a 32-bit unsigned integer.\n";
            return false;
        }
        options.label_seed =
            static_cast<std::uint32_t>(seed);
        seed_seen = true;
    }
    return true;
}

std::uint64_t elapsedNanoseconds(Clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start).count());
}

std::string jsonEscape(const std::string& text) {
    std::ostringstream output;
    for (char raw_character : text) {
        const unsigned char character =
            static_cast<unsigned char>(raw_character);
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
                    output
                        << "\\u"
                        << std::hex << std::setw(4)
                        << std::setfill('0')
                        << static_cast<int>(character)
                        << std::dec << std::setfill(' ');
                } else {
                    output
                        << static_cast<char>(character);
                }
        }
    }
    return output.str();
}

bool ensureParentDirectory(
    const std::filesystem::path& path) {
    const std::filesystem::path parent =
        path.parent_path();
    if (parent.empty()) return true;
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (!error) return true;
    std::cerr
        << "Error: Cannot create directory "
        << parent.string() << ": "
        << error.message() << '\n';
    return false;
}

const char* selectivityModeNameLocal(SelectivityMode mode) {
    return selectivityModeName(mode);
}

void writeTiming(
    std::ostream& output,
    const std::map<std::string, std::uint64_t>& timings) {
    for (const auto& item : timings) {
        output << item.first << "_ns: "
               << item.second << '\n'
               << item.first << "_ms: "
               << std::fixed << std::setprecision(6)
               << static_cast<double>(item.second) /
                      1'000'000.0
               << '\n';
    }
}

} // namespace

int main(int argc, char* argv[]) {
    CliOptions options;
    if (!parseCli(argc, argv, options)) {
        return argc == 2 &&
               std::string(argv[1]) == "--help"
            ? 0
            : 1;
    }

    std::error_code directory_error;
    std::filesystem::create_directories(
        options.output_directory, directory_error);
    if (directory_error) {
        std::cerr
            << "Error: Cannot create output directory "
            << options.output_directory.string()
            << ": " << directory_error.message()
            << '\n';
        return 1;
    }

    const std::string dataset_name =
        std::filesystem::path(
            options.temporal_graph_file).stem().string();
    const std::filesystem::path result_file =
        options.output_directory /
        ("matching_results_" +
         dataset_name + ".txt");
    const std::filesystem::path timing_file =
        options.output_directory /
        ("timing_results_" +
         dataset_name + ".txt");
    if (!options.metrics_file_explicit) {
        options.metrics_file =
            options.output_directory /
            ("metrics_results_" +
             dataset_name + ".json");
    }
    if (!ensureParentDirectory(options.metrics_file)) {
        return 1;
    }

    const auto total_start = Clock::now();
    std::map<std::string, std::uint64_t> timings;

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
    timings["readAndFilterTemporalGraph"] =
        elapsedNanoseconds(stage_start);
    timings["readTemporalGraph"] =
        static_cast<std::uint64_t>(
            std::max(
                0LL,
                load_timings.read_milliseconds)) *
        1'000'000ULL;
    timings["filterTemporalGraph"] =
        static_cast<std::uint64_t>(
            std::max(
                0LL,
                load_timings.filter_milliseconds)) *
        1'000'000ULL;

    Graph query_graph;
    stage_start = Clock::now();
    if (!readQueryGraph(
            options.query_graph_file,
            query_graph)) {
        return 2;
    }
    timings["readQueryGraph"] =
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
            temporal_graph
                .vertex_active_durations[vertex];
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
    timings["labelStatistics"] =
        elapsedNanoseconds(stage_start);

    stage_start = Clock::now();
    const QueryDecomposition decomposition =
        decomposeQuery(
            query_graph,
            label_vertex_counts,
            label_average_lifespans,
            options.selectivity_mode);
    timings["queryDecomposition"] =
        elapsedNanoseconds(stage_start);
    if (decomposition.root < 0 ||
        !decomposition.connected) {
        std::cerr
            << "Error: Query graph must be connected and have a root.\n";
        return 3;
    }

    stage_start = Clock::now();
    TDTree td_tree(
        temporal_graph,
        query_graph,
        decomposition,
        options.minimum_duration);
    const std::size_t root_candidate_count =
        td_tree.rootCandidates().size();
    const std::size_t candidate_relation_entries =
        td_tree.candidateRelationCount();
    timings["buildTDTree"] =
        elapsedNanoseconds(stage_start);

    stage_start = Clock::now();
    const QueryPlan plan = makeQueryPlan(
        query_graph,
        decomposition,
        td_tree,
        options.order_mode);
    timings["buildQueryPlan"] =
        elapsedNanoseconds(stage_start);

    stage_start = Clock::now();
    CandidateDagCompiler compiler;
    const CandidateDag dag =
        compiler.compile(
            query_graph,
            plan,
            decomposition);
    timings["compileCandidateDag"] =
        elapsedNanoseconds(stage_start);
    timings["hostPreprocess"] =
        elapsedNanoseconds(total_start);

    GpuMatcherOptions gpu_options;
    gpu_options.device = options.gpu_device;
    gpu_options.frontier_budget_bytes =
        options.gpu_memory_bytes;
    gpu_options.early_prune =
        options.early_prune;
    gpu_options.result_digest =
        options.result_digest ||
        options.verify_cpu;
    gpu_options.collect_statistics =
        options.collect_statistics;
    gpu_options.variant =
        options.gpu_variant;

    GpuMatchSummary gpu_summary;
    try {
        GpuMatcher matcher(
            temporal_graph,
            query_graph,
            plan,
            dag,
            td_tree,
            options.minimum_duration,
            gpu_options);
        gpu_summary = matcher.run();
    } catch (const std::exception& error) {
        std::cerr
            << "Error: GPU execution failed: "
            << error.what() << '\n';
        return 5;
    }

    timings["flattenGpuData"] =
        gpu_summary.metrics.flatten_nanoseconds;
    timings["gpuH2D"] =
        gpu_summary.metrics.h2d_nanoseconds;
    timings["gpuKernel"] =
        gpu_summary.metrics.kernel_nanoseconds;
    timings["gpuD2H"] =
        gpu_summary.metrics.d2h_nanoseconds;
    timings["gpuAllocation"] =
        gpu_summary.metrics
            .device_allocation_nanoseconds;
    timings["gpuSearch"] =
        gpu_summary.metrics.gpu_search_nanoseconds;
    timings["gpuReadyToResult"] =
        gpu_summary.metrics
            .gpu_ready_to_result_nanoseconds;
    timings["enumerateMatches"] =
        gpu_summary.metrics
            .gpu_ready_to_result_nanoseconds;

    bool cpu_verification_passed = false;
    std::uint64_t cpu_verification_count = 0;
    std::string cpu_verification_digest = "not_run";
    if (options.verify_cpu) {
        stage_start = Clock::now();
        GopherMatcherOptions cpu_options;
        cpu_options.threads = 1;
        cpu_options.order_mode =
            options.order_mode;
        cpu_options.early_prune =
            options.early_prune;
        cpu_options.result_digest = true;
        cpu_options.executor.memo_mode =
            MemoMode::None;
        cpu_options.executor.collect_statistics =
            false;
        GopherMatcher cpu_matcher(
            temporal_graph,
            query_graph,
            plan,
            dag,
            td_tree,
            options.minimum_duration,
            cpu_options);
        const GopherMatchSummary cpu_summary =
            cpu_matcher.run(MatchOutputMode::CountOnly);
        timings["verifyCpu"] =
            elapsedNanoseconds(stage_start);
        cpu_verification_count =
            cpu_summary.match_count;
        cpu_verification_digest =
            cpu_summary.result_digest;
        cpu_verification_passed =
            !cpu_summary.count_overflow &&
            !gpu_summary.count_overflow &&
            cpu_summary.match_count ==
                gpu_summary.match_count &&
            cpu_summary.result_digest ==
                gpu_summary.result_digest;
        if (!cpu_verification_passed) {
            std::cerr
                << "Error: CPU/GPU verification failed: CPU count="
                << cpu_summary.match_count
                << " digest="
                << cpu_summary.result_digest
                << ", GPU count="
                << gpu_summary.match_count
                << " digest="
                << gpu_summary.result_digest
                << '\n';
            return 6;
        }
    }

    timings["endToEnd"] =
        elapsedNanoseconds(total_start);

    const GpuMatcherMetrics& metrics =
        gpu_summary.metrics;
    const auto optionalStatistic =
        [&](std::uint64_t value) {
            return metrics.statistics_collected
                ? std::to_string(value)
                : std::string("null");
        };

    std::ofstream result_output(
        result_file, std::ios::binary);
    if (!result_output.is_open()) {
        std::cerr
            << "Error: Could not write "
            << result_file.string() << '\n';
        return 4;
    }
    result_output
        << "[Run]\n"
        << "engine: gopher_gpu\n"
        << "backend: gpu\n"
        << "mode: count-only\n"
        << "label_mode: "
        << labelModeName(options.label_mode) << '\n'
        << "label_seed: " << options.label_seed << '\n'
        << "gpu_variant: "
        << gpuExecutionVariantName(
               options.gpu_variant)
        << '\n'
        << "order: "
        << orderModeName(options.order_mode)
        << '\n'
        << "selectivity: "
        << selectivityModeNameLocal(
               options.selectivity_mode)
        << '\n'
        << "query_root: " << plan.root << '\n'
        << "root_selectivity_score: "
        << std::setprecision(17)
        << plan.root_selectivity_score << '\n'
        << "memo_requested: "
        << memoModeName(options.requested_memo)
        << '\n'
        << "memo_effective: device_fused_no_runtime_memo\n"
        << "threads_requested: "
        << options.threads_requested << '\n'
        << "cpu_preprocess_threads: 1\n"
        << "gpu_threads: hardware\n"
        << "fallback_used: false\n"
        << "device: "
        << gpu_summary.metrics.device_name << '\n'
        << "architecture: "
        << gpu_summary.metrics.architecture << '\n'
        << "plan_id: " << plan.plan_id << '\n'
        << "plan_order:";
    for (int query_vertex : plan.order) {
        result_output << ' ' << query_vertex;
    }
    result_output
        << "\n\n[Selectivity Summary]\n";
    td_tree.writeSelectivitySummary(result_output);
    result_output
        << "\n[Candidate Summary]\n";
    td_tree.writeCandidateSummary(result_output);
    result_output
        << "\n[Final Matches]\n"
        << "Count: ";
    if (gpu_summary.count_overflow) {
        result_output << "OVERFLOW\n";
    } else {
        result_output
            << gpu_summary.match_count << '\n';
    }
    result_output
        << "result_digest: "
        << gpu_summary.result_digest
        << "\n\n[Statistics]\n"
        << "metric_schema_version: logical_extensions_v1\n"
        << "root_candidate_count: "
        << root_candidate_count << '\n'
        << "candidate_relation_entries: "
        << candidate_relation_entries << '\n'
        << "raw_extension_candidates: "
        << optionalStatistic(
               gpu_summary.metrics.generated_candidates)
        << '\n'
        << "post_structural_candidates: "
        << optionalStatistic(
               gpu_summary.metrics.generated_candidates)
        << '\n'
        << "accepted_extensions: "
        << optionalStatistic(
               gpu_summary.metrics.accepted_candidates)
        << '\n';
    result_output.flush();

    std::ofstream timing_output(timing_file);
    if (!timing_output.is_open()) {
        std::cerr
            << "Error: Could not write "
            << timing_file.string() << '\n';
        return 4;
    }
    timing_output
        << "engine: gopher_gpu\n"
        << "backend: gpu\n"
        << "mode: count-only\n"
        << "label_mode: "
        << labelModeName(options.label_mode) << '\n'
        << "label_seed: " << options.label_seed << '\n'
        << "gpu_variant: "
        << gpuExecutionVariantName(
               options.gpu_variant)
        << '\n'
        << "order: "
        << orderModeName(options.order_mode)
        << '\n'
        << "selectivity: "
        << selectivityModeNameLocal(
               options.selectivity_mode)
        << '\n'
        << "query_root: " << plan.root << '\n'
        << "root_selectivity_score: "
        << std::setprecision(17)
        << plan.root_selectivity_score << '\n'
        << "memo_requested: "
        << memoModeName(options.requested_memo)
        << '\n'
        << "memo_effective: device_fused_no_runtime_memo\n"
        << "threads_requested: "
        << options.threads_requested << '\n'
        << "threads_used: hardware\n"
        << "cpu_preprocess_threads: 1\n"
        << "early_prune: "
        << (options.early_prune ? "on" : "off")
        << '\n'
        << "statistics_enabled: "
        << (options.collect_statistics
                ? "on"
                : "off")
        << '\n'
        << "fallback_used: false\n";
    timing_output << "plan_order:";
    for (int query_vertex : plan.order) {
        timing_output << ' ' << query_vertex;
    }
    timing_output
        << "\nmetric_schema_version: logical_extensions_v1\n"
        << "root_candidate_count: "
        << root_candidate_count << '\n'
        << "candidate_relation_entries: "
        << candidate_relation_entries << '\n';
    writeTiming(timing_output, timings);
    timing_output
        << "gpu_device_name: "
        << gpu_summary.metrics.device_name << '\n'
        << "gpu_architecture: "
        << gpu_summary.metrics.architecture << '\n'
        << "gpu_kernel_launches: "
        << gpu_summary.metrics.kernel_launches
        << '\n'
        << "gpu_depth_host_round_trips: "
        << gpu_summary.metrics
               .depth_host_round_trips
        << '\n'
        << "gpu_device_synchronizations: "
        << gpu_summary.metrics
               .device_synchronizations
        << '\n'
        << "gpu_launch_sync_overhead_ns: "
        << gpu_summary.metrics
               .launch_and_sync_overhead_nanoseconds
        << '\n'
        << "gpu_peak_frontier_states: "
        << gpu_summary.metrics.peak_frontier_states
        << '\n'
        << "gpu_peak_frontier_device_bytes: "
        << gpu_summary.metrics
               .peak_frontier_device_bytes
        << '\n'
        << "gpu_persistent_device_bytes: "
        << gpu_summary.metrics
               .persistent_device_bytes
        << '\n'
        << "gpu_h2d_bytes: "
        << gpu_summary.metrics.h2d_bytes << '\n'
        << "gpu_d2h_bytes: "
        << gpu_summary.metrics.d2h_bytes << '\n'
        << "raw_extension_candidates: "
        << optionalStatistic(
               gpu_summary.metrics.generated_candidates)
        << '\n'
        << "post_structural_candidates: "
        << optionalStatistic(
               gpu_summary.metrics.generated_candidates)
        << '\n'
        << "generated_candidates: "
        << optionalStatistic(
               gpu_summary.metrics.generated_candidates)
        << '\n'
        << "accepted_candidates: "
        << optionalStatistic(
               gpu_summary.metrics.accepted_candidates)
        << '\n'
        << "accepted_extensions: "
        << optionalStatistic(
               gpu_summary.metrics.accepted_candidates)
        << '\n'
        << "pruned_injectivity: "
        << optionalStatistic(
               gpu_summary.metrics.pruned_injectivity)
        << '\n'
        << "pruned_structural: "
        << optionalStatistic(
               gpu_summary.metrics.pruned_structural)
        << '\n'
        << "pruned_temporal: "
        << optionalStatistic(
               gpu_summary.metrics.pruned_temporal)
        << '\n'
        << "result_digest: "
        << gpu_summary.result_digest << '\n';
    timing_output.flush();

    std::ofstream metrics_output(
        options.metrics_file);
    if (!metrics_output.is_open()) {
        std::cerr
            << "Error: Could not write "
            << options.metrics_file.string()
            << '\n';
        return 4;
    }
    metrics_output
        << "{\n"
        << "  \"schema_version\": 2,\n"
        << "  \"engine\": \"gopher_gpu\",\n"
        << "  \"backend\": \"gpu\",\n"
        << "  \"gpu_variant\": \""
        << gpuExecutionVariantName(
               options.gpu_variant)
        << "\",\n"
        << "  \"fallback_used\": false,\n"
        << "  \"mode\": \"count-only\",\n"
        << "  \"label_mode\": \""
        << labelModeName(options.label_mode) << "\",\n"
        << "  \"metric_schema_version\": "
           "\"logical_extensions_v1\",\n"
        << "  \"root_candidate_count\": "
        << root_candidate_count << ",\n"
        << "  \"candidate_relation_entries\": "
        << candidate_relation_entries << ",\n"
        << "  \"order\": \""
        << orderModeName(options.order_mode)
        << "\",\n"
        << "  \"selectivity\": \""
        << selectivityModeNameLocal(
               options.selectivity_mode)
        << "\",\n"
        << "  \"root_selectivity_score\": "
        << std::setprecision(17)
        << plan.root_selectivity_score << ",\n"
        << "  \"memo_requested\": \""
        << memoModeName(options.requested_memo)
        << "\",\n"
        << "  \"memo_effective\": "
           "\"device_fused_no_runtime_memo\",\n"
        << "  \"threads_requested\": "
        << options.threads_requested << ",\n"
        << "  \"cpu_preprocess_threads\": 1,\n"
        << "  \"gpu_threads\": \"hardware\",\n"
        << "  \"early_prune\": "
        << (options.early_prune
                ? "true"
                : "false")
        << ",\n"
        << "  \"statistics_enabled\": "
        << (options.collect_statistics
                ? "true"
                : "false")
        << ",\n"
        << "  \"minimum_duration\": "
        << options.minimum_duration << ",\n"
        << "  \"label_seed\": "
        << options.label_seed << ",\n"
        << "  \"match_count\": "
        << gpu_summary.match_count << ",\n"
        << "  \"count_overflow\": "
        << (gpu_summary.count_overflow
                ? "true"
                : "false")
        << ",\n"
        << "  \"result_digest\": \""
        << jsonEscape(gpu_summary.result_digest)
        << "\",\n"
        << "  \"host_preprocess_ms\": "
        << static_cast<double>(
               timings["hostPreprocess"]) /
               1'000'000.0
        << ",\n"
        << "  \"flatten_gpu_data_ms\": "
        << static_cast<double>(
               metrics.flatten_nanoseconds) /
               1'000'000.0
        << ",\n"
        << "  \"gpu_h2d_ms\": "
        << static_cast<double>(
               metrics.h2d_nanoseconds) /
               1'000'000.0
        << ",\n"
        << "  \"gpu_kernel_ms\": "
        << static_cast<double>(
               metrics.kernel_nanoseconds) /
               1'000'000.0
        << ",\n"
        << "  \"gpu_launch_sync_overhead_ms\": "
        << static_cast<double>(
               metrics
                   .launch_and_sync_overhead_nanoseconds) /
               1'000'000.0
        << ",\n"
        << "  \"gpu_depth_round_trip_ms\": "
        << static_cast<double>(
               metrics
                   .depth_host_round_trip_nanoseconds) /
               1'000'000.0
        << ",\n"
        << "  \"gpu_allocation_ms\": "
        << static_cast<double>(
               metrics
                   .device_allocation_nanoseconds) /
               1'000'000.0
        << ",\n"
        << "  \"gpu_d2h_ms\": "
        << static_cast<double>(
               metrics.d2h_nanoseconds) /
               1'000'000.0
        << ",\n"
        << "  \"gpu_search_ms\": "
        << static_cast<double>(
               metrics.gpu_search_nanoseconds) /
               1'000'000.0
        << ",\n"
        << "  \"gpu_ready_to_result_ms\": "
        << static_cast<double>(
               metrics
                   .gpu_ready_to_result_nanoseconds) /
               1'000'000.0
        << ",\n"
        << "  \"end_to_end_ms\": "
        << static_cast<double>(
               timings["endToEnd"]) /
               1'000'000.0
        << ",\n"
        << "  \"verification\": {\n"
        << "    \"requested\": "
        << (options.verify_cpu
                ? "true"
                : "false")
        << ",\n"
        << "    \"passed\": "
        << (cpu_verification_passed
                ? "true"
                : "false")
        << ",\n"
        << "    \"cpu_count\": "
        << cpu_verification_count << ",\n"
        << "    \"cpu_digest\": \""
        << jsonEscape(cpu_verification_digest)
        << "\"\n"
        << "  },\n"
        << "  \"device\": {\n"
        << "    \"index\": "
        << options.gpu_device << ",\n"
        << "    \"name\": \""
        << jsonEscape(metrics.device_name)
        << "\",\n"
        << "    \"architecture\": \""
        << jsonEscape(metrics.architecture)
        << "\",\n"
        << "    \"compute_units\": "
        << metrics.compute_units << ",\n"
        << "    \"hip_multiprocessor_count\": "
        << metrics.compute_units << ",\n"
        << "    \"compute_units_source\": "
           "\"hipDeviceProp_t.multiProcessorCount"
           " (WGP count on gfx11)\",\n"
        << "    \"warp_size\": "
        << metrics.warp_size << ",\n"
        << "    \"block_size\": "
        << metrics.block_size << ",\n"
        << "    \"hip_runtime_version\": "
        << metrics.hip_runtime_version << ",\n"
        << "    \"hip_driver_version\": "
        << metrics.hip_driver_version << ",\n"
        << "    \"global_memory_bytes\": "
        << metrics.device_global_memory_bytes
        << "\n"
        << "  },\n"
        << "  \"gpu\": {\n"
        << "    \"frontier_budget_bytes\": "
        << options.gpu_memory_bytes << ",\n"
        << "    \"persistent_device_bytes\": "
        << metrics.persistent_device_bytes
        << ",\n"
        << "    \"peak_frontier_device_bytes\": "
        << metrics.peak_frontier_device_bytes
        << ",\n"
        << "    \"peak_frontier_states\": "
        << metrics.peak_frontier_states << ",\n"
        << "    \"frontier_levels\": "
        << metrics.frontier_levels << ",\n"
        << "    \"kernel_launches\": "
        << metrics.kernel_launches << ",\n"
        << "    \"depth_host_round_trips\": "
        << metrics.depth_host_round_trips
        << ",\n"
        << "    \"depth_host_round_trip_ns\": "
        << metrics.depth_host_round_trip_nanoseconds
        << ",\n"
        << "    \"depth_count_d2h_bytes\": "
        << metrics.depth_count_d2h_bytes
        << ",\n"
        << "    \"device_synchronizations\": "
        << metrics.device_synchronizations
        << ",\n"
        << "    \"frontier_validation_passes\": "
        << metrics.frontier_validation_passes
        << ",\n"
        << "    \"frontier_materialization_passes\": "
        << metrics.frontier_materialization_passes
        << ",\n"
        << "    \"launch_and_sync_overhead_ns\": "
        << metrics.launch_and_sync_overhead_nanoseconds
        << ",\n"
        << "    \"resident_capacity_states\": "
        << metrics.resident_capacity_states
        << ",\n"
        << "    \"resident_grid_blocks\": "
        << metrics.resident_grid_blocks
        << ",\n"
        << "    \"occupancy_blocks_per_multiprocessor\": "
        << metrics.occupancy_blocks_per_multiprocessor
        << ",\n"
        << "    \"device_allocation_ns\": "
        << metrics.device_allocation_nanoseconds
        << ",\n"
        << "    \"h2d_bytes\": "
        << metrics.h2d_bytes << ",\n"
        << "    \"d2h_bytes\": "
        << metrics.d2h_bytes << ",\n"
        << "    \"dag_nodes_lowered\": "
        << metrics.dag_nodes_lowered << "\n"
        << "  },\n"
        << "  \"depths\": [";
    for (std::size_t index = 0;
         index < metrics.depths.size();
         ++index) {
        const GpuDepthMetrics& depth =
            metrics.depths[index];
        const double mean_candidates =
            depth.input_states == 0
            ? 0.0
            : static_cast<double>(
                  depth.generated_candidates) /
              static_cast<double>(
                  depth.input_states);
        const double maximum_to_mean =
            mean_candidates == 0.0
            ? 0.0
            : static_cast<double>(
                  depth.maximum_candidates_per_state) /
              mean_candidates;
        if (index != 0) metrics_output << ',';
        metrics_output
            << "\n    {\"depth\": "
            << depth.depth
            << ", \"input_states\": "
            << depth.input_states
            << ", \"output_states\": "
            << depth.output_states
            << ", \"workload_metrics_available\": "
            << (metrics.statistics_collected
                    ? "true"
                    : "false")
            << ", \"generated_candidates\": ";
        if (metrics.statistics_collected) {
            metrics_output
                << depth.generated_candidates;
        } else {
            metrics_output << "null";
        }
        metrics_output
            << ", \"maximum_candidates_per_state\": ";
        if (metrics.statistics_collected) {
            metrics_output
                << depth.maximum_candidates_per_state;
        } else {
            metrics_output << "null";
        }
        metrics_output
            << ", \"mean_candidates_per_state\": ";
        if (metrics.statistics_collected) {
            metrics_output << mean_candidates;
        } else {
            metrics_output << "null";
        }
        metrics_output
            << ", \"maximum_to_mean_load_ratio\": ";
        if (metrics.statistics_collected) {
            metrics_output << maximum_to_mean;
        } else {
            metrics_output << "null";
        }
        metrics_output
            << ", \"kernel_timing_available\": "
            << (options.gpu_variant ==
                        GpuExecutionVariant::
                            LevelSynchronous
                    ? "true"
                    : "false")
            << ", \"kernel_ns\": ";
        if (options.gpu_variant ==
            GpuExecutionVariant::LevelSynchronous) {
            metrics_output << depth.kernel_nanoseconds;
        } else {
            metrics_output << "null";
        }
        metrics_output
            << ", \"host_round_trip_ns\": "
            << depth.host_round_trip_nanoseconds
            << ", \"grid_blocks\": "
            << depth.grid_blocks
            << '}';
    }
    if (!metrics.depths.empty()) {
        metrics_output << '\n';
    }
    metrics_output
        << "  ],\n"
        << "  \"statistics\": {\n"
        << "    \"raw_extension_candidates\": "
        << optionalStatistic(
               metrics.generated_candidates) << ",\n"
        << "    \"post_structural_candidates\": "
        << optionalStatistic(
               metrics.generated_candidates) << ",\n"
        << "    \"generated_candidates\": "
        << optionalStatistic(
               metrics.generated_candidates) << ",\n"
        << "    \"accepted_candidates\": "
        << optionalStatistic(
               metrics.accepted_candidates) << ",\n"
        << "    \"accepted_extensions\": "
        << optionalStatistic(
               metrics.accepted_candidates) << ",\n"
        << "    \"pruned_injectivity\": "
        << optionalStatistic(
               metrics.pruned_injectivity) << ",\n"
        << "    \"pruned_structural\": "
        << optionalStatistic(
               metrics.pruned_structural) << ",\n"
        << "    \"pruned_temporal\": "
        << optionalStatistic(
               metrics.pruned_temporal) << ",\n"
        << "    \"temporal_interval_steps\": "
        << optionalStatistic(
               metrics.temporal_interval_steps)
        << ",\n"
        << "    \"directed_edge_lookups\": "
        << optionalStatistic(
               metrics.directed_edge_lookups)
        << "\n"
        << "  },\n"
        << "  \"timings_nanoseconds\": {";
    bool first_timing = true;
    for (const auto& timing : timings) {
        if (!first_timing) metrics_output << ',';
        first_timing = false;
        metrics_output
            << "\n    \""
            << jsonEscape(timing.first)
            << "\": " << timing.second;
    }
    if (!timings.empty()) metrics_output << '\n';
    metrics_output
        << "  },\n"
        << "  \"input\": {\n"
        << "    \"data_file\": \""
        << jsonEscape(
               options.temporal_graph_file)
        << "\",\n"
        << "    \"query_file\": \""
        << jsonEscape(options.query_graph_file)
        << "\",\n"
        << "    \"occurrences\": "
        << temporal_graph.input_occurrence_count
        << ",\n"
        << "    \"unique_directed_edges\": "
        << temporal_graph.input_unique_edge_count
        << ",\n"
        << "    \"retained_edges\": "
        << temporal_graph.filtered_edge_count
        << ",\n"
        << "    \"active_vertices\": "
        << temporal_graph.num_vertices << ",\n"
        << "    \"query_vertices\": "
        << query_graph.num_vertices << "\n"
        << "  },\n"
        << "  \"plan\": {\n"
        << "    \"id\": " << plan.plan_id
        << ",\n"
        << "    \"root\": " << plan.root
        << ",\n"
        << "    \"root_selectivity_score\": "
        << std::setprecision(17)
        << plan.root_selectivity_score << ",\n"
        << "    \"order\": [";
    for (std::size_t index = 0;
         index < plan.order.size();
         ++index) {
        if (index != 0) metrics_output << ", ";
        metrics_output << plan.order[index];
    }
    metrics_output
        << "]\n"
        << "  },\n"
        << "  \"result_file\": \""
        << jsonEscape(result_file.string())
        << "\",\n"
        << "  \"timing_file\": \""
        << jsonEscape(timing_file.string())
        << "\"\n"
        << "}\n";
    metrics_output.flush();

    if (!result_output.good() ||
        !timing_output.good() ||
        !metrics_output.good()) {
        std::cerr
            << "Error: Failed while writing GPU outputs.\n";
        return 4;
    }

    std::cout
        << "Temporal graph: "
        << temporal_graph.input_occurrence_count
        << " occurrences, "
        << temporal_graph.input_unique_edge_count
        << " unique directed edges, "
        << temporal_graph.filtered_edge_count
        << " retained edges, "
        << temporal_graph.num_vertices
        << " active vertices\n"
        << "Query root: q" << plan.root
        << " | DFS order:";
    for (int query_vertex : plan.order) {
        std::cout << ' ' << query_vertex;
    }
    std::cout
        << "\nengine: gopher_gpu\n"
        << "backend: gpu\n"
        << "label_mode: "
        << labelModeName(options.label_mode) << '\n'
        << "label_seed: " << options.label_seed << '\n'
        << "gpu_variant: "
        << gpuExecutionVariantName(
               options.gpu_variant)
        << '\n'
        << "device: " << metrics.device_name
        << '\n'
        << "architecture: "
        << metrics.architecture << '\n'
        << "order: "
        << orderModeName(options.order_mode)
        << '\n'
        << "selectivity: "
        << selectivityModeNameLocal(
               options.selectivity_mode)
        << '\n'
        << "query_root: " << plan.root << '\n'
        << "root_selectivity_score: "
        << std::setprecision(17)
        << plan.root_selectivity_score << '\n'
        << "memo_requested: "
        << memoModeName(options.requested_memo)
        << '\n'
        << "memo_effective: "
           "device_fused_no_runtime_memo\n"
        << "threads_requested: "
        << options.threads_requested << '\n'
        << "threads_used: hardware\n"
        << "cpu_preprocess_threads: 1\n"
        << "fallback_used: false\n"
        << "statistics_enabled: "
        << (options.collect_statistics
                ? "on"
                : "off")
        << '\n'
        << "metric_schema_version: logical_extensions_v1\n"
        << "root_candidate_count: "
        << root_candidate_count << '\n'
        << "candidate_relation_entries: "
        << candidate_relation_entries << '\n'
        << "raw_extension_candidates: "
        << optionalStatistic(
               metrics.generated_candidates) << '\n'
        << "post_structural_candidates: "
        << optionalStatistic(
               metrics.generated_candidates) << '\n'
        << "generated_candidates: "
        << optionalStatistic(
               metrics.generated_candidates) << '\n'
        << "accepted_candidates: "
        << optionalStatistic(
               metrics.accepted_candidates) << '\n'
        << "accepted_extensions: "
        << optionalStatistic(
               metrics.accepted_candidates) << '\n'
        << "pruned_injectivity: "
        << optionalStatistic(
               metrics.pruned_injectivity) << '\n'
        << "pruned_structural: "
        << optionalStatistic(
               metrics.pruned_structural) << '\n'
        << "pruned_temporal: "
        << optionalStatistic(
               metrics.pruned_temporal) << '\n'
        << "gpu_kernel_ns: "
        << metrics.kernel_nanoseconds << '\n'
        << "gpu_h2d_ns: "
        << metrics.h2d_nanoseconds << '\n'
        << "gpu_d2h_ns: "
        << metrics.d2h_nanoseconds << '\n'
        << "gpu_depth_host_round_trips: "
        << metrics.depth_host_round_trips
        << '\n'
        << "gpu_device_synchronizations: "
        << metrics.device_synchronizations
        << '\n'
        << "gpu_launch_sync_overhead_ns: "
        << metrics.launch_and_sync_overhead_nanoseconds
        << '\n'
        << "gpu_ready_to_result_ns: "
        << metrics.gpu_ready_to_result_nanoseconds
        << '\n'
        << "result_digest: "
        << gpu_summary.result_digest << '\n'
        << "cpu_verification: "
        << (options.verify_cpu
                ? (cpu_verification_passed
                       ? "pass"
                       : "fail")
                : "not_run")
        << '\n'
        << "Final durable matches: ";
    if (gpu_summary.count_overflow) {
        std::cout << "OVERFLOW\n";
    } else {
        std::cout
            << gpu_summary.match_count << '\n';
    }
    std::cout
        << "match_count: "
        << gpu_summary.match_count << '\n'
        << "enumerate_matches_ns: "
        << timings["enumerateMatches"]
        << '\n'
        << "end_to_end_ns: "
        << timings["endToEnd"] << '\n'
        << "Results saved to: "
        << result_file.string() << '\n'
        << "Timing saved to: "
        << timing_file.string() << '\n'
        << "Metrics saved to: "
        << options.metrics_file.string()
        << '\n';
    return 0;
}
