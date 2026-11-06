// V7 Direct-only correctness campaign, derived from the preserved v6 driver.
// The included v5 fixture generator/oracle and the copied R96 fixture body are
// unchanged: seed, RNG call order, loop order, horizon strata and graph content
// exactly match v6. Only dfs/win_s/final execute; this is not an alpha/c rerun.
// F1-specific units are separate from the unchanged frozen fixture campaign.
// Never invoke the preserved v5/v6 main drivers.
#define main preserved_v5_driver_never_invoked_by_v7
#include "test_v5_matcher.cpp"
#undef main

#include "../BIndexExecution.h"
#include "../WindowStore.h"
#include "../WStoreFile.h"
#include <atomic>
#include <cctype>
#include <exception>

namespace {
struct V7Config {
    std::string name;
    bool probe = false, window = false, loaded = false, adjacency = false;
    BIndexMode bmode = BIndexMode::None;
    double alpha = .5;
    std::size_t c = 1;
};
std::vector<V7Config> v7Configs() {
    // Exactly the requested v7 arms. Alpha is ignored by Always mode; this
    // retains the v6 bidx_always constructor value, not a new alpha experiment.
    std::vector<V7Config> result{
        {"dfs"}, {"win_s", true, true, true},
        {"final", true, true, true, false, BIndexMode::Always, .5, 1}};
    require(result.size() == 3, "v7 fixed configuration count differs");
    return result;
}
std::string v7Object(const std::string& text, const std::string& key) {
    const std::string marker = '"' + key + "\":";
    auto begin = text.find(marker);
    require(begin != std::string::npos, "missing object " + key);
    begin += marker.size();
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    require(begin < text.size() && text[begin] == '{', "non-object " + key);
    bool quoted = false, escaped = false;
    int depth = 0;
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
    throw std::runtime_error("unterminated object " + key);
}
std::uint64_t v7Number(const std::string& text, const std::string& key) {
    const std::string marker = '"' + key + "\":";
    auto begin = text.find(marker);
    require(begin != std::string::npos, "missing unsigned field " + key);
    begin += marker.size();
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    const auto end = text.find_first_not_of("0123456789", begin);
    require(begin < text.size() && begin != end, "non-integer field " + key);
    return std::stoull(text.substr(begin, end - begin));
}

struct FixtureStore {
    fs::path input, store;
    std::string sha;
    std::uint64_t verified_rows = 0;
};
FixtureStore makeFixtureStore(const fs::path& scratch, const Fixture& f) {
    FixtureStore result{scratch / "fixture.graph", scratch / "fixture.wstore", "", 0};
    require(!fs::exists(result.input) && !fs::exists(result.store), "fixture scratch is not empty");
    // Canonical explicit occurrence serialization of this manually constructed
    // graph (including singletons used by the oracle). No parser/F2 mutation.
    std::ostringstream text;
    for (const auto& edge : f.graph.temporal_edges)
        for (const auto& interval : edge.activeIntervals())
            for (std::int64_t t = interval.start; t <= interval.end; ++t)
                text << f.graph.externalId(edge.u) << ' ' << f.graph.externalId(edge.v) << ' ' << t << '\n';
    writeNew(result.input, text.str());
    const auto built = wstore_file::build(f.graph, result.input.string(), "v6_correctness_fixture",
                                         {2, 3, 4, 5}, result.store.string());
    result.sha = wstore_file::graphFileSha256(result.input.string());
    require(result.sha == built.header.graph_file_sha256, "fixture input SHA differs from build receipt");
    for (int k : {2, 3, 4, 5}) {
        const auto section = wstore_file::load(result.store.string(), result.sha,
            f.graph.temporal_edges.size(), f.graph.num_vertices, k);
        require(section.section.k == k && section.section.words <= 16, "wrong/oversized W-store section");
        require(section.metrics.read_bytes == section.header.header_bytes + section.section.bytes,
                "W-store load read other k payloads");
        result.verified_rows += wstore_file::verify(f.graph, section);
    }
    require(result.verified_rows == 4 * f.graph.temporal_edges.size(), "not every edge/k W row was verified");
    return result;
}
void removeSuccessfulFixture(const FixtureStore& paths) {
    // Exactly two files created under the new exclusive correctness directory.
    // A mismatch throws BEFORE this function, preserving the counterexample.
    require(fs::remove(paths.store), "cannot remove successful generated store");
    require(fs::remove(paths.input), "cannot remove successful generated input");
}

void v7Native(const Fixture& f, const QueryPlan& plan, const TDTree& tree,
              const FixtureStore& files, const V7Config& config, std::size_t B,
              std::size_t threads, bool counted, NativeResult& result) {
    result = NativeResult{};
    result.counted = counted;
    Observation observation;
    observation.fixture = &f;
    OperatorCounters counters;
    GopherMatcherOptions options;
    options.threads = threads;
    options.parallel_mode = ParallelMode::Roots;
    options.early_prune = true;
    options.result_digest = true;
    options.direct_p4 = config.probe;
    options.window_algebra = config.window;
    options.window_lazy = false;
    if (config.loaded) { options.wstore_path = files.store.string(); options.graph_file_sha256 = files.sha; }
    if (config.adjacency) {
        options.jindex_mode = JIndexMode::Always;
        options.jindex_block_size = B; // B8/32 cover the frozen diagnostic sensitivity; primary is B16.
        options.jindex_alpha = 1.;
        options.jindex_verify_superset = true;
    }
    options.bindex_mode = config.bmode;
    options.bindex_alpha = config.alpha;
    options.bindex_block_size = B;
    options.bindex_min_size = 64;
    options.bindex_reuse_threshold = config.c;
    options.bindex_verify_superset = config.bmode != BIndexMode::None;
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
#define SAVE_V7_OP(name) result.ops[#name] = counters.ops_##name.load(std::memory_order_relaxed);
        DSM_OPS_FIELDS(SAVE_V7_OP)
#undef SAVE_V7_OP
    };
    try {
        // Only this Direct constructor. No CandidateDagCompiler, optimize pass,
        // CandidateDag object, DAG executor or DAG matcher is invoked in v7.
        GopherMatcher matcher(f.graph, f.query, plan, tree, f.k, options);
        result.summary = matcher.run(MatchOutputMode::CountOnly);
    } catch (...) { capture(); throw; }
    capture();
}
void checkV7(const Fixture& f, const Answers& expected, const NativeResult& actual,
             const V7Config& config, std::size_t B, const NativeResult* direct) {
    require(!actual.duplicate && !actual.invalid, "duplicate mapping or invalid/unmodified native intervals");
    require(!actual.summary.count_overflow, "native count overflow");
    require(actual.summary.match_count == actual.answers.size(), "count differs from exact observer");
    require(actual.answers == expected, "exact mapping AND native active intervals differ from independent oracle");
    if (direct) {
        require(actual.summary.result_digest == direct->summary.result_digest, "digest differs from legacy Direct");
        if (actual.counted)
            require(actual.ops.at("logical_extensions") == direct->ops.at("logical_extensions"),
                    "accepted fixed-plan extensions differ from legacy Direct");
    }
    if (config.loaded) {
        require(actual.summary.v5_window_built_edges == 0, "loaded W-store rebuilt edge windows");
        require(actual.summary.v5_window_relevant_edges == f.graph.temporal_edges.size(),
                "loaded W-store does not cover all graph temporal IDs");
        require(!actual.summary.wstore_loading_json.empty(), "loaded W-store has no load receipt");
    } else if (config.window) {
        require(actual.summary.v5_window_built_edges == actual.summary.v5_window_relevant_edges,
                "query eager W did not build all relevant windows");
    } else {
        require(actual.summary.v5_window_relevant_edges == 0 && actual.summary.v5_window_built_edges == 0,
                "window-off arm created a W-store");
    }
    if (config.adjacency) {
        const auto& j = actual.summary.jindex_execution_json;
        require(j.find("\"window_source\":\"window_store\"") != std::string::npos,
                "adjacency control did not use loaded W bits");
        require(v7Number(j, "B") == B, "adjacency control B differs from requested correctness setting");
    }
    if (config.bmode == BIndexMode::None) {
        require(actual.summary.bindex_execution_json.empty(), "B-index OFF created an index");
        return;
    }
    const auto& j = actual.summary.bindex_execution_json;
    require(j.find("\"schema\":\"v6_bindex_execution_v1\"") != std::string::npos &&
            j.find("\"status\":\"OK\"") != std::string::npos &&
            j.find("\"verify_superset\":true") != std::string::npos,
            "B-index schema/debug/counter status missing");
    require(v7Number(j, "B") == B && v7Number(j, "c") == config.c && v7Number(j, "n_min") == 64,
            "B-index frozen parameters changed");
    require(v7Number(j, "persistent_index_bytes") <= v7Number(j, "cap_bytes") &&
            v7Number(j, "peak_index_plus_build_scratch_bytes") <= v7Number(j, "cap_bytes"),
            "B-index exceeded memory cap instead of scan fallback");
    require(v7Number(j, "built_indexes") <= v7Number(j, "registered_blocks") &&
            v7Number(j, "built_indexes") <= v7Number(j, "build_attempts"),
            "B-index published more than once per registered TD block");
    if (actual.counted) {
        const auto totals = v7Object(j, "totals");
        require(v7Number(totals, "states") == v7Number(totals, "index_selected") + v7Number(totals, "scan_selected"),
                "B-index state decisions do not reconcile");
        require(v7Number(totals, "bucket_entries_consumed") >= v7Number(totals, "unique_extracted_candidates"),
                "CSR entries are smaller than extracted unique candidates");
        require(v7Number(totals, "missing_parent_edges") == 0 && v7Number(totals, "empty_parent_windows") == 0,
                "TD parent-edge/W invariant violated");
        require(v7Number(totals, "time_passes") <= v7Number(totals, "time_checks"),
                "exact W time-pass funnel exceeds checks");
    } else {
        require(j.find("\"totals\":null") != std::string::npos &&
                j.find("\"counters_enabled\":false") != std::string::npos,
                "ops OFF B-index collected totals");
    }
}
void preserveV7Failure(const fs::path& out, const Fixture& f, const std::string& arm,
                       const std::string& reason, const Answers& expected, const NativeResult& actual) {
    counterexample(out, f, arm, reason, expected, actual);
    if (!fs::exists(out / "v7_native_failure.json")) {
        std::ostringstream context;
        context << "{\"arm\":" << quote(arm) << ",\"bindex\":";
        if (actual.summary.bindex_execution_json.empty()) context << "null"; else context << actual.summary.bindex_execution_json;
        context << ",\"aindex\":";
        if (actual.summary.jindex_execution_json.empty()) context << "null"; else context << actual.summary.jindex_execution_json;
        context << ",\"generated_store_preserved\":true,\"no_retry\":true}\n";
        writeNew(out / "v7_native_failure.json", context.str());
    }
}
void logV7(std::ostream& log, const Fixture& f, const V7Config& config, std::size_t B,
           std::size_t threads, const NativeResult& result) {
    log << f.k << ',' << f.shape << ',' << f.stratum << ',' << f.instance << ',' << f.horizon << ','
        << config.name << ',' << B << ',' << threads << ',' << result.counted << ',' << result.answers.size()
        << ',' << result.ops.at("logical_extensions") << ',' << result.ops.at("candidates")
        << ',' << result.summary.threads_used << ',' << result.callback_threads << '\n';
}
void headerV7(std::ostream& out) {
    out << "k,shape,stratum,instance,horizon,configuration,B,requested_threads,ops_enabled,answers,"
           "logical_extensions,candidates,threads_used,callback_threads\n";
}

