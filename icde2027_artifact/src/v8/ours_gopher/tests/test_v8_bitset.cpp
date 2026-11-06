// V8 raw-bitset correctness, not performance evidence. Reuse exactly v7's
// v5 fixture generator/RNG/oracle; never invoke the preserved driver.
#define main preserved_v5_driver_never_invoked_by_v8
#include "test_v5_matcher.cpp"
#undef main

#include "../RawBitset.h"
#include "../WStoreFile.h"

namespace {
struct RawFixtureStore {
    fs::path input, store;
    std::string sha;
    raw_bitset::BitVerification verified;
};

RawFixtureStore rawFixtureStore(const fs::path& scratch, const Fixture& f) {
    RawFixtureStore result{scratch / "fixture.graph", scratch / "fixture.rawbits", "", {}};
    require(!fs::exists(result.input) && !fs::exists(result.store), "raw fixture files already exist");
    std::ostringstream text;
    for (const auto& edge : f.graph.temporal_edges)
        for (const auto& interval : edge.activeIntervals())
            for (std::int64_t t = interval.start; t <= interval.end; ++t)
                text << f.graph.externalId(edge.u) << ' ' << f.graph.externalId(edge.v) << ' ' << t << '\n';
    writeNew(result.input, text.str());
    const auto built = wstore_file::build(f.graph, result.input.string(), "v8_raw_correctness_fixture",
                                        {1}, result.store.string());
    result.sha = wstore_file::graphFileSha256(result.input.string());
    require(result.sha == built.header.graph_file_sha256, "raw fixture SHA differs");
    const auto loaded = wstore_file::load(result.store.string(), result.sha,
        f.graph.temporal_edges.size(), f.graph.num_vertices, 1);
    require(loaded.header.sections.size() == 1 && loaded.section.k == 1, "raw store is not k-independent");
    require(wstore_file::verify(f.graph, loaded) == f.graph.temporal_edges.size(), "row verification incomplete");
    result.verified = raw_bitset::verifyEveryBit(f.graph, loaded);
    require(result.verified.edges == f.graph.temporal_edges.size(), "independent bit verification incomplete");
    return result;
}

void removeRawFixture(const RawFixtureStore& files) {
    // Exactly the two files this campaign created. Any mismatch throws before
    // cleanup and preserves graph, store and counterexample in the new folder.
    require(fs::remove(files.input), "cannot remove successful raw fixture input");
    require(fs::remove(files.store), "cannot remove successful raw fixture store");
}

void rawNative(const Fixture& f, const QueryPlan& plan, const TDTree& tree,
               const RawFixtureStore& files, bool raw, std::size_t threads,
               bool counted, NativeResult& result) {
    result = NativeResult{};
    result.counted = counted;
    Observation observation;
    observation.fixture = &f;
    OperatorCounters counters;
    GopherMatcherOptions options;
    options.threads = threads;
    options.parallel_mode = ParallelMode::Roots;
    options.order_mode = OrderMode::Paper;
    options.early_prune = true;
    options.result_digest = true;
    options.direct_p4 = raw;
    options.window_algebra = raw;
    options.raw_bitset = raw;
    if (raw) { options.wstore_path = files.store.string(); options.graph_file_sha256 = files.sha; }
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
#define SAVE_RAW_OP(name) result.ops[#name] = counters.ops_##name.load(std::memory_order_relaxed);
        DSM_OPS_FIELDS(SAVE_RAW_OP)
#undef SAVE_RAW_OP
    };
    try {
        GopherMatcher matcher(f.graph, f.query, plan, tree, f.k, options);
        result.summary = matcher.run(MatchOutputMode::CountOnly);
    } catch (...) { capture(); throw; }
    capture();
}

bool loopFree(const Fixture& f) {
    return std::none_of(f.arcs.begin(), f.arcs.end(), [](auto edge) { return edge.first == edge.second; });
}

