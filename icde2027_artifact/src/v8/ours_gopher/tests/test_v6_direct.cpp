// V6 Direct-only correctness campaign. Reuse the independent byte-time oracle
// and fixture constructors, NEVER the preserved main/native DAG helpers.
#define main preserved_v5_driver_never_invoked_by_v6
#include "test_v5_matcher.cpp"
#undef main

#include "../BIndexExecution.h"
#include "../WindowStore.h"
#include "../WStoreFile.h"
#include <cctype>

namespace {
struct V6Config {
    std::string name;
    bool probe = false, window = false, loaded = false, adjacency = false;
    BIndexMode bmode = BIndexMode::None;
    double alpha = .5;
    std::size_t c = 1;
};
std::vector<V6Config> v6Configs() {
    std::vector<V6Config> result{
        {"dfs"}, {"probe", true}, {"win_q", true, true},
        {"win_s", true, true, true}, {"aidx_always", true, true, true, true}};
    for (std::size_t c : {1U, 2U})
        result.push_back({"bidx_always_c" + std::to_string(c), true, true, true, false, BIndexMode::Always, .5, c});
    for (double alpha : {.25, .5, 1.}) for (std::size_t c : {1U, 2U})
        result.push_back({"bidx_adaptive_a" + std::string(alpha == .25 ? "025" : alpha == .5 ? "05" : "1") +
            "_c" + std::to_string(c), true, true, true, false, BIndexMode::Adaptive, alpha, c});
    require(result.size() == 13, "v6 fixed configuration count differs");
    return result;
}
std::string v6Object(const std::string& text, const std::string& key) {
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
std::uint64_t v6Number(const std::string& text, const std::string& key) {
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

void v6Native(const Fixture& f, const QueryPlan& plan, const TDTree& tree,
              const FixtureStore& files, const V6Config& config, std::size_t B,
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
#define SAVE_V6_OP(name) result.ops[#name] = counters.ops_##name.load(std::memory_order_relaxed);
        DSM_OPS_FIELDS(SAVE_V6_OP)
#undef SAVE_V6_OP
    };
    try {
        // Only this Direct constructor. No CandidateDagCompiler, optimize pass,
        // CandidateDag object, DAG executor or DAG matcher is invoked in v6.
        GopherMatcher matcher(f.graph, f.query, plan, tree, f.k, options);
        result.summary = matcher.run(MatchOutputMode::CountOnly);
    } catch (...) { capture(); throw; }
    capture();
}
void checkV6(const Fixture& f, const Answers& expected, const NativeResult& actual,
             const V6Config& config, std::size_t B, const NativeResult* direct) {
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
        require(v6Number(j, "B") == B, "adjacency control B differs from requested correctness setting");
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
    require(v6Number(j, "B") == B && v6Number(j, "c") == config.c && v6Number(j, "n_min") == 64,
            "B-index frozen parameters changed");
    require(v6Number(j, "persistent_index_bytes") <= v6Number(j, "cap_bytes") &&
            v6Number(j, "peak_index_plus_build_scratch_bytes") <= v6Number(j, "cap_bytes"),
            "B-index exceeded memory cap instead of scan fallback");
    require(v6Number(j, "built_indexes") <= v6Number(j, "registered_blocks") &&
            v6Number(j, "built_indexes") <= v6Number(j, "build_attempts"),
            "B-index published more than once per registered TD block");
    if (actual.counted) {
        const auto totals = v6Object(j, "totals");
        require(v6Number(totals, "states") == v6Number(totals, "index_selected") + v6Number(totals, "scan_selected"),
                "B-index state decisions do not reconcile");
        require(v6Number(totals, "bucket_entries_consumed") >= v6Number(totals, "unique_extracted_candidates"),
                "CSR entries are smaller than extracted unique candidates");
        require(v6Number(totals, "missing_parent_edges") == 0 && v6Number(totals, "empty_parent_windows") == 0,
                "TD parent-edge/W invariant violated");
        require(v6Number(totals, "time_passes") <= v6Number(totals, "time_checks"),
                "exact W time-pass funnel exceeds checks");
    } else {
        require(j.find("\"totals\":null") != std::string::npos &&
                j.find("\"counters_enabled\":false") != std::string::npos,
                "ops OFF B-index collected totals");
    }
}
void preserveV6Failure(const fs::path& out, const Fixture& f, const std::string& arm,
                       const std::string& reason, const Answers& expected, const NativeResult& actual) {
    counterexample(out, f, arm, reason, expected, actual);
    if (!fs::exists(out / "v6_native_failure.json")) {
        std::ostringstream context;
        context << "{\"arm\":" << quote(arm) << ",\"bindex\":";
        if (actual.summary.bindex_execution_json.empty()) context << "null"; else context << actual.summary.bindex_execution_json;
        context << ",\"aindex\":";
        if (actual.summary.jindex_execution_json.empty()) context << "null"; else context << actual.summary.jindex_execution_json;
        context << ",\"generated_store_preserved\":true,\"no_retry\":true}\n";
        writeNew(out / "v6_native_failure.json", context.str());
    }
}
void logV6(std::ostream& log, const Fixture& f, const V6Config& config, std::size_t B,
           std::size_t threads, const NativeResult& result) {
    log << f.k << ',' << f.shape << ',' << f.stratum << ',' << f.instance << ',' << f.horizon << ','
        << config.name << ',' << B << ',' << threads << ',' << result.counted << ',' << result.answers.size()
        << ',' << result.ops.at("logical_extensions") << ',' << result.ops.at("candidates")
        << ',' << result.summary.threads_used << ',' << result.callback_threads << '\n';
}
void headerV6(std::ostream& out) {
    out << "k,shape,stratum,instance,horizon,configuration,B,requested_threads,ops_enabled,answers,"
           "logical_extensions,candidates,threads_used,callback_threads\n";
}

void v6Random(const fs::path& out, const fs::path& scratch) {
    std::mt19937_64 rng(kSeed);
    const auto configs = v6Configs();
    std::uint64_t instances = 0, checks = 0, rows = 0;
    std::ofstream log(out / "v6_differential_checks.csv");
    headerV6(log);
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
                    v6Native(f, plan, tree, files, config, 16, 1, true, actual);
                    checkV6(f, expected, actual, config, 16, config.name == "dfs" ? nullptr : &direct);
                    logV6(log, f, config, 16, 1, actual);
                    if (config.name == "dfs") direct = std::move(actual);
                    ++checks;
                }
                rows += files.verified_rows;
                removeSuccessfulFixture(files);
                ++instances;
                positive += !expected.empty(); zero += expected.empty();
                log.flush(); require(static_cast<bool>(log), "random log write failed");
            } catch (const std::exception& error) {
                preserveV6Failure(out, f, arm, error.what(), expected, actual);
                throw;
            }
        }
        require(positive > 0 && zero > 0, "random stratum lacks positive or zero coverage");
        if (comma) strata << ','; comma = true;
        strata << "{\"k\":" << k << ",\"shape\":" << quote(shape) << ",\"stratum\":" << stratum
               << ",\"instances\":200,\"positive\":" << positive << ",\"zero\":" << zero << '}';
        std::cout << "V6 RANDOM PASS k=" << k << " shape=" << shape << " stratum=" << stratum << std::endl;
    }
    require(instances == 22400 && checks == 291200, "v6 random campaign incomplete");
    std::ostringstream receipt;
    receipt << "{\"status\":\"PASS\",\"seed\":" << kSeed << ",\"random_graphs\":22400,\"configurations\":13,"
               "\"native_exact_set_comparisons\":291200,\"verified_edge_k_rows\":" << rows
            << ",\"strata\":[" << strata.str() << "],\"DAG_compilation_or_execution\":false,"
               "\"oracle\":\"independent byte-time mapping and unmodified interval set\"}\n";
    writeNew(out / "v6_differential_receipt.json", receipt.str());
}