void v7Random(const fs::path& out, const fs::path& scratch) {
    std::mt19937_64 rng(kSeed);
    const auto configs = v7Configs();
    std::uint64_t instances = 0, checks = 0, rows = 0;
    std::ofstream log(out / "v7_differential_checks.csv");
    headerV7(log);
    std::ostringstream strata;
    bool comma = false;
    for (int k : {2, 3, 4, 5}) for (const auto& shape : kShapes) for (int stratum = 0; stratum < 4; ++stratum) {
        std::uint64_t positive = 0, zero = 0;
        for (int instance = 0; instance < kPerStratum; ++instance) {
            const auto f = randomFixture(rng, k, shape, stratum, instance);
            Answers expected;
            NativeResult actual, direct;
            std::string arm = "oracle_and_store";
            try {
                expected = independentOracle(f);
                require(!f.planted || !expected.empty(), "planted oracle witness missing");
                if (instance % 3 == 2) require(expected.empty(), "fixed negative fixture is not negative");
                const auto files = makeFixtureStore(scratch, f);
                const auto decomposition = decompositionFor(f);
                const TDTree tree(f.graph, f.query, decomposition, f.k);
                const auto plan = makeQueryPlan(f.query, decomposition, tree, OrderMode::Paper);
                for (const auto& config : configs) {
                    arm = config.name;
                    v7Native(f, plan, tree, files, config, 16, 1, true, actual);
                    checkV7(f, expected, actual, config, 16, config.name == "dfs" ? nullptr : &direct);
                    logV7(log, f, config, 16, 1, actual);
                    if (config.name == "dfs") direct = std::move(actual);
                    ++checks;
                }
                rows += files.verified_rows;
                removeSuccessfulFixture(files);
                ++instances;
                positive += !expected.empty(); zero += expected.empty();
                log.flush(); require(static_cast<bool>(log), "random log write failed");
            } catch (const std::exception& error) {
                preserveV7Failure(out, f, arm, error.what(), expected, actual);
                throw;
            }
        }
        require(positive > 0 && zero > 0, "random stratum lacks positive or zero coverage");
        if (comma) strata << ','; comma = true;
        strata << "{\"k\":" << k << ",\"shape\":" << quote(shape) << ",\"stratum\":" << stratum
               << ",\"instances\":200,\"positive\":" << positive << ",\"zero\":" << zero << '}';
        std::cout << "V7 RANDOM PASS k=" << k << " shape=" << shape << " stratum=" << stratum << std::endl;
    }
    require(instances == 22400 && checks == 67200, "v7 random campaign incomplete");
    std::ostringstream receipt;
    receipt << "{\"status\":\"PASS\",\"seed\":" << kSeed << ",\"random_graphs\":22400,\"configurations\":3,"
               "\"native_exact_set_comparisons\":67200,\"verified_edge_k_rows\":" << rows
            << ",\"strata\":[" << strata.str() << "],\"DAG_compilation_or_execution\":false,"
               "\"oracle\":\"independent byte-time mapping and unmodified interval set\"}\n";
    writeNew(out / "v7_differential_receipt.json", receipt.str());
}