void checkRaw(const Fixture& f, const Answers& expected, const NativeResult& actual,
              bool raw, const NativeResult* direct) {
    require(!actual.invalid && !actual.duplicate && !actual.summary.count_overflow,
            "raw result duplicate/invalid/overflow");
    require(actual.answers == expected && actual.summary.match_count == expected.size(),
            "raw exact mapping+normalized output interval set differs from byte-time oracle");
    require(actual.summary.bindex_execution_json.empty() && actual.summary.jindex_execution_json.empty(),
            "raw arm unexpectedly constructed an index");
    if (raw) {
        require(actual.summary.v5_window_built_edges == 0 &&
                actual.summary.v5_window_relevant_edges == f.graph.temporal_edges.size(),
                "raw loaded store rebuilt or omitted edge rows");
        require(actual.summary.wstore_loading_json.find("\"k\":1") != std::string::npos,
                "raw matcher loaded a query-eroded section");
    }
    if (!direct) return;
    require(actual.summary.result_digest == direct->summary.result_digest, "raw mapping digest differs");
    if (actual.counted) {
        require(actual.ops.at("logical_extensions") == direct->ops.at("logical_extensions"),
                "raw accepted extensions differ from DFS");
        if (loopFree(f)) {
            const auto kappas = direct->ops.at("kappa_executed");
            const auto intersections = direct->ops.at("time_intersections_executed");
            require(kappas >= intersections, "invalid DFS temporal check subtraction");
            require(actual.ops.at("bitset_time_checks") == kappas - intersections,
                    "raw extension temporal check count differs from DFS");
            require(actual.ops.at("bitset_time_checks") == actual.ops.at("window_candidate_checks"),
                    "raw loop-free counter crosscheck failed");
            require(actual.ops.at("bitset_time_passes") == actual.ops.at("logical_extensions"),
                    "raw passes and accepted extensions differ");
        }
    }
}

void rawKernelUnits(const fs::path& out) {
    std::mt19937_64 rng(20261028ULL);
    std::uint64_t checks = 0;
    const std::array<int, 12> horizons{{1, 2, 3, 63, 64, 65, 127, 128, 129, 200, 1023, 1024}};
    const std::array<int, 14> durations{{1, 2, 3, 4, 5, 31, 32, 63, 64, 65, 127, 128, 1024, 1025}};
    for (int horizon : horizons) for (int pattern = 0; pattern < 32; ++pattern) {
        Times times(static_cast<std::size_t>(horizon), 0);
        for (int t = 0; t < horizon; ++t) {
            const bool set = pattern == 0 ? false : pattern == 1 ? true : pattern == 2 ? t % 2 == 0 :
                pattern == 3 ? t >= 60 && t <= 68 : pattern == 4 ? t >= 123 && t <= 132 :
                pattern == 5 ? t >= horizon - 5 : (rng() % 8) < static_cast<std::uint64_t>(pattern % 8);
            times[static_cast<std::size_t>(t)] = static_cast<unsigned char>(set);
        }
        std::array<std::uint64_t, window_algebra::kDenseWords> bits{};
        for (std::size_t t = 0; t < times.size(); ++t)
            if (times[t]) bits[t / 64] |= std::uint64_t{1} << (t % 64);
        const auto domain = window_algebra::Domain::make(-1025, -1025 + horizon - 1, 1);
        const auto view = window_algebra::WindowView::dense(domain, bits.data(), domain.wordCount());
        const auto before = bits;
        for (int k : durations) {
            raw_bitset::Scratch scratch{};
            raw_bitset::RunWork counted;
            const auto actual = raw_bitset::hasRun(view, k, scratch, &counted);
            require(actual == independentHasRun(times, k), "doubling shift-AND differs from scalar run oracle");
            require(bits == before, "run test mutated raw branch support");
            std::uint64_t steps = 0, len = 1;
            if (!view.isEmpty() && k <= horizon)
                while (len < static_cast<std::uint64_t>(k)) { len += std::min(len, static_cast<std::uint64_t>(k) - len); ++steps; }
            require(counted.shift_and_steps == steps && counted.shift_word_ops == steps * domain.wordCount(),
                    "shift-AND operation counter differs from exact loop count");
            ++checks;
        }
    }
    require(checks == 5376, "raw kernel fixed unit count differs");
    writeNew(out / "v8_raw_kernel_receipt.json",
        "{\"status\":\"PASS\",\"checks\":5376,\"signed_axis\":true,\"word_boundaries\":true,"
        "\"k_above_span\":true,\"k_64_128_no_shift_ub\":true,\"branch_input_immutable\":true}\n");
}

void rawStoreUnits(const fs::path& out, const fs::path& scratch) {
    const auto f = loopFixture("positive_both_loops", 65);
    const auto files = rawFixtureStore(scratch, f);
    auto loaded = wstore_file::load(files.store.string(), files.sha,
        f.graph.temporal_edges.size(), f.graph.num_vertices, 1);
    require(!loaded.payload.empty(), "raw store unit fixture empty");
    loaded.payload.front() ^= 1;
    bool rejected = false;
    try { (void)raw_bitset::verifyEveryBit(f.graph, loaded); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "independent raw verifier accepted flipped temporal bit");
    rejected = false;
    try { (void)wstore_file::load(files.store.string(), std::string(64, '0'),
        f.graph.temporal_edges.size(), f.graph.num_vertices, 1); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "raw store accepted wrong graph SHA");
    removeRawFixture(files);
    writeNew(out / "v8_raw_store_receipt.json",
        "{\"status\":\"PASS\",\"all_edge_timestamp_and_padding_bits\":true,"
        "\"independent_bit_flip_rejected\":true,\"wrong_input_sha_rejected\":true}\n");
}

