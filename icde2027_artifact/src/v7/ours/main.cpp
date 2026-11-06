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
#include <memory>
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
#include "temporal_cost_planner.h"
#include "v3_run_contract.h"

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
    // Protocol-only tokens are dispatched before legacy arity parsing and
    // before any graph/input I/O.  Mixed or malformed invocations therefore
    // fail closed instead of falling through to the positional CLI.
    if (dsm::v3::containsV3CliToken(argc, argv)) {
        return dsm::v3::runClosedV3Cli(argc, argv);
    }
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <Data Graph> <Query Graph> <Minimum Duration k> "
                     "[Label Seed] [--count-only] "
                     "[--label-mode sequential-rng|stable-id] "
                     "[--selectivity "
                     "tdtree|paper|intended|legacy-reciprocal|legacy-extracted-product|structural|survival|"
                     "temporal-cost|temporal-cost-v2] "
                     "[--prefilter on|off] "
                     "[--threads N] "
                     "[--tcc-beam-width N] "
                     "[--tcc-sample-cap N] "
                     "[--tcc-max-catalog-probes N] "
                     "[--tcc-max-state-expansions N] "
                     "[--tcc-max-extension-probes N] "
                     "[--tcc-max-interval-intersections N] "
                     "[--tcc-pilot-extension-probes N] "
                     "[--tcc-pilot-prefix-cap N] "
                     "[--tcc-rescore-top-l N] "
                     "[--tcc-max-candidate-trees N] "
                     "[--tcc-opportunity-margin F] "
                     "[--tcc-catalog-probe-cost-weight F] "
                     "[--tcc-cost-gate on|off]\n";
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
    TemporalCostPlannerOptions temporal_cost_options;
    bool tcc_beam_width_seen = false;
    bool tcc_sample_cap_seen = false;
    bool tcc_max_catalog_probes_seen = false;
    bool tcc_max_state_expansions_seen = false;
    bool tcc_max_extension_probes_seen = false;
    bool tcc_max_interval_intersections_seen = false;
    bool tcc_pilot_extension_probes_seen = false;
    bool tcc_pilot_prefix_cap_seen = false;
    bool tcc_rescore_top_l_seen = false;
    bool tcc_max_candidate_trees_seen = false;
    bool tcc_opportunity_margin_seen = false;
    bool tcc_catalog_probe_cost_weight_seen = false;
    bool tcc_cost_gate_seen = false;
    auto set_selectivity_mode = [&](const std::string& value) {
        if (selectivity_mode_seen) {
            std::cerr << "Error: --selectivity may be specified only once.\n";
            return false;
        }
        if (!parseSelectivityMode(value, selectivity_mode)) {
            std::cerr
                << "Error: selectivity mode must be tdtree, paper, intended, "
                   "legacy-reciprocal, legacy-extracted-product, structural, survival, "
                   "temporal-cost, or temporal-cost-v2.\n";
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
    auto set_tcc_cost_gate = [&](const std::string& value) {
        if (tcc_cost_gate_seen) {
            std::cerr
                << "Error: --tcc-cost-gate may be specified only once.\n";
            return false;
        }
        if (value == "on") {
            temporal_cost_options.cost_gate_enabled = true;
        } else if (value == "off") {
            temporal_cost_options.cost_gate_enabled = false;
        } else {
            std::cerr
                << "Error: --tcc-cost-gate must be on or off.\n";
            return false;
        }
        tcc_cost_gate_seen = true;
        return true;
    };
    auto set_positive_size = [](
                                 const std::string& option,
                                 const std::string& value,
                                 std::size_t& target,
                                 bool& seen) {
        if (seen) {
            std::cerr << "Error: " << option
                      << " may be specified only once.\n";
            return false;
        }
        try {
            if (value.empty() || value.front() == '-') {
                throw std::invalid_argument(option);
            }
            std::size_t parsed_characters = 0;
            const unsigned long long parsed =
                std::stoull(value, &parsed_characters);
            if (parsed_characters != value.size() ||
                parsed == 0 ||
                parsed > std::numeric_limits<std::size_t>::max()) {
                throw std::out_of_range(option);
            }
            target = static_cast<std::size_t>(parsed);
            seen = true;
            return true;
        } catch (const std::exception&) {
            std::cerr << "Error: " << option
                      << " must be a positive integer.\n";
            return false;
        }
    };
    auto set_nonnegative_u64 = [](
                                   const std::string& option,
                                   const std::string& value,
                                   std::uint64_t& target,
                                   bool& seen) {
        if (seen) {
            std::cerr << "Error: " << option
                      << " may be specified only once.\n";
            return false;
        }
        try {
            if (value.empty() || value.front() == '-') {
                throw std::invalid_argument(option);
            }
            std::size_t parsed_characters = 0;
            const unsigned long long parsed =
                std::stoull(value, &parsed_characters);
            if (parsed_characters != value.size()) {
                throw std::invalid_argument(option);
            }
            target = static_cast<std::uint64_t>(parsed);
            seen = true;
            return true;
        } catch (const std::exception&) {
            std::cerr << "Error: " << option
                      << " must be a non-negative integer.\n";
            return false;
        }
    };
    auto set_nonnegative_double = [](
                                      const std::string& option,
                                      const std::string& value,
                                      double& target,
                                      bool& seen) {
        if (seen) {
            std::cerr << "Error: " << option
                      << " may be specified only once.\n";
            return false;
        }
        try {
            std::size_t parsed_characters = 0;
            const double parsed =
                std::stod(value, &parsed_characters);
            if (parsed_characters != value.size() ||
                !std::isfinite(parsed) || parsed < 0.0) {
                throw std::invalid_argument(option);
            }
            target = parsed;
            seen = true;
            return true;
        } catch (const std::exception&) {
            std::cerr << "Error: " << option
                      << " must be a finite non-negative number.\n";
            return false;
        }
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
        if (argument == "--tcc-cost-gate") {
            if (argument_index + 1 >= argc) {
                std::cerr
                    << "Error: --tcc-cost-gate requires a value.\n";
                return 1;
            }
            if (!set_tcc_cost_gate(argv[++argument_index])) return 1;
            continue;
        }
        constexpr const char* tcc_cost_gate_prefix =
            "--tcc-cost-gate=";
        if (argument.rfind(tcc_cost_gate_prefix, 0) == 0) {
            if (!set_tcc_cost_gate(argument.substr(
                    std::char_traits<char>::length(
                        tcc_cost_gate_prefix)))) {
                return 1;
            }
            continue;
        }
        if (argument == "--tcc-beam-width") {
            if (argument_index + 1 >= argc) {
                std::cerr
                    << "Error: --tcc-beam-width requires a value.\n";
                return 1;
            }
            if (!set_positive_size(
                    "--tcc-beam-width",
                    argv[++argument_index],
                    temporal_cost_options.beam_width,
                    tcc_beam_width_seen)) {
                return 1;
            }
            continue;
        }
        constexpr const char* tcc_beam_width_prefix =
            "--tcc-beam-width=";
        if (argument.rfind(tcc_beam_width_prefix, 0) == 0) {
            if (!set_positive_size(
                    "--tcc-beam-width",
                    argument.substr(
                        std::char_traits<char>::length(
                            tcc_beam_width_prefix)),
                    temporal_cost_options.beam_width,
                    tcc_beam_width_seen)) {
                return 1;
            }
            continue;
        }
        if (argument == "--tcc-sample-cap") {
            if (argument_index + 1 >= argc) {
                std::cerr
                    << "Error: --tcc-sample-cap requires a value.\n";
                return 1;
            }
            if (!set_positive_size(
                    "--tcc-sample-cap",
                    argv[++argument_index],
                    temporal_cost_options.sample_cap,
                    tcc_sample_cap_seen)) {
                return 1;
            }
            continue;
        }
        constexpr const char* tcc_sample_cap_prefix =
            "--tcc-sample-cap=";
        if (argument.rfind(tcc_sample_cap_prefix, 0) == 0) {
            if (!set_positive_size(
                    "--tcc-sample-cap",
                    argument.substr(
                        std::char_traits<char>::length(
                            tcc_sample_cap_prefix)),
                    temporal_cost_options.sample_cap,
                    tcc_sample_cap_seen)) {
                return 1;
            }
            continue;
        }
        auto parse_tcc_budget = [&](
                                    const char* option,
                                    std::uint64_t& target,
                                    bool& seen) {
            const std::string prefix = std::string(option) + "=";
            if (argument == option) {
                if (argument_index + 1 >= argc) {
                    std::cerr << "Error: " << option
                              << " requires a value.\n";
                    return -1;
                }
                return set_nonnegative_u64(
                           option,
                           argv[++argument_index],
                           target,
                           seen)
                    ? 1
                    : -1;
            }
            if (argument.rfind(prefix, 0) == 0) {
                return set_nonnegative_u64(
                           option,
                           argument.substr(prefix.size()),
                           target,
                           seen)
                    ? 1
                    : -1;
            }
            return 0;
        };
        int tcc_budget_result = parse_tcc_budget(
            "--tcc-max-catalog-probes",
            temporal_cost_options.max_catalog_probes,
            tcc_max_catalog_probes_seen);
        if (tcc_budget_result < 0) return 1;
        if (tcc_budget_result > 0) continue;
        if (argument == "--tcc-pilot-prefix-cap") {
            if (argument_index + 1 >= argc) {
                std::cerr
                    << "Error: --tcc-pilot-prefix-cap requires a value.\n";
                return 1;
            }
            if (!set_positive_size(
                    "--tcc-pilot-prefix-cap",
                    argv[++argument_index],
                    temporal_cost_options.pilot_prefix_cap,
                    tcc_pilot_prefix_cap_seen)) {
                return 1;
            }
            continue;
        }
        constexpr const char* tcc_pilot_prefix_cap_prefix =
            "--tcc-pilot-prefix-cap=";
        if (argument.rfind(tcc_pilot_prefix_cap_prefix, 0) == 0) {
            if (!set_positive_size(
                    "--tcc-pilot-prefix-cap",
                    argument.substr(std::char_traits<char>::length(
                        tcc_pilot_prefix_cap_prefix)),
                    temporal_cost_options.pilot_prefix_cap,
                    tcc_pilot_prefix_cap_seen)) {
                return 1;
            }
            continue;
        }
        if (argument == "--tcc-rescore-top-l") {
            if (argument_index + 1 >= argc) {
                std::cerr
                    << "Error: --tcc-rescore-top-l requires a value.\n";
                return 1;
            }
            if (!set_positive_size(
                    "--tcc-rescore-top-l",
                    argv[++argument_index],
                    temporal_cost_options.rescore_top_l,
                    tcc_rescore_top_l_seen)) {
                return 1;
            }
            continue;
        }
        constexpr const char* tcc_rescore_top_l_prefix =
            "--tcc-rescore-top-l=";
        if (argument.rfind(tcc_rescore_top_l_prefix, 0) == 0) {
            if (!set_positive_size(
                    "--tcc-rescore-top-l",
                    argument.substr(std::char_traits<char>::length(
                        tcc_rescore_top_l_prefix)),
                    temporal_cost_options.rescore_top_l,
                    tcc_rescore_top_l_seen)) {
                return 1;
            }
            continue;
        }
        tcc_budget_result = parse_tcc_budget(
            "--tcc-max-state-expansions",
            temporal_cost_options.max_state_expansions,
            tcc_max_state_expansions_seen);
        if (tcc_budget_result < 0) return 1;
        if (tcc_budget_result > 0) continue;
        tcc_budget_result = parse_tcc_budget(
            "--tcc-max-extension-probes",
            temporal_cost_options.max_extension_probes,
            tcc_max_extension_probes_seen);
        if (tcc_budget_result < 0) return 1;
        if (tcc_budget_result > 0) continue;
        tcc_budget_result = parse_tcc_budget(
            "--tcc-max-interval-intersections",
            temporal_cost_options.max_interval_intersections,
            tcc_max_interval_intersections_seen);
        if (tcc_budget_result < 0) return 1;
        if (tcc_budget_result > 0) continue;
        tcc_budget_result = parse_tcc_budget(
            "--tcc-max-candidate-trees",
            temporal_cost_options.max_candidate_trees_built,
            tcc_max_candidate_trees_seen);
        if (tcc_budget_result < 0) return 1;
        if (tcc_budget_result > 0) continue;
        tcc_budget_result = parse_tcc_budget(
            "--tcc-pilot-extension-probes",
            temporal_cost_options.pilot_extension_probe_cap,
            tcc_pilot_extension_probes_seen);
        if (tcc_budget_result < 0) return 1;
        if (tcc_budget_result > 0) continue;
        auto parse_tcc_double = [&] (
                                    const char* option,
                                    double& target,
                                    bool& seen) {
            const std::string prefix = std::string(option) + "=";
            if (argument == option) {
                if (argument_index + 1 >= argc) {
                    std::cerr << "Error: " << option
                              << " requires a value.\n";
                    return -1;
                }
                return set_nonnegative_double(
                           option,
                           argv[++argument_index],
                           target,
                           seen)
                    ? 1
                    : -1;
            }
            if (argument.rfind(prefix, 0) == 0) {
                return set_nonnegative_double(
                           option,
                           argument.substr(prefix.size()),
                           target,
                           seen)
                    ? 1
                    : -1;
            }
            return 0;
        };
        int tcc_double_result = parse_tcc_double(
            "--tcc-opportunity-margin",
            temporal_cost_options.opportunity_margin_fraction,
            tcc_opportunity_margin_seen);
        if (tcc_double_result < 0) return 1;
        if (tcc_double_result > 0) continue;
        tcc_double_result = parse_tcc_double(
            "--tcc-catalog-probe-cost-weight",
            temporal_cost_options.catalog_probe_cost_weight,
            tcc_catalog_probe_cost_weight_seen);
        if (tcc_double_result < 0) return 1;
        if (tcc_double_result > 0) continue;
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
    const bool temporal_cost_mode =
        selectivity_mode == SelectivityMode::TemporalCost ||
        selectivity_mode == SelectivityMode::TemporalCostV2;
    if ((tcc_beam_width_seen || tcc_sample_cap_seen) &&
        !temporal_cost_mode) {
        std::cerr
            << "Error: --tcc-beam-width and --tcc-sample-cap require "
               "--selectivity temporal-cost or temporal-cost-v2.\n";
        return 1;
    }
    if ((tcc_max_catalog_probes_seen ||
         tcc_max_state_expansions_seen ||
         tcc_max_extension_probes_seen ||
         tcc_max_interval_intersections_seen ||
         tcc_pilot_extension_probes_seen ||
         tcc_pilot_prefix_cap_seen ||
         tcc_rescore_top_l_seen ||
         tcc_max_candidate_trees_seen ||
         tcc_opportunity_margin_seen ||
         tcc_catalog_probe_cost_weight_seen ||
         tcc_cost_gate_seen) &&
        selectivity_mode != SelectivityMode::TemporalCostV2) {
        std::cerr
            << "Error: deterministic TCC work-budget controls require "
               "--selectivity temporal-cost-v2.\n";
        return 1;
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
    std::uint64_t controllable_query_processing_ns = 0;

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
    const std::uint64_t temporal_graph_fingerprint =
        temporalGraphContentFingerprint(temporal_graph);
    std::cout << "Temporal graph: " << temporal_graph.input_occurrence_count
              << " occurrences, " << temporal_graph.input_unique_edge_count
              << " unique directed edges, " << temporal_graph.filtered_edge_count
              << " retained edges, " << temporal_graph.num_vertices
              << " active vertices, label mode="
              << labelModeName(label_mode)
              << ", label seed=" << label_seed << ".\n";
    std::cout << "Temporal graph content fingerprint (FNV-1a-64): "
              << temporal_graph_fingerprint << ".\n";
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

    std::array<double, kLabelCount>
        label_mean_vertex_active_snapshots{};
    for (std::size_t label = 0; label < kLabelCount; ++label) {
        if (label_vertex_counts[label] > 0) {
            label_mean_vertex_active_snapshots[label] =
                static_cast<double>(label_duration_sums[label]) /
                static_cast<double>(label_vertex_counts[label]);
        }
    }
    timings["labelStatistics"] = elapsedMilliseconds(stage_start);

    SurvivalSelectivityStatistics survival_statistics;
    const SurvivalSelectivityStatistics* survival_statistics_pointer =
        nullptr;
    if (selectivity_mode == SelectivityMode::Structural ||
        selectivity_mode == SelectivityMode::Survival) {
        stage_start = std::chrono::steady_clock::now();
        survival_statistics =
            computeSurvivalSelectivityStatistics(
                temporal_graph,
                query_graph,
                minimum_duration);
        timings["survivalSelectivityStatistics"] =
            elapsedMilliseconds(stage_start);
        survival_statistics_pointer = &survival_statistics;
    }

    TemporalPlanDiagnostics temporal_plan_diagnostics;
    const auto controllable_start = std::chrono::steady_clock::now();
    stage_start = std::chrono::steady_clock::now();
    QueryDecomposition decomposition;
    std::unique_ptr<TDTree> planned_td_tree;
    if (selectivity_mode == SelectivityMode::TemporalCost) {
        temporal_cost_options.sample_seed = label_seed;
        decomposition = planTemporalCostQuery(
            temporal_graph,
            query_graph,
            minimum_duration,
            label_vertex_counts,
            label_mean_vertex_active_snapshots,
            temporal_cost_options,
            &temporal_plan_diagnostics);
        timings["temporalCostPlanning"] =
            temporal_plan_diagnostics.planning_milliseconds;
    } else if (selectivity_mode == SelectivityMode::TemporalCostV2) {
        temporal_cost_options.sample_seed = label_seed;
        TemporalCostPlanResult plan_result = planTemporalCostV2(
            temporal_graph,
            query_graph,
            minimum_duration,
            label_vertex_counts,
            label_mean_vertex_active_snapshots,
            temporal_cost_options,
            &temporal_plan_diagnostics);
        decomposition = std::move(plan_result.decomposition);
        planned_td_tree = std::move(plan_result.selected_tree);
        timings["temporalCostPlanning"] =
            temporal_plan_diagnostics.planning_milliseconds;
    } else {
        decomposition = decomposeQuery(
            query_graph,
            label_vertex_counts,
            label_mean_vertex_active_snapshots,
            selectivity_mode,
            survival_statistics_pointer);
        timings["queryDecomposition"] =
            elapsedMilliseconds(stage_start);
    }
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
              << " selectivity_formula_id="
              << selectivityFormulaId(decomposition.selectivity_mode)
              << " selectivity_score=" << std::setprecision(17)
              << decomposition.temporal_selectivity[decomposition.root]
              << " | plan order:";
    for (int query_vertex : decomposition.dfs_order) std::cout << ' ' << query_vertex;
    std::cout << " | non-tree edges=" << decomposition.non_tree_edges.size() << '\n';

    stage_start = std::chrono::steady_clock::now();
    std::unique_ptr<TDTree> td_tree;
    if (planned_td_tree != nullptr) {
        td_tree = std::move(planned_td_tree);
        // The handed-off tree was built inside temporalCostPlanning.  This
        // stage records only additional caller-side build work.
        timings["buildTDTree"] = 0;
    } else {
        td_tree = std::make_unique<TDTree>(
            temporal_graph,
            query_graph,
            decomposition,
            minimum_duration);
        timings["buildTDTree"] = elapsedMilliseconds(stage_start);
    }
    td_tree->print_res();

    const std::string matching_result_file = "matching_results_" + dataset_name + ".txt";
    const MatchSummary match_summary = td_tree->save_res(
        matching_result_file, output_mode, threads);
    if (!match_summary.output_written) {
        std::cerr << "Error: Could not write " << matching_result_file << '\n';
        return 4;
    }
    timings["enumerateMatches"] = match_summary.enumeration_milliseconds;
    const auto controllable_elapsed =
        std::chrono::steady_clock::now() - controllable_start;
    controllable_query_processing_ns =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                controllable_elapsed).count());
    timings["controllable"] =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            controllable_elapsed).count();
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
                  << '\n';
    if (decomposition.selectivity_mode ==
            SelectivityMode::TemporalCost ||
        decomposition.selectivity_mode ==
            SelectivityMode::TemporalCostV2) {
        timing_output
            << "temporal_cost_estimated_total_candidates: "
            << std::setprecision(17)
            << temporal_plan_diagnostics
                   .selected_estimated_total_candidates
            << '\n'
            << "temporal_cost_beam_width: "
            << temporal_plan_diagnostics.beam_width << '\n'
            << "temporal_cost_sample_cap: "
            << temporal_plan_diagnostics.sample_cap << '\n'
            << "temporal_cost_sample_seed: "
            << temporal_plan_diagnostics.sample_seed << '\n'
            << "temporal_cost_planning_ns: "
            << temporal_plan_diagnostics.planning_nanoseconds << '\n'
            << "temporal_cost_sampled_baseline_plan_selected: "
            << (temporal_plan_diagnostics
                        .sampled_baseline_plan_selected
                    ? "true"
                    : "false")
            << '\n'
            << "temporal_cost_sampled_baseline_estimated_total: "
            << std::setprecision(17)
            << temporal_plan_diagnostics
                   .sampled_baseline_estimated_total_candidates
            << '\n'
            << "temporal_cost_sampled_baseline_estimate_exact: "
            << (temporal_plan_diagnostics
                        .sampled_baseline_estimate_exact
                    ? "true"
                    : "false")
            << '\n'
            << "temporal_cost_roots_evaluated: "
            << temporal_plan_diagnostics.root_plans.size()
            << '\n'
            << "temporal_cost_selected_root_candidates: "
            << temporal_plan_diagnostics.selected_root_candidates
            << '\n'
            << "temporal_cost_selected_raw_extensions: "
            << temporal_plan_diagnostics
                   .selected_raw_extension_candidates
            << '\n'
            << "temporal_cost_selected_root_plus_raw: "
            << temporal_plan_diagnostics
                   .selected_root_plus_raw_candidates
            << '\n';
        if (temporal_plan_diagnostics.v2_enabled) {
            timing_output
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
                << (temporal_plan_diagnostics
                            .v2_forced_candidate_selected
                        ? "true" : "false") << '\n'
                << "tcc_v2_max_catalog_probes: "
                << temporal_plan_diagnostics.max_catalog_probes << '\n'
                << "tcc_v2_max_state_expansions: "
                << temporal_plan_diagnostics.max_state_expansions << '\n'
                << "tcc_v2_max_extension_probes: "
                << temporal_plan_diagnostics.max_extension_probes << '\n'
                << "tcc_v2_max_interval_intersections: "
                << temporal_plan_diagnostics.max_interval_intersections
                << '\n'
                << "tcc_v2_max_candidate_trees_built: "
                << temporal_plan_diagnostics.max_candidate_trees_built
                << '\n'
                << "tcc_v2_pilot_extension_probe_cap: "
                << temporal_plan_diagnostics.pilot_extension_probe_cap
                << '\n'
                << "tcc_v2_pilot_prefix_cap: "
                << temporal_plan_diagnostics.pilot_prefix_cap << '\n'
                << "tcc_v2_catalog_probes: "
                << temporal_plan_diagnostics.catalog_probes << '\n'
                << "tcc_v2_state_expansions: "
                << temporal_plan_diagnostics.state_expansions << '\n'
                << "tcc_v2_extension_probes: "
                << temporal_plan_diagnostics.extension_probes << '\n'
                << "tcc_v2_beam_extension_probes: "
                << temporal_plan_diagnostics.beam_extension_probes << '\n'
                << "tcc_v2_incumbent_extension_probes: "
                << temporal_plan_diagnostics.incumbent_extension_probes
                << '\n'
                << "tcc_v2_candidate_extension_probes: "
                << temporal_plan_diagnostics.candidate_extension_probes
                << '\n'
                << "tcc_v2_fixed_plan_extension_probes: "
                << temporal_plan_diagnostics.fixed_plan_extension_probes
                << '\n'
                << "tcc_v2_interval_intersections: "
                << temporal_plan_diagnostics.interval_intersections << '\n'
                << "tcc_v2_temporal_edge_applications: "
                << temporal_plan_diagnostics.temporal_edge_applications
                << '\n'
                << "tcc_v2_fixed_plan_scoring_method: "
                << temporal_plan_diagnostics.fixed_plan_scoring_method
                << '\n'
                << "tcc_v2_fixed_plan_scores: "
                << temporal_plan_diagnostics.fixed_plan_scores.size()
                << '\n'
                << "tcc_v2_pilot_extension_probes: "
                << temporal_plan_diagnostics.pilot_extension_probes
                << '\n'
                << "tcc_v2_predicted_kci_controllable_cost: "
                << temporal_plan_diagnostics
                       .predicted_kci_controllable_cost << '\n'
                << "tcc_v2_predicted_tcc_controllable_cost: "
                << temporal_plan_diagnostics
                       .predicted_tcc_controllable_cost << '\n'
                << "tcc_v2_opportunity_safety_margin: "
                << temporal_plan_diagnostics
                       .opportunity_safety_margin << '\n'
                << "tcc_v2_opportunity_margin_fraction: "
                << temporal_plan_diagnostics
                       .opportunity_margin_fraction << '\n'
                << "tcc_v2_catalog_probe_cost_weight: "
                << temporal_plan_diagnostics
                       .catalog_probe_cost_weight << '\n'
                << "tcc_v2_opportunity_pilot_spent_cost: "
                << temporal_plan_diagnostics
                       .opportunity_pilot_spent_cost << '\n'
                << "tcc_v2_opportunity_pilot_state_cost: "
                << temporal_plan_diagnostics
                       .opportunity_pilot_state_cost << '\n'
                << "tcc_v2_opportunity_pilot_extension_cost: "
                << temporal_plan_diagnostics
                       .opportunity_pilot_extension_cost << '\n'
                << "tcc_v2_opportunity_pilot_interval_cost: "
                << temporal_plan_diagnostics
                       .opportunity_pilot_interval_cost << '\n'
                << "tcc_v2_opportunity_incumbent_build_cost: "
                << temporal_plan_diagnostics
                       .opportunity_incumbent_build_cost << '\n'
                << "tcc_v2_opportunity_catalog_floor_probes: "
                << temporal_plan_diagnostics
                       .opportunity_catalog_floor_probes << '\n'
                << "tcc_v2_opportunity_catalog_floor_cost: "
                << temporal_plan_diagnostics
                       .opportunity_catalog_floor_cost << '\n'
                << "tcc_v2_opportunity_optimistic_exec_cost: "
                << temporal_plan_diagnostics
                       .opportunity_optimistic_exec_cost << '\n'
                << "tcc_v2_incumbent_root_candidates: "
                << temporal_plan_diagnostics
                       .incumbent_root_candidates << '\n'
                << "tcc_v2_incumbent_raw_extensions: "
                << temporal_plan_diagnostics
                       .incumbent_raw_extension_candidates << '\n'
                << "tcc_v2_incumbent_relation_entries: "
                << temporal_plan_diagnostics
                       .incumbent_relation_entries << '\n'
                << "tcc_v2_final_planner_spent_cost: "
                << temporal_plan_diagnostics
                       .final_planner_spent_cost << '\n'
                << "tcc_v2_final_incumbent_build_cost: "
                << temporal_plan_diagnostics
                       .final_incumbent_build_cost << '\n'
                << "tcc_v2_final_catalog_cost: "
                << temporal_plan_diagnostics.final_catalog_cost << '\n'
                << "tcc_v2_final_state_expansion_cost: "
                << temporal_plan_diagnostics
                       .final_state_expansion_cost << '\n'
                << "tcc_v2_final_extension_probe_cost: "
                << temporal_plan_diagnostics
                       .final_extension_probe_cost << '\n'
                << "tcc_v2_final_interval_intersection_cost: "
                << temporal_plan_diagnostics
                       .final_interval_intersection_cost << '\n'
                << "tcc_v2_final_rejected_tree_cost: "
                << temporal_plan_diagnostics
                       .final_rejected_tree_cost << '\n'
                << "tcc_v2_final_cost_comparison_available: "
                << (temporal_plan_diagnostics
                            .final_cost_comparison_available
                        ? "true" : "false") << '\n'
                << "tcc_v2_final_candidate_build_exec_cost: "
                << temporal_plan_diagnostics
                       .final_candidate_build_exec_cost << '\n'
                << "tcc_v2_final_candidate_h1_cost: "
                << temporal_plan_diagnostics.final_candidate_h1_cost
                << '\n'
                << "tcc_v2_final_kci_h1_cost: "
                << temporal_plan_diagnostics.final_kci_h1_cost
                << '\n'
                << "tcc_v2_incumbent_plan_fingerprint: "
                << temporal_plan_diagnostics
                       .incumbent_plan_fingerprint << '\n'
                << "tcc_v2_incumbent_selectivity_formula_id: "
                << temporal_plan_diagnostics
                       .incumbent_selectivity_formula_id << '\n'
                << "tcc_v2_selected_plan_fingerprint: "
                << temporal_plan_diagnostics
                       .selected_plan_fingerprint << '\n'
                << "tcc_v2_candidate_trees_built: "
                << temporal_plan_diagnostics.candidate_trees_built
                << '\n'
                << "tcc_v2_rescore_top_l: "
                << temporal_plan_diagnostics.rescore_top_l << '\n'
                << "tcc_v2_catalogue_complete_plans: "
                << temporal_plan_diagnostics.catalogue_complete_plans
                << '\n'
                << "tcc_v2_candidate_plans_rescored: "
                << temporal_plan_diagnostics.candidate_plans_rescored
                << '\n'
                << "tcc_v2_candidate_tree_relation_entries_built: "
                << temporal_plan_diagnostics
                       .candidate_tree_relation_entries_built << '\n'
                << "tcc_v2_rejected_candidate_tree_relation_entries: "
                << temporal_plan_diagnostics
                       .rejected_candidate_tree_relation_entries << '\n'
                << "tcc_v2_selected_tree_handoff: "
                << (temporal_plan_diagnostics.selected_tree_handoff
                        ? "true" : "false") << '\n'
                << "tcc_v2_incumbent_tree_build_ms: "
                << temporal_plan_diagnostics
                       .incumbent_tree_build_milliseconds << '\n'
                << "tcc_v2_selected_tree_build_ms: "
                << temporal_plan_diagnostics
                       .selected_tree_build_milliseconds << '\n';
            for (std::size_t score_index = 0;
                 score_index <
                     temporal_plan_diagnostics.fixed_plan_scores.size();
                 ++score_index) {
                const TemporalFixedPlanEstimate& score =
                    temporal_plan_diagnostics
                        .fixed_plan_scores[score_index];
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
                    << (score.reused_incumbent_score
                            ? "true" : "false") << '\n';
            }
        }
        for (const TemporalRootPlanEstimate& root_plan :
             temporal_plan_diagnostics.root_plans) {
            timing_output
                << "temporal_cost_root_q" << root_plan.root
                << "_estimated_total_candidates: "
                << std::setprecision(17)
                << root_plan.estimated_total_candidates << '\n'
                << "temporal_cost_root_q" << root_plan.root
                << "_catalogue_estimated_total_candidates: "
                << root_plan
                       .catalogue_estimated_total_candidates
                << '\n'
                << "temporal_cost_root_q" << root_plan.root
                << "_estimate_exact: "
                << (root_plan.estimated_total_exact
                        ? "true"
                        : "false")
                << '\n';
            for (std::size_t step = 0;
                 step < root_plan.transitions.size();
                 ++step) {
                const TemporalTransitionEstimate& transition =
                    root_plan.transitions[step];
                timing_output
                    << "temporal_cost_root_q" << root_plan.root
                    << "_step_" << (step + 1)
                    << "_sampled_prefixes: "
                    << transition.sampled_prefixes << '\n'
                    << "temporal_cost_root_q" << root_plan.root
                    << "_step_" << (step + 1)
                    << "_input_population_exact: "
                    << (transition.input_prefix_population_exact
                            ? "true"
                            : "false")
                    << '\n'
                    << "temporal_cost_root_q" << root_plan.root
                    << "_step_" << (step + 1)
                    << "_overall_extension_pass_rate: "
                    << std::setprecision(17)
                    << transition.overall_extension_pass_rate
                    << '\n';
            }
        }
    }
    if (decomposition.structural_candidate_counts.size() ==
            static_cast<std::size_t>(query_graph.num_vertices) &&
        decomposition.compatible_edge_counts.size() ==
            static_cast<std::size_t>(query_graph.num_vertices) &&
        decomposition.durable_compatible_edge_counts.size() ==
            static_cast<std::size_t>(query_graph.num_vertices) &&
        decomposition.durability_survival_rates.size() ==
            static_cast<std::size_t>(query_graph.num_vertices)) {
        const std::size_t root_index =
            static_cast<std::size_t>(decomposition.root);
        timing_output
            << "root_structural_candidate_count: "
            << decomposition.structural_candidate_counts[root_index]
            << '\n'
            << "root_compatible_edge_count: "
            << decomposition.compatible_edge_counts[root_index]
            << '\n'
            << "root_durable_compatible_edge_count: "
            << decomposition.durable_compatible_edge_counts[root_index]
            << '\n'
            << "root_durability_survival_rate: "
            << std::setprecision(17)
            << decomposition.durability_survival_rates[root_index]
            << '\n';
    }
    timing_output << "threads_requested: "
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
                  << td_tree->candidateRelationCount() << '\n'
                  << "dfs_states: "
                  << match_summary.dfs_states << '\n'
                  << "metric_schema_version: logical_extensions_v1\n"
                  << "raw_extension_candidates: "
                  << match_summary.generated_candidates << '\n'
                  << "realized_root_plus_raw_candidates: "
                  << (match_summary.generated_candidates >
                              std::numeric_limits<std::uint64_t>::max() -
                                  match_summary.root_candidate_count
                          ? std::numeric_limits<std::uint64_t>::max()
                          : match_summary.generated_candidates +
                                match_summary.root_candidate_count)
                  << '\n'
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
    timing_output << "\nplan_parent:";
    for (int parent : decomposition.parent) {
        timing_output << ' ' << parent;
    }
    timing_output << '\n';
    timing_output << "controllableQueryProcessing_ns: "
                  << controllable_query_processing_ns << '\n';
    timing_output << "depth_0_root_candidates: "
                  << match_summary.root_candidate_count << '\n';
    for (std::size_t depth = 1;
         depth < decomposition.dfs_order.size();
         ++depth) {
        timing_output
            << "depth_" << depth
            << "_query_vertex: "
            << decomposition.dfs_order[depth] << '\n'
            << "depth_" << depth
            << "_raw_extensions: "
            << match_summary
                   .generated_candidates_by_depth[depth]
            << '\n'
            << "depth_" << depth
            << "_accepted_extensions: "
            << match_summary
                   .accepted_candidates_by_depth[depth]
            << '\n'
            << "depth_" << depth
            << "_pruned_injectivity: "
            << match_summary
                   .pruned_injectivity_by_depth[depth]
            << '\n'
            << "depth_" << depth
            << "_pruned_temporal_support: "
            << match_summary
                   .pruned_temporal_support_by_depth[depth]
            << '\n';
    }
    const std::array<const char*, 12> timing_order{{
        "readTemporalGraph",
        "filterTemporalGraph",
        "readAndFilterTemporalGraph",
        "readQueryGraph",
        "labelStatistics",
        "survivalSelectivityStatistics",
        "temporalCostPlanning",
        "queryDecomposition",
        "buildTDTree",
        "enumerateMatches",
        "controllable",
        "endToEnd"}};
    for (const char* timing_name : timing_order) {
        const auto timing = timings.find(timing_name);
        if (timing != timings.end()) {
            timing_output << timing->first << ": " << timing->second << " ms\n";
        }
    }

    const std::size_t input_graph_memory = temporal_graph.getMemoryUsage();
    const std::size_t td_tree_memory = td_tree->getMemoryUsage();
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
              << td_tree->candidateRelationCount() << '\n'
              << "controllableQueryProcessing_ns: "
              << controllable_query_processing_ns << '\n'
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