Fixture v7LargeFixture(int horizon, int k, int variant) {
    Fixture f;
    f.shape = variant ? "v6_R96_reciprocal_rootloop" : "v6_R96_reciprocal";
    f.k = k; f.horizon = horizon; f.origin = variant ? -1025 : -65;
    f.instance = variant; f.stratum = horizon; f.force_root_zero = true;
    f.arcs = {{0, 1}, {1, 2}, {2, 1}, {2, 2}};
    if (variant) f.arcs.emplace_back(0, 0);
    initializeQuery(f, {0, 1, 2});
    initializeHistories(f, 116); // 16 roots + 4 anchors + 96 real TD candidates.
    f.graph.vertex_labels.assign(116, Label{2});
    for (int v = 0; v < 16; ++v) f.graph.vertex_labels[v] = Label{0};
    for (int v = 16; v < 20; ++v) f.graph.vertex_labels[v] = Label{1};
    auto fill = [&](int from, int to, int begin, int end) {
        for (int t = begin; t <= end; ++t) f.histories[from][to][t] = 1;
    };
    for (int root = 0; root < 16; ++root) {
        for (int anchor = 16; anchor < 20; ++anchor)
            fill(root, anchor, 0, root % 2 == 0 ? k - 1 : horizon - 1);
        if (variant) fill(root, root, 0, horizon - 1);
    }
    for (int anchor = 16; anchor < 20; ++anchor) for (int candidate = 20; candidate < 116; ++candidate) {
        const int position = candidate - 20;
        const int begin = position < 12 ? 0 : horizon / 2;
        const int end = position == 0 || position >= 12 ? horizon - 1 : k - 1;
        fill(anchor, candidate, begin, end);
        // Distinct reciprocal slots can disagree in time while both remain
        // individually durable and all 96 positions remain in the TD block.
        if (position >= 90) fill(candidate, anchor, 0, k - 1);
        else fill(candidate, anchor, begin, end);
        fill(candidate, candidate, 0, horizon - 1);
    }
    finishGraph(f);
    return f;
}
void v7FixedAndParallel(const fs::path& out, const fs::path& scratch) {
    const auto configs = v7Configs();
    std::ofstream log(out / "v7_large_parallel_checks.csv");
    headerV7(log);
    struct Coverage { std::uint64_t indexed = 0, scanned = 0, cost_scan = 0, prebuild = 0, built = 0; };
    std::map<std::string, Coverage> coverage;
    std::uint64_t pairs = 0, ops_off = 0, fixtures = 0, max_callbacks = 0;
    for (int horizon : {65, 130, 200}) for (int k : {2, 3, 4, 5}) for (int variant : {0, 1}) {
        const auto f = v7LargeFixture(horizon, k, variant);
        Answers expected;
        NativeResult actual, direct;
        std::string arm = "large_oracle_and_store";
        try {
            expected = independentOracle(f);
            require(!expected.empty(), "large indexed fixture has no answers");
            const auto files = makeFixtureStore(scratch, f);
            const auto decomposition = decompositionFor(f);
            const TDTree tree(f.graph, f.query, decomposition, f.k);
            const auto plan = makeQueryPlan(f.query, decomposition, tree, OrderMode::Paper);
            require(plan.root == 0 && plan.parent[2] == 1 && tree.rootCandidates().size() == 16,
                    "large fixture does not preserve fixed 16-root plan");
            for (int anchor = 16; anchor < 20; ++anchor) {
                const auto* R = tree.candidatesForParent(2, anchor);
                require(R && R->size() == 96, "large fixture did not create R=96 (degree alone is insufficient)");
            }
            v7Native(f, plan, tree, files, configs.front(), 16, 1, true, direct);
            checkV7(f, expected, direct, configs.front(), 16, nullptr);
            for (std::size_t B : {8U, 16U, 32U}) for (const auto& config : configs) {
                arm = config.name + "_B" + std::to_string(B);
                v7Native(f, plan, tree, files, config, B, 1, true, actual);
                checkV7(f, expected, actual, config, B, &direct);
                logV7(log, f, config, B, 1, actual);
                NativeResult serial = std::move(actual);
                v7Native(f, plan, tree, files, config, B, 16, true, actual);
                checkV7(f, expected, actual, config, B, &direct);
                require(actual.answers == serial.answers && actual.summary.result_digest == serial.summary.result_digest &&
                        actual.ops.at("logical_extensions") == serial.ops.at("logical_extensions"),
                        "1T/16T exact answers, digest or logical extensions differ");
                require(actual.summary.threads_used == 16, "large fixture did not request/use 16 worker slots");
                max_callbacks = std::max<std::uint64_t>(max_callbacks, actual.callback_threads);
                logV7(log, f, config, B, 16, actual);
                ++pairs;
                if (config.bmode != BIndexMode::None) {
                    for (const auto* result : {&serial, &actual}) {
                        const auto& j = result->summary.bindex_execution_json;
                        const auto totals = v7Object(j, "totals");
                        auto& totals_out = coverage[config.name + "_B" + std::to_string(B)];
                        totals_out.indexed += v7Number(totals, "index_selected");
                        totals_out.scanned += v7Number(totals, "scan_selected");
                        totals_out.cost_scan += v7Number(totals, "cost_rejected_scans");
                        totals_out.prebuild += v7Number(totals, "prebuild_scans");
                        totals_out.built += v7Number(j, "built_indexes");
                        require(v7Number(j, "built_indexes") == 4 && v7Number(j, "build_attempts") == 4,
                                "R=96 fixture did not build exactly once for each of four eligible blocks");
                        require(v7Number(totals, "index_selected") > 0, "large fixture never selected B-index");
                        require(v7Number(totals, "small_block_scans") > 0, "small R scan path was not exercised");
                        require(v7Number(totals, "prebuild_scans") == 0 &&
                                v7Number(totals, "cost_rejected_scans") == 0,
                                "final introduced delayed-build or alpha-cost decisions");
                    }
                    // Save a bounded production-schema sample, not every answer.
                    if (horizon == 65 && k == 3 && variant == 0)
                        writeNew(out / (config.name + "_B" + std::to_string(B) + "_production.json"),
                                 actual.summary.bindex_execution_json + "\n");
                }
                if (horizon == 65 && k == 3 && variant == 0) {
                    arm += "_ops_off";
                    v7Native(f, plan, tree, files, config, B, 16, false, actual);
                    checkV7(f, expected, actual, config, B, &direct);
                    logV7(log, f, config, B, 16, actual);
                    ++ops_off;
                }
            }
            removeSuccessfulFixture(files);
            ++fixtures;
            log.flush(); require(static_cast<bool>(log), "large parallel log failed");
        } catch (const std::exception& error) {
            preserveV7Failure(out, f, arm, error.what(), expected, actual);
            throw;
        }
    }
    for (const auto& [name, c] : coverage) {
        require(c.indexed > 0 && c.scanned > 0 && c.built > 0, "missing indexed/scan/build coverage " + name);
        require(c.cost_scan == 0 && c.prebuild == 0,
                "final coverage contains an adaptive or delayed-build decision " + name);
    }
    require(fixtures == 24 && pairs == 216 && ops_off == 9 && max_callbacks > 1,
            "large/parallel/ops-off coverage incomplete");
    std::ostringstream receipt;
    receipt << "{\"status\":\"PASS\",\"fixtures\":24,\"R_per_eligible_block\":96,\"parallel_pairs\":216,"
               "\"ops_off_checks\":9,\"B\":[8,16,32],\"maximum_callback_threads\":" << max_callbacks
            << ",\"exact_mapping_intervals_digest_extensions_equal\":true,\"four_once_builds_per_run\":true}\n";
    writeNew(out / "v7_parallel_receipt.json", receipt.str());
}