void rawRandom(const fs::path& out, const fs::path& scratch) {
    std::mt19937_64 rng(kSeed); // EXACT v7 seed, RNG calls, and loop order.
    std::uint64_t instances = 0, raw_checks = 0, direct_checks = 0, bits = 0, edges = 0;
    std::ofstream log(out / "v8_bitset_differential.csv");
    log << "k,shape,stratum,instance,horizon,answers,logical_extensions,time_checks,time_passes,edge_words_read,shift_steps,exact_sets_equal\n";
    for (int k : {2, 3, 4, 5}) for (const auto& shape : kShapes) for (int stratum = 0; stratum < 4; ++stratum) {
        std::uint64_t positive = 0, zero = 0;
        for (int instance = 0; instance < kPerStratum; ++instance) {
            const auto f = randomFixture(rng, k, shape, stratum, instance);
            Answers expected;
            NativeResult direct, actual;
            std::string arm = "oracle_and_raw_store";
            try {
                expected = independentOracle(f);
                require(!f.planted || !expected.empty(), "v7 planted fixture witness disappeared");
                if (instance % 3 == 2) require(expected.empty(), "v7 negative fixture changed");
                const auto files = rawFixtureStore(scratch, f);
                const auto decomposition = decompositionFor(f);
                const TDTree tree(f.graph, f.query, decomposition, f.k);
                const auto plan = makeQueryPlan(f.query, decomposition, tree, OrderMode::Paper);
                arm = "dfs";
                rawNative(f, plan, tree, files, false, 1, true, direct);
                checkRaw(f, expected, direct, false, nullptr); ++direct_checks;
                arm = "bitset";
                rawNative(f, plan, tree, files, true, 1, true, actual);
                checkRaw(f, expected, actual, true, &direct); ++raw_checks;
                bits += files.verified.logical_bits + files.verified.padding_bits;
                edges += files.verified.edges;
                log << k << ',' << shape << ',' << stratum << ',' << instance << ',' << f.horizon << ','
                    << actual.answers.size() << ',' << actual.ops.at("logical_extensions") << ','
                    << actual.ops.at("bitset_time_checks") << ',' << actual.ops.at("bitset_time_passes") << ','
                    << actual.ops.at("bitset_edge_words_read") << ',' << actual.ops.at("bitset_shift_and_steps") << ",true\n";
                log.flush(); require(static_cast<bool>(log), "raw differential log write failed");
                removeRawFixture(files);
                ++instances; positive += !expected.empty(); zero += expected.empty();
            } catch (const std::exception& error) {
                counterexample(out, f, arm, error.what(), expected, actual);
                throw;
            }
        }
        require(positive > 0 && zero > 0, "v7 stratum positive/negative coverage changed");
        std::cout << "V8 RAW RANDOM PASS k=" << k << " shape=" << shape << " stratum=" << stratum << std::endl;
    }
    require(instances == 22400 && raw_checks == 22400 && direct_checks == 22400, "raw random corpus incomplete");
    std::ostringstream receipt;
    receipt << "{\"status\":\"PASS\",\"seed\":" << kSeed << ",\"random_graphs\":22400,"
        "\"raw_exact_set_checks\":22400,\"dfs_exact_set_checks\":22400,\"logical_extensions_and_time_checks_equal\":true,"
        "\"verified_edges\":" << edges << ",\"verified_all_bits_with_padding\":" << bits << ","
        "\"v7_fixture_seed_rng_order_preserved\":true,\"performance_runs\":0}\n";
    writeNew(out / "v8_bitset_differential_receipt.json", receipt.str());
}