Fixture v6LargeFixture(int horizon, int k, int variant) {
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
void v6FixedAndParallel(const fs::path& out, const fs::path& scratch) {
    const auto configs = v6Configs();
    std::ofstream log(out / "v6_large_parallel_checks.csv");
    headerV6(log);
    struct Coverage { std::uint64_t indexed = 0, scanned = 0, cost_scan = 0, prebuild = 0, built = 0; };
    std::map<std::string, Coverage> coverage;
    std::uint64_t pairs = 0, ops_off = 0, fixtures = 0, max_callbacks = 0;
    for (int horizon : {65, 130, 200}) for (int k : {2, 3, 4, 5}) for (int variant : {0, 1}) {
        const auto f = v6LargeFixture(horizon, k, variant);
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
            v6Native(f, plan, tree, files, configs.front(), 16, 1, true, direct);
            checkV6(f, expected, direct, configs.front(), 16, nullptr);
            for (std::size_t B : {8U, 16U, 32U}) for (const auto& config : configs) {
                arm = config.name + "_B" + std::to_string(B);
                v6Native(f, plan, tree, files, config, B, 1, true, actual);
                checkV6(f, expected, actual, config, B, &direct);
                logV6(log, f, config, B, 1, actual);
                NativeResult serial = std::move(actual);
                v6Native(f, plan, tree, files, config, B, 16, true, actual);
                checkV6(f, expected, actual, config, B, &direct);
                require(actual.answers == serial.answers && actual.summary.result_digest == serial.summary.result_digest &&
                        actual.ops.at("logical_extensions") == serial.ops.at("logical_extensions"),
                        "1T/16T exact answers, digest or logical extensions differ");
                require(actual.summary.threads_used == 16, "large fixture did not request/use 16 worker slots");
                max_callbacks = std::max<std::uint64_t>(max_callbacks, actual.callback_threads);
                logV6(log, f, config, B, 16, actual);
                ++pairs;
                if (config.bmode != BIndexMode::None) {
                    for (const auto* result : {&serial, &actual}) {
                        const auto& j = result->summary.bindex_execution_json;
                        const auto totals = v6Object(j, "totals");
                        auto& totals_out = coverage[config.name + "_B" + std::to_string(B)];
                        totals_out.indexed += v6Number(totals, "index_selected");
                        totals_out.scanned += v6Number(totals, "scan_selected");
                        totals_out.cost_scan += v6Number(totals, "cost_rejected_scans");
                        totals_out.prebuild += v6Number(totals, "prebuild_scans");
                        totals_out.built += v6Number(j, "built_indexes");
                        require(v6Number(j, "built_indexes") == 4 && v6Number(j, "build_attempts") == 4,
                                "R=96 fixture did not build exactly once for each of four eligible blocks");
                        require(v6Number(totals, "index_selected") > 0, "large fixture never selected B-index");
                        require(v6Number(totals, "small_block_scans") > 0, "small R scan path was not exercised");
                        if (config.c == 2)
                            require(v6Number(totals, "prebuild_scans") == 4,
                                    "c2 should scan exactly the first visit to each of four eligible blocks");
                    }
                    // Save a bounded production-schema sample, not every answer.
                    if (horizon == 65 && k == 3 && variant == 0)
                        writeNew(out / (config.name + "_B" + std::to_string(B) + "_production.json"),
                                 actual.summary.bindex_execution_json + "\n");
                }
                if (horizon == 65 && k == 3 && variant == 0) {
                    arm += "_ops_off";
                    v6Native(f, plan, tree, files, config, B, 16, false, actual);
                    checkV6(f, expected, actual, config, B, &direct);
                    logV6(log, f, config, B, 16, actual);
                    ++ops_off;
                }
            }
            removeSuccessfulFixture(files);
            ++fixtures;
            log.flush(); require(static_cast<bool>(log), "large parallel log failed");
        } catch (const std::exception& error) {
            preserveV6Failure(out, f, arm, error.what(), expected, actual);
            throw;
        }
    }
    for (const auto& [name, c] : coverage) {
        require(c.indexed > 0 && c.scanned > 0 && c.built > 0, "missing indexed/scan/build coverage " + name);
        if (name.find("adaptive") != std::string::npos)
            require(c.cost_scan > 0, "adaptive cost rejection was not exercised " + name);
        if (name.find("_c2") != std::string::npos) require(c.prebuild > 0, "c2 reuse threshold not exercised " + name);
    }
    require(fixtures == 24 && pairs == 936 && ops_off == 39 && max_callbacks > 1,
            "large/parallel/ops-off coverage incomplete");
    std::ostringstream receipt;
    receipt << "{\"status\":\"PASS\",\"fixtures\":24,\"R_per_eligible_block\":96,\"parallel_pairs\":936,"
               "\"ops_off_checks\":39,\"B\":[8,16,32],\"maximum_callback_threads\":" << max_callbacks
            << ",\"exact_mapping_intervals_digest_extensions_equal\":true,\"four_once_builds_per_run\":true}\n";
    writeNew(out / "v6_parallel_receipt.json", receipt.str());
}