void v7LoopUnits(const fs::path& out, const fs::path& scratch) {
    const auto configs = v7Configs();
    const std::array<std::string, 6> names{"disjoint_cumulative_support", "missing_root_loop", "short_root_loop",
        "missing_nonroot_loop", "short_nonroot_loop", "positive_both_loops"};
    std::uint64_t checks = 0;
    std::ofstream log(out / "v7_loop_checks.csv"); headerV7(log);
    for (const auto& name : names) for (int horizon : {8, 65, 130, 200}) {
        const auto f = loopFixture(name, horizon);
        Answers expected;
        NativeResult actual, direct;
        std::string arm = "loop_oracle";
        try {
            expected = independentOracle(f);
            require(expected.empty() == (name != "positive_both_loops"), "fixed loop oracle truth differs");
            const auto files = makeFixtureStore(scratch, f);
            const auto decomposition = decompositionFor(f);
            const TDTree tree(f.graph, f.query, decomposition, f.k);
            const auto plan = makeQueryPlan(f.query, decomposition, tree, OrderMode::Paper);
            for (const auto& config : configs) {
                arm = config.name;
                v7Native(f, plan, tree, files, config, 16, 1, true, actual);
                checkV7(f, expected, actual, config, 16, config.name == "dfs" ? nullptr : &direct);
                logV7(log, f, config, 16, 1, actual);
                if (config.name == "dfs") direct = std::move(actual);
                ++checks;
            }
            removeSuccessfulFixture(files);
        } catch (const std::exception& error) {
            preserveV7Failure(out, f, arm, error.what(), expected, actual);
            throw;
        }
    }
    require(checks == 72, "fixed loop/cumulative-support coverage incomplete");
    writeNew(out / "v7_loop_receipt.json",
        "{\"status\":\"PASS\",\"fixtures\":24,\"negative_fixtures\":20,\"exact_set_comparisons\":72}\n");
}