// Copied byte-for-byte fixture body from v7LargeFixture; renamed only.
Fixture rawLargeFixture(int horizon, int k, int variant) {
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

void rawBoundaryAndParallel(const fs::path& out, const fs::path& scratch) {
    std::uint64_t loop_fixtures = 0, parallel_pairs = 0, off_checks = 0, maximum_callbacks = 0;
    std::ofstream log(out / "v8_bitset_boundary_parallel.csv");
    log << "shape,horizon,k,threads,ops_enabled,answers,extensions,exact_sets_equal\n";
    auto check_fixture = [&](const Fixture& f, bool parallel) {
        Answers expected;
        NativeResult direct, actual;
        try {
            expected = independentOracle(f);
            const auto files = rawFixtureStore(scratch, f);
            const auto decomposition = decompositionFor(f);
            const TDTree tree(f.graph, f.query, decomposition, f.k);
            const auto plan = makeQueryPlan(f.query, decomposition, tree, OrderMode::Paper);
            rawNative(f, plan, tree, files, false, 1, true, direct);
            checkRaw(f, expected, direct, false, nullptr);
            rawNative(f, plan, tree, files, true, 1, true, actual);
            checkRaw(f, expected, actual, true, &direct);
            log << f.shape << ',' << f.horizon << ',' << f.k << ",1,true," << actual.answers.size()
                << ',' << actual.ops.at("logical_extensions") << ",true\n";
            if (parallel) {
                NativeResult serial = std::move(actual);
                rawNative(f, plan, tree, files, true, 16, true, actual);
                checkRaw(f, expected, actual, true, &direct);
                require(actual.summary.threads_used == 16, "raw parallel fixture did not use 16 worker slots");
                for (const auto* field : {"logical_extensions", "bitset_time_checks", "bitset_time_passes",
                                          "bitset_edge_words_read", "bitset_shift_and_steps", "bitset_shift_word_ops"})
                    require(actual.ops.at(field) == serial.ops.at(field), "raw 1T/16T operation counts differ");
                maximum_callbacks = std::max<std::uint64_t>(maximum_callbacks, actual.callback_threads);
                log << f.shape << ',' << f.horizon << ',' << f.k << ",16,true," << actual.answers.size()
                    << ',' << actual.ops.at("logical_extensions") << ",true\n";
                ++parallel_pairs;
                rawNative(f, plan, tree, files, true, 16, false, actual);
                checkRaw(f, expected, actual, true, &direct);
                require(actual.ops.at("bitset_time_checks") == 0, "ops-off raw arm collected counters");
                ++off_checks;
            }
            log.flush(); require(static_cast<bool>(log), "raw boundary log failed");
            removeRawFixture(files);
        } catch (const std::exception& error) {
            counterexample(out, f, "bitset_boundary", error.what(), expected, actual); throw;
        }
    };
    // Exact existing v7 loop corpus: six names x four original horizons.
    for (const auto* name : {"disjoint_cumulative_support", "missing_root_loop", "short_root_loop",
                             "missing_nonroot_loop", "short_nonroot_loop", "positive_both_loops"})
        for (int horizon : {8, 65, 130, 200}) { check_fixture(loopFixture(name, horizon), false); ++loop_fixtures; }
    for (int horizon : {65, 130, 200}) for (int k : {2, 3, 4, 5}) for (int variant : {0, 1})
        check_fixture(rawLargeFixture(horizon, k, variant), true);
    require(loop_fixtures == 24 && parallel_pairs == 24 && off_checks == 24 && maximum_callbacks > 1,
            "raw boundary/parallel coverage incomplete");
    writeNew(out / "v8_bitset_boundary_receipt.json",
        "{\"status\":\"PASS\",\"loop_fixtures\":24,\"large_R96_fixtures\":24,\"parallel_pairs\":24,"
        "\"ops_off_checks\":24,\"exact_sets_and_counters_equal\":true}\n");
}
}

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "Usage: test_v8_bitset NEW_OUTPUT_DIRECTORY\n"; return 2; }
    const fs::path out(argv[1]);
    if (fs::exists(out)) { std::cerr << "Refusing existing output " << out << '\n'; return 2; }
    fs::create_directories(out / "scratch");
    try {
        rawKernelUnits(out);
        rawStoreUnits(out, out / "scratch");
        rawBoundaryAndParallel(out, out / "scratch");
        rawRandom(out, out / "scratch");
        writeNew(out / "v8_bitset_receipt.json",
            "{\"schema\":\"v8_bitset_correctness_v1\",\"status\":\"PASS\",\"random_graphs\":22400,"
            "\"seed\":20261020,\"raw_exact_set_comparisons\":22400,\"dfs_exact_set_comparisons\":22400,"
            "\"loop_fixtures\":24,\"parallel_pairs\":24,\"ops_off_checks\":24,\"run_kernel_checks\":5376,"
            "\"all_edge_raw_bits_independently_verified\":true,\"v7_fixture_order_preserved\":true,"
            "\"performance_runs\":0,\"real_graph_checks_pending\":true}\n");
        std::cout << "V8 BITSET CORRECTNESS PASS" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        const auto receipt = "{\"status\":\"STOPPED\",\"reason\":" + quote(error.what()) +
            ",\"no_further_instances\":true,\"no_retry\":true,\"performance_runs\":0}\n";
        if (!fs::exists(out / "counterexample.json")) writeNew(out / "counterexample.json", receipt);
        writeNew(out / "STOP.json", receipt);
        writeNew(out / "v8_bitset_receipt.json", receipt);
        std::cerr << "STOP V8 BITSET: " << error.what() << '\n';
        return 1;
    }
}