void v6LoopUnits(const fs::path& out, const fs::path& scratch) {
    const auto configs = v6Configs();
    const std::array<std::string, 6> names{"disjoint_cumulative_support", "missing_root_loop", "short_root_loop",
        "missing_nonroot_loop", "short_nonroot_loop", "positive_both_loops"};
    std::uint64_t checks = 0;
    std::ofstream log(out / "v6_loop_checks.csv"); headerV6(log);
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
                v6Native(f, plan, tree, files, config, 16, 1, true, actual);
                checkV6(f, expected, actual, config, 16, config.name == "dfs" ? nullptr : &direct);
                logV6(log, f, config, 16, 1, actual);
                if (config.name == "dfs") direct = std::move(actual);
                ++checks;
            }
            removeSuccessfulFixture(files);
        } catch (const std::exception& error) {
            preserveV6Failure(out, f, arm, error.what(), expected, actual);
            throw;
        }
    }
    require(checks == 312, "fixed loop/cumulative-support coverage incomplete");
    writeNew(out / "v6_loop_receipt.json",
        "{\"status\":\"PASS\",\"fixtures\":24,\"negative_fixtures\":20,\"exact_set_comparisons\":312}\n");
}

void v6ComponentUnits(const fs::path& out, const fs::path& scratch) {
    const auto f = v6LargeFixture(200, 3, 0);
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
        BIndexExecution index(f.graph, f.query, plan, tree, windows, f.k, B, 64, 2, .5,
                              BIndexExecution::Mode::Always, true, true);
        BIndexExecution::WorkerScratch scratch_bits;
        scratch_bits.reserve(index.maximumBlockSize());
        const auto first = index.select(2, 16, *R, universal, scratch_bits);
        require(!first.used, "c2 must scan the first eligible visit");
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
        const auto total = v6Object(evidence.str(), "totals");
        require(v6Number(evidence.str(), "built_indexes") == 1 && v6Number(evidence.str(), "build_attempts") == 1 &&
                v6Number(total, "prebuild_scans") == 1, "per-block build-once/reuse threshold mismatch");
        require(v6Number(total, "universal_states") == 2 && v6Number(total, "universal_index_selected") == 1 &&
                v6Number(total, "universal_scan_selected") == 1, "universal decision accounting differs");
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
    cap_scratch.reserve(cap_index.maximumBlockSize());
    for (int q : {1, 2}) {
        const auto* block = cap_tree.candidatesForParent(q, q - 1);
        require(block && block->size() == 1, "cap unit TD block missing");
        const auto selected = cap_index.select(q, q - 1, *block,
            window_algebra::WindowView::universal(cap_windows.domain()), cap_scratch);
        require(!selected.used, "over-cap index did not fall back to scan");
    }
    std::ostringstream cap_json; cap_index.writeJson(cap_json);
    require(v6Number(cap_json.str(), "build_attempts") == 1 && v6Number(cap_json.str(), "built_indexes") == 0 &&
            v6Number(v6Object(cap_json.str(), "totals"), "cap_denial_scans") == 2,
            "global sticky cap denial retried a later block or failed to scan");
    writeNew(out / "bindex_cap_denial_unit.json", cap_json.str() + "\n");
    removeSuccessfulFixture(cap_files);
    std::ostringstream receipt;
    receipt << "{\"status\":\"PASS\",\"direct_index_selections\":" << selections
            << ",\"reused_scratch_preserves_earlier_selection\":true,\"reciprocal_parent_direction\":true,"
               "\"sha_mismatch_rejected_before_search\":true,\"sticky_global_cap_scan\":true,"
               "\"unit_cap_Nmin\":1,\"performance_Nmin_unchanged\":64}\n";
    writeNew(out / "v6_component_receipt.json", receipt.str());
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "Usage: test_v6_direct NEW_OUTPUT_DIRECTORY\n"; return 2; }
    const fs::path out(argv[1]);
    if (fs::exists(out)) { std::cerr << "Refusing existing output " << out << '\n'; return 2; }
    fs::create_directories(out / "scratch");
    try {
        v6ComponentUnits(out, out / "scratch");
        v6LoopUnits(out, out / "scratch");
        v6FixedAndParallel(out, out / "scratch");
        v6Random(out, out / "scratch");
        writeNew(out / "v6_direct_receipt.json",
            "{\"status\":\"PASS\",\"seed\":20261020,\"random_graphs\":22400,\"configurations\":13,"
            "\"random_exact_set_comparisons\":291200,\"boundary_loop_fixtures\":24,\"boundary_loop_comparisons\":312,"
            "\"large_R96_fixtures\":24,\"parallel_pairs\":936,\"ops_off_checks\":39,"
            "\"independent_mapping_and_unmodified_interval_oracle\":true,\"DAG_compilation_or_execution\":false,"
            "\"loaded_edge_windows_rebuilt\":false,\"all_four_k_sections_bit_verified_per_fixture\":true,"
            "\"old_executable_parity_requires_separate_evidence\":true,\"performance_runs\":0,\"failures\":0}\n");
        std::cout << "V6 DIRECT CORRECTNESS PASS" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        const auto receipt = "{\"status\":\"STOPPED\",\"reason\":" + quote(error.what()) +
            ",\"no_further_test_instances\":true,\"no_retry\":true,\"performance_runs\":0}\n";
        if (!fs::exists(out / "counterexample.json")) writeNew(out / "counterexample.json", receipt);
        writeNew(out / "STOP.json", receipt);
        writeNew(out / "v6_direct_receipt.json", receipt);
        std::cerr << "STOP V6 DIRECT CORRECTNESS: " << error.what() << '\n';
        return 1;
    }
}