void v7ComponentUnits(const fs::path& out, const fs::path& scratch) {
    const auto f = v7LargeFixture(200, 3, 0);
    const auto files = makeFixtureStore(scratch, f);
    const auto decomposition = decompositionFor(f);
    const TDTree tree(f.graph, f.query, decomposition, f.k);
    const auto plan = makeQueryPlan(f.query, decomposition, tree, OrderMode::Paper);
    const WindowStore windows(f.graph, files.store.string(), files.sha, f.k);
    const auto* R = tree.candidatesForParent(2, 16);
    require(R && R->size() == 96, "component fixture missing R96");
    const auto universal = window_algebra::WindowView::universal(windows.domain());
    const auto* root_edge = f.graph.findTemporalEdge(0, 16);
    require(root_edge != nullptr, "component narrow support edge missing");
    window_algebra::WindowBuffer support;
    window_algebra::buildInto(windows.domain(), root_edge->activeIntervals().data(),
                              root_edge->activeIntervals().size(), support);
    std::uint64_t selections = 0;
    for (std::size_t B : {8U, 16U, 32U}) {
        BIndexExecution index(f.graph, f.query, plan, tree, windows, f.k, B, 64, 1, .5,
                              BIndexExecution::Mode::Always, true, true);
        BIndexExecution::WorkerScratch scratch_bits;
        const auto first = index.select(2, 16, *R, universal, scratch_bits);
        require(first.used && first.candidates == *R,
                "final c1 must index the first eligible visit");
        const auto second = index.select(2, 16, *R, universal, scratch_bits);
        require(second.used && second.candidates == *R && second.parent_outgoing && second.parent_query == 1,
                "reciprocal TD parent must use parent->child and universal all buckets");
        const auto third = index.select(2, 16, *R, support.view(), scratch_bits);
        require(third.used && third.candidates.size() == 12, "narrow support should extract the 12 early R positions");
        require(second.candidates == *R && second.parent_entries.size() == R->size(),
                "worker scratch reuse changed a previous frame-owned selection");
        require(scratch_bits.touched_words.empty() &&
                std::all_of(scratch_bits.bitmap.begin(), scratch_bits.bitmap.end(), [](std::uint64_t v) { return v == 0; }),
                "B-index did not clear only-touched bitmap state after selection");
        for (std::size_t i = 0; i < third.candidates.size(); ++i) {
            const auto& edge = f.graph.temporal_edges[third.parent_entries[i]->temporal_edge_id];
            require(edge.u == 16 && edge.v == third.candidates[i], "reciprocal parent handle bound wrong direction");
        }
        std::ostringstream evidence; index.writeJson(evidence);
        const auto total = v7Object(evidence.str(), "totals");
        require(v7Number(evidence.str(), "built_indexes") == 1 && v7Number(evidence.str(), "build_attempts") == 1 &&
                v7Number(total, "prebuild_scans") == 0, "final build-once/first-use invariant mismatch");
        require(v7Number(total, "universal_states") == 2 && v7Number(total, "universal_index_selected") == 2 &&
                v7Number(total, "universal_scan_selected") == 0, "universal decision accounting differs");
        writeNew(out / ("bindex_component_B" + std::to_string(B) + ".json"), evidence.str() + "\n");
        selections += 3;
    }
    bool rejected = false;
    std::string wrong = files.sha;
    wrong[0] = wrong[0] == '0' ? '1' : '0';
    try { const WindowStore invalid(f.graph, files.store.string(), wrong, f.k); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "W-store accepted a different actual graph SHA");
    removeSuccessfulFixture(files);

    // A small graph with a wide time axis forces the CSR offset array over the
    // REAL capacity cap. Nmin=1 is unit-only, not an experimental arm setting.
    Fixture cap_fixture;
    cap_fixture.shape = "cap_denial_unit"; cap_fixture.horizon = 1000; cap_fixture.k = 3;
    cap_fixture.force_root_zero = true; cap_fixture.arcs = {{0, 1}, {1, 2}};
    initializeQuery(cap_fixture, {0, 1, 2}); initializeHistories(cap_fixture, 3);
    cap_fixture.graph.vertex_labels = {0, 1, 2};
    std::fill(cap_fixture.histories[0][1].begin(), cap_fixture.histories[0][1].end(), 1);
    std::fill(cap_fixture.histories[1][2].begin(), cap_fixture.histories[1][2].end(), 1);
    finishGraph(cap_fixture);
    const auto cap_files = makeFixtureStore(scratch, cap_fixture);
    const auto cap_decomposition = decompositionFor(cap_fixture);
    const TDTree cap_tree(cap_fixture.graph, cap_fixture.query, cap_decomposition, cap_fixture.k);
    const auto cap_plan = makeQueryPlan(cap_fixture.query, cap_decomposition, cap_tree, OrderMode::Paper);
    const WindowStore cap_windows(cap_fixture.graph, cap_files.store.string(), cap_files.sha, cap_fixture.k);
    BIndexExecution cap_index(cap_fixture.graph, cap_fixture.query, cap_plan, cap_tree, cap_windows,
        cap_fixture.k, 8, 1, 1, .5, BIndexExecution::Mode::Always, true, true);
    BIndexExecution::WorkerScratch cap_scratch;
    for (int q : {1, 2}) {
        const auto* block = cap_tree.candidatesForParent(q, q - 1);
        require(block && block->size() == 1, "cap unit TD block missing");
        const auto selected = cap_index.select(q, q - 1, *block,
            window_algebra::WindowView::universal(cap_windows.domain()), cap_scratch);
        require(!selected.used, "over-cap index did not fall back to scan");
    }
    std::ostringstream cap_json; cap_index.writeJson(cap_json);
    require(v7Number(cap_json.str(), "build_attempts") == 1 && v7Number(cap_json.str(), "built_indexes") == 0 &&
            v7Number(v7Object(cap_json.str(), "totals"), "cap_denial_scans") == 2,
            "global sticky cap denial retried a later block or failed to scan");
    require(v7Number(cap_json.str(), "sparse_registered_used_blocks") == 1 && cap_scratch.storageBytes() == 0,
            "denial allocated scratch or registered a later unused block");
    writeNew(out / "bindex_cap_denial_unit.json", cap_json.str() + "\n");
    removeSuccessfulFixture(cap_files);
    std::ostringstream receipt;
    receipt << "{\"status\":\"PASS\",\"direct_index_selections\":" << selections
            << ",\"reused_scratch_preserves_earlier_selection\":true,\"reciprocal_parent_direction\":true,"
               "\"sha_mismatch_rejected_before_search\":true,\"sticky_global_cap_scan\":true,"
               "\"unit_cap_Nmin\":1,\"performance_Nmin_unchanged\":64}\n";
    writeNew(out / "v7_component_receipt.json", receipt.str());
}

// An independent test-only implementation of the exact old capacity formula.
// Production must obtain this receipt inside existing loader loops; the scan
// here is deliberately confined to correctness tests, never excluded timing.
std::uint64_t v7CapacityReference(const Graph& graph) {
    std::uint64_t bytes = 0;
    auto add = [&](std::size_t count, std::size_t size) {
        require(!size || count <= std::numeric_limits<std::uint64_t>::max() / size,
                "unit capacity multiplication overflow");
        const auto part = static_cast<std::uint64_t>(count) * size;
        require(part <= std::numeric_limits<std::uint64_t>::max() - bytes, "unit capacity sum overflow");
        bytes += part;
    };
    add(graph.adj.capacity(), sizeof(std::vector<Edge>));
    add(graph.in_adj.capacity(), sizeof(std::vector<Edge>));
    for (const auto& row : graph.adj) add(row.capacity(), sizeof(Edge));
    for (const auto& row : graph.in_adj) add(row.capacity(), sizeof(Edge));
    add(graph.temporal_edges.capacity(), sizeof(TemporalEdge));
    for (const auto& edge : graph.temporal_edges) add(edge.activeIntervals().capacity(), sizeof(TimeInterval));
    return bytes;
}
std::string v7IndexJson(const BIndexExecution& index) {
    std::ostringstream out;
    index.writeJson(out);
    return out.str();
}
void v7CheckPayload(const std::string& json) {
    const auto M = v7Number(json, "registered_blocks");
    const auto U = v7Number(json, "sparse_registered_used_blocks");
    const auto actual = v7Number(json, "actual_registry_payload_bytes");
    require(v7Number(json, "legacy_block_header_bytes") == 40 &&
            v7Number(json, "sparse_node_header_bytes") == 24 &&
            v7Number(json, "sparse_bucket_header_bytes") == 8,
            "frozen LP64 registry payload sizes differ");
    require(U <= M && actual == (U ? M * 8 + U * 24 : 0) && actual <= M * 40,
            "lazy registry payload proof failed");
    require(v7Number(json, "actual_persistent_index_payload_bytes") <= v7Number(json, "persistent_index_bytes") &&
            v7Number(json, "persistent_index_bytes") <= v7Number(json, "cap_bytes") &&
            v7Number(json, "peak_index_plus_build_scratch_bytes") <= v7Number(json, "cap_bytes"),
            "actual payload or unchanged virtual budget exceeds cap");
}
void v7CapacityUnits(const fs::path& out, const fs::path& scratch) {
    const auto raw = scratch / "f1_raw.graph";
    const auto empty = scratch / "f1_empty.graph";
    writeNew(raw, "# duplicates, shuffled histories, self-loop, and filtered singleton\n"
                  "10 20 8\n20 20 2\n10 20 2\n30 40 7\n20 20 1\n10 20 1\n"
                  "10 20 2\n10 20 9\n10 20 10\n20 10 -1\n20 10 -2\n");
    writeNew(empty, "# no temporal edges\n");
    std::uint64_t checks = 0;
    for (int minimum : {1, 2, 3, 20}) {
        Graph graph;
        TemporalGraphLoadTimings receipt;
        require(readTemporalGraph(raw.string(), graph, kDefaultLabelSeed, &receipt,
                                  LabelMode::StableId, nullptr, minimum), "raw capacity load failed");
        require(receipt.adjacency_temporal_capacity_bytes &&
                *receipt.adjacency_temporal_capacity_bytes == v7CapacityReference(graph),
                "raw finalized capacity receipt differs from exact old sum");
        // Prepared inputs use the same parser, but may have different reserve
        // histories. Compare EACH receipt with its own actual final capacities.
        const auto prepared = scratch / ("f1_prepared_" + std::to_string(minimum) + ".graph");
        std::ostringstream serialized;
        for (const auto& edge : graph.temporal_edges)
            for (const auto& interval : edge.activeIntervals())
                for (std::int64_t t = interval.start; t <= interval.end; ++t)
                    serialized << graph.externalId(edge.u) << ' ' << graph.externalId(edge.v) << ' ' << t << '\n';
        writeNew(prepared, serialized.str());
        Graph loaded;
        TemporalGraphLoadTimings prepared_receipt;
        require(readTemporalGraph(prepared.string(), loaded, kDefaultLabelSeed, &prepared_receipt,
                                  LabelMode::StableId, nullptr, minimum), "prepared capacity load failed");
        require(prepared_receipt.adjacency_temporal_capacity_bytes &&
                *prepared_receipt.adjacency_temporal_capacity_bytes == v7CapacityReference(loaded),
                "prepared finalized capacity receipt differs from exact old sum");
        require(temporalGraphContentFingerprint(graph) == temporalGraphContentFingerprint(loaded),
                "prepared fixture changed graph content or stable labels");
        require(fs::remove(prepared), "cannot remove successful capacity fixture");
        checks += 2;
    }
    Graph graph;
    TemporalGraphLoadTimings receipt;
    receipt.adjacency_temporal_capacity_bytes = 123; // must be reset, including valid zero.
    require(readTemporalGraph(empty.string(), graph, kDefaultLabelSeed, &receipt) &&
            receipt.adjacency_temporal_capacity_bytes && *receipt.adjacency_temporal_capacity_bytes == 0 &&
            v7CapacityReference(graph) == 0, "empty load returned a stale/absent capacity receipt");
    require(fs::remove(raw) && fs::remove(empty), "cannot remove successful capacity input");
    std::ostringstream json;
    json << "{\"status\":\"PASS\",\"exact_capacity_comparisons\":" << checks + 1
         << ",\"raw_prepared_filters\":[1,2,3,20],\"empty_zero_receipt\":true,"
            "\"no_Graph_embedded_cache\":true,\"test_reference_scan_only\":true}\n";
    writeNew(out / "v7_f1_capacity_receipt.json", json.str());
}

