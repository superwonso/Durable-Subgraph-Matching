// Production P8 correctness extension. Preserve the original 17-configuration
// driver and its receipts byte-for-byte: reuse its independent byte-time oracle
// and fixture constructors, but NEVER invoke its renamed main().
#define main preserved_v5_seventeen_configuration_driver_not_invoked
#include "test_v5_matcher.cpp"
#undef main

#include "../JIndexExecution.h"
#include <cctype>

namespace {

struct P8Configuration {
    Configuration base;
    JIndexMode mode = JIndexMode::Always;
    double alpha = 1.0;
};

std::vector<P8Configuration> p8Configurations() {
    std::vector<P8Configuration> result;
    for (bool lazy : {false, true}) for (double alpha : {0.5, 1.0, 2.0}) {
        const std::string suffix = std::string(lazy ? "_lazy" : "_eager")
            + "_a" + (alpha == 0.5 ? "05" : alpha == 1.0 ? "10" : "20");
        result.push_back({{"direct_p4_w_jidx_always" + suffix, -1, true, true, lazy},
                          JIndexMode::Always, alpha});
        for (int mask : {1, 3, 5, 7}) for (JIndexMode mode : {JIndexMode::Adaptive, JIndexMode::Always}) {
            const auto name = std::string("dag_mask") + std::to_string(mask) + "_w_jidx_"
                + (mode == JIndexMode::Adaptive ? "adaptive" : "always") + suffix;
            result.push_back({{name, mask, false, true, lazy}, mode, alpha});
        }
    }
    require(result.size() == 54, "P8 fixed valid configuration matrix is incomplete");
    return result;
}

std::string p8Object(const std::string& text, const std::string& marker) {
    const auto found = text.find(marker);
    require(found != std::string::npos, "missing P8 JSON marker " + marker);
    std::size_t begin = found + marker.size();
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    require(begin < text.size() && text[begin] == '{', "P8 JSON field is not an object");
    int depth = 0;
    bool quoted = false, escaped = false;
    for (std::size_t i = begin; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return text.substr(begin, i - begin + 1);
    }
    throw std::runtime_error("unterminated P8 JSON object");
}

std::uint64_t p8Number(const std::string& text, const std::string& key) {
    const auto marker = '"' + key + "\":";
    const auto found = text.find(marker);
    require(found != std::string::npos, "missing P8 integer " + key);
    std::size_t begin = found + marker.size();
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    const auto end = text.find_first_not_of("0123456789", begin);
    require(begin < text.size() && end != begin, "P8 value is not an unsigned integer");
    return std::stoull(text.substr(begin, end - begin));
}

void p8Native(const Fixture& fixture, const QueryPlan& plan, const TDTree& tree,
              const CandidateDag* dag, const P8Configuration& configuration, std::size_t B,
              std::size_t threads, bool counted, NativeResult& result) {
    result = NativeResult{};
    result.counted = counted;
    Observation observation;
    observation.fixture = &fixture;
    OperatorCounters counters;
    GopherMatcherOptions options;
    options.threads = threads;
    options.parallel_mode = ParallelMode::Roots;
    options.early_prune = true;
    options.result_digest = true;
    options.window_algebra = true;
    options.window_lazy = configuration.base.lazy;
    options.direct_p4 = configuration.base.direct_p4;
    options.jindex_mode = configuration.mode;
    options.jindex_alpha = configuration.alpha;
    options.jindex_block_size = B;
    options.jindex_verify_superset = true; // Real indexed branch vs exact necessary-condition check.
    options.executor.memo_mode = MemoMode::None;
    options.executor.collect_statistics = false;
    options.executor.ops = counted ? &counters : nullptr;
    options.exact_match_observer = observe;
    options.exact_match_observer_context = &observation;
    auto capture = [&] {
        result.answers = std::move(observation.answers);
        result.callback_threads = observation.threads.size();
        result.duplicate = observation.duplicate;
        result.invalid = observation.invalid;
#define SAVE_P8_OP(name) result.ops[#name] = counters.ops_##name.load(std::memory_order_relaxed);
        DSM_OPS_FIELDS(SAVE_P8_OP)
#undef SAVE_P8_OP
    };
    try {
        if (dag) {
            GopherMatcher matcher(fixture.graph, fixture.query, plan, *dag, tree, fixture.k, options);
            result.summary = matcher.run(MatchOutputMode::CountOnly);
        } else {
            GopherMatcher matcher(fixture.graph, fixture.query, plan, tree, fixture.k, options);
            result.summary = matcher.run(MatchOutputMode::CountOnly);
        }
    } catch (...) {
        capture();
        throw;
    }
    capture();
}

void p8Check(const Fixture& fixture, const Answers& expected, const NativeResult& actual,
             const P8Configuration& config, std::size_t B, const NativeResult& direct) {
    checkResult(expected, actual, config.base);
    require(actual.summary.result_digest == direct.summary.result_digest,
            "P8 mapping digest differs from unchanged legacy Direct");
    if (actual.counted)
        require(actual.ops.at("logical_extensions") == direct.ops.at("logical_extensions"),
                "P8 fixed-plan logical extensions differ from legacy Direct");
    const auto& json = actual.summary.jindex_execution_json;
    require(json.find("\"schema\":\"v5_jindex_execution_v1\"") != std::string::npos
            && json.find("\"production\":true") != std::string::npos
            && json.find("\"observer_only\":false") != std::string::npos,
            "P8 native execution profile is missing or is only a shadow observer");
    require(json.find("\"plans_frozen\":true") != std::string::npos
            && json.find("\"status\":\"OK\"") != std::string::npos
            && json.find("\"counter_overflow\":false") != std::string::npos
            && json.find("\"verify_superset\":true") != std::string::npos,
            "P8 fixed correctness fixture has invalid/unavailable index state");
    require(p8Number(json, "B") == B && p8Number(json, "k") == static_cast<std::uint64_t>(fixture.k),
            "P8 block size/minimum duration metadata differs");
    const std::string mode = config.mode == JIndexMode::Adaptive ? "adaptive" : "always";
    require(json.find("\"mode\":\"" + mode + '"') != std::string::npos,
            "P8 execution mode differs");
    require(p8Number(json, "persistent_index_bytes") <= p8Number(json, "cap_bytes")
            && p8Number(json, "peak_index_plus_build_scratch_bytes") <= p8Number(json, "cap_bytes"),
            "P8 fixed fixture exceeded its index memory cap");
    // The J-index is once-lazy in both W modes. This auxiliary thread-time
    // includes its build work exactly once; lazy W may additionally build its
    // own window entries. It is NOT an extra term in enum-plus-build time.
    const auto index_build_thread_ns = p8Number(json, "build_thread_cpu_ns_sum");
    if (config.base.lazy)
        require(actual.summary.v5_lazy_build_thread_nanoseconds >= index_build_thread_ns,
                "lazy W auxiliary time omitted production J-index build work");
    else
        require(actual.summary.v5_lazy_build_thread_nanoseconds == index_build_thread_ns,
                "eager W auxiliary time differs from production J-index build work");
    if (actual.counted) {
        require(json.find("\"counters_enabled\":true") != std::string::npos
                && json.find("\"thread_totals_equal_node_sums\":true") != std::string::npos,
                "P8 operation evidence is missing");
        const auto totals = p8Object(json, "\"totals\":");
        require(p8Number(totals, "states") == p8Number(totals, "index_selected")
                                            + p8Number(totals, "scan_selected"),
                "P8 indexed/scan selections do not reconcile");
    } else {
        require(json.find("\"counters_enabled\":false") != std::string::npos
                && json.find("\"totals\":null") != std::string::npos,
                "P8 counters OFF collected a totals object");
    }
}

void p8Failure(const fs::path& out, const Fixture& fixture, const std::string& arm,
               const std::string& reason, const Answers& expected, const NativeResult& actual,
               std::size_t B, std::size_t threads) {
    counterexample(out, fixture, arm, reason, expected, actual);
    if (!fs::exists(out / "p8_counterexample_context.json")) {
        std::ostringstream json;
        json << "{\"arm\":" << quote(arm) << ",\"B\":" << B << ",\"threads\":" << threads
             << ",\"superset_debug_enabled\":true,\"production_profile\":";
        if (actual.summary.jindex_execution_json.empty()) json << "null";
        else json << actual.summary.jindex_execution_json;
        json << ",\"no_retry\":true}\n";
        writeNew(out / "p8_counterexample_context.json", json.str());
    }
}

// Label-compatible answer space is 8 roots x 4 anchors x 8 candidates. Extra
// wrong-label neighbors make the CANONICAL in/out driver degree >=64 without
// increasing the brute-force oracle's mapping space. Narrow and broad root J
// alternate; padding occupies the late half, so adaptive search sees cheap
// index states and expensive index states without changing any runtime setting.
Fixture p8LargeFixture(int horizon, int k, int variant) {
    Fixture f;
    f.shape = variant ? "p8_reciprocal_root_nonroot_loops" : "p8_reciprocal_nonroot_loop";
    f.horizon = horizon; f.k = k; f.origin = variant ? -1025 : -65;
    f.instance = variant; f.stratum = horizon; f.force_root_zero = true;
    f.arcs = {{0, 1}, {1, 2}, {2, 1}, {2, 2}};
    if (variant) f.arcs.emplace_back(0, 0);
    initializeQuery(f, {0, 1, 2});
    const int vertices = variant ? 128 : 96;
    initializeHistories(f, vertices);
    f.graph.vertex_labels.assign(vertices, Label{3});
    for (int v = 0; v < 8; ++v) f.graph.vertex_labels[v] = Label{0};
    for (int v = 8; v < 12; ++v) f.graph.vertex_labels[v] = Label{1};
    for (int v = 12; v < 20; ++v) f.graph.vertex_labels[v] = Label{2};
    auto fill = [&](int from, int to, int begin, int end) {
        for (int t = std::max(0, begin); t <= std::min(horizon - 1, end); ++t)
            f.histories[from][to][t] = 1;
    };
    for (int root = 0; root < 8; ++root) {
        if (root % 2 == 0) fill(root, 8 + root % 4, 0, k - 1);
        else fill(root, 8 + root % 4, 0, horizon - 1);
        if (variant && root != 7) fill(root, root, 0, horizon - 1);
    }
    for (int anchor = 8; anchor < 12; ++anchor) {
        for (int candidate = 12; candidate < 20; ++candidate) {
            const int pattern = candidate - 12;
            if (pattern == 0) {
                fill(anchor, candidate, 0, horizon - 1);
                fill(candidate, anchor, 0, horizon - 1);
            } else {
                const std::array<int, 7> starts{0, horizon - k, horizon / 2, 15, 63, 127, horizon - k};
                const int first = std::min(horizon - k, starts[pattern - 1]);
                fill(anchor, candidate, first, first + k - 1);
                fill(candidate, anchor, first, first + k - 1);
                if (pattern == 4) {
                    fill(anchor, candidate, horizon - k, horizon - 1);
                    fill(candidate, anchor, horizon - k, horizon - 1);
                }
                // Positive-duration individual reciprocal edges with disjoint
                // histories must fail the exact common support test.
                if (pattern == 6) {
                    std::fill(f.histories[candidate][anchor].begin(),
                              f.histories[candidate][anchor].end(), 0);
                    fill(candidate, anchor, horizon - k, horizon - 1);
                }
            }
        }
        for (int padding = 20; padding < vertices; ++padding) {
            fill(anchor, padding, horizon / 2, horizon - 1);
            fill(padding, anchor, horizon / 2, horizon - 1);
        }
    }
    for (int candidate = 12; candidate < 20; ++candidate) {
        if (candidate == 19) fill(candidate, candidate, 0, k - 2); // Short loop: never a match.
        else fill(candidate, candidate, 0, horizon - 1);
    }
    finishGraph(f);
    for (int anchor = 8; anchor < 12; ++anchor)
        require(f.graph.adj[anchor].size() >= 64 && f.graph.in_adj[anchor].size() >= 64,
                "large fixture cannot reach the production index eligibility threshold");
    return f;
}

struct P8Coverage {
    std::uint64_t indexed = 0, scanned = 0, cost_rejected = 0, built_indexes = 0;
    std::uint64_t universal_fallback = 0, small_degree_fallback = 0;
};

void p8LargeAndParallel(const fs::path& out) {
    const auto configs = p8Configurations();
    std::map<std::string, P8Coverage> coverage;
    std::uint64_t fixtures = 0, pairs = 0, ops_off = 0, max_callbacks = 0;
    std::ofstream log(out / "p8_large_parallel_checks.csv");
    log << "fixture,horizon,k,B,configuration,answers,logical_extensions,candidates,"
           "index_selected,scan_selected,cost_rejected_states,built_indexes,threads_used,callback_threads\n";
    for (int horizon : {65, 200, 1026, 1027, 1100}) for (int k : {2, 3, 4}) for (int variant : {0, 1}) {
        const Fixture fixture = p8LargeFixture(horizon, k, variant);
        Answers expected;
        NativeResult current;
        std::string arm = "oracle_or_plan";
        std::size_t current_B = 16, current_threads = 1;
        try {
            expected = independentOracle(fixture);
            require(!expected.empty(), "large fixture lost its planted exact matches");
            const auto decomposition = decompositionFor(fixture);
            const TDTree tree(fixture.graph, fixture.query, decomposition, fixture.k);
            const auto plan = makeQueryPlan(fixture.query, decomposition, tree, OrderMode::Paper);
            require(plan.root == 0 && plan.order == std::vector<int>({0, 1, 2}),
                    "large fixture fixed root/order changed");
            const auto dags = compileDags(fixture, plan, tree, decomposition);
            NativeResult direct;
            const Configuration baseline{"legacy_direct_reference", -1, false, false, false};
            nativeRun(fixture, plan, tree, nullptr, baseline, 1, true, direct);
            checkResult(expected, direct, baseline);
            for (std::size_t B : {std::size_t{16}, std::size_t{32}, std::size_t{64}, std::size_t{128}}) {
                current_B = B;
                for (const auto& config : configs) {
                    const auto* dag = config.base.physical_mask < 0 ? nullptr : &dags.at(config.base.physical_mask);
                    const auto identity = dag ? dagIdentity(*dag) : "";
                    NativeResult serial;
                    for (std::size_t threads : {std::size_t{1}, std::size_t{16}}) {
                        current_threads = threads;
                        arm = config.base.name + "_B" + std::to_string(B) + "_t" + std::to_string(threads);
                        p8Native(fixture, plan, tree, dag, config, B, threads, true, current);
                        p8Check(fixture, expected, current, config, B, direct);
                        require(!dag || dagIdentity(*dag) == identity, "P8 changed the compiled input DAG");
                        const auto total = p8Object(current.summary.jindex_execution_json, "\"totals\":");
                        const auto indexed = p8Number(total, "index_selected");
                        const auto scanned = p8Number(total, "scan_selected");
                        const auto rejected = p8Number(total, "cost_rejected_states");
                        const auto built = p8Number(current.summary.jindex_execution_json, "built_indexes");
                        if (threads == 1) {
                            serial = current;
                            auto& combined = coverage[config.base.name];
                            combined.indexed += indexed; combined.scanned += scanned;
                            combined.cost_rejected += rejected; combined.built_indexes += built;
                            combined.universal_fallback += p8Number(total, "fallback_universal");
                            combined.small_degree_fallback += p8Number(total, "fallback_small_degree");
                        } else {
                            require(current.summary.threads_used > 1, "large P8 parallel test used only one worker");
                            require(current.answers == serial.answers
                                    && current.summary.result_digest == serial.summary.result_digest
                                    && current.ops.at("logical_extensions") == serial.ops.at("logical_extensions")
                                    && current.ops.at("candidates") == serial.ops.at("candidates"),
                                    "P8 1T/16T full-set/digest/extension/candidate equality failed");
                            const auto serial_total = p8Object(serial.summary.jindex_execution_json, "\"totals\":");
                            require(total == serial_total,
                                    "P8 deterministic selection counters differ between 1T and 16T");
                            max_callbacks = std::max<std::uint64_t>(max_callbacks, current.callback_threads);
                            ++pairs;
                        }
                        log << variant << ',' << horizon << ',' << k << ',' << B << ',' << config.base.name
                            << ',' << current.answers.size() << ',' << current.ops.at("logical_extensions")
                            << ',' << current.ops.at("candidates") << ',' << indexed << ',' << scanned
                            << ',' << rejected << ',' << built << ',' << current.summary.threads_used
                            << ',' << current.callback_threads << '\n';
                        if (horizon == 200 && k == 3 && variant == 0 && B == 16)
                            writeNew(out / (config.base.name + "_t" + std::to_string(threads) + "_production.json"),
                                     current.summary.jindex_execution_json + '\n');
                    }
                    if (horizon == 65 && k == 3 && variant == 0 && B == 16) {
                        arm = config.base.name + "_ops_off";
                        current_threads = 1;
                        p8Native(fixture, plan, tree, dag, config, B, 1, false, current);
                        p8Check(fixture, expected, current, config, B, direct);
                        ++ops_off;
                    }
                }
            }
            ++fixtures;
            log.flush();
            require(static_cast<bool>(log), "large P8 test log write failed");
            std::cout << "P8 LARGE PASS T=" << horizon << " k=" << k << " variant=" << variant << std::endl;
        } catch (const std::exception& error) {
            p8Failure(out, fixture, arm, error.what(), expected, current, current_B, current_threads);
            throw;
        }
    }
    std::ostringstream covered;
    bool comma = false;
    for (const auto& config : configs) {
        const auto& c = coverage.at(config.base.name);
        require(c.indexed > 0 && c.scanned > 0 && c.built_indexes > 0,
                "P8 configuration did not exercise both real indexed and scan paths: " + config.base.name);
        if (config.mode == JIndexMode::Adaptive)
            require(c.cost_rejected > 0, "P8 adaptive cost-based scan branch was not exercised: " + config.base.name);
        require(c.universal_fallback > 0 || c.small_degree_fallback > 0,
                "P8 fallback path was not exercised");
        if (comma) covered << ','; comma = true;
        covered << "{\"configuration\":" << quote(config.base.name)
                << ",\"index_selected\":" << c.indexed << ",\"scan_selected\":" << c.scanned
                << ",\"cost_rejected_states\":" << c.cost_rejected
                << ",\"built_indexes\":" << c.built_indexes << '}';
    }
    require(fixtures == 30 && pairs == 6480 && ops_off == 54 && max_callbacks > 1,
            "P8 large/parallel fixed coverage incomplete");
    writeNew(out / "p8_large_parallel_receipt.json",
             "{\"status\":\"PASS\",\"fixtures\":30,\"vertices\":[96,128],"
             "\"horizons\":[65,200,1026,1027,1100],\"k\":[2,3,4],\"B\":[16,32,64,128],"
             "\"configurations\":54,\"serial_parallel_pairs\":6480,\"ops_off_checks\":54,"
             "\"negative_origins\":true,\"reciprocal_root_nonroot_loops\":true,"
             "\"dense_and_interval_window_domains\":true,\"superset_debug_enabled\":true,"
             "\"max_callback_threads\":" + std::to_string(max_callbacks) + ",\"coverage\":[" + covered.str() + "]}\n");
}

void p8RandomDifferential(const fs::path& out) {
    const auto configs = p8Configurations();
    std::mt19937_64 random(kSeed);
    std::uint64_t instances = 0, comparisons = 0;
    std::ofstream log(out / "p8_differential_cases.csv");
    std::ofstream work(out / "p8_differential_ops.csv");
    log << "k,shape,stratum,instance,vertices,edges,horizon,origin,answers,p8_configurations,exact_sets_equal\n";
    work << "k,shape,stratum,instance,configuration,answers,logical_extensions,candidates\n";
    for (int k : {2, 3, 4}) for (const auto& shape : kShapes) for (int stratum = 0; stratum < 4; ++stratum) {
        std::uint64_t positives = 0, zeros = 0;
        for (int instance = 0; instance < kPerStratum; ++instance) {
            const Fixture fixture = randomFixture(random, k, shape, stratum, instance);
            Answers expected;
            NativeResult current;
            std::string arm = "oracle_or_plan";
            try {
                expected = independentOracle(fixture);
                require(!fixture.planted || !expected.empty(), "random planted P8 witness missing");
                if (instance % 3 == 2) require(expected.empty(), "fixed P8 zero fixture has an oracle answer");
                const auto decomposition = decompositionFor(fixture);
                const TDTree tree(fixture.graph, fixture.query, decomposition, fixture.k);
                const auto plan = makeQueryPlan(fixture.query, decomposition, tree, OrderMode::Paper);
                const auto dags = compileDags(fixture, plan, tree, decomposition);
                NativeResult direct;
                const Configuration baseline{"legacy_direct_reference", -1, false, false, false};
                nativeRun(fixture, plan, tree, nullptr, baseline, 1, true, direct);
                checkResult(expected, direct, baseline);
                for (const auto& config : configs) {
                    arm = config.base.name;
                    const auto* dag = config.base.physical_mask < 0 ? nullptr : &dags.at(config.base.physical_mask);
                    const auto before = dag ? dagIdentity(*dag) : "";
                    p8Native(fixture, plan, tree, dag, config, 16, 1, true, current);
                    p8Check(fixture, expected, current, config, 16, direct);
                    require(!dag || before == dagIdentity(*dag), "P8 changed its input Candidate-DAG");
                    work << k << ',' << shape << ',' << stratum << ',' << instance << ',' << config.base.name
                         << ',' << current.answers.size() << ',' << current.ops.at("logical_extensions")
                         << ',' << current.ops.at("candidates") << '\n';
                    ++comparisons;
                }
                ++instances; positives += !expected.empty(); zeros += expected.empty();
                log << k << ',' << shape << ',' << stratum << ',' << instance << ','
                    << fixture.graph.num_vertices << ',' << fixture.graph.temporal_edges.size() << ','
                    << fixture.horizon << ',' << fixture.origin << ',' << expected.size() << ",54,true\n";
                log.flush(); work.flush();
                require(static_cast<bool>(log) && static_cast<bool>(work), "P8 random evidence write failed");
            } catch (const std::exception& error) {
                p8Failure(out, fixture, arm, error.what(), expected, current, 16, 1);
                throw;
            }
        }
        require(positives > 0 && zeros > 0, "P8 random stratum lacks positive/negative coverage");
        std::cout << "P8 DIFFERENTIAL PASS k=" << k << " shape=" << shape << " stratum=" << stratum << std::endl;
    }
    require(instances == 16800 && comparisons == 907200, "P8 mandatory random coverage incomplete");
    writeNew(out / "p8_differential_receipt.json",
             "{\"status\":\"PASS\",\"seed\":20261020,\"random_graphs\":16800,\"P8_configurations\":54,"
             "\"P8_exact_set_comparisons\":907200,\"legacy_direct_reference_runs\":16800,"
             "\"B\":16,\"alpha_values\":[0.5,1,2],\"W_modes\":[\"eager\",\"once_lazy\"],"
             "\"independent_byte_time_oracle\":true,\"unmodified_native_active_intervals\":true,"
             "\"logical_extensions_equal_legacy_direct\":true,\"superset_debug_enabled\":true}\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "Usage: test_v5_p8 NEW_OUTPUT_DIRECTORY\n"; return 2; }
    const fs::path out(argv[1]);
    if (fs::exists(out)) { std::cerr << "Refusing existing P8 output directory\n"; return 2; }
    fs::create_directories(out);
    try {
        // Real indexed branches first: never spend the random campaign merely
        // proving small-degree fallback while an indexed mismatch exists.
        p8LargeAndParallel(out);
        p8RandomDifferential(out);
        writeNew(out / "v5_p8_receipt.json",
                 "{\"status\":\"PASS\",\"seed\":20261020,\"P8_configurations\":54,"
                 "\"random_graphs\":16800,\"P8_random_exact_set_comparisons\":907200,"
                 "\"large_fixtures\":30,\"parallel_pairs\":6480,\"ops_off_checks\":54,"
                 "\"B16_full_random\":true,\"other_B_large_fixture_only\":true,"
                 "\"independent_mapping_and_active_interval_oracle\":true,"
                 "\"logical_extensions_equal_legacy_direct\":true,\"superset_debug_enabled\":true,"
                 "\"original_17_configuration_receipts_preserved\":true,\"W_property_campaign_repeated\":false,"
                 "\"P9_tested\":false,\"performance_runs\":0,\"failures\":0}\n");
        std::cout << "V5 P8 CORRECTNESS PASS" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        const auto receipt = "{\"status\":\"STOPPED\",\"reason\":" + quote(error.what())
            + ",\"no_further_test_instances\":true,\"no_automatic_retry\":true,\"performance_runs\":0}\n";
        if (!fs::exists(out / "counterexample.json")) writeNew(out / "counterexample.json", receipt);
        writeNew(out / "STOP.json", receipt);
        writeNew(out / "v5_p8_receipt.json", receipt);
        std::cerr << "STOP V5 P8 CORRECTNESS: " << error.what() << '\n';
        return 1;
    }
}
