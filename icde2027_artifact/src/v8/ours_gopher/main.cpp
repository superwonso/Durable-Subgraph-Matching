#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
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
#include "CandidateDagPasses.h"
#include "OperatorCounters.h"
#include "DagExecutor.h"
#include "ExperimentContract.h"
#include "GopherMatcher.h"
#include "JIndexSimulation.h"
#include "WindowProgram.h"
#include "WStoreFile.h"
#include "LabelProjection.h"
#include "SearchStatistics.h"
#include "TDTree.h"
#include "Utils.h"
#include "query_decomposition.h"
#include "temporal_cost_planner.h"

namespace {

using Clock = std::chrono::steady_clock;

enum class Engine {
    Gopher,
    Legacy
};

const char* engineName(Engine engine) {
    return engine == Engine::Legacy ? "legacy" : "gopher";
}

bool gopherBuildIdentityBound() {
    return std::string(kGopherSourceIdentity) != "unbound" &&
        std::string(kGopherCommonFlagsIdentity) != "unbound";
}

struct CliOptions {
    DagPasses dag_passes;
    double physical_alpha = 1.0;
    bool direct_opt = false;
    bool window_algebra = false;
    bool raw_bitset = false;
    bool window_lazy = false;
    bool direct_p4 = false;
    bool jindex_simulate = false;
    JIndexMode jindex_mode = JIndexMode::None;
    double jindex_alpha = 1.0;
    std::size_t jindex_block_size = 16;
    std::string wstore_path;
    BIndexMode bindex_mode = BIndexMode::None;
    double bindex_alpha = 0.5;
    std::size_t bindex_block_size = 16;
    std::size_t bindex_min_size = 64;
    std::size_t bindex_reuse_threshold = 2;
    bool bindex_verify_superset = false;
    bool ops_counters = false;
    bool adaptive_diagnostics = false;
    bool neighbor_copy_timing = false;
    OrderSimulationMode order_simulation = OrderSimulationMode::Disabled;
    bool lmax_filter = false;
    std::string dump_dag;
    std::string temporal_graph_file;
    std::string query_graph_file;
    int minimum_duration = 0;
    std::uint32_t label_seed = kDefaultLabelSeed;
    LabelMode label_mode = LabelMode::SequentialRng;
    std::filesystem::path global_label_map_file;
    std::filesystem::path query_label_projection_file;
    Engine engine = Engine::Gopher;
    OrderMode order_mode = OrderMode::Paper;
    SelectivityMode selectivity_mode = SelectivityMode::Paper;
    TemporalCostPlannerOptions temporal_cost_options;
    MemoMode memo_mode = MemoMode::Dag;
    MemoPolicy memo_policy = MemoPolicy::Legacy;
    RuntimeKeyMode runtime_key_mode = RuntimeKeyMode::RequiredBindings;
    RuntimeKeyDiagnosticsMode runtime_key_diagnostics =
        RuntimeKeyDiagnosticsMode::Off;
    std::uint64_t exact_runtime_key_cap = 0;
    MemoOptions memo_options;
    ExperimentVariant experiment_variant = ExperimentVariant::DurableDFS;
    bool experiment_variant_explicit = true;
    std::size_t threads = 1;
    ParallelMode parallel_mode = ParallelMode::Roots;
    std::size_t prefix_depth = 0;
    std::size_t tasks_per_thread = 8;
    MatchOutputMode output_mode = MatchOutputMode::Full;
    bool verify_dag = false;
    bool early_prune = true;
    bool result_digest = false;
    bool correctness_preflight = false;
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
        << "  V6 executable: Direct DFS only; no DAG execution arms.\n"
        << "  --wstore PATH              Read selected k section; strict actual input SHA check\n"
        << "  --bindex off|always|adaptive  Candidate-block index (default: off)\n"
        << "  --bindex-alpha 0.25|0.5|1  Duplicate-inclusive threshold (default: 0.5)\n"
        << "  --bindex-c 1|2             Block uses before construction (default: 2)\n"
        << "  --bindex-block-size 8|16|32  Main B=16; other values counters only\n"
        << "  --bindex-min-size 64       Minimum R size (fixed: 64)\n"
        << "  --bindex-verify-superset   Correctness-only candidate completeness assertion\n"
        << "  --dag-passes none|p1,p2,p3,p4,p5,p6  Compiler rewrites (default: none; no memo)\n"
        << "  --physical-alpha 0.5|1|2    P5 fixed state-cost multiplier (default: 1)\n"
        << "  --direct-opt                Apply selected passes manually to Direct DFS\n"
        << "  --direct-p4 on|off         Plain Direct edge-probe/handle control (default: off)\n"
        << "  --window-algebra on|off    Exact k-window support lowering (default: off)\n"
        << "  --raw-bitset on|off        V8 raw T(e) store plus shift-AND run check (default: off)\n"
        << "  --window-build eager|lazy  Query-k window preparation (default: eager)\n"
        << "  --jindex-simulate          A2 shadow index observer; does not prune S1\n"
        << "  --jindex off|adaptive|always  Production P8 (default: off; requires W/P4)\n"
        << "  --jindex-alpha 0.5|1|2     Frozen state-cost sensitivity (default: 1)\n"
        << "  --jindex-block-size 16|32|64|128  Original-axis window block width (default: 16)\n"
        << "  --dag-lowering off         P9 unavailable: G4 is INDETERMINATE\n"
        << "  --ops-counters on|off       Untimed operation census (default: off)\n"
        << "  --adaptive-diagnostics on|off Stage-A state sizes and shadow funnel (default: off)\n"
        << "  --neighbor-copy-timing on|off Diagnostic copy-loop timer (default: off)\n"
        << "  --order-sim static|dyn-min-cand|dyn-min-cand-j\n"
        << "                               Direct-only logical-work diagnostic (default: disabled/static search)\n"
        << "  --lmax-filter on|off        k-dependent edge filter (default: off/F2)\n"
        << "  --dump-dag PATH             Write before/after DOT and node counts\n"
        << "  --label-mode sequential-rng|stable-id\n"
        << "                               Vertex label assignment (default: sequential-rng)\n"
        << "  --global-label-map PATH      Versioned explicit global vertex-to-class map\n"
        << "  --query-label-projection PATH\n"
        << "                               Query-bound bijection from selected global classes to A-E\n"
        << "  --engine gopher|legacy       Search engine (default: gopher)\n"
        << "  --selectivity tdtree|paper|intended|legacy-reciprocal|legacy-extracted-product|structural|survival|temporal-cost|temporal-cost-v2\n"
        << "                               Root-score interpretation (default: paper)\n"
        << "  --tcc-beam-width N           TCC beam width (default: 64)\n"
        << "  --tcc-sample-cap N            TCC rescore sample cap (default: 4096)\n"
        << "  --tcc-max-catalog-probes N    V2 deterministic catalog budget (default: 50000000)\n"
        << "  --tcc-max-state-expansions N  V2 deterministic state budget (default: 4096)\n"
        << "  --tcc-max-extension-probes N  V2 deterministic extension budget (default: 1000000)\n"
        << "  --tcc-max-interval-intersections N V2 interval-merge budget (default: 10000000)\n"
        << "  --tcc-pilot-extension-probes N V2 pilot extension cap (default: 4096)\n"
        << "  --tcc-pilot-prefix-cap N      V2 bottom-hash prefix cap (default: 64)\n"
        << "  --tcc-rescore-top-l N         V2 catalogue winners rescored (default: 2)\n"
        << "  --tcc-max-candidate-trees N   V2 candidate-tree build budget (default: 2)\n"
        << "  --tcc-opportunity-margin F    V2 global H=1 margin fraction (default: 0.05)\n"
        << "  --tcc-catalog-probe-cost-weight F V2 global catalog work weight (default: 1.0)\n"
        << "  --tcc-cost-gate on|off       V2 development cost gate (default: on)\n"
        << "  --order paper|reuse          Fixed query plan (default: paper)\n"
        << "  --memo none|structural|temporal|dag\n"
        << "                               DAG memoization ablation (default: dag)\n"
        << "  --memo-policy legacy|structural-v2\n"
        << "                               Memo execution policy (default: legacy)\n"
        << "  --memo-key full-prefix|required-bindings\n"
        << "                               Runtime binding projection (default: required-bindings)\n"
        << "  --runtime-key-diagnostics off|exact\n"
        << "                               Equality-safe key census for diagnostic-only runs (default: off)\n"
        << "  --exact-runtime-key-cap N    Required positive distinct-key cap in exact mode\n"
        << "  --experiment-variant durable-dfs|dag-no-memo|dag-full-prefix-memo|dag-dep-key-memo|\n"
        << "                       dag-no-intern-no-memo|dag-intern-no-memo|\n"
        << "                       dag-intern-full-prefix-memo|dag-intern-dep-key-memo\n"
        << "                               Closed Candidate-DAG arm; E2 static-canonicalization variants additionally fix paper selectivity, one thread, early pruning, and 256/8/64 MiB/cache-shard controls\n"
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
        << "  --correctness-preflight      Permit digest-on only for an untimed closed-arm correctness check\n"
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
    bool global_label_map_seen = false;
    bool query_label_projection_seen = false;
    bool count_only_seen = false;
    bool engine_seen = false;
    bool selectivity_seen = false;
    bool order_seen = false;
    bool memo_seen = false;
    bool memo_policy_seen = false;
    bool memo_key_seen = false;
    bool runtime_key_diagnostics_seen = false;
    bool exact_runtime_key_cap_seen = false;
    bool experiment_variant_seen = false;
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
    bool tcc_beam_seen = false;
    bool tcc_sample_seen = false;
    bool tcc_catalog_seen = false;
    bool tcc_state_seen = false;
    bool tcc_extension_seen = false;
    bool tcc_interval_seen = false;
    bool tcc_pilot_extension_seen = false;
    bool tcc_pilot_prefix_seen = false;
    bool tcc_rescore_top_l_seen = false;
    bool tcc_max_candidate_trees_seen = false;
    bool tcc_margin_seen = false;
    bool tcc_catalog_weight_seen = false;
    bool tcc_cost_gate_seen = false;
    bool adaptive_diagnostics_seen = false;
    bool neighbor_copy_timing_seen = false;
    bool order_simulation_seen = false;
    bool physical_alpha_seen = false;
    bool window_algebra_seen = false;
    bool raw_bitset_seen = false;
    bool window_build_seen = false;
    bool direct_p4_seen = false;
    bool jindex_simulate_seen = false;
    bool jindex_seen = false, jindex_alpha_seen = false, jindex_block_seen = false;
    std::unordered_map<std::string, bool> v6_seen;

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
    auto parseSize = [](
                         const char* text,
                         std::size_t& target,
                         bool positive) {
        std::uint64_t parsed = 0;
        if (text == nullptr || !parseUnsigned(text, parsed) ||
            (positive && parsed == 0) ||
            parsed > std::numeric_limits<std::size_t>::max()) {
            return false;
        }
        target = static_cast<std::size_t>(parsed);
        return true;
    };
    auto parseNonnegativeDouble = [](
                                      const char* text,
                                      double& target) {
        if (text == nullptr) return false;
        try {
            const std::string value = text;
            std::size_t parsed = 0;
            const double number = std::stod(value, &parsed);
            if (parsed != value.size() || !std::isfinite(number) ||
                number < 0.0) {
                return false;
            }
            target = number;
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };

    for (int index = 4; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--raw-bitset") {
            if (!rejectDuplicate(raw_bitset_seen, argument)) return false;
            const char* value = requireValue(index, argument);
            if (!value || (std::string(value) != "on" && std::string(value) != "off")) {
                std::cerr << "Error: --raw-bitset requires on or off.\n";
                return false;
            }
            options.raw_bitset = std::string(value) == "on";
            continue;
        }
        if (argument == "--wstore" || argument == "--bindex" ||
            argument == "--bindex-alpha" || argument == "--bindex-c" ||
            argument == "--bindex-block-size" || argument == "--bindex-min-size" ||
            argument == "--bindex-verify-superset") {
            if (!rejectDuplicate(v6_seen[argument], argument)) return false;
            if (argument == "--bindex-verify-superset") {
                options.bindex_verify_superset = true;
                continue;
            }
            const char* value = requireValue(index, argument);
            if (!value) return false;
            const std::string name(value);
            if (argument == "--wstore") options.wstore_path = name;
            else if (argument == "--bindex") {
                if (name == "off") options.bindex_mode = BIndexMode::None;
                else if (name == "always") options.bindex_mode = BIndexMode::Always;
                else if (name == "adaptive") options.bindex_mode = BIndexMode::Adaptive;
                else { std::cerr << "Error: --bindex requires off, always or adaptive.\n"; return false; }
            } else if (argument == "--bindex-alpha") {
                if (!parseNonnegativeDouble(value, options.bindex_alpha) ||
                    (options.bindex_alpha != 0.25 && options.bindex_alpha != 0.5 && options.bindex_alpha != 1.0)) {
                    std::cerr << "Error: v6 alpha must be 0.25, 0.5 or 1; alpha=2 is excluded.\n"; return false;
                }
            } else if (argument == "--bindex-c") {
                if ((name != "1" && name != "2") || !parseSize(value, options.bindex_reuse_threshold, true)) {
                    std::cerr << "Error: --bindex-c requires 1 or 2.\n"; return false;
                }
            } else if (argument == "--bindex-block-size") {
                if ((name != "8" && name != "16" && name != "32") || !parseSize(value, options.bindex_block_size, true)) {
                    std::cerr << "Error: --bindex-block-size requires 8, 16 or 32.\n"; return false;
                }
            } else if (name != "64") {
                std::cerr << "Error: v6 N_min is fixed at 64.\n"; return false;
            }
            continue;
        }
        if (argument == "--jindex") {
            if (!rejectDuplicate(jindex_seen, argument)) return false;
            const char* value = requireValue(index, argument);
            if (!value) return false;
            const std::string name(value);
            if (name == "off") options.jindex_mode = JIndexMode::None;
            else if (name == "adaptive") options.jindex_mode = JIndexMode::Adaptive;
            else if (name == "always") options.jindex_mode = JIndexMode::Always;
            else { std::cerr << "Error: --jindex requires off, adaptive or always.\n"; return false; }
            continue;
        }
        if (argument == "--jindex-alpha") {
            if (!rejectDuplicate(jindex_alpha_seen, argument)) return false;
            const char* value = requireValue(index, argument);
            if (!value || !parseNonnegativeDouble(value, options.jindex_alpha) ||
                (options.jindex_alpha != 0.5 && options.jindex_alpha != 1.0 && options.jindex_alpha != 2.0)) {
                std::cerr << "Error: --jindex-alpha requires 0.5, 1 or 2.\n"; return false;
            }
            continue;
        }
        if (argument == "--jindex-block-size") {
            if (!rejectDuplicate(jindex_block_seen, argument)) return false;
            const char* value = requireValue(index, argument);
            const std::string text = value ? value : "";
            if (text != "8" && text != "16" && text != "32" && text != "64" && text != "128") {
                std::cerr << "Error: --jindex-block-size requires 8, 16, 32, 64 or 128.\n"; return false;
            }
            options.jindex_block_size = static_cast<std::size_t>(std::stoul(text));
            continue;
        }
        if (argument == "--jindex-simulate") {
            if (!rejectDuplicate(jindex_simulate_seen, argument)) return false;
            options.jindex_simulate = true;
            continue;
        }
        if (argument == "--dag-lowering") {
            const char* value = requireValue(index, argument);
            if (value == nullptr || std::string(value) != "off") {
                std::cerr << "Error: P9 unavailable: G4 kernel-inclusive profiling is INDETERMINATE; only off is supported.\n";
                return false;
            }
            continue;
        }
        if (argument == "--window-algebra" || argument == "--direct-p4") {
            bool& seen = argument == "--window-algebra" ? window_algebra_seen : direct_p4_seen;
            if (!rejectDuplicate(seen, argument)) return false;
            const char* value = requireValue(index, argument);
            if (value == nullptr || (std::string(value) != "on" && std::string(value) != "off")) {
                std::cerr << "Error: " << argument << " requires on or off.\n";
                return false;
            }
            (argument == "--window-algebra" ? options.window_algebra : options.direct_p4) =
                std::string(value) == "on";
            continue;
        }
        if (argument == "--window-build") {
            if (!rejectDuplicate(window_build_seen, argument)) return false;
            const char* value = requireValue(index, argument);
            if (value == nullptr || (std::string(value) != "eager" && std::string(value) != "lazy")) {
                std::cerr << "Error: --window-build requires eager or lazy.\n";
                return false;
            }
            options.window_lazy = std::string(value) == "lazy";
            continue;
        }
        if (argument == "--adaptive-diagnostics" || argument == "--neighbor-copy-timing") {
            bool& seen = argument == "--adaptive-diagnostics" ? adaptive_diagnostics_seen : neighbor_copy_timing_seen;
            if (!rejectDuplicate(seen, argument)) return false;
            const char* value = requireValue(index, argument);
            if (value == nullptr || (std::string(value) != "on" && std::string(value) != "off")) {
                std::cerr << "Error: " << argument << " requires on or off.\n";
                return false;
            }
            (argument == "--adaptive-diagnostics" ? options.adaptive_diagnostics : options.neighbor_copy_timing) = std::string(value) == "on";
            continue;
        }
        if (argument == "--order-sim") {
            if (!rejectDuplicate(order_simulation_seen, argument)) return false;
            const char* value = requireValue(index, argument);
            if (value == nullptr || !parseOrderSimulationMode(value, options.order_simulation) ||
                options.order_simulation == OrderSimulationMode::Disabled) {
                std::cerr << "Error: --order-sim requires static, dyn-min-cand, or dyn-min-cand-j.\n";
                return false;
            }
            continue;
        }
        if (argument == "--dag-passes" || argument.rfind("--dag-passes=", 0) == 0) {
            const char* value = argument == "--dag-passes"
                ? requireValue(index, argument) : argument.c_str() + 13;
            if (value == nullptr) return false;
            try {
                options.dag_passes = parseDagPasses(value);
            } catch (const std::exception&) {
                std::cerr << "Error: --dag-passes accepts none or a comma-separated subset of p1,p2,p3,p4,p5,p6.\n";
                return false;
            }
            continue;
        }
        if (argument == "--physical-alpha" || argument.rfind("--physical-alpha=", 0) == 0) {
            if (!rejectDuplicate(physical_alpha_seen, "--physical-alpha")) return false;
            const char* value = argument == "--physical-alpha"
                ? requireValue(index, argument) : argument.c_str() + 17;
            if (!parseNonnegativeDouble(value, options.physical_alpha) ||
                (options.physical_alpha != 0.5 && options.physical_alpha != 1.0 &&
                 options.physical_alpha != 2.0)) {
                std::cerr << "Error: --physical-alpha accepts only the preregistered 0.5, 1, or 2.\n";
                return false;
            }
            continue;
        }
        if (argument == "--direct-opt") {
            options.direct_opt = true;
            continue;
        }
        if (argument == "--ops-counters" || argument == "--lmax-filter") {
            const char* value = requireValue(index, argument);
            if (value == nullptr || (std::string(value) != "on" && std::string(value) != "off")) {
                std::cerr << "Error: " << argument << " requires on or off.\n";
                return false;
            }
            (argument == "--ops-counters" ? options.ops_counters : options.lmax_filter) = std::string(value) == "on";
            continue;
        }
        if (argument == "--dump-dag") {
            const char* value = requireValue(index, argument);
            if (value == nullptr || std::string(value).empty()) return false;
            options.dump_dag = value;
            continue;
        }
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
        if (argument == "--global-label-map") {
            if (!rejectDuplicate(
                    global_label_map_seen, "--global-label-map")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr || std::string(value).empty()) {
                std::cerr << "Error: --global-label-map cannot be empty.\n";
                return false;
            }
            options.global_label_map_file = value;
            continue;
        }
        if (argument == "--query-label-projection") {
            if (!rejectDuplicate(
                    query_label_projection_seen,
                    "--query-label-projection")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr || std::string(value).empty()) {
                std::cerr
                    << "Error: --query-label-projection cannot be empty.\n";
                return false;
            }
            options.query_label_projection_file = value;
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
                    << "Error: --selectivity must be tdtree, paper, intended, "
                       "legacy-reciprocal, legacy-extracted-product, structural, survival, "
                       "temporal-cost, or temporal-cost-v2.\n";
                return false;
            }
            continue;
        }
        if (argument == "--tcc-beam-width" ||
            argument == "--tcc-sample-cap" ||
            argument == "--tcc-max-catalog-probes" ||
            argument == "--tcc-max-state-expansions" ||
            argument == "--tcc-max-extension-probes" ||
            argument == "--tcc-max-interval-intersections" ||
            argument == "--tcc-pilot-extension-probes" ||
            argument == "--tcc-pilot-prefix-cap" ||
            argument == "--tcc-rescore-top-l" ||
            argument == "--tcc-max-candidate-trees" ||
            argument == "--tcc-opportunity-margin" ||
            argument == "--tcc-catalog-probe-cost-weight") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return false;
            bool* seen = nullptr;
            bool parsed = false;
            if (argument == "--tcc-beam-width") {
                seen = &tcc_beam_seen;
                parsed = parseSize(
                    value,
                    options.temporal_cost_options.beam_width,
                    true);
            } else if (argument == "--tcc-sample-cap") {
                seen = &tcc_sample_seen;
                parsed = parseSize(
                    value,
                    options.temporal_cost_options.sample_cap,
                    true);
            } else if (argument == "--tcc-max-catalog-probes") {
                seen = &tcc_catalog_seen;
                parsed = parseUnsigned(
                    value,
                    options.temporal_cost_options.max_catalog_probes);
            } else if (argument == "--tcc-max-state-expansions") {
                seen = &tcc_state_seen;
                parsed = parseUnsigned(
                    value,
                    options.temporal_cost_options.max_state_expansions);
            } else if (argument == "--tcc-max-extension-probes") {
                seen = &tcc_extension_seen;
                parsed = parseUnsigned(
                    value,
                    options.temporal_cost_options.max_extension_probes);
            } else if (argument ==
                       "--tcc-max-interval-intersections") {
                seen = &tcc_interval_seen;
                parsed = parseUnsigned(
                    value,
                    options.temporal_cost_options
                        .max_interval_intersections);
            } else if (argument == "--tcc-pilot-extension-probes") {
                seen = &tcc_pilot_extension_seen;
                parsed = parseUnsigned(
                    value,
                    options.temporal_cost_options
                        .pilot_extension_probe_cap);
            } else if (argument == "--tcc-pilot-prefix-cap") {
                seen = &tcc_pilot_prefix_seen;
                parsed = parseSize(
                    value,
                    options.temporal_cost_options.pilot_prefix_cap,
                    true);
            } else if (argument == "--tcc-rescore-top-l") {
                seen = &tcc_rescore_top_l_seen;
                parsed = parseSize(
                    value,
                    options.temporal_cost_options.rescore_top_l,
                    true);
            } else if (argument == "--tcc-max-candidate-trees") {
                seen = &tcc_max_candidate_trees_seen;
                parsed = parseUnsigned(
                    value,
                    options.temporal_cost_options
                        .max_candidate_trees_built);
            } else if (argument == "--tcc-opportunity-margin") {
                seen = &tcc_margin_seen;
                parsed = parseNonnegativeDouble(
                    value,
                    options.temporal_cost_options
                        .opportunity_margin_fraction);
            } else {
                seen = &tcc_catalog_weight_seen;
                parsed = parseNonnegativeDouble(
                    value,
                    options.temporal_cost_options
                        .catalog_probe_cost_weight);
            }
            if (!rejectDuplicate(*seen, argument)) return false;
            if (!parsed) {
                std::cerr << "Error: " << argument
                          << " has an invalid numeric value.\n";
                return false;
            }
            continue;
        }
        if (argument == "--tcc-cost-gate") {
            if (!rejectDuplicate(
                    tcc_cost_gate_seen, "--tcc-cost-gate")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr) return false;
            const std::string mode = value;
            if (mode == "on") {
                options.temporal_cost_options.cost_gate_enabled = true;
            } else if (mode == "off") {
                options.temporal_cost_options.cost_gate_enabled = false;
            } else {
                std::cerr
                    << "Error: --tcc-cost-gate must be on or off.\n";
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
        if (argument == "--memo-policy") {
            if (!rejectDuplicate(
                    memo_policy_seen, "--memo-policy")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr ||
                !parseMemoPolicy(value, options.memo_policy)) {
                std::cerr
                    << "Error: --memo-policy must be legacy or "
                       "structural-v2.\n";
                return false;
            }
            continue;
        }
        if (argument == "--memo-key") {
            if (!rejectDuplicate(memo_key_seen, "--memo-key")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr ||
                !parseRuntimeKeyMode(value, options.runtime_key_mode)) {
                std::cerr
                    << "Error: --memo-key must be full-prefix or "
                       "required-bindings.\n";
                return false;
            }
            continue;
        }
        if (argument == "--runtime-key-diagnostics") {
            if (!rejectDuplicate(
                    runtime_key_diagnostics_seen,
                    "--runtime-key-diagnostics")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr ||
                !parseRuntimeKeyDiagnosticsMode(
                    value, options.runtime_key_diagnostics)) {
                std::cerr
                    << "Error: --runtime-key-diagnostics must be off or "
                       "exact.\n";
                return false;
            }
            continue;
        }
        if (argument == "--exact-runtime-key-cap") {
            if (!rejectDuplicate(
                    exact_runtime_key_cap_seen,
                    "--exact-runtime-key-cap")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr ||
                !parseUnsigned(value, options.exact_runtime_key_cap) ||
                options.exact_runtime_key_cap == 0) {
                std::cerr
                    << "Error: --exact-runtime-key-cap must be a positive "
                       "integer.\n";
                return false;
            }
            continue;
        }
        if (argument == "--experiment-variant") {
            if (!rejectDuplicate(
                    experiment_variant_seen,
                    "--experiment-variant")) {
                return false;
            }
            const char* value = requireValue(index, argument);
            if (value == nullptr ||
                !parseExperimentVariant(
                    value, options.experiment_variant)) {
                std::cerr
                    << "Error: --experiment-variant must be durable-dfs, "
                       "dag-no-memo, dag-full-prefix-memo, or "
                       "dag-dep-key-memo, or one of the four closed "
                       "dag-(no-)intern E2 variants.\n";
                return false;
            }
            options.experiment_variant_explicit = true;
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
        if (argument == "--correctness-preflight") {
            if (!rejectDuplicate(
                    options.correctness_preflight,
                    "--correctness-preflight")) {
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
    const bool tcc_mode =
        options.selectivity_mode == SelectivityMode::TemporalCost ||
        options.selectivity_mode == SelectivityMode::TemporalCostV2;
    if (global_label_map_seen != query_label_projection_seen) {
        std::cerr
            << "Error: --global-label-map and --query-label-projection must "
               "be specified together.\n";
        return false;
    }
    if (global_label_map_seen) {
        if (label_mode_seen || seed_seen) {
            std::cerr
                << "Error: explicit label artifacts cannot be combined with "
                   "--label-mode or a positional label seed.\n";
            return false;
        }
        options.label_mode = LabelMode::ExplicitProjection;
    }
    if ((tcc_beam_seen || tcc_sample_seen) && !tcc_mode) {
        std::cerr
            << "Error: TCC beam/sample controls require temporal-cost or "
               "temporal-cost-v2.\n";
        return false;
    }
    if ((tcc_catalog_seen || tcc_state_seen || tcc_extension_seen ||
         tcc_interval_seen ||
         tcc_pilot_extension_seen || tcc_pilot_prefix_seen ||
         tcc_rescore_top_l_seen || tcc_max_candidate_trees_seen ||
         tcc_margin_seen || tcc_catalog_weight_seen ||
         tcc_cost_gate_seen) &&
        options.selectivity_mode != SelectivityMode::TemporalCostV2) {
        std::cerr
            << "Error: deterministic TCC gate/budget controls require "
               "temporal-cost-v2.\n";
        return false;
    }
    if (options.correctness_preflight &&
        !options.experiment_variant_explicit) {
        std::cerr
            << "Error: --correctness-preflight requires an explicit "
               "--experiment-variant.\n";
        return false;
    }
    if (options.experiment_variant_explicit) {
        const bool static_canonicalization =
            experimentVariantIsStaticCanonicalization(
                options.experiment_variant);
        if (engine_seen && options.engine != Engine::Gopher) {
            std::cerr
                << "Error: --experiment-variant requires --engine gopher.\n";
            return false;
        }
        if (order_seen && options.order_mode != OrderMode::Paper) {
            std::cerr
                << "Error: --experiment-variant fixes --order paper.\n";
            return false;
        }
        if (parallel_seen &&
            options.parallel_mode != ParallelMode::Roots) {
            std::cerr
                << "Error: --experiment-variant fixes --parallel roots.\n";
            return false;
        }
        if (memo_seen || memo_key_seen) {
            std::cerr
                << "Error: --experiment-variant defines memo and key mode; "
                   "do not also pass --memo, --no-memo, or --memo-key.\n";
            return false;
        }
        if (memo_policy_seen &&
            options.memo_policy != MemoPolicy::Legacy) {
            std::cerr
                << "Error: --experiment-variant fixes --memo-policy legacy.\n";
            return false;
        }
        if (cache_work_seen &&
            options.memo_options.minimum_estimated_work != 0) {
            std::cerr
                << "Error: --experiment-variant uses non-cost-aware eager "
                   "admission; --cache-min-work must be 0 if specified.\n";
            return false;
        }
        if (static_canonicalization) {
            if (selectivity_seen &&
                options.selectivity_mode != SelectivityMode::Paper) {
                std::cerr
                    << "Error: static-canonicalization variants fix "
                       "--selectivity paper.\n";
                return false;
            }
            if (threads_seen && options.threads != 1) {
                std::cerr
                    << "Error: static-canonicalization variants fix "
                       "--threads 1.\n";
                return false;
            }
            if (early_prune_seen && !options.early_prune) {
                std::cerr
                    << "Error: static-canonicalization variants fix "
                       "--early-prune on.\n";
                return false;
            }
            if (cache_seen &&
                options.memo_options.budget_bytes != (256ULL << 20)) {
                std::cerr
                    << "Error: static-canonicalization variants fix "
                       "--cache-mb 256.\n";
                return false;
            }
            if (cache_entry_seen &&
                options.memo_options.max_entry_bytes != (8ULL << 20)) {
                std::cerr
                    << "Error: static-canonicalization variants fix "
                       "--cache-entry-mb 8.\n";
                return false;
            }
        }
        if (prefix_depth_seen || tasks_per_thread_seen) {
            std::cerr
                << "Error: prefix scheduler controls are not part of the "
                   "root-scheduled experiment contract.\n";
            return false;
        }
        if (verify_seen) {
            std::cerr
                << "Error: --verify-dag is an untimed correctness option and "
                   "is not part of the experiment timing contract.\n";
            return false;
        }
        if (options.correctness_preflight &&
            (!result_digest_seen || !options.result_digest)) {
            std::cerr
                << "Error: --correctness-preflight requires "
                   "--result-digest on.\n";
            return false;
        }
        if (!options.correctness_preflight &&
            result_digest_seen && options.result_digest) {
            std::cerr
                << "Error: result digests are outside the primary experiment "
                   "timing contract; use --correctness-preflight for an "
                   "untimed digest check.\n";
            return false;
        }
        if (options.selectivity_mode == SelectivityMode::TemporalCostV2) {
            std::cerr
                << "Error: temporal-cost-v2 currently builds and hands off a "
                   "TD-Tree inside planning, so its phases cannot satisfy the "
                   "prepared-query timing contract.\n";
            return false;
        }

        options.engine = Engine::Gopher;
        options.order_mode = OrderMode::Paper;
        options.parallel_mode = ParallelMode::Roots;
        options.output_mode = MatchOutputMode::CountOnly;
        options.memo_policy = MemoPolicy::Legacy;
        options.memo_options.cost_aware_admission = false;
        options.memo_options.minimum_estimated_work = 0;
        if (static_canonicalization) {
            options.selectivity_mode = SelectivityMode::Paper;
            options.threads = 1;
            options.early_prune = true;
            options.memo_options.budget_bytes = 256ULL << 20;
            options.memo_options.max_entry_bytes = 8ULL << 20;
            options.memo_options.shard_count = 64;
        }
        if (!options.correctness_preflight) {
            options.result_digest = false;
        }
        if (!statistics_seen) options.collect_statistics = false;

        switch (options.experiment_variant) {
            case ExperimentVariant::DurableDFS:
            case ExperimentVariant::DagNoMemo:
            case ExperimentVariant::DagNoInternNoMemo:
            case ExperimentVariant::DagInternNoMemo:
                options.memo_mode = MemoMode::None;
                options.runtime_key_mode =
                    RuntimeKeyMode::RequiredBindings;
                break;
            case ExperimentVariant::DagFullPrefixMemo:
            case ExperimentVariant::DagInternFullPrefixMemo:
                options.memo_mode = MemoMode::Dag;
                options.runtime_key_mode = RuntimeKeyMode::FullPrefix;
                break;
            case ExperimentVariant::DagDepKeyMemo:
            case ExperimentVariant::DagInternDepKeyMemo:
                options.memo_mode = MemoMode::Dag;
                options.runtime_key_mode =
                    RuntimeKeyMode::RequiredBindings;
                break;
            case ExperimentVariant::Legacy:
                break;
        }
    }
    if (options.jindex_simulate &&
        (options.window_algebra || options.direct_p4 || options.direct_opt || options.jindex_mode != JIndexMode::None ||
         !options.experiment_variant_explicit ||
         options.experiment_variant != ExperimentVariant::DagNoMemo ||
         !options.dag_passes.p4 || !options.dag_passes.p5 || !options.dag_passes.p6 ||
         options.dag_passes.temporalRewriteAny() ||
         options.memo_mode != MemoMode::None || options.collect_statistics ||
         options.parallel_mode != ParallelMode::Roots || !options.early_prune ||
         options.order_mode != OrderMode::Paper || options.adaptive_diagnostics ||
         options.neighbor_copy_timing || options.order_simulation != OrderSimulationMode::Disabled)) {
        std::cerr << "Error: --jindex-simulate observes unmodified S1 only: DAG-NoMemo P4/P5/P6, roots/paper, new transformations off.\n";
        return false;
    }
    if (options.window_lazy && !options.window_algebra) {
        std::cerr << "Error: --window-build lazy requires --window-algebra on.\n";
        return false;
    }
    if (options.raw_bitset &&
        (!options.direct_p4 || !options.window_algebra || options.window_lazy ||
         options.wstore_path.empty() || options.bindex_mode != BIndexMode::None ||
         options.jindex_mode != JIndexMode::None || options.lmax_filter)) {
        std::cerr << "Error: raw-bitset requires Direct P4, eager loaded k=1 store, unchanged F2 and indexes off.\n";
        return false;
    }
    if ((!options.wstore_path.empty() || options.bindex_mode != BIndexMode::None) &&
        (!options.window_algebra || !options.direct_p4 || options.window_lazy || options.lmax_filter)) {
        std::cerr << "Error: W-store/B-index requires Direct P4 + eager W and the unchanged F2 input.\n";
        return false;
    }
    if (options.bindex_mode != BIndexMode::None && options.jindex_mode != JIndexMode::None) {
        std::cerr << "Error: adjacency and candidate-block indices are separate arms.\n";
        return false;
    }
    if (options.jindex_mode != JIndexMode::None &&
        (!options.window_algebra || (options.direct_p4 && options.jindex_mode != JIndexMode::Always))) {
        std::cerr << "Error: P8 requires W; the plain Direct P4 control supports always only.\n";
        return false;
    }
    if (options.window_algebra || options.direct_p4) {
        if (!options.experiment_variant_explicit ||
            options.engine != Engine::Gopher || options.memo_mode != MemoMode::None ||
            options.collect_statistics || !options.early_prune ||
            options.order_mode != OrderMode::Paper || options.parallel_mode != ParallelMode::Roots ||
            options.verify_dag || options.runtime_key_diagnostics != RuntimeKeyDiagnosticsMode::Off ||
            options.adaptive_diagnostics || options.neighbor_copy_timing ||
            options.order_simulation != OrderSimulationMode::Disabled || options.direct_opt) {
            std::cerr << "Error: v5 controls require an explicit existing Direct/DAG-NoMemo variant, "
                         "gopher, roots, fixed paper order, memo none, statistics off, early-prune on, "
                         "and no adaptive/order-sim/key diagnostics, verify-dag or --direct-opt.\n";
            return false;
        }
        if (options.direct_p4) {
            if (options.experiment_variant != ExperimentVariant::DurableDFS || options.dag_passes.any()) {
                std::cerr << "Error: --direct-p4 on requires --experiment-variant durable-dfs "
                             "with --dag-passes none; it is a plain Direct control, not a DAG variant.\n";
                return false;
            }
        } else if (options.experiment_variant != ExperimentVariant::DagNoMemo ||
                   !options.dag_passes.p4 || options.dag_passes.temporalRewriteAny()) {
            std::cerr << "Error: window DAG lowering requires --experiment-variant dag-no-memo "
                         "and P4 (optionally P5/P6), with P1/P2/P3 off. "
                         "Direct windows require --direct-p4 on.\n";
            return false;
        }
    }
    if (options.adaptive_diagnostics || options.neighbor_copy_timing ||
        options.order_simulation != OrderSimulationMode::Disabled) {
        if (options.adaptive_diagnostics && options.dag_passes.any()) {
            std::cerr << "Error: A1 shadow funnel requires the unchanged P0/Direct path with all passes off.\n";
            return false;
        }
        if (options.engine != Engine::Gopher || options.memo_mode != MemoMode::None ||
            options.collect_statistics || !options.early_prune || options.verify_dag ||
            options.parallel_mode != ParallelMode::Roots) {
            std::cerr << "Error: adaptive diagnostics require gopher, roots, no memo/statistics/verify-dag, early pruning on.\n";
            return false;
        }
        if (options.order_simulation != OrderSimulationMode::Disabled &&
            (options.experiment_variant != ExperimentVariant::DurableDFS || options.dag_passes.any())) {
            std::cerr << "Error: --order-sim is a Direct-only diagnostic with all compiler passes off.\n";
            return false;
        }
        if (options.order_simulation != OrderSimulationMode::Disabled &&
            options.order_simulation != OrderSimulationMode::Static && options.adaptive_diagnostics) {
            std::cerr << "Error: A1 fixed-plan funnel cannot be combined with dynamic A2 scoring.\n";
            return false;
        }
        if (options.neighbor_copy_timing && (options.adaptive_diagnostics || options.ops_counters ||
            options.order_simulation != OrderSimulationMode::Disabled || options.dag_passes.any() ||
            options.experiment_variant != ExperimentVariant::DagNoMemo)) {
            std::cerr << "Error: isolated copy timing requires P0 DAG-NoMemo with all other diagnostics off.\n";
            return false;
        }
    }
    if (options.dag_passes.any()) {
        if (options.memo_mode != MemoMode::None || options.collect_statistics ||
            !options.early_prune || options.engine != Engine::Gopher || options.verify_dag) {
            std::cerr << "Error: compiler passes require memo none, statistics off, early-prune on, engine gopher, and no legacy --verify-dag (use the separate set-equality gate).\n";
            return false;
        }
        if (options.experiment_variant == ExperimentVariant::DurableDFS && !options.direct_opt) {
            std::cerr << "Error: Direct DFS passes require --direct-opt.\n";
            return false;
        }
    }
    if (options.physical_alpha != 1.0 && !options.dag_passes.p5) {
        std::cerr << "Error: a nondefault --physical-alpha requires P5.\n";
        return false;
    }
    if (options.dag_passes.physicalAny() &&
        (options.experiment_variant == ExperimentVariant::DurableDFS || options.direct_opt ||
         options.parallel_mode != ParallelMode::Roots ||
         options.runtime_key_diagnostics != RuntimeKeyDiagnosticsMode::Off)) {
        std::cerr << "Error: P4-P6 require a roots-scheduled DAG with runtime-key diagnostics off; they do not enable a Direct optimization.\n";
        return false;
    }
    if (options.direct_opt && (options.experiment_variant != ExperimentVariant::DurableDFS || !options.dag_passes.any())) {
        std::cerr << "Error: --direct-opt requires durable-dfs and selected passes.\n";
        return false;
    }
    if (options.runtime_key_diagnostics ==
        RuntimeKeyDiagnosticsMode::Exact) {
        if (!exact_runtime_key_cap_seen) {
            std::cerr
                << "Error: exact runtime-key diagnostics require "
                   "--exact-runtime-key-cap.\n";
            return false;
        }
        if (options.engine != Engine::Gopher ||
            options.experiment_variant == ExperimentVariant::DurableDFS) {
            std::cerr
                << "Error: exact runtime-key diagnostics require a "
                   "Candidate-DAG executor.\n";
            return false;
        }
        if (options.memo_mode != MemoMode::None &&
            options.memo_mode != MemoMode::Dag) {
            std::cerr
                << "Error: exact runtime-key diagnostics require --memo "
                   "none or dag.\n";
            return false;
        }
        if (options.memo_policy != MemoPolicy::Legacy ||
            options.memo_options.cost_aware_admission ||
            options.memo_options.minimum_estimated_work != 0) {
            std::cerr
                << "Error: exact runtime-key diagnostics require legacy "
                   "non-cost-aware eager admission (--no-cost-aware and "
                   "--cache-min-work 0 outside a closed experiment arm).\n";
            return false;
        }
        if (!options.collect_statistics) {
            std::cerr
                << "Error: exact runtime-key diagnostic runs require "
                   "--statistics on.\n";
            return false;
        }
    } else if (exact_runtime_key_cap_seen) {
        std::cerr
            << "Error: --exact-runtime-key-cap requires "
               "--runtime-key-diagnostics exact.\n";
        return false;
    }
    if (kSearchStatisticsHooksErasedAtBuild &&
        (options.collect_statistics ||
         options.runtime_key_diagnostics !=
             RuntimeKeyDiagnosticsMode::Off)) {
        std::cerr
            << "Error: this erased-control build accepts only "
               "--statistics off with runtime-key diagnostics off.\n";
        return false;
    }
    if (kSearchStatisticsHooksErasedAtBuild &&
        !gopherBuildIdentityBound()) {
        std::cerr
            << "Error: erased-control builds require bound source and "
               "common compiler-flags identities.\n";
        return false;
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

std::uint64_t elapsedNanoseconds(
    Clock::time_point start,
    Clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            end - start).count());
}

bool addNanoseconds(
    std::uint64_t& total,
    std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) {
        return false;
    }
    total += value;
    return true;
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

void writeLabelProjectionReceiptText(
    std::ostream& output,
    const ExplicitRuntimeLabelMap& receipt) {
    output
        << "label_projection_validation: validated\n"
        << "global_label_cardinality: "
        << receipt.global_label_cardinality << '\n'
        << "global_label_map_file_sha256: "
        << receipt.global_label_map_file_sha256 << '\n'
        << "global_label_map_mapping_sha256: "
        << receipt.global_label_map_mapping_sha256 << '\n'
        << "query_label_projection_file_sha256: "
        << receipt.query_label_projection_file_sha256 << '\n'
        << "query_file_sha256: "
        << receipt.query_file_sha256 << '\n'
        << "global_label_map_vertex_count: "
        << receipt.labels_by_external_id.size() << '\n'
        << "projected_invalid_vertex_count: "
        << receipt.invalid_vertex_count << '\n'
        << "selected_global_classes_in_A_to_E_order: ";
    for (std::size_t index = 0;
         index < receipt.selected_global_classes.size();
         ++index) {
        if (index != 0U) output << ',';
        output << receipt.selected_global_classes[index];
    }
    output << "\nruntime_label_vertex_counts_A_to_E: ";
    for (std::size_t index = 0;
         index < receipt.runtime_class_counts.size();
         ++index) {
        if (index != 0U) output << ',';
        output << receipt.runtime_class_counts[index];
    }
    output << '\n';
}

void writeLabelProjectionReceiptJson(
    std::ostream& output,
    const ExplicitRuntimeLabelMap& receipt) {
    output
        << "  \"label_projection\": {\n"
        << "    \"validation\": \"validated\",\n"
        << "    \"global_label_cardinality\": "
        << receipt.global_label_cardinality << ",\n"
        << "    \"global_label_map_file_sha256\": \""
        << receipt.global_label_map_file_sha256 << "\",\n"
        << "    \"global_label_map_mapping_sha256\": \""
        << receipt.global_label_map_mapping_sha256 << "\",\n"
        << "    \"query_label_projection_file_sha256\": \""
        << receipt.query_label_projection_file_sha256 << "\",\n"
        << "    \"query_file_sha256\": \""
        << receipt.query_file_sha256 << "\",\n"
        << "    \"global_label_map_vertex_count\": "
        << receipt.labels_by_external_id.size() << ",\n"
        << "    \"projected_invalid_vertex_count\": "
        << receipt.invalid_vertex_count << ",\n"
        << "    \"selected_global_classes_in_A_to_E_order\": [";
    for (std::size_t index = 0;
         index < receipt.selected_global_classes.size();
         ++index) {
        if (index != 0U) output << ", ";
        output << receipt.selected_global_classes[index];
    }
    output << "],\n    \"runtime_label_vertex_counts_A_to_E\": [";
    for (std::size_t index = 0;
         index < receipt.runtime_class_counts.size();
         ++index) {
        if (index != 0U) output << ", ";
        output << receipt.runtime_class_counts[index];
    }
    output << "]\n  }";
}

void writeStatisticsJson(
    std::ostream& output,
    const SearchStatistics& s,
    bool enabled) {
    bool first = true;
    auto field = [&](const std::string& name, std::uint64_t value) {
        if (!first) output << ',';
        first = false;
        output << "\n    \"" << name << "\": ";
        if (enabled) {
            output << value;
        } else {
            output << "null";
        }
    };
    auto boolean_field = [&](const std::string& name, bool value) {
        if (!first) output << ',';
        first = false;
        output << "\n    \"" << name << "\": ";
        if (enabled) {
            output << (value ? "true" : "false");
        } else {
            output << "null";
        }
    };
    auto receipt_boolean_field = [&](
                                     const std::string& name,
                                     bool value) {
        if (!first) output << ',';
        first = false;
        output << "\n    \"" << name << "\": "
               << (value ? "true" : "false");
    };
    auto string_field = [&](
                            const std::string& name,
                            const std::string& value) {
        if (!first) output << ',';
        first = false;
        output << "\n    \"" << name << "\": \""
               << jsonEscape(value) << '"';
    };
    auto mean_field = [&](
                          const std::string& name,
                          std::uint64_t sum,
                          std::uint64_t count) {
        if (!first) output << ',';
        first = false;
        output << "\n    \"" << name << "\": ";
        if (!enabled || count == 0) {
            output << "null";
        } else {
            output << static_cast<long double>(sum) /
                          static_cast<long double>(count);
        }
    };
    string_field(
        "dag_operator_statistics_schema_version",
        kDagOperatorStatisticsSchemaVersion);
    receipt_boolean_field(
        "search_statistics_hooks_erased_at_build",
        kSearchStatisticsHooksErasedAtBuild);
    string_field(
        "search_statistics_build_profile",
        kSearchStatisticsBuildProfile);
    string_field("gopher_source_identity", kGopherSourceIdentity);
    string_field(
        "gopher_common_flags_identity",
        kGopherCommonFlagsIdentity);
    string_field("gopher_compiler_identity", kGopherCompilerIdentity);
    boolean_field(
        "dag_operator_reconciliation_passed",
        s.dag_operator_reconciliation_passed);
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
    STAT_JSON(td_block_cache_hits);
    STAT_JSON(neighbor_lookup_cache_hits);
    STAT_JSON(structural_intersection_cache_hits);
    STAT_JSON(edge_time_cache_hits);
    STAT_JSON(temporal_intersection_cache_hits);
    STAT_JSON(longest_run_cache_hits);
    STAT_JSON(structural_parent_subtrees_skipped);
    STAT_JSON(structural_v2_intersection_nodes);
    STAT_JSON(structural_v2_eligible_nodes);
    STAT_JSON(structural_v2_no_projection_reuse_nodes);
    STAT_JSON(structural_v2_eligible_probes);
    STAT_JSON(structural_v2_projection_bypasses);
    STAT_JSON(dag_nodes_attempted);
    STAT_JSON(dag_nodes_materialized);
    STAT_JSON(dag_nodes_merged);
    boolean_field(
        "exact_runtime_key_diagnostics",
        s.exact_runtime_key_diagnostics);
    STAT_JSON(exact_runtime_key_cap);
    STAT_JSON(exact_runtime_key_invocations);
    STAT_JSON(exact_runtime_key_distinct);
    field(
        "exact_runtime_key_reuse_opportunities",
        s.exact_runtime_key_invocations -
            s.exact_runtime_key_distinct);
    STAT_JSON(runtime_key_construction_nanoseconds);
    STAT_JSON(cache_lookup_nanoseconds);
    STAT_JSON(structural_compute_nanoseconds);
    STAT_JSON(temporal_compute_nanoseconds);
    STAT_JSON(longest_compute_nanoseconds);
    STAT_JSON(search_nanoseconds);
#undef STAT_JSON
    for (std::size_t operation = 0;
         operation < kDagOpCount;
         ++operation) {
        const std::string metric = dagOpMetricName(
            static_cast<DagOp>(operation));
        field(
            "exact_runtime_key_invocations_" + metric,
            s.exact_runtime_key_invocations_by_op[operation]);
        field(
            "exact_runtime_key_distinct_" + metric,
            s.exact_runtime_key_distinct_by_op[operation]);
        field(
            "runtime_key_construction_nanoseconds_" + metric,
            s.runtime_key_construction_nanoseconds_by_op[operation]);
        field(
            "cache_lookup_nanoseconds_" + metric,
            s.cache_lookup_nanoseconds_by_op[operation]);
        field(
            "dag_operator_invocations_" + metric,
            s.dag_operator_invocations_by_op[operation]);
        field(
            "dag_operator_cache_hits_" + metric,
            s.dag_operator_cache_hits_by_op[operation]);
        field(
            "dag_operator_cache_misses_" + metric,
            s.dag_operator_cache_misses_by_op[operation]);
        field(
            "dag_operator_exclusive_compute_nanoseconds_" + metric,
            s.dag_operator_exclusive_compute_nanoseconds_by_op[
                operation]);
        field(
            "dag_operator_exclusive_miss_compute_nanoseconds_" + metric,
            s
                .dag_operator_exclusive_miss_compute_nanoseconds_by_op[
                    operation]);
        field(
            "dag_operator_result_units_sum_" + metric,
            s.dag_operator_result_units_sum_by_op[operation]);
        field(
            "dag_operator_result_observations_" + metric,
            s.dag_operator_result_observations_by_op[operation]);
        mean_field(
            "dag_operator_result_units_mean_" + metric,
            s.dag_operator_result_units_sum_by_op[operation],
            s.dag_operator_result_observations_by_op[operation]);
        field(
            "dag_operator_cache_admissions_" + metric,
            s.dag_operator_cache_admissions_by_op[operation]);
        field(
            "dag_operator_cache_admitted_bytes_" + metric,
            s.dag_operator_cache_admitted_bytes_by_op[operation]);
    }
    field(
        "longest_run_returned_length_sum",
        s.longest_run_returned_length_sum);
    field(
        "longest_run_returned_length_observations",
        s.longest_run_returned_length_observations);
    mean_field(
        "longest_run_returned_length_mean",
        s.longest_run_returned_length_sum,
        s.longest_run_returned_length_observations);
    output << '\n';
}

void writeDagTopologyJson(
    std::ostream& output,
    const CandidateDagTopology& topology,
    bool include_static_canonicalization_fields) {
    output
        << "{\n"
        << "    \"nodes_attempted\": "
        << topology.nodes_attempted << ",\n"
        << "    \"nodes_materialized\": "
        << topology.nodes_materialized << ",\n"
        << "    \"nodes_merged\": "
        << topology.nodes_merged << ",\n"
        << "    \"total_parent_edges\": "
        << topology.total_parent_edges << ",\n"
        << "    \"shared_nodes\": "
        << topology.shared_nodes << ",\n"
        << "    \"max_fanout\": "
        << topology.max_fanout << ",\n"
        << "    \"max_depth\": "
        << topology.max_depth;
    if (include_static_canonicalization_fields) {
        output
            << ",\n"
            << "    \"materialized_topology_scope\": "
               "\"entire retained arena including unreachable "
               "flattened intermediates\",\n"
            << "    \"fanout_consumer_scope\": "
               "\"internal parent-edge consumers only; structural, "
               "temporal, and longest-run root-array references are "
               "excluded\",\n"
            << "    \"reachable_topology_scope\": "
               "\"nodes reachable from structural, temporal, and "
               "longest-run root arrays; shared-node and fanout counts "
               "still include internal parent-edge consumers only\",\n"
            << "    \"reachable_nodes\": "
            << topology.reachable_nodes << ",\n"
            << "    \"unreachable_materialized_nodes\": "
            << topology.unreachable_materialized_nodes << ",\n"
            << "    \"reachable_parent_edges\": "
            << topology.reachable_parent_edges << ",\n"
            << "    \"reachable_shared_nodes\": "
            << topology.reachable_shared_nodes << ",\n"
            << "    \"reachable_max_fanout\": "
            << topology.reachable_max_fanout << ",\n"
            << "    \"reachable_max_depth\": "
            << topology.reachable_max_depth;
    }
    output << ",\n    \"op_node_counts\": {";
    for (std::size_t index = 0; index < kDagOpCount; ++index) {
        if (index > 0) output << ',';
        const DagOp op = static_cast<DagOp>(index);
        output << "\n      \"" << dagOpMetricName(op) << "\": "
               << topology.op_node_counts[index];
    }
    output << "\n    }";
    if (include_static_canonicalization_fields) {
        auto write_op_counts = [&output](
                                   const char* field,
                                   const std::array<
                                       std::uint64_t,
                                       kDagOpCount>& counts) {
            output << ",\n    \"" << field << "\": {";
            for (std::size_t index = 0;
                 index < kDagOpCount;
                 ++index) {
                if (index > 0) output << ',';
                const DagOp op = static_cast<DagOp>(index);
                output << "\n      \"" << dagOpMetricName(op)
                       << "\": " << counts[index];
            }
            output << "\n    }";
        };
        write_op_counts(
            "op_node_attempt_counts",
            topology.op_node_attempt_counts);
        write_op_counts(
            "reachable_op_node_counts",
            topology.reachable_op_node_counts);
    }
    output << "\n  }";
}

void writeDagCompilationAuditJson(
    std::ostream& output,
    const CandidateDag& dag,
    const CandidateDagCompilationAudit& audit,
    CandidateDagInternMode intern_mode) {
    auto write_string_array = [&output](
                                  const std::vector<std::string>& values,
                                  bool empty_is_null) {
        output << '[';
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (index > 0) output << ',';
            output << '\n' << "      ";
            if (empty_is_null && values[index].empty()) {
                output << "null";
            } else {
                output << '\"' << jsonEscape(values[index]) << '\"';
            }
        }
        if (!values.empty()) output << '\n' << "    ";
        output << ']';
    };
    auto write_int_array = [&output](const std::vector<int>& values) {
        output << '[';
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (index > 0) output << ',';
            output << values[index];
        }
        output << ']';
    };

    output
        << "{\n"
        << "    \"schema_version\": "
           "\"candidate_dag_compilation_audit_v1\",\n"
        << "    \"interning_mode\": \""
        << candidateDagInternModeName(intern_mode) << "\",\n"
        << "    \"attempt_count\": " << audit.attempts.size()
        << ",\n"
        << "    \"attempts\": [";
    for (std::size_t index = 0; index < audit.attempts.size(); ++index) {
        const CandidateDagAttemptRecord& record = audit.attempts[index];
        if (index > 0) output << ',';
        output
            << "\n      {\"attempt_index\": "
            << record.attempt_index
            << ", \"op\": \"" << dagOpMetricName(record.op)
            << "\", \"result_kind\": \""
            << dagResultKindName(record.result_kind)
            << "\", \"parents\": ";
        output << '[';
        for (std::size_t parent = 0;
             parent < record.parent_expressions.size();
             ++parent) {
            if (parent > 0) output << ',';
            output << '\"'
                   << jsonEscape(record.parent_expressions[parent])
                   << '\"';
        }
        output << "], \"dependencies\": ";
        write_int_array(record.dependencies);
        output
            << ", \"parameter\": {\"first_query_vertex\": "
            << record.parameter.first_query_vertex
            << ", \"second_query_vertex\": "
            << record.parameter.second_query_vertex
            << "}, \"canonical_expression\": \""
            << jsonEscape(record.canonical_expression)
            << "\", \"static_fingerprint\": "
            << record.static_fingerprint << '}';
    }
    if (!audit.attempts.empty()) output << '\n' << "    ";
    output << "],\n"
           << "    \"canonical_parent_deduplicated_node_ids\": [";
    for (std::size_t index = 0;
         index < audit.canonical_parent_deduplicated_node_ids.size();
         ++index) {
        if (index > 0) output << ',';
        output << audit.canonical_parent_deduplicated_node_ids[index];
    }
    output << "],\n"
           << "    \"canonical_parent_deduplicated_node_ids_scope\": "
              "\"diagnostic-local materialized node IDs only; never "
              "compared across interning modes\",\n"
           << "    \"root_expressions\": {\n"
           << "      \"structural\": ";
    write_string_array(audit.structural_root_expressions, true);
    output << ",\n      \"temporal\": ";
    write_string_array(audit.temporal_root_expressions, true);
    output << ",\n      \"longest_run\": ";
    write_string_array(audit.longest_run_root_expressions, true);
    output << "\n    },\n"
           << "    \"materialized_nodes\": [";
    for (std::size_t index = 0; index < dag.nodes.size(); ++index) {
        const DagNode& node = dag.nodes[index];
        if (index > 0) output << ',';
        output
            << "\n      {\"node_id\": " << node.id
            << ", \"op\": \"" << dagOpMetricName(node.op)
            << "\", \"result_kind\": \""
            << dagResultKindName(node.result_kind)
            << "\", \"parents\": [";
        for (std::size_t parent = 0;
             parent < node.parents.size();
             ++parent) {
            if (parent > 0) output << ',';
            output << '\"'
                   << jsonEscape(
                          dag.node(node.parents[parent])
                              .canonical_expression)
                   << '\"';
        }
        output << "], \"dependencies\": ";
        write_int_array(node.dependencies);
        output
            << ", \"parameter\": {\"first_query_vertex\": "
            << node.parameter.first_query_vertex
            << ", \"second_query_vertex\": "
            << node.parameter.second_query_vertex
            << "}, \"canonical_expression\": \""
            << jsonEscape(node.canonical_expression)
            << "\", \"static_fingerprint\": "
            << node.static_fingerprint
            << ", \"depth\": " << node.depth << '}';
    }
    if (!dag.nodes.empty()) output << '\n' << "    ";
    output << "]\n  }";
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
    if (options.experiment_variant != ExperimentVariant::DurableDFS ||
        options.engine != Engine::Gopher || options.dag_passes.any() ||
        options.direct_opt || !options.dump_dag.empty() || options.jindex_simulate) {
        std::cerr << "Error: td_tree_direct_v6 supports Direct DFS only; DAG arms are disabled.\n";
        return 1;
    }

    OperatorCounters operator_counters;
    AdaptiveDiagnostics adaptive_diagnostics(options.adaptive_diagnostics, options.neighbor_copy_timing);
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

    ExplicitRuntimeLabelMap explicit_label_map;
    const ExplicitRuntimeLabelMap* explicit_label_map_pointer = nullptr;
    if (options.label_mode == LabelMode::ExplicitProjection) {
        const auto projection_start = Clock::now();
        std::string projection_error;
        if (!loadExplicitRuntimeLabelMap(
                options.global_label_map_file.string(),
                options.query_label_projection_file.string(),
                options.query_graph_file,
                explicit_label_map,
                projection_error)) {
            std::cerr
                << "Error: Invalid explicit query label projection: "
                << projection_error << '\n';
            return 2;
        }
        timings_ns["readAndValidateLabelProjection"] =
            elapsedNanoseconds(projection_start);
        explicit_label_map_pointer = &explicit_label_map;
    }

    Graph temporal_graph;
    TemporalGraphLoadTimings load_timings;
    auto stage_start = Clock::now();
    if (!readTemporalGraph(
            options.temporal_graph_file,
            temporal_graph,
            options.label_seed,
            &load_timings,
            options.label_mode,
            explicit_label_map_pointer,
            options.lmax_filter ? options.minimum_duration : 2)) {
        return 2;
    }
    timings_ns["readAndFilterTemporalGraph"] =
        elapsedNanoseconds(stage_start);
    if (!load_timings.adjacency_temporal_capacity_bytes) {
        std::cerr << "Error: successful graph load has no finalized capacity receipt.\n";
        return 2;
    }
    timings_ns["readTemporalGraph"] =
        static_cast<std::uint64_t>(
            std::max(0LL, load_timings.read_milliseconds)) *
        1'000'000ULL;
    timings_ns["filterTemporalGraph"] =
        static_cast<std::uint64_t>(
            std::max(0LL, load_timings.filter_milliseconds)) *
        1'000'000ULL;
    const std::uint64_t temporal_graph_fingerprint =
        temporalGraphContentFingerprint(temporal_graph);
    std::string actual_graph_file_sha256;
    if (!options.wstore_path.empty()) {
        const auto sha_start = Clock::now();
        try {
            actual_graph_file_sha256 = wstore_file::graphFileSha256(options.temporal_graph_file);
        } catch (const std::exception& error) {
            std::cerr << "Error: cannot bind W-store to actual graph file: " << error.what() << '\n';
            return 2;
        }
        // Input verification is graph-loading work, not query preparation or enum.
        timings_ns["wstoreGraphSha256"] = elapsedNanoseconds(sha_start);
    }

    Graph query_graph;
    stage_start = Clock::now();
    if (!readQueryGraph(
            options.query_graph_file, query_graph)) {
        return 2;
    }
    if (explicit_label_map_pointer != nullptr) {
        const std::size_t selected_count =
            explicit_label_map.selected_global_classes.size();
        for (Label label : query_graph.vertex_labels) {
            if (static_cast<std::size_t>(label) >= selected_count) {
                std::cerr
                    << "Error: Query uses runtime label "
                    << labelToString(label)
                    << " without a selected global class in the projection.\n";
                return 2;
            }
        }
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
        label_mean_vertex_active_snapshots{};
    for (std::size_t label = 0;
         label < kLabelCount;
         ++label) {
        if (label_vertex_counts[label] != 0) {
            label_mean_vertex_active_snapshots[label] =
                static_cast<double>(
                    label_duration_sums[label]) /
                static_cast<double>(
                    label_vertex_counts[label]);
        }
    }
    timings_ns["labelStatistics"] =
        elapsedNanoseconds(stage_start);

    SurvivalSelectivityStatistics survival_statistics;
    const SurvivalSelectivityStatistics* survival_statistics_pointer =
        nullptr;
    if (options.selectivity_mode == SelectivityMode::Structural ||
        options.selectivity_mode == SelectivityMode::Survival) {
        stage_start = Clock::now();
        survival_statistics = computeSurvivalSelectivityStatistics(
            temporal_graph,
            query_graph,
            options.minimum_duration);
        timings_ns["survivalSelectivityStatistics"] =
            elapsedNanoseconds(stage_start);
        survival_statistics_pointer = &survival_statistics;
    }

    TemporalPlanDiagnostics temporal_plan_diagnostics;
    const auto controllable_start = Clock::now();
    PreparedQueryPhaseTimings experiment_phases;
    experiment_phases.dag_compilation_applicable =
        options.engine == Engine::Gopher &&
        (!options.experiment_variant_explicit ||
         experimentVariantUsesDag(options.experiment_variant));
    stage_start = Clock::now();
    QueryDecomposition decomposition;
    std::unique_ptr<TDTree> planned_td_tree;
    if (options.selectivity_mode == SelectivityMode::TemporalCost) {
        options.temporal_cost_options.sample_seed = options.label_seed;
        decomposition = planTemporalCostQuery(
            temporal_graph,
            query_graph,
            options.minimum_duration,
            label_vertex_counts,
            label_mean_vertex_active_snapshots,
            options.temporal_cost_options,
            &temporal_plan_diagnostics);
        timings_ns["temporalCostPlanning"] =
            temporal_plan_diagnostics.planning_nanoseconds;
    } else if (options.selectivity_mode ==
               SelectivityMode::TemporalCostV2) {
        options.temporal_cost_options.sample_seed = options.label_seed;
        TemporalCostPlanResult plan_result = planTemporalCostV2(
            temporal_graph,
            query_graph,
            options.minimum_duration,
            label_vertex_counts,
            label_mean_vertex_active_snapshots,
            options.temporal_cost_options,
            &temporal_plan_diagnostics);
        decomposition = std::move(plan_result.decomposition);
        planned_td_tree = std::move(plan_result.selected_tree);
        timings_ns["temporalCostPlanning"] =
            temporal_plan_diagnostics.planning_nanoseconds;
    } else {
        decomposition = decomposeQuery(
            query_graph,
            label_vertex_counts,
            label_mean_vertex_active_snapshots,
            options.selectivity_mode,
            survival_statistics_pointer);
        timings_ns["queryDecomposition"] =
            elapsedNanoseconds(stage_start);
    }
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
    if (options.experiment_variant_explicit) {
        const auto planning_end = Clock::now();
        experiment_phases.planning_nanoseconds =
            elapsedNanoseconds(controllable_start, planning_end);
        stage_start = planning_end;
    } else {
        stage_start = Clock::now();
    }

    std::unique_ptr<TDTree> td_tree;
    if (planned_td_tree != nullptr) {
        td_tree = std::move(planned_td_tree);
        // The handed-off tree was built inside temporalCostPlanning.  This
        // stage records only additional caller-side build work.
        timings_ns["buildTDTree"] = 0;
    } else {
        td_tree = std::make_unique<TDTree>(
            temporal_graph,
            query_graph,
            decomposition,
            options.minimum_duration);
        timings_ns["buildTDTree"] =
            elapsedNanoseconds(stage_start);
    }
    if (options.experiment_variant_explicit) {
        const auto td_construction_end = Clock::now();
        experiment_phases.td_construction_nanoseconds =
            elapsedNanoseconds(stage_start, td_construction_end);
        stage_start = td_construction_end;
    }

    QueryPlan plan;
    CandidateDag dag;
    CandidateDagCompilationAudit dag_compilation_audit;
    const bool collect_dag_compilation_audit =
        options.experiment_variant_explicit &&
        experimentVariantIsStaticCanonicalization(
            options.experiment_variant) &&
        options.correctness_preflight;
    const CandidateDagInternMode dag_intern_mode =
        options.experiment_variant_explicit &&
        experimentVariantIsStaticCanonicalization(
            options.experiment_variant) &&
        !experimentVariantInternsDag(options.experiment_variant)
        ? CandidateDagInternMode::NoIntern
        : CandidateDagInternMode::Intern;
    GopherMatchSummary gopher_summary;
    std::unique_ptr<JIndexSimulation> jindex_simulation;
    MatchSummary legacy_summary;
    SearchStatistics search_statistics;
    std::string result_digest = "disabled";
    std::size_t threads_used = 1;
    std::size_t cache_memory_bytes = 0;
    std::string parallel_mode_effective =
        engineName(options.engine);
    ParallelExecutionStatistics parallel_statistics;

    if (options.engine == Engine::Gopher) {
        if (!options.experiment_variant_explicit) {
            stage_start = Clock::now();
        }
        plan = makeQueryPlan(
            query_graph,
            decomposition,
            *td_tree,
            options.order_mode);
        const auto query_plan_end = Clock::now();
        timings_ns["buildQueryPlan"] = elapsedNanoseconds(
            stage_start, query_plan_end);
        if (options.experiment_variant_explicit &&
            !addNanoseconds(
                experiment_phases.planning_nanoseconds,
                timings_ns["buildQueryPlan"])) {
            std::cerr << "Error: planning timer overflow.\n";
            return 4;
        }

        stage_start = query_plan_end;
        if (experiment_phases.dag_compilation_applicable) {
            CandidateDagCompiler compiler;
            dag = compiler.compile(
                query_graph,
                plan,
                decomposition,
                dag_intern_mode,
                collect_dag_compilation_audit
                    ? &dag_compilation_audit
                    : nullptr);
            if (!options.dump_dag.empty()) {
                dumpCandidateDag(dag, plan, options.dump_dag + "_before", "P0");
            }
            if (options.dag_passes.any()) {
                dag = optimizeCandidateDag(dag, temporal_graph, query_graph, plan,
                                           *td_tree, options.dag_passes, options.physical_alpha);
            }
            if (!options.dump_dag.empty()) {
                dumpCandidateDag(dag, plan, options.dump_dag + "_after", dagPassesName(options.dag_passes));
                if (options.window_algebra) {
                    // This is the actual typed DAG lowering, not a synthetic
                    // Candidate-DAG invented for the plain Direct control.
                    WindowProgram(dag, plan, options.jindex_mode != JIndexMode::None)
                        .dump(dag, options.dump_dag + ".v5");
                }
            }
            timings_ns["compileCandidateDag"] =
                elapsedNanoseconds(stage_start);
        } else {
            timings_ns["compileCandidateDag"] = 0;
        }
        const auto dag_compilation_end = Clock::now();
        if (options.experiment_variant_explicit) {
            experiment_phases.dag_compilation_nanoseconds =
                experiment_phases.dag_compilation_applicable
                ? elapsedNanoseconds(stage_start, dag_compilation_end)
                : 0;
        }

        stage_start = dag_compilation_end;
        GopherMatcherOptions matcher_options;
        matcher_options.threads = options.threads;
        matcher_options.parallel_mode = options.parallel_mode;
        matcher_options.prefix_depth = options.prefix_depth;
        matcher_options.tasks_per_thread =
            options.tasks_per_thread;
        matcher_options.order_mode = options.order_mode;
        matcher_options.early_prune = options.early_prune;
        matcher_options.result_digest = options.result_digest;
        matcher_options.order_simulation = options.order_simulation;
        matcher_options.window_algebra = options.window_algebra;
        matcher_options.raw_bitset = options.raw_bitset;
        matcher_options.window_lazy = options.window_lazy;
        matcher_options.direct_p4 = options.direct_p4;
        matcher_options.jindex_mode = options.jindex_mode;
        matcher_options.jindex_alpha = options.jindex_alpha;
        matcher_options.jindex_block_size = options.jindex_block_size;
        matcher_options.wstore_path = options.wstore_path;
        matcher_options.graph_file_sha256 = actual_graph_file_sha256;
        matcher_options.bindex_mode = options.bindex_mode;
        matcher_options.bindex_alpha = options.bindex_alpha;
        matcher_options.bindex_block_size = options.bindex_block_size;
        matcher_options.bindex_min_size = options.bindex_min_size;
        matcher_options.bindex_reuse_threshold = options.bindex_reuse_threshold;
        matcher_options.bindex_verify_superset = options.bindex_verify_superset;
        matcher_options.graph_adjacency_temporal_capacity_bytes =
            load_timings.adjacency_temporal_capacity_bytes;
        if (options.jindex_simulate) {
            jindex_simulation = std::make_unique<JIndexSimulation>(
                temporal_graph, query_graph, plan, dag, *td_tree, options.minimum_duration);
            matcher_options.jindex_simulation = jindex_simulation.get();
        }
        matcher_options.executor.memo_mode =
            options.memo_mode;
        matcher_options.executor.memo_policy =
            options.memo_policy;
        matcher_options.executor.runtime_key_mode =
            options.runtime_key_mode;
        matcher_options.executor.runtime_key_diagnostics =
            options.runtime_key_diagnostics;
        matcher_options.executor.exact_runtime_key_cap =
            options.exact_runtime_key_cap;
        matcher_options.executor.memo_options =
            options.memo_options;
        matcher_options.executor.verify_dag =
            options.verify_dag;
        matcher_options.executor.collect_statistics =
            options.collect_statistics;
        matcher_options.executor.ops = options.ops_counters ? &operator_counters : nullptr;
        matcher_options.executor.adaptive = options.adaptive_diagnostics || options.neighbor_copy_timing
            ? &adaptive_diagnostics : nullptr;
        matcher_options.executor.direct_passes = options.direct_opt ? options.dag_passes : DagPasses{};

        auto run_matcher = [&](GopherMatcher& matcher) {
            const auto matcher_setup_end = Clock::now();
            if (options.experiment_variant_explicit &&
                !addNanoseconds(
                    experiment_phases.planning_nanoseconds,
                    elapsedNanoseconds(stage_start, matcher_setup_end))) {
                std::cerr << "Error: planning timer overflow.\n";
                return false;
            }
            const auto execution_start = matcher_setup_end;
            try {
                gopher_summary = matcher.save_res(
                    result_file.string(), options.output_mode);
            } catch (const std::exception& error) {
                std::cerr
                    << "Error: matching failed: "
                    << error.what() << '\n';
                return false;
            }
            const auto execution_end = Clock::now();
            if (options.experiment_variant_explicit) {
                const std::uint64_t execution_nanoseconds =
                    elapsedNanoseconds(execution_start, execution_end);
                experiment_phases.enumeration_nanoseconds =
                    gopher_summary.enumeration_nanoseconds;
                if (experiment_phases.enumeration_nanoseconds >
                    execution_nanoseconds) {
                    std::cerr
                        << "Error: enumeration timer exceeds its enclosing "
                           "execution interval.\n";
                    return false;
                }
                // run()'s v2 primary interval ends after exact worker-result
                // and digest reduction.  The remaining save_res interval is
                // a legacy residual containing post-primary statistics/cache
                // diagnostics plus result preamble/count-patch/flush work; it
                // is not part of the pure-online primary metric.
                experiment_phases.output_finalization_nanoseconds =
                    execution_nanoseconds -
                    experiment_phases.enumeration_nanoseconds;
            }
            return true;
        };

        bool matcher_completed = false;
        if (options.experiment_variant_explicit &&
            options.experiment_variant ==
                ExperimentVariant::DurableDFS) {
            GopherMatcher matcher(
                temporal_graph,
                query_graph,
                plan,
                *td_tree,
                options.minimum_duration,
                matcher_options);
            matcher_completed = run_matcher(matcher);
        } else {
            GopherMatcher matcher(
                temporal_graph,
                query_graph,
                plan,
                dag,
                *td_tree,
                options.minimum_duration,
                matcher_options);
            matcher_completed = run_matcher(matcher);
        }
        if (!matcher_completed) return 4;
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
        legacy_summary = td_tree->save_res(
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
    const auto controllable_end = Clock::now();
    const std::uint64_t controllable_nanoseconds =
        elapsedNanoseconds(controllable_start, controllable_end);
    std::uint64_t prepared_query_total_ns = 0;
    std::uint64_t phase_boundary_residual_ns = 0;
    timings_ns["controllableQueryProcessing"] =
        controllable_nanoseconds;
    if (options.experiment_variant_explicit) {
        std::string phase_error;
        if (!experiment_phases.validate(&phase_error)) {
            std::cerr << "Error: Invalid prepared-query phase receipt: "
                      << phase_error << '\n';
            return 4;
        }
        if (!experiment_phases.totalNanoseconds(
                prepared_query_total_ns)) {
            std::cerr
                << "Error: Prepared-query phase total overflow.\n";
            return 4;
        }
        if (prepared_query_total_ns > controllable_nanoseconds) {
            std::cerr
                << "Error: Prepared-query phases overlap their enclosing "
                   "boundary.\n";
            return 4;
        }
        phase_boundary_residual_ns =
            controllable_nanoseconds - prepared_query_total_ns;
        if (!addNanoseconds(
                experiment_phases.output_finalization_nanoseconds,
                phase_boundary_residual_ns) ||
            !experiment_phases.totalNanoseconds(
                prepared_query_total_ns) ||
            prepared_query_total_ns != controllable_nanoseconds) {
            std::cerr
                << "Error: Prepared-query phase reconciliation failed.\n";
            return 4;
        }
        timings_ns["phasePlanning"] =
            experiment_phases.planning_nanoseconds;
        timings_ns["phaseTDConstruction"] =
            experiment_phases.td_construction_nanoseconds;
        timings_ns["phaseDAGCompilation"] =
            experiment_phases.dag_compilation_nanoseconds;
        timings_ns["phaseEnumeration"] =
            experiment_phases.enumeration_nanoseconds;
        timings_ns["phaseOutputFinalization"] =
            experiment_phases.output_finalization_nanoseconds;
        timings_ns["phasePreparedQueryTotal"] =
            prepared_query_total_ns;
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
    const bool e2_closed_controls =
        !options.experiment_variant_explicit ||
        !experimentVariantIsStaticCanonicalization(
            options.experiment_variant) ||
        (options.engine == Engine::Gopher &&
         options.selectivity_mode == SelectivityMode::Paper &&
         options.order_mode == OrderMode::Paper &&
         options.parallel_mode == ParallelMode::Roots &&
         options.threads == 1 &&
         threads_used == 1 &&
         options.early_prune &&
         options.memo_policy == MemoPolicy::Legacy &&
         options.memo_options.budget_bytes == (256ULL << 20) &&
         options.memo_options.max_entry_bytes == (8ULL << 20) &&
         options.memo_options.shard_count == 64 &&
         options.memo_options.minimum_estimated_work == 0 &&
         !options.memo_options.cost_aware_admission &&
         options.output_mode == MatchOutputMode::CountOnly);
    const bool off_overhead_comparison_eligible =
        !options.correctness_preflight &&
        !options.collect_statistics &&
        options.runtime_key_diagnostics == RuntimeKeyDiagnosticsMode::Off &&
        gopherBuildIdentityBound() &&
        e2_closed_controls;
    const bool primary_timing_eligible =
        off_overhead_comparison_eligible &&
        !options.jindex_simulate &&
        !kSearchStatisticsHooksErasedAtBuild;

    // Capture product peak RSS before the E2 topology audit allocates its
    // reachability/fanout scratch vectors.  The topology summary is retained
    // as a mechanism receipt but must not contaminate the product RSS value.
    const std::size_t peak_memory = getPeakRSS();
    const std::size_t graph_memory =
        temporal_graph.getMemoryUsage();
    const std::size_t td_tree_memory =
        td_tree->getMemoryUsage();
    const std::size_t dag_memory =
        experiment_phases.dag_compilation_applicable
        ? dag.getMemoryUsage()
        : 0;
    CandidateDagTopology dag_topology;
    if (experiment_phases.dag_compilation_applicable) {
        dag_topology = dag.topologySummary();
    }
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
        << "label_seed: " << options.label_seed << '\n';
    if (explicit_label_map_pointer != nullptr) {
        writeLabelProjectionReceiptText(
            timing_output, explicit_label_map);
    }
    timing_output
        << "temporal_graph_content_fingerprint_fnv1a64: "
        << temporal_graph_fingerprint << '\n'
        << "selectivity_mode: "
        << selectivityModeName(decomposition.selectivity_mode) << '\n'
        << "selectivity_formula_id: "
        << selectivityFormulaId(decomposition.selectivity_mode) << '\n'
        << "query_root: " << decomposition.root << '\n'
        << "root_selectivity_score: "
        << std::setprecision(17)
        << decomposition.temporal_selectivity[
               static_cast<std::size_t>(decomposition.root)]
        << '\n'
        << "tcc_planning_ms: "
        << temporal_plan_diagnostics.planning_milliseconds << '\n'
        << "tcc_planning_ns: "
        << temporal_plan_diagnostics.planning_nanoseconds << '\n'
        << "tcc_beam_width: "
        << temporal_plan_diagnostics.beam_width << '\n'
        << "tcc_sample_cap: "
        << temporal_plan_diagnostics.sample_cap << '\n'
        << "tcc_sample_seed: "
        << temporal_plan_diagnostics.sample_seed << '\n'
        << "tcc_selected_estimated_total_candidates: "
        << temporal_plan_diagnostics.selected_estimated_total_candidates
        << '\n'
        << "tcc_sampled_baseline_selected: "
        << (temporal_plan_diagnostics.sampled_baseline_plan_selected
            ? "true" : "false") << '\n'
        << "tcc_root_plans_evaluated: "
        << temporal_plan_diagnostics.root_plans.size() << '\n'
        << "tcc_selected_root_candidates: "
        << temporal_plan_diagnostics.selected_root_candidates << '\n'
        << "tcc_selected_raw_extensions: "
        << temporal_plan_diagnostics.selected_raw_extension_candidates
        << '\n'
        << "tcc_selected_root_plus_raw: "
        << temporal_plan_diagnostics.selected_root_plus_raw_candidates
        << '\n'
        << "tcc_v2_decision: "
        << temporal_plan_diagnostics.v2_decision << '\n'
        << "tcc_v2_decision_reason: "
        << temporal_plan_diagnostics.v2_decision_reason << '\n'
        << "tcc_v2_admission_reason: "
        << temporal_plan_diagnostics.v2_admission_reason << '\n'
        << "tcc_v2_fallback_phase: "
        << temporal_plan_diagnostics.v2_fallback_phase << '\n'
        << "tcc_v2_fallback_reason: "
        << temporal_plan_diagnostics.v2_fallback_reason << '\n'
        << "tcc_v2_budget_exhausted: "
        << (temporal_plan_diagnostics.v2_budget_exhausted
                ? "true" : "false") << '\n'
        << "tcc_v2_cost_gate_enabled: "
        << (temporal_plan_diagnostics.v2_cost_gate_enabled
                ? "true" : "false") << '\n'
        << "tcc_v2_cost_gate_bypassed: "
        << (temporal_plan_diagnostics.v2_cost_gate_bypassed
                ? "true" : "false") << '\n'
        << "tcc_v2_forced_candidate_selected: "
        << (temporal_plan_diagnostics.v2_forced_candidate_selected
                ? "true" : "false") << '\n'
        << "tcc_v2_catalog_probes: "
        << temporal_plan_diagnostics.catalog_probes << '\n'
        << "tcc_v2_state_expansions: "
        << temporal_plan_diagnostics.state_expansions << '\n'
        << "tcc_v2_extension_probes: "
        << temporal_plan_diagnostics.extension_probes << '\n'
        << "tcc_v2_beam_extension_probes: "
        << temporal_plan_diagnostics.beam_extension_probes << '\n'
        << "tcc_v2_incumbent_extension_probes: "
        << temporal_plan_diagnostics.incumbent_extension_probes << '\n'
        << "tcc_v2_candidate_extension_probes: "
        << temporal_plan_diagnostics.candidate_extension_probes << '\n'
        << "tcc_v2_fixed_plan_extension_probes: "
        << temporal_plan_diagnostics.fixed_plan_extension_probes << '\n'
        << "tcc_v2_interval_intersections: "
        << temporal_plan_diagnostics.interval_intersections << '\n'
        << "tcc_v2_temporal_edge_applications: "
        << temporal_plan_diagnostics.temporal_edge_applications << '\n'
        << "tcc_v2_fixed_plan_scoring_method: "
        << temporal_plan_diagnostics.fixed_plan_scoring_method << '\n'
        << "tcc_v2_fixed_plan_scores: "
        << temporal_plan_diagnostics.fixed_plan_scores.size() << '\n'
        << "tcc_v2_pilot_extension_probes: "
        << temporal_plan_diagnostics.pilot_extension_probes << '\n'
        << "tcc_v2_pilot_extension_probe_cap: "
        << temporal_plan_diagnostics.pilot_extension_probe_cap << '\n'
        << "tcc_v2_pilot_prefix_cap: "
        << temporal_plan_diagnostics.pilot_prefix_cap << '\n'
        << "tcc_v2_rescore_top_l: "
        << temporal_plan_diagnostics.rescore_top_l << '\n'
        << "tcc_v2_max_candidate_trees_built: "
        << temporal_plan_diagnostics.max_candidate_trees_built << '\n'
        << "tcc_v2_max_interval_intersections: "
        << temporal_plan_diagnostics.max_interval_intersections << '\n'
        << "tcc_v2_catalogue_complete_plans: "
        << temporal_plan_diagnostics.catalogue_complete_plans << '\n'
        << "tcc_v2_candidate_plans_rescored: "
        << temporal_plan_diagnostics.candidate_plans_rescored << '\n'
        << "tcc_v2_candidate_trees_built: "
        << temporal_plan_diagnostics.candidate_trees_built << '\n'
        << "tcc_v2_candidate_tree_relation_entries_built: "
        << temporal_plan_diagnostics
               .candidate_tree_relation_entries_built << '\n'
        << "tcc_v2_rejected_candidate_tree_relation_entries: "
        << temporal_plan_diagnostics
               .rejected_candidate_tree_relation_entries << '\n'
        << "tcc_v2_opportunity_margin_fraction: "
        << temporal_plan_diagnostics.opportunity_margin_fraction << '\n'
        << "tcc_v2_catalog_probe_cost_weight: "
        << temporal_plan_diagnostics.catalog_probe_cost_weight << '\n'
        << "tcc_v2_opportunity_pilot_spent_cost: "
        << temporal_plan_diagnostics.opportunity_pilot_spent_cost << '\n'
        << "tcc_v2_opportunity_pilot_state_cost: "
        << temporal_plan_diagnostics.opportunity_pilot_state_cost << '\n'
        << "tcc_v2_opportunity_pilot_extension_cost: "
        << temporal_plan_diagnostics.opportunity_pilot_extension_cost
        << '\n'
        << "tcc_v2_opportunity_pilot_interval_cost: "
        << temporal_plan_diagnostics.opportunity_pilot_interval_cost
        << '\n'
        << "tcc_v2_opportunity_incumbent_build_cost: "
        << temporal_plan_diagnostics.opportunity_incumbent_build_cost
        << '\n'
        << "tcc_v2_opportunity_catalog_floor_probes: "
        << temporal_plan_diagnostics.opportunity_catalog_floor_probes
        << '\n'
        << "tcc_v2_opportunity_catalog_floor_cost: "
        << temporal_plan_diagnostics.opportunity_catalog_floor_cost
        << '\n'
        << "tcc_v2_opportunity_optimistic_exec_cost: "
        << temporal_plan_diagnostics.opportunity_optimistic_exec_cost
        << '\n'
        << "tcc_v2_predicted_kci_controllable_cost: "
        << temporal_plan_diagnostics.predicted_kci_controllable_cost
        << '\n'
        << "tcc_v2_predicted_tcc_controllable_cost: "
        << temporal_plan_diagnostics.predicted_tcc_controllable_cost
        << '\n'
        << "tcc_v2_final_planner_spent_cost: "
        << temporal_plan_diagnostics.final_planner_spent_cost << '\n'
        << "tcc_v2_final_incumbent_build_cost: "
        << temporal_plan_diagnostics.final_incumbent_build_cost << '\n'
        << "tcc_v2_final_catalog_cost: "
        << temporal_plan_diagnostics.final_catalog_cost << '\n'
        << "tcc_v2_final_state_expansion_cost: "
        << temporal_plan_diagnostics.final_state_expansion_cost << '\n'
        << "tcc_v2_final_extension_probe_cost: "
        << temporal_plan_diagnostics.final_extension_probe_cost << '\n'
        << "tcc_v2_final_interval_intersection_cost: "
        << temporal_plan_diagnostics.final_interval_intersection_cost
        << '\n'
        << "tcc_v2_final_rejected_tree_cost: "
        << temporal_plan_diagnostics.final_rejected_tree_cost << '\n'
        << "tcc_v2_final_cost_comparison_available: "
        << (temporal_plan_diagnostics.final_cost_comparison_available
                ? "true" : "false") << '\n'
        << "tcc_v2_final_candidate_build_exec_cost: "
        << temporal_plan_diagnostics.final_candidate_build_exec_cost
        << '\n'
        << "tcc_v2_final_candidate_h1_cost: "
        << temporal_plan_diagnostics.final_candidate_h1_cost << '\n'
        << "tcc_v2_final_kci_h1_cost: "
        << temporal_plan_diagnostics.final_kci_h1_cost << '\n'
        << "tcc_v2_incumbent_plan_fingerprint: "
        << temporal_plan_diagnostics.incumbent_plan_fingerprint << '\n'
        << "tcc_v2_incumbent_selectivity_formula_id: "
        << temporal_plan_diagnostics.incumbent_selectivity_formula_id << '\n'
        << "tcc_v2_selected_plan_fingerprint: "
        << temporal_plan_diagnostics.selected_plan_fingerprint << '\n'
        << "tcc_v2_selected_tree_handoff: "
        << (temporal_plan_diagnostics.selected_tree_handoff
                ? "true" : "false") << '\n'
        << "tcc_v2_incumbent_tree_build_ms: "
        << temporal_plan_diagnostics.incumbent_tree_build_milliseconds
        << '\n'
        << "tcc_v2_selected_tree_build_ms: "
        << temporal_plan_diagnostics.selected_tree_build_milliseconds
        << '\n';
    for (std::size_t score_index = 0;
         score_index < temporal_plan_diagnostics.fixed_plan_scores.size();
         ++score_index) {
        const TemporalFixedPlanEstimate& score =
            temporal_plan_diagnostics.fixed_plan_scores[score_index];
        timing_output
            << "tcc_v2_fixed_plan_" << score_index
            << "_fingerprint: " << score.plan_fingerprint << '\n'
            << "tcc_v2_fixed_plan_" << score_index
            << "_probe_cap: " << score.extension_probe_cap << '\n'
            << "tcc_v2_fixed_plan_" << score_index
            << "_probes: " << score.extension_probes << '\n'
            << "tcc_v2_fixed_plan_" << score_index
            << "_estimated_total: "
            << score.estimated_total_candidates << '\n'
            << "tcc_v2_fixed_plan_" << score_index
            << "_reused_incumbent: "
            << (score.reused_incumbent_score ? "true" : "false")
            << '\n';
    }
    timing_output
        << "metric_schema_version: logical_extensions_v1\n"
        << "dag_operator_statistics_schema_version: "
        << kDagOperatorStatisticsSchemaVersion << '\n'
        << "search_statistics_hooks_erased_at_build: "
        << (kSearchStatisticsHooksErasedAtBuild ? "true" : "false")
        << '\n'
        << "search_statistics_build_profile: "
        << kSearchStatisticsBuildProfile << '\n'
        << "gopher_source_identity: " << kGopherSourceIdentity << '\n'
        << "gopher_common_flags_identity: "
        << kGopherCommonFlagsIdentity << '\n'
        << "gopher_compiler_identity: "
        << kGopherCompilerIdentity << '\n'
        << "gopher_build_identity_bound: "
        << (gopherBuildIdentityBound() ? "true" : "false") << '\n'
        << "root_candidate_count: "
        << td_tree->rootCandidates().size() << '\n'
        << "candidate_relation_entries: "
        << td_tree->candidateRelationCount() << '\n'
        << "order: " << orderModeName(options.order_mode) << '\n'
        << "memo: " << memoModeName(options.memo_mode) << '\n'
        << "memo_policy: " << memoPolicyName(options.memo_policy) << '\n'
        << "runtime_key_mode: "
        << runtimeKeyModeName(options.runtime_key_mode) << '\n'
        << "runtime_key_diagnostics: "
        << runtimeKeyDiagnosticsModeName(
               options.runtime_key_diagnostics) << '\n'
        << "exact_runtime_key_cap: "
        << options.exact_runtime_key_cap << '\n'
        << "runtime_key_diagnostic_only: "
        << (options.runtime_key_diagnostics ==
                    RuntimeKeyDiagnosticsMode::Exact
                ? "true" : "false") << '\n'
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
        << "correctness_preflight: "
        << (options.correctness_preflight ? "true" : "false") << '\n'
        << "statistics_enabled: "
        << (options.collect_statistics ? "on" : "off") << '\n'
        << "off_overhead_comparison_eligible: "
        << (off_overhead_comparison_eligible ? "true" : "false")
        << '\n'
        << "cache_budget_bytes: "
        << options.memo_options.budget_bytes << '\n'
        << "cache_max_entry_bytes: "
        << options.memo_options.max_entry_bytes << '\n'
        << "cache_shards: "
        << options.memo_options.shard_count << '\n'
        << "cache_minimum_work: "
        << options.memo_options.minimum_estimated_work << '\n'
        << "cost_aware_admission: "
        << (options.memo_options.cost_aware_admission
                ? "true" : "false") << '\n'
        << "count_overflow: "
        << (count_overflow ? "true" : "false") << '\n';
    if (options.experiment_variant_explicit) {
        timing_output
            << "experiment_contract_version: "
            << experimentContractSchemaVersion(
                   options.experiment_variant) << '\n'
            << "experiment_variant: "
            << experimentVariantName(options.experiment_variant) << '\n';
        if (experimentVariantIsStaticCanonicalization(
                options.experiment_variant)) {
            timing_output
                << "dag_interning: "
                << experimentDagInterningName(
                       options.experiment_variant) << '\n';
        }
        timing_output
            << "scheduler_contract: roots\n"
            << "order_contract: paper\n"
            << "memo_admission: "
            << experimentAdmissionName(options.experiment_variant) << '\n'
            << "experiment_runtime_key_mode: "
            << experimentRuntimeKeyName(options.experiment_variant) << '\n'
            << "primary_timing_eligible: "
            << (primary_timing_eligible ? "true" : "false") << '\n'
            << "phase_schema_version: "
            << PreparedQueryPhaseTimings::schema_version << '\n'
            << "dag_compilation_applicable: "
            << (experiment_phases.dag_compilation_applicable
                    ? "true" : "false") << '\n'
            << "phase_planning_ns: "
            << experiment_phases.planning_nanoseconds << '\n'
            << "phase_td_construction_ns: "
            << experiment_phases.td_construction_nanoseconds << '\n'
            << "phase_dag_compilation_ns: "
            << experiment_phases.dag_compilation_nanoseconds << '\n'
            << "phase_enumeration_ns: "
            << experiment_phases.enumeration_nanoseconds << '\n'
            << "phase_output_finalization_ns: "
            << experiment_phases.output_finalization_nanoseconds << '\n'
            << "phase_finalization_boundary_residual_ns: "
            << phase_boundary_residual_ns << '\n'
            << "phase_prepared_query_total_ns: "
            << prepared_query_total_ns << '\n'
            << "phase_sum_matches_total: true\n";
    }
    if (options.engine == Engine::Gopher) {
        dsm::pure_online_v2::writePrimaryMetrics(
            timing_output, gopher_summary.primary_metrics);
        timing_output << "primary_excludes_save_res: true\n";
    }
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
                      << result_digest << '\n'
                      << "parallel_statistics_enabled: "
                      << (options.collect_statistics ? "on" : "off")
                      << '\n';
        if (options.collect_statistics) {
            timing_output
                << "parallel_statistics_collected: true\n"
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
            writeSearchStatistics(
                timing_output, search_statistics);
        } else {
            timing_output
                << "parallel_statistics_collected: false\n"
                << "statistics_collected: false\n";
        }
    }

    std::uint64_t enum_plus_build_nanoseconds = gopher_summary.enumeration_nanoseconds;
    if (!addNanoseconds(enum_plus_build_nanoseconds, gopher_summary.v5_build_nanoseconds)) {
        std::cerr << "Error: enumeration plus query-specific window build timer overflow.\n";
        return 4;
    }
    std::ofstream metrics_output(options.metrics_file);
    if (!metrics_output.is_open()) {
        std::cerr << "Error: Could not write "
                  << options.metrics_file.string() << '\n';
        return 4;
    }
    const std::vector<int>& reported_order =
        options.engine == Engine::Gopher
        ? plan.order
        : decomposition.dfs_order;
    const std::vector<int>& reported_parent =
        options.engine == Engine::Gopher
        ? plan.parent
        : decomposition.parent;
    const std::string reported_query_fingerprint =
        options.engine == Engine::Gopher
        ? std::to_string(plan.query_fingerprint)
        : "null";
    metrics_output
        << "{\n"
        << "  \"schema_version\": 2,\n"
        << "  \"metric_schema_version\": "
           "\"logical_extensions_v1\",\n"
        << "  \"dag_operator_statistics_schema_version\": \""
        << kDagOperatorStatisticsSchemaVersion << "\",\n"
        << "  \"search_statistics_hooks_erased_at_build\": "
        << (kSearchStatisticsHooksErasedAtBuild ? "true" : "false")
        << ",\n"
        << "  \"search_statistics_build_profile\": \""
        << kSearchStatisticsBuildProfile << "\",\n"
        << "  \"gopher_source_identity\": \""
        << jsonEscape(kGopherSourceIdentity) << "\",\n"
        << "  \"gopher_common_flags_identity\": \""
        << jsonEscape(kGopherCommonFlagsIdentity) << "\",\n"
        << "  \"gopher_compiler_identity\": \""
        << jsonEscape(kGopherCompilerIdentity) << "\",\n"
        << "  \"gopher_build_identity_bound\": "
        << (gopherBuildIdentityBound() ? "true" : "false")
        << ",\n"
        << "  \"engine\": \"" << engineName(options.engine)
        << "\",\n"
        << "  \"mode\": \""
        << matchOutputModeName(options.output_mode) << "\",\n"
        << "  \"label_mode\": \""
        << labelModeName(options.label_mode) << "\",\n";
    if (explicit_label_map_pointer != nullptr) {
        writeLabelProjectionReceiptJson(
            metrics_output, explicit_label_map);
        metrics_output << ",\n";
    }
    metrics_output
        << "  \"temporal_graph_content_fingerprint_fnv1a64\": "
        << temporal_graph_fingerprint << ",\n"
        << "  \"selectivity_mode\": \""
        << selectivityModeName(decomposition.selectivity_mode)
        << "\",\n"
        << "  \"selectivity_formula_id\": \""
        << selectivityFormulaId(decomposition.selectivity_mode)
        << "\",\n"
        << "  \"query_root\": " << decomposition.root << ",\n"
        << "  \"root_selectivity_score\": "
        << std::setprecision(17)
        << decomposition.temporal_selectivity[
               static_cast<std::size_t>(decomposition.root)]
        << ",\n"
        << "  \"tcc_planning_ms\": "
        << temporal_plan_diagnostics.planning_milliseconds << ",\n"
        << "  \"tcc_planning_ns\": "
        << temporal_plan_diagnostics.planning_nanoseconds << ",\n"
        << "  \"tcc_beam_width\": "
        << temporal_plan_diagnostics.beam_width << ",\n"
        << "  \"tcc_sample_cap\": "
        << temporal_plan_diagnostics.sample_cap << ",\n"
        << "  \"tcc_sample_seed\": "
        << temporal_plan_diagnostics.sample_seed << ",\n"
        << "  \"tcc_selected_estimated_total_candidates\": "
        << temporal_plan_diagnostics.selected_estimated_total_candidates
        << ",\n"
        << "  \"tcc_sampled_baseline_selected\": "
        << (temporal_plan_diagnostics.sampled_baseline_plan_selected
            ? "true" : "false") << ",\n"
        << "  \"tcc_root_plans_evaluated\": "
        << temporal_plan_diagnostics.root_plans.size() << ",\n"
        << "  \"tcc_selected_root_candidates\": "
        << temporal_plan_diagnostics.selected_root_candidates << ",\n"
        << "  \"tcc_selected_raw_extensions\": "
        << temporal_plan_diagnostics.selected_raw_extension_candidates
        << ",\n"
        << "  \"tcc_selected_root_plus_raw\": "
        << temporal_plan_diagnostics.selected_root_plus_raw_candidates
        << ",\n"
        << "  \"tcc_v2_decision\": \""
        << jsonEscape(temporal_plan_diagnostics.v2_decision)
        << "\",\n"
        << "  \"tcc_v2_decision_reason\": \""
        << jsonEscape(temporal_plan_diagnostics.v2_decision_reason)
        << "\",\n"
        << "  \"tcc_v2_admission_reason\": \""
        << jsonEscape(temporal_plan_diagnostics.v2_admission_reason)
        << "\",\n"
        << "  \"tcc_v2_fallback_phase\": \""
        << jsonEscape(temporal_plan_diagnostics.v2_fallback_phase)
        << "\",\n"
        << "  \"tcc_v2_fallback_reason\": \""
        << jsonEscape(temporal_plan_diagnostics.v2_fallback_reason)
        << "\",\n"
        << "  \"tcc_v2_budget_exhausted\": "
        << (temporal_plan_diagnostics.v2_budget_exhausted
                ? "true" : "false") << ",\n"
        << "  \"tcc_v2_cost_gate_enabled\": "
        << (temporal_plan_diagnostics.v2_cost_gate_enabled
                ? "true" : "false") << ",\n"
        << "  \"tcc_v2_cost_gate_bypassed\": "
        << (temporal_plan_diagnostics.v2_cost_gate_bypassed
                ? "true" : "false") << ",\n"
        << "  \"tcc_v2_forced_candidate_selected\": "
        << (temporal_plan_diagnostics.v2_forced_candidate_selected
                ? "true" : "false") << ",\n"
        << "  \"tcc_v2_catalog_probes\": "
        << temporal_plan_diagnostics.catalog_probes << ",\n"
        << "  \"tcc_v2_state_expansions\": "
        << temporal_plan_diagnostics.state_expansions << ",\n"
        << "  \"tcc_v2_extension_probes\": "
        << temporal_plan_diagnostics.extension_probes << ",\n"
        << "  \"tcc_v2_beam_extension_probes\": "
        << temporal_plan_diagnostics.beam_extension_probes << ",\n"
        << "  \"tcc_v2_incumbent_extension_probes\": "
        << temporal_plan_diagnostics.incumbent_extension_probes << ",\n"
        << "  \"tcc_v2_candidate_extension_probes\": "
        << temporal_plan_diagnostics.candidate_extension_probes << ",\n"
        << "  \"tcc_v2_fixed_plan_extension_probes\": "
        << temporal_plan_diagnostics.fixed_plan_extension_probes << ",\n"
        << "  \"tcc_v2_interval_intersections\": "
        << temporal_plan_diagnostics.interval_intersections << ",\n"
        << "  \"tcc_v2_temporal_edge_applications\": "
        << temporal_plan_diagnostics.temporal_edge_applications << ",\n"
        << "  \"tcc_v2_fixed_plan_scoring_method\": \""
        << jsonEscape(temporal_plan_diagnostics.fixed_plan_scoring_method)
        << "\",\n"
        << "  \"tcc_v2_pilot_extension_probes\": "
        << temporal_plan_diagnostics.pilot_extension_probes << ",\n"
        << "  \"tcc_v2_pilot_extension_probe_cap\": "
        << temporal_plan_diagnostics.pilot_extension_probe_cap << ",\n"
        << "  \"tcc_v2_pilot_prefix_cap\": "
        << temporal_plan_diagnostics.pilot_prefix_cap << ",\n"
        << "  \"tcc_v2_opportunity_margin_fraction\": "
        << temporal_plan_diagnostics.opportunity_margin_fraction
        << ",\n"
        << "  \"tcc_v2_catalog_probe_cost_weight\": "
        << temporal_plan_diagnostics.catalog_probe_cost_weight << ",\n"
        << "  \"tcc_v2_max_catalog_probes\": "
        << temporal_plan_diagnostics.max_catalog_probes << ",\n"
        << "  \"tcc_v2_max_state_expansions\": "
        << temporal_plan_diagnostics.max_state_expansions << ",\n"
        << "  \"tcc_v2_max_extension_probes\": "
        << temporal_plan_diagnostics.max_extension_probes << ",\n"
        << "  \"tcc_v2_max_interval_intersections\": "
        << temporal_plan_diagnostics.max_interval_intersections << ",\n"
        << "  \"tcc_v2_rescore_top_l\": "
        << temporal_plan_diagnostics.rescore_top_l << ",\n"
        << "  \"tcc_v2_max_candidate_trees_built\": "
        << temporal_plan_diagnostics.max_candidate_trees_built
        << ",\n"
        << "  \"tcc_v2_catalogue_complete_plans\": "
        << temporal_plan_diagnostics.catalogue_complete_plans << ",\n"
        << "  \"tcc_v2_candidate_plans_rescored\": "
        << temporal_plan_diagnostics.candidate_plans_rescored << ",\n"
        << "  \"tcc_v2_candidate_trees_built\": "
        << temporal_plan_diagnostics.candidate_trees_built << ",\n"
        << "  \"tcc_v2_candidate_tree_relation_entries_built\": "
        << temporal_plan_diagnostics
               .candidate_tree_relation_entries_built << ",\n"
        << "  \"tcc_v2_rejected_candidate_tree_relation_entries\": "
        << temporal_plan_diagnostics
               .rejected_candidate_tree_relation_entries << ",\n"
        << "  \"tcc_v2_opportunity_pilot_spent_cost\": "
        << temporal_plan_diagnostics.opportunity_pilot_spent_cost
        << ",\n"
        << "  \"tcc_v2_opportunity_pilot_state_cost\": "
        << temporal_plan_diagnostics.opportunity_pilot_state_cost
        << ",\n"
        << "  \"tcc_v2_opportunity_pilot_extension_cost\": "
        << temporal_plan_diagnostics.opportunity_pilot_extension_cost
        << ",\n"
        << "  \"tcc_v2_opportunity_pilot_interval_cost\": "
        << temporal_plan_diagnostics.opportunity_pilot_interval_cost
        << ",\n"
        << "  \"tcc_v2_opportunity_incumbent_build_cost\": "
        << temporal_plan_diagnostics.opportunity_incumbent_build_cost
        << ",\n"
        << "  \"tcc_v2_opportunity_catalog_floor_probes\": "
        << temporal_plan_diagnostics.opportunity_catalog_floor_probes
        << ",\n"
        << "  \"tcc_v2_opportunity_catalog_floor_cost\": "
        << temporal_plan_diagnostics.opportunity_catalog_floor_cost
        << ",\n"
        << "  \"tcc_v2_opportunity_optimistic_exec_cost\": "
        << temporal_plan_diagnostics.opportunity_optimistic_exec_cost
        << ",\n"
        << "  \"tcc_v2_incumbent_root_candidates\": "
        << temporal_plan_diagnostics.incumbent_root_candidates
        << ",\n"
        << "  \"tcc_v2_incumbent_raw_extensions\": "
        << temporal_plan_diagnostics.incumbent_raw_extension_candidates
        << ",\n"
        << "  \"tcc_v2_incumbent_relation_entries\": "
        << temporal_plan_diagnostics.incumbent_relation_entries
        << ",\n"
        << "  \"tcc_v2_predicted_kci_controllable_cost\": "
        << temporal_plan_diagnostics.predicted_kci_controllable_cost
        << ",\n"
        << "  \"tcc_v2_predicted_tcc_controllable_cost\": "
        << temporal_plan_diagnostics.predicted_tcc_controllable_cost
        << ",\n"
        << "  \"tcc_v2_final_candidate_h1_cost\": "
        << temporal_plan_diagnostics.final_candidate_h1_cost << ",\n"
        << "  \"tcc_v2_final_planner_spent_cost\": "
        << temporal_plan_diagnostics.final_planner_spent_cost
        << ",\n"
        << "  \"tcc_v2_final_incumbent_build_cost\": "
        << temporal_plan_diagnostics.final_incumbent_build_cost
        << ",\n"
        << "  \"tcc_v2_final_catalog_cost\": "
        << temporal_plan_diagnostics.final_catalog_cost << ",\n"
        << "  \"tcc_v2_final_state_expansion_cost\": "
        << temporal_plan_diagnostics.final_state_expansion_cost
        << ",\n"
        << "  \"tcc_v2_final_extension_probe_cost\": "
        << temporal_plan_diagnostics.final_extension_probe_cost
        << ",\n"
        << "  \"tcc_v2_final_interval_intersection_cost\": "
        << temporal_plan_diagnostics.final_interval_intersection_cost
        << ",\n"
        << "  \"tcc_v2_final_rejected_tree_cost\": "
        << temporal_plan_diagnostics.final_rejected_tree_cost
        << ",\n"
        << "  \"tcc_v2_final_cost_comparison_available\": "
        << (temporal_plan_diagnostics.final_cost_comparison_available
                ? "true" : "false") << ",\n"
        << "  \"tcc_v2_final_candidate_build_exec_cost\": "
        << temporal_plan_diagnostics.final_candidate_build_exec_cost
        << ",\n"
        << "  \"tcc_v2_final_kci_h1_cost\": "
        << temporal_plan_diagnostics.final_kci_h1_cost << ",\n"
        << "  \"tcc_v2_incumbent_plan_fingerprint\": "
        << temporal_plan_diagnostics.incumbent_plan_fingerprint
        << ",\n"
        << "  \"tcc_v2_incumbent_selectivity_formula_id\": \""
        << temporal_plan_diagnostics.incumbent_selectivity_formula_id
        << "\",\n"
        << "  \"tcc_v2_selected_plan_fingerprint\": "
        << temporal_plan_diagnostics.selected_plan_fingerprint
        << ",\n"
        << "  \"tcc_v2_selected_tree_handoff\": "
        << (temporal_plan_diagnostics.selected_tree_handoff
                ? "true" : "false") << ",\n"
        << "  \"tcc_v2_incumbent_tree_build_ms\": "
        << temporal_plan_diagnostics.incumbent_tree_build_milliseconds
        << ",\n"
        << "  \"tcc_v2_selected_tree_build_ms\": "
        << temporal_plan_diagnostics.selected_tree_build_milliseconds
        << ",\n"
        << "  \"tcc_v2_fixed_plan_scores\": [";
    for (std::size_t score_index = 0;
         score_index < temporal_plan_diagnostics.fixed_plan_scores.size();
         ++score_index) {
        if (score_index != 0) metrics_output << ',';
        const TemporalFixedPlanEstimate& score =
            temporal_plan_diagnostics.fixed_plan_scores[score_index];
        metrics_output
            << "\n    {\"fingerprint\": " << score.plan_fingerprint
            << ", \"root\": " << score.root
            << ", \"incumbent\": "
            << (score.incumbent ? "true" : "false")
            << ", \"reused_incumbent\": "
            << (score.reused_incumbent_score ? "true" : "false")
            << ", \"probe_cap\": " << score.extension_probe_cap
            << ", \"probes\": " << score.extension_probes
            << ", \"estimated_total\": "
            << score.estimated_total_candidates
            << ", \"estimate_exact\": "
            << (score.estimated_total_exact ? "true" : "false")
            << '}';
    }
    if (!temporal_plan_diagnostics.fixed_plan_scores.empty()) {
        metrics_output << '\n';
    }
    metrics_output
        << "  ],\n"
        << "  \"root_candidate_count\": "
        << td_tree->rootCandidates().size() << ",\n"
        << "  \"candidate_relation_entries\": "
        << td_tree->candidateRelationCount() << ",\n"
        << "  \"order\": \"" << orderModeName(options.order_mode)
        << "\",\n"
        << "  \"memo\": \"" << memoModeName(options.memo_mode)
        << "\",\n"
        << "  \"memo_policy\": \""
        << memoPolicyName(options.memo_policy) << "\",\n"
        << "  \"runtime_key_mode\": \""
        << runtimeKeyModeName(options.runtime_key_mode) << "\",\n"
        << "  \"runtime_key_diagnostics\": \""
        << runtimeKeyDiagnosticsModeName(
               options.runtime_key_diagnostics) << "\",\n"
        << "  \"exact_runtime_key_cap\": "
        << options.exact_runtime_key_cap << ",\n"
        << "  \"runtime_key_diagnostic_only\": "
        << (options.runtime_key_diagnostics ==
                    RuntimeKeyDiagnosticsMode::Exact
                ? "true" : "false") << ",\n"
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
        << "  \"correctness_preflight\": "
        << (options.correctness_preflight ? "true" : "false")
        << ",\n"
        << "  \"statistics_enabled\": "
        << (options.collect_statistics ? "true" : "false") << ",\n"
        << "  \"off_overhead_comparison_eligible\": "
        << (off_overhead_comparison_eligible ? "true" : "false")
        << ",\n"
        << "  \"cache_budget_bytes\": "
        << options.memo_options.budget_bytes << ",\n"
        << "  \"cache_max_entry_bytes\": "
        << options.memo_options.max_entry_bytes << ",\n"
        << "  \"cache_shards\": "
        << options.memo_options.shard_count << ",\n"
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
        << jsonEscape(result_digest) << "\",\n";
    if (options.experiment_variant_explicit) {
        metrics_output
            << "  \"experiment_contract\": {\n"
            << "    \"schema_version\": \""
            << experimentContractSchemaVersion(
                   options.experiment_variant)
            << "\",\n"
            << "    \"variant\": \""
            << experimentVariantName(options.experiment_variant)
            << "\",\n";
        if (experimentVariantIsStaticCanonicalization(
                options.experiment_variant)) {
            metrics_output
                << "    \"dag_interning\": \""
                << experimentDagInterningName(
                       options.experiment_variant)
                << "\",\n";
        }
        metrics_output
            << "    \"scheduler\": \"roots\",\n"
            << "    \"order\": \"paper\",\n"
            << "    \"memo_admission\": \""
            << experimentAdmissionName(options.experiment_variant)
            << "\",\n"
            << "    \"runtime_key_mode\": \""
            << experimentRuntimeKeyName(options.experiment_variant)
            << "\",\n"
            << "    \"correctness_preflight\": "
            << (options.correctness_preflight ? "true" : "false")
            << ",\n"
            << "    \"primary_timing_eligible\": "
            << (primary_timing_eligible ? "true" : "false") << ",\n"
            << "    \"phase_schema_version\": \""
            << PreparedQueryPhaseTimings::schema_version << "\",\n"
            << "    \"dag_compilation_applicable\": "
            << (experiment_phases.dag_compilation_applicable
                    ? "true" : "false") << ",\n"
            << "    \"phases_nanoseconds\": {\n"
            << "      \"planning\": "
            << experiment_phases.planning_nanoseconds << ",\n"
            << "      \"td_construction\": "
            << experiment_phases.td_construction_nanoseconds << ",\n"
            << "      \"dag_compilation\": "
            << experiment_phases.dag_compilation_nanoseconds << ",\n"
            << "      \"enumeration\": "
            << experiment_phases.enumeration_nanoseconds << ",\n"
            << "      \"output_finalization\": "
            << experiment_phases.output_finalization_nanoseconds << ",\n"
            << "      \"finalization_boundary_residual\": "
            << phase_boundary_residual_ns << ",\n"
            << "      \"prepared_query_total\": "
            << prepared_query_total_ns << "\n"
            << "    },\n"
            << "    \"phase_sum_matches_total\": true\n"
            << "  },\n";
    } else {
        metrics_output << "  \"experiment_contract\": null,\n";
    }
    metrics_output
        << "  \"parallel_statistics\": {\n"
        << "    \"diagnostics_enabled\": "
        << (options.collect_statistics ? "true" : "false");
    auto parallel_uint = [&](
                              const char* name,
                              std::uint64_t value) {
        metrics_output << ",\n    \"" << name << "\": ";
        if (options.collect_statistics) {
            metrics_output << value;
        } else {
            metrics_output << "null";
        }
    };
    auto parallel_double = [&](const char* name, double value) {
        metrics_output << ",\n    \"" << name << "\": ";
        if (options.collect_statistics) {
            metrics_output << value;
        } else {
            metrics_output << "null";
        }
    };
    parallel_uint("root_tasks_claimed", parallel_statistics.root_tasks_claimed);
    parallel_uint(
        "prefix_tasks_spawned",
        parallel_statistics.prefix_tasks_spawned);
    parallel_uint(
        "prefix_split_attempts",
        parallel_statistics.prefix_split_attempts);
    parallel_uint(
        "prefix_split_rejected",
        parallel_statistics.prefix_split_rejected);
    parallel_uint("queue_claims", parallel_statistics.queue_claims);
    parallel_uint("queue_peak", parallel_statistics.queue_peak);
    parallel_uint(
        "queue_state_bytes_peak",
        parallel_statistics.queue_state_bytes_peak);
    parallel_uint(
        "workers_with_tasks",
        static_cast<std::uint64_t>(parallel_statistics.workers_with_tasks));
    parallel_uint("worker_tasks_min", parallel_statistics.worker_tasks_min);
    parallel_uint("worker_tasks_max", parallel_statistics.worker_tasks_max);
    parallel_uint("worker_tasks_total", parallel_statistics.worker_tasks_total);
    parallel_uint(
        "worker_dfs_states_min",
        parallel_statistics.worker_dfs_states_min);
    parallel_uint(
        "worker_dfs_states_max",
        parallel_statistics.worker_dfs_states_max);
    parallel_uint(
        "worker_dfs_states_total",
        parallel_statistics.worker_dfs_states_total);
    parallel_uint(
        "worker_active_nanoseconds_min",
        parallel_statistics.worker_active_nanoseconds_min);
    parallel_uint(
        "worker_active_nanoseconds_max",
        parallel_statistics.worker_active_nanoseconds_max);
    parallel_uint(
        "worker_active_nanoseconds_total",
        parallel_statistics.worker_active_nanoseconds_total);
    parallel_double("worker_tasks_cv", parallel_statistics.worker_tasks_cv);
    parallel_double(
        "worker_dfs_states_cv",
        parallel_statistics.worker_dfs_states_cv);
    parallel_double(
        "worker_active_nanoseconds_cv",
        parallel_statistics.worker_active_nanoseconds_cv);
    parallel_double(
        "worker_utilization",
        parallel_statistics.worker_utilization);
    metrics_output
        << ",\n    \"task_queue_capacity\": "
        << parallel_statistics.task_queue_capacity
        << ",\n    \"prefix_depth_limit\": "
        << parallel_statistics.prefix_depth_limit
        << "\n  },\n"
        << "  \"dag_topology\": ";
    if (experiment_phases.dag_compilation_applicable) {
        writeDagTopologyJson(
            metrics_output,
            dag_topology,
            options.experiment_variant_explicit &&
                experimentVariantIsStaticCanonicalization(
                    options.experiment_variant));
    } else {
        metrics_output << "null";
    }
    metrics_output << ",\n";
    metrics_output << "  \"jindex_execution\": "
        << (gopher_summary.jindex_execution_json.empty() ? "null" : gopher_summary.jindex_execution_json)
        << ",\n";
    if (options.experiment_variant_explicit &&
        experimentVariantIsStaticCanonicalization(
            options.experiment_variant)) {
        metrics_output << "  \"dag_compilation_audit\": ";
        if (collect_dag_compilation_audit) {
            writeDagCompilationAuditJson(
                metrics_output,
                dag,
                dag_compilation_audit,
                dag_intern_mode);
        } else {
            metrics_output << "null";
        }
        metrics_output << ",\n";
    }
    metrics_output
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
        << "    \"finalized_adjacency_temporal_capacity\": "
        << *load_timings.adjacency_temporal_capacity_bytes << ",\n"
        << "    \"graph\": " << graph_memory << ",\n"
        << "    \"td_tree\": " << td_tree_memory << ",\n"
        << "    \"dag\": " << dag_memory << ",\n"
        << "    \"cache\": " << cache_memory_bytes << ",\n"
        << "    \"other\": " << other_memory << ",\n"
        << "    \"peak_rss\": " << peak_memory << "\n"
        << "  },\n"
        << "  \"plan\": {\n"
        << "    \"id\": " << plan.plan_id << ",\n"
        << "    \"query_fingerprint\": "
        << reported_query_fingerprint << ",\n"
        << "    \"root\": " << decomposition.root << ",\n"
        << "    \"order\": [";
    for (std::size_t index = 0;
         index < reported_order.size();
         ++index) {
        if (index > 0) metrics_output << ", ";
        metrics_output << reported_order[index];
    }
    metrics_output
        << "],\n"
        << "    \"parent\": [";
    for (std::size_t index = 0;
         index < reported_parent.size();
         ++index) {
        if (index > 0) metrics_output << ", ";
        metrics_output << reported_parent[index];
    }
    metrics_output
        << "]\n"
        << "  },\n"
        << "  \"statistics\": {";
    writeStatisticsJson(
        metrics_output,
        search_statistics,
        options.collect_statistics);
    metrics_output << "  },\n";
    operator_counters.writeJsonMembers(metrics_output, options.ops_counters);
    adaptive_diagnostics.writeJsonMembers(metrics_output, options.adaptive_diagnostics || options.neighbor_copy_timing);
    metrics_output << "  \"order_simulation_mode\": \"" << orderSimulationModeName(options.order_simulation) << "\",\n";
    metrics_output << "  \"order_simulation\": ";
    if (options.order_simulation == OrderSimulationMode::Disabled) metrics_output << "null";
    else gopher_summary.order_simulation.writeJson(metrics_output);
    metrics_output << ",\n";
    metrics_output << "  \"jindex_simulation\": ";
    if (jindex_simulation) jindex_simulation->writeJson(metrics_output);
    else metrics_output << "null";
    metrics_output << ",\n";
    std::uint64_t retained_occurrences = 0;
    for (const auto& edge : temporal_graph.temporal_edges) retained_occurrences += edge.active_snapshot_count;
    metrics_output
        << "  \"ops_dag_passes\": \"" << dagPassesName(options.dag_passes) << "\",\n"
        << "  \"physical_alpha\": " << options.physical_alpha << ",\n"
        << "  \"ops_direct_opt\": " << (options.direct_opt ? "true" : "false") << ",\n"
        << "  \"direct_p4\": " << (options.direct_p4 ? "true" : "false") << ",\n"
        << "  \"window_algebra\": " << (options.window_algebra ? "true" : "false") << ",\n"
        << "  \"v8_raw_bitset\": " << (options.raw_bitset ? "true" : "false") << ",\n"
        << "  \"v8_query_k\": " << options.minimum_duration << ",\n"
        << "  \"window_build\": \"" << (options.window_lazy ? "lazy" : "eager") << "\",\n"
        << "  \"jindex\": \"" << (options.jindex_mode == JIndexMode::None ? "off" :
            options.jindex_mode == JIndexMode::Adaptive ? "adaptive" : "always") << "\",\n"
        << "  \"jindex_alpha\": " << options.jindex_alpha << ",\n"
        << "  \"jindex_block_size\": " << options.jindex_block_size << ",\n"
        << "  \"v6_wstore_loading\": " << (gopher_summary.wstore_loading_json.empty() ? "null" : gopher_summary.wstore_loading_json) << ",\n"
        << "  \"v6_bindex_execution\": " << (gopher_summary.bindex_execution_json.empty() ? "null" : gopher_summary.bindex_execution_json) << ",\n"
        << "  \"v6_bindex\": \"" << (options.bindex_mode == BIndexMode::None ? "off" :
            options.bindex_mode == BIndexMode::Always ? "always" : "adaptive") << "\",\n"
        << "  \"v6_bindex_alpha\": " << options.bindex_alpha << ",\n"
        << "  \"v6_bindex_c\": " << options.bindex_reuse_threshold << ",\n"
        << "  \"enum_start_monotonic_ns\": " << gopher_summary.enum_start_monotonic_ns << ",\n"
        << "  \"enum_end_monotonic_ns\": " << gopher_summary.enum_end_monotonic_ns << ",\n"
        << "  \"v5_build_nanoseconds\": " << gopher_summary.v5_build_nanoseconds << ",\n"
        << "  \"v5_lazy_build_thread_nanoseconds\": " << gopher_summary.v5_lazy_build_thread_nanoseconds << ",\n"
        << "  \"enum_plus_build_nanoseconds\": " << enum_plus_build_nanoseconds << ",\n"
        << "  \"v5_window_bytes\": " << gopher_summary.v5_window_bytes << ",\n"
        << "  \"v5_window_relevant_edges\": " << gopher_summary.v5_window_relevant_edges << ",\n"
        << "  \"v5_window_built_edges\": " << gopher_summary.v5_window_built_edges << ",\n"
        << "  \"v5_window_forced_intervals\": " << (gopher_summary.v5_window_forced_intervals ? "true" : "false") << ",\n"
        << "  \"ops_lmax_filter\": " << (options.lmax_filter ? "true" : "false") << ",\n"
        << "  \"ops_lmax_cache_payload_bytes\": " << temporal_graph.temporal_edges.size() * sizeof(int) << ",\n"
        << "  \"ops_lmax_cache_incremental_bytes\": 0,\n"
        << "  \"ops_dag_nodes_eliminated_by_passes\": " << dag.nodes_eliminated_by_passes << ",\n"
        << "  \"ops_retained_edges\": " << temporal_graph.temporal_edges.size() << ",\n"
        << "  \"ops_retained_occurrences\": " << retained_occurrences << ",\n"
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
        << "label_seed: " << options.label_seed << '\n';
    if (explicit_label_map_pointer != nullptr) {
        writeLabelProjectionReceiptText(
            std::cout, explicit_label_map);
    }
    std::cout
        << "temporal_graph_content_fingerprint_fnv1a64: "
        << temporal_graph_fingerprint << '\n'
        << "selectivity_mode: "
        << selectivityModeName(decomposition.selectivity_mode) << '\n'
        << "selectivity_formula_id: "
        << selectivityFormulaId(decomposition.selectivity_mode) << '\n'
        << "query_root: " << decomposition.root << '\n'
        << "root_selectivity_score: "
        << std::setprecision(17)
        << decomposition.temporal_selectivity[
               static_cast<std::size_t>(decomposition.root)]
        << '\n'
        << "tcc_planning_ms: "
        << temporal_plan_diagnostics.planning_milliseconds << '\n'
        << "tcc_selected_estimated_total_candidates: "
        << temporal_plan_diagnostics.selected_estimated_total_candidates
        << '\n'
        << "metric_schema_version: logical_extensions_v1\n"
        << "dag_operator_statistics_schema_version: "
        << kDagOperatorStatisticsSchemaVersion << '\n'
        << "search_statistics_hooks_erased_at_build: "
        << (kSearchStatisticsHooksErasedAtBuild ? "true" : "false")
        << '\n'
        << "search_statistics_build_profile: "
        << kSearchStatisticsBuildProfile << '\n'
        << "gopher_source_identity: " << kGopherSourceIdentity << '\n'
        << "gopher_common_flags_identity: "
        << kGopherCommonFlagsIdentity << '\n'
        << "gopher_compiler_identity: "
        << kGopherCompilerIdentity << '\n'
        << "gopher_build_identity_bound: "
        << (gopherBuildIdentityBound() ? "true" : "false") << '\n'
        << "root_candidate_count: "
        << td_tree->rootCandidates().size() << '\n'
        << "candidate_relation_entries: "
        << td_tree->candidateRelationCount() << '\n'
        << "order: " << orderModeName(options.order_mode) << '\n'
        << "memo: " << memoModeName(options.memo_mode) << '\n'
        << "memo_policy: " << memoPolicyName(options.memo_policy) << '\n'
        << "runtime_key_mode: "
        << runtimeKeyModeName(options.runtime_key_mode) << '\n'
        << "runtime_key_diagnostics: "
        << runtimeKeyDiagnosticsModeName(
               options.runtime_key_diagnostics) << '\n'
        << "exact_runtime_key_cap: "
        << options.exact_runtime_key_cap << '\n'
        << "runtime_key_diagnostic_only: "
        << (options.runtime_key_diagnostics ==
                    RuntimeKeyDiagnosticsMode::Exact
                ? "true" : "false") << '\n'
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
        << "correctness_preflight: "
        << (options.correctness_preflight ? "true" : "false") << '\n'
        << "statistics_enabled: "
        << (options.collect_statistics ? "on" : "off") << '\n'
        << "off_overhead_comparison_eligible: "
        << (off_overhead_comparison_eligible ? "true" : "false")
        << '\n'
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
    if (options.experiment_variant_explicit) {
        std::cout
            << "experiment_contract_version: "
            << experimentContractSchemaVersion(
                   options.experiment_variant) << '\n'
            << "experiment_variant: "
            << experimentVariantName(options.experiment_variant) << '\n';
        if (experimentVariantIsStaticCanonicalization(
                options.experiment_variant)) {
            std::cout
                << "dag_interning: "
                << experimentDagInterningName(
                       options.experiment_variant) << '\n';
        }
        std::cout
            << "scheduler_contract: roots\n"
            << "order_contract: paper\n"
            << "memo_admission: "
            << experimentAdmissionName(options.experiment_variant) << '\n'
            << "experiment_runtime_key_mode: "
            << experimentRuntimeKeyName(options.experiment_variant) << '\n'
            << "primary_timing_eligible: "
            << (primary_timing_eligible ? "true" : "false") << '\n'
            << "phase_prepared_query_total_ns: "
            << prepared_query_total_ns << '\n';
    }
    if (options.engine == Engine::Gopher) {
        std::cout
            << "parallel_statistics_enabled: "
            << (options.collect_statistics ? "on" : "off") << '\n';
        if (options.collect_statistics) {
            std::cout
                << "parallel_statistics_collected: true\n"
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
            writeSearchStatistics(
                std::cout, search_statistics);
        } else {
            std::cout
                << "parallel_statistics_collected: false\n"
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