Fixture v7F1StarFixture(int candidates) {
    Fixture f;
    f.shape = "v7_F1_same_parent_two_query_blocks"; f.k = 3; f.horizon = 8; f.origin = -65;
    f.arcs = {{0, 1}, {0, 2}};
    initializeQuery(f, {0, 1, 1});
    initializeHistories(f, candidates + 1);
    f.graph.vertex_labels.assign(candidates + 1, Label{1}); f.graph.vertex_labels[0] = Label{0};
    for (int v = 1; v <= candidates; ++v)
        std::fill(f.histories[0][v].begin(), f.histories[0][v].end(), 1);
    finishGraph(f);
    return f;
}
QueryDecomposition v7F1StarDecomposition(const Fixture& f) {
    QueryDecomposition qd;
    qd.root = 0; qd.connected = true; qd.parent = {-1, 0, 0}; qd.dfs_order = {0, 1, 2};
    qd.vertex_labels = f.query.vertex_labels; qd.temporal_selectivity.assign(3, 1.0);
    qd.spanning_tree_adj = {{1, 2}, {}, {}};
    return qd;
}
void v7F1BoundaryUnits(const fs::path& out, const fs::path& scratch) {
    BIndexExecution::WorkerScratch reusable;
    std::uint64_t checks = 0;
    for (int n : {63, 64, 65, 129}) {
        const auto f = v7F1StarFixture(n);
        const auto files = makeFixtureStore(scratch, f);
        const auto decomposition = v7F1StarDecomposition(f);
        const TDTree tree(f.graph, f.query, decomposition, f.k);
        const auto plan = makeQueryPlan(f.query, decomposition, tree, OrderMode::Paper);
        const WindowStore windows(f.graph, files.store.string(), files.sha, f.k);
        const auto universal = window_algebra::WindowView::universal(windows.domain());
        const auto* R1 = tree.candidatesForParent(1, 0);
        const auto* R2 = tree.candidatesForParent(2, 0);
        require(R1 && R2 && R1 != R2 && *R1 == *R2 && R1->size() == static_cast<std::size_t>(n),
                "F1 fixture did not preserve separate same-parent/query TD identities");
        BIndexExecution cached(f.graph, f.query, plan, tree, windows, f.k, 16, 64, 1, .5,
            BIndexExecution::Mode::Always, true, true, v7CapacityReference(f.graph));
        BIndexExecution fallback(f.graph, f.query, plan, tree, windows, f.k, 16, 64, 1, .5,
            BIndexExecution::Mode::Always, true, true);
        BIndexExecution::WorkerScratch reference_scratch;
        const auto before = v7IndexJson(cached);
        require(v7Number(before, "actual_registry_payload_bytes") == 0 &&
                v7Number(before, "sparse_registered_used_blocks") == 0,
                "registry allocated before first eligible use");
        require(before.find("\"baseline_capacity_source\":\"finalized_graph_load_receipt\"") != std::string::npos &&
                v7IndexJson(fallback).find("\"baseline_capacity_source\":\"uncached_graph_scan\"") != std::string::npos,
                "capacity receipt/fallback source reporting differs");
        BIndexExecution::Selection held;
        for (int q : {1, 2}) {
            const auto& R = q == 1 ? *R1 : *R2;
            const auto selected = cached.select(q, 0, R, universal, reusable);
            const auto reference = fallback.select(q, 0, R, universal, reference_scratch);
            require(selected.used == (n >= 64) && selected.used == reference.used &&
                    selected.candidates == reference.candidates && selected.parent_entries == reference.parent_entries &&
                    selected.n_idx == reference.n_idx, "capacity receipt changes index decision or extraction");
            if (n >= 64) require(selected.candidates == R, "boundary bitmap omitted or reordered candidates");
            if (q == 1) held = selected;
            ++checks;
        }
        if (n >= 64) {
            require(held.candidates == *R1 && held.parent_entries.size() == R1->size(),
                    "scratch growth or multi-q reuse mutated earlier owned selection");
            require(reusable.bitmap.size() == (static_cast<std::size_t>(n) + 63) / 64 &&
                    reusable.touched_words.empty() &&
                    std::all_of(reusable.bitmap.begin(), reusable.bitmap.end(), [](std::uint64_t x) { return x == 0; }),
                    "on-selection scratch failed grow/reset at word boundary");
        } else require(reusable.storageBytes() == 0 && reference_scratch.storageBytes() == 0,
                       "all-small blocks allocated worker scratch");
        auto evidence = v7IndexJson(cached);
        const auto fallback_evidence = v7IndexJson(fallback);
        require(v7Number(evidence, "baseline_adjacency_temporal_capacity_bytes") == v7CapacityReference(f.graph) &&
                v7Number(evidence, "cap_bytes") == v7Number(fallback_evidence, "cap_bytes") &&
                v7Number(evidence, "persistent_index_bytes") == v7Number(fallback_evidence, "persistent_index_bytes") &&
                v7Object(evidence, "totals") == v7Object(fallback_evidence, "totals"),
                "receipt/fallback capacity decisions or deterministic counters differ");
        require(v7Number(evidence, "built_indexes") == (n >= 64 ? 2U : 0U) &&
                v7Number(evidence, "sparse_registered_used_blocks") == (n >= 64 ? 2U : 0U),
                "multi-q blocks aliased or small block registered metadata");
        v7CheckPayload(evidence);
        writeNew(out / ("v7_f1_R" + std::to_string(n) + ".json"), evidence + "\n");
        // Check identity after balanced successful calls; this intentionally
        // throwing call need not reconcile the normal decision-counter funnel.
        if (n >= 64) {
            bool rejected = false;
            try { (void)cached.select(1, 0, *R2, universal, reusable); }
            catch (const std::logic_error&) { rejected = true; }
            require(rejected, "same-values wrong-query TD block identity was accepted");
        }
        removeSuccessfulFixture(files);
    }
    std::ostringstream json;
    json << "{\"status\":\"PASS\",\"R\":[63,64,65,129],\"receipt_fallback_selection_pairs\":" << checks
         << ",\"zero_eligible_registry_and_scratch_bytes\":0,\"same_parent_multi_q_identity\":true,"
            "\"grow_reset_and_owned_selection\":true,\"payload_not_allocator_RSS_bound\":true}\n";
    writeNew(out / "v7_f1_boundary_receipt.json", json.str());
}

void v7F1RaceUnit(const fs::path& out, const fs::path& scratch) {
    const auto f = v7F1StarFixture(129);
    const auto files = makeFixtureStore(scratch, f);
    const auto decomposition = v7F1StarDecomposition(f);
    const TDTree tree(f.graph, f.query, decomposition, f.k);
    const auto plan = makeQueryPlan(f.query, decomposition, tree, OrderMode::Paper);
    const WindowStore windows(f.graph, files.store.string(), files.sha, f.k);
    const auto* R = tree.candidatesForParent(1, 0);
    require(R && R->size() == 129, "race fixture TD block missing");
    BIndexExecution index(f.graph, f.query, plan, tree, windows, f.k, 16, 64, 1, .5,
        BIndexExecution::Mode::Always, true, true, v7CapacityReference(f.graph));
    constexpr std::size_t workers = 16;
    std::array<BIndexExecution::WorkerScratch, workers> scratch_bits;
    std::array<std::exception_ptr, workers> errors{};
    std::array<std::thread, workers> threads;
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> go{false};
    for (std::size_t t = 0; t < workers; ++t) threads[t] = std::thread([&, t] {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        try {
            const auto selected = index.select(1, 0, *R,
                window_algebra::WindowView::universal(windows.domain()), scratch_bits[t]);
            require(selected.used && selected.candidates == *R && selected.parent_entries.size() == R->size(),
                    "concurrent first-use selection differs");
            require(scratch_bits[t].bitmap.size() == 3 && scratch_bits[t].touched_words.empty() &&
                    std::all_of(scratch_bits[t].bitmap.begin(), scratch_bits[t].bitmap.end(),
                                [](std::uint64_t x) { return x == 0; }), "concurrent worker scratch not independent/reset");
        } catch (...) { errors[t] = std::current_exception(); }
    });
    while (ready.load(std::memory_order_acquire) != workers) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    for (const auto& error : errors) if (error) std::rethrow_exception(error);
    const auto json = v7IndexJson(index);
    const auto totals = v7Object(json, "totals");
    require(v7Number(json, "build_attempts") == 1 && v7Number(json, "built_indexes") == 1 &&
            v7Number(json, "sparse_registered_used_blocks") == 1 &&
            v7Number(totals, "states") == workers && v7Number(totals, "index_selected") == workers,
            "race published duplicate index/node or lost state decisions");
    // The original event is pre-lock and schedule-dependent, NOT build count.
    require(v7Number(totals, "usage_tickets") >= 1 && v7Number(totals, "usage_tickets") <= workers &&
            v7Number(totals, "prebuild_scans") == 0, "race changed pre-lock usage event semantics");
    v7CheckPayload(json);
    writeNew(out / "v7_f1_race.json", json + "\n");
    removeSuccessfulFixture(files);
}

void v7F1StickyReadyUnit(const fs::path& out, const fs::path& scratch) {
    Fixture f;
    f.shape = "v7_F1_ready_survives_global_denial"; f.k = 3; f.horizon = 1000;
    f.force_root_zero = true; f.arcs = {{0, 1}, {1, 2}};
    initializeQuery(f, {0, 1, 2}); initializeHistories(f, 65);
    f.graph.vertex_labels.assign(65, Label{4});
    f.graph.vertex_labels[0] = 0; f.graph.vertex_labels[1] = 1; f.graph.vertex_labels[2] = 2;
    for (int t = 0; t < f.k; ++t) f.histories[0][1][t] = 1;
    std::fill(f.histories[1][2].begin(), f.histories[1][2].end(), 1);
    finishGraph(f);
    const auto files = makeFixtureStore(scratch, f);
    const auto decomposition = decompositionFor(f);
    const TDTree tree(f.graph, f.query, decomposition, f.k);
    const auto plan = makeQueryPlan(f.query, decomposition, tree, OrderMode::Paper);
    const WindowStore windows(f.graph, files.store.string(), files.sha, f.k);
    BIndexExecution index(f.graph, f.query, plan, tree, windows, f.k, 8, 1, 1, .5,
        BIndexExecution::Mode::Always, true, true, v7CapacityReference(f.graph));
    BIndexExecution::WorkerScratch scratch_bits;
    const auto* first = tree.candidatesForParent(1, 0);
    const auto* second = tree.candidatesForParent(2, 1);
    require(first && second && first->size() == 1 && second->size() == 1, "sticky-ready fixture TD blocks missing");
    const auto prior = window_algebra::WindowView::universal(windows.domain());
    const auto held = index.select(1, 0, *first, prior, scratch_bits);
    require(held.used && held.candidates == *first, "sticky-ready first real-cap index should fit");
    require(!index.select(2, 1, *second, prior, scratch_bits).used &&
            !index.select(2, 1, *second, prior, scratch_bits).used, "sticky-ready later real-cap denial should scan");
    const auto reused = index.select(1, 0, *first, prior, scratch_bits);
    require(reused.used && reused.candidates == held.candidates && reused.parent_entries == held.parent_entries,
            "already published index became unavailable after sticky global denial");
    const auto json = v7IndexJson(index);
    const auto totals = v7Object(json, "totals");
    require(v7Number(json, "build_attempts") == 2 && v7Number(json, "built_indexes") == 1 &&
            v7Number(json, "cap_denied_indexes") == 1 && v7Number(totals, "index_selected") == 2 &&
            v7Number(totals, "cap_denial_scans") == 2 && v7Number(totals, "usage_tickets") == 2,
            "sticky-ready build/denial/reuse counters differ");
    v7CheckPayload(json);
    writeNew(out / "v7_f1_ready_after_denial.json", json + "\n");
    removeSuccessfulFixture(files);
}

void v7F1MetadataDenialUnit(const fs::path& out, const fs::path& scratch) {
    Fixture f;
    f.shape = "v7_F1_metadata_cap_denial"; f.k = 3; f.horizon = 8;
    constexpr int query_size = 12;
    for (int q = 1; q < query_size; ++q) f.arcs.emplace_back(q - 1, q);
    initializeQuery(f, std::vector<Label>(query_size, Label{0})); initializeHistories(f, 2);
    f.graph.vertex_labels.assign(2, Label{0});
    std::fill(f.histories[0][1].begin(), f.histories[0][1].end(), 1);
    std::fill(f.histories[1][0].begin(), f.histories[1][0].end(), 1);
    finishGraph(f);
    QueryDecomposition decomposition;
    decomposition.root = 0; decomposition.connected = true;
    decomposition.parent.assign(query_size, -1); decomposition.dfs_order.resize(query_size);
    decomposition.spanning_tree_adj.resize(query_size);
    decomposition.temporal_selectivity.assign(query_size, 1.0);
    decomposition.vertex_labels = f.query.vertex_labels;
    for (int q = 0; q < query_size; ++q) {
        decomposition.dfs_order[q] = q;
        if (q) { decomposition.parent[q] = q - 1; decomposition.spanning_tree_adj[q - 1].push_back(q); }
    }
    const auto files = makeFixtureStore(scratch, f);
    const TDTree tree(f.graph, f.query, decomposition, f.k);
    const auto plan = makeQueryPlan(f.query, decomposition, tree, OrderMode::Paper);
    const WindowStore windows(f.graph, files.store.string(), files.sha, f.k);
    BIndexExecution index(f.graph, f.query, plan, tree, windows, f.k, 16, 1, 1, .5,
        BIndexExecution::Mode::Always, true, true, v7CapacityReference(f.graph));
    const auto before = v7IndexJson(index);
    require(before.find("\"metadata_cap_denied\":true") != std::string::npos &&
            v7Number(before, "legacy_registry_charge_bytes") > v7Number(before, "cap_bytes"),
            "metadata unit did not exceed the unchanged REAL capacity debit");
    const auto* R = tree.candidatesForParent(1, 0);
    require(R && !R->empty(), "metadata denial fixture lacks eligible block");
    BIndexExecution::WorkerScratch scratch_bits;
    require(!index.select(1, 0, *R, window_algebra::WindowView::universal(windows.domain()), scratch_bits).used &&
            scratch_bits.storageBytes() == 0, "metadata-denied call selected index or allocated scratch");
    const auto json = v7IndexJson(index);
    require(v7Number(json, "build_attempts") == 0 && v7Number(json, "actual_registry_payload_bytes") == 0 &&
            v7Number(json, "sparse_registered_used_blocks") == 0, "metadata denial allocated registry or tried construction");
    v7CheckPayload(json);
    writeNew(out / "v7_f1_metadata_denial.json", json + "\n");
    removeSuccessfulFixture(files);
}

void v7F1Units(const fs::path& out, const fs::path& scratch) {
    v7CapacityUnits(out, scratch);
    v7F1BoundaryUnits(out, scratch);
    v7F1RaceUnit(out, scratch);
    v7F1StickyReadyUnit(out, scratch);
    v7F1MetadataDenialUnit(out, scratch);
    writeNew(out / "v7_f1_receipt.json",
        "{\"status\":\"PASS\",\"capacity_receipt_exact\":true,\"R\":[63,64,65,129],"
        "\"zero_eligible_allocation_none\":true,\"multi_q_identity\":true,\"concurrent_workers\":16,"
        "\"singleton_publication\":true,\"sticky_denial_and_ready_reuse\":true,\"metadata_denial\":true,"
        "\"scratch_grow_reset\":true,\"virtual_legacy_debit_payload_bound\":true,\"F2_changes\":false,"
        "\"unit_only_Nmin_1_cap_fixtures\":true,\"performance_Nmin\":64,\"performance_runs\":0}\n");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "Usage: test_v7_direct NEW_OUTPUT_DIRECTORY\n"; return 2; }
    const fs::path out(argv[1]);
    if (fs::exists(out)) { std::cerr << "Refusing existing output " << out << '\n'; return 2; }
    fs::create_directories(out / "scratch");
    try {
        v7F1Units(out, out / "scratch");
        v7ComponentUnits(out, out / "scratch");
        v7LoopUnits(out, out / "scratch");
        v7FixedAndParallel(out, out / "scratch");
        v7Random(out, out / "scratch");
        writeNew(out / "v7_direct_receipt.json",
            "{\"schema\":\"v7_direct_correctness_v1\",\"status\":\"PASS\","
            "\"arms\":[\"dfs\",\"win_s\",\"final\"],\"seed\":20261020,\"random_graphs\":22400,\"configurations\":3,"
            "\"random_exact_set_comparisons\":67200,\"boundary_loop_fixtures\":24,\"boundary_loop_comparisons\":72,"
            "\"large_R96_fixtures\":24,\"parallel_pairs\":216,\"ops_off_checks\":9,"
            "\"independent_mapping_and_unmodified_interval_oracle\":true,\"DAG_compilation_or_execution\":false,"
            "\"loaded_edge_windows_rebuilt\":false,\"all_four_k_sections_bit_verified_per_fixture\":true,"
            "\"v6_fixture_seed_rng_order_and_boundaries_preserved\":true,"
            "\"comparison_count_change_due_only_to_three_arm_scope\":true,"
            "\"F1_units_receipt\":\"v7_f1_receipt.json\",\"F2_changes\":false,"
            "\"old_executable_parity_requires_separate_evidence\":true,\"performance_runs\":0,\"failures\":0}\n");
        std::cout << "V7 DIRECT CORRECTNESS PASS" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        const auto receipt = "{\"status\":\"STOPPED\",\"reason\":" + quote(error.what()) +
            ",\"no_further_test_instances\":true,\"no_retry\":true,\"performance_runs\":0}\n";
        if (!fs::exists(out / "counterexample.json")) writeNew(out / "counterexample.json", receipt);
        writeNew(out / "STOP.json", receipt);
        writeNew(out / "v7_direct_receipt.json", receipt);
        std::cerr << "STOP V7 DIRECT CORRECTNESS: " << error.what() << '\n';
        return 1;
    }
}

