// Stage-A correctness only; root owns build/run permission. Reuse the exact
// prior independent bitmask oracle and graph builder, without executing its
// old main/property/pass experiments from this executable.
#define main prior_operator_passes_main_not_called
#include "test_operator_passes.cpp"
#undef main

#include "../AdaptiveDiagnostics.h"
#include "../OrderSimulator.h"

#include <mutex>
#include <thread>
#include <tuple>

namespace {

const std::array<std::string, 7> kAdaptiveShapes{{
    "path2", "path3", "cycle3", "cycle4", "star3", "diamond4", "tailed_cycle5"}};

std::vector<std::pair<int, int>> adaptiveArcs(const std::string& shape) {
    if (shape == "diamond4") return {{0, 1}, {0, 2}, {1, 2}, {1, 3}, {2, 3}};
    if (shape == "tailed_cycle5") return {{0, 1}, {1, 2}, {2, 0}, {2, 3}, {3, 4}};
    return shapeArcs(shape);
}

void initializeQuery(Fixture& f, const std::vector<Label>& labels) {
    f.query.num_vertices = static_cast<int>(labels.size());
    f.query.vertex_labels = labels;
    f.query.adj.assign(labels.size(), {});
    f.query.in_adj.assign(labels.size(), {});
    f.query.external_ids.resize(labels.size());
    std::iota(f.query.external_ids.begin(), f.query.external_ids.end(), 0);
    for (auto [a, b] : f.arcs) {
        f.query.adj[static_cast<std::size_t>(a)].push_back({b, -1});
        f.query.in_adj[static_cast<std::size_t>(b)].push_back({a, -1});
    }
    for (auto* adjacency : {&f.query.adj, &f.query.in_adj}) {
        for (auto& edges : *adjacency) std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.to < b.to; });
    }
}

Fixture makeAdaptiveFixture(std::mt19937_64& rng, int k, const std::string& shape, int instance) {
    // The first five shapes retain the prior generator without modification.
    if (shape != "diamond4" && shape != "tailed_cycle5") return makeFixture(rng, k, shape, instance);
    Fixture f;
    f.k = k; f.shape = shape; f.instance = instance;
    f.horizon = 1 + static_cast<int>(rng() % 10);
    f.graph.num_vertices = 8 + static_cast<int>(rng() % 5);
    f.arcs = adaptiveArcs(shape);
    const std::size_t n = static_cast<std::size_t>(f.graph.num_vertices);
    const std::size_t qn = shape == "diamond4" ? 4 : 5;
    const bool same_label = instance % 4 == 0;
    f.graph.vertex_labels.resize(n);
    for (auto& label : f.graph.vertex_labels) label = same_label ? Label{0} : static_cast<Label>(rng() % 3);
    std::vector<Label> labels(qn);
    for (std::size_t q = 0; q < qn; ++q) labels[q] = same_label ? Label{0} : static_cast<Label>(q % 3);
    initializeQuery(f, labels);
    f.histories.assign(n, std::vector<Bits>(n, 0));
    std::vector<std::pair<int, int>> available;
    for (int u = 0; u < f.graph.num_vertices; ++u) for (int v = 0; v < f.graph.num_vertices; ++v) {
        if (u != v) available.emplace_back(u, v);
    }
    std::shuffle(available.begin(), available.end(), rng);
    const std::size_t target_edges = 20 + static_cast<std::size_t>(rng() % 21);
    std::set<std::pair<int, int>> selected;
    f.planted = f.horizon >= k && instance % 3 != 2;
    Bits planted_support = 0;
    if (f.planted) {
        const int start = static_cast<int>(rng() % static_cast<Bits>(f.horizon - k + 1));
        for (int t = start; t < start + k; ++t) planted_support |= Bits{1} << t;
        for (std::size_t q = 0; q < qn; ++q) f.graph.vertex_labels[q] = labels[q];
        selected.insert(f.arcs.begin(), f.arcs.end());
    }
    for (auto edge : available) {
        if (selected.size() == target_edges) break;
        selected.insert(edge);
    }
    for (auto edge : selected) {
        Bits mask = randomBits(rng, f.horizon, 1 + static_cast<int>(rng() % 7));
        if (mask == 0) mask = Bits{1} << (rng() % static_cast<Bits>(f.horizon));
        if (f.planted && std::find(f.arcs.begin(), f.arcs.end(), edge) != f.arcs.end()) mask |= planted_support;
        f.histories[static_cast<std::size_t>(edge.first)][static_cast<std::size_t>(edge.second)] = mask;
    }
    finishGraph(f.graph, f.histories, f.horizon);
    require(f.graph.temporal_edges.size() == target_edges, "adaptive graph generator edge-count mismatch");
    return f;
}

struct ExactObservation {
    const Fixture* fixture = nullptr;
    std::mutex mutex;
    Answers answers;
    std::set<std::thread::id> callback_threads;
    bool duplicate = false;
    bool invalid_intervals = false;
};

void observeExact(const SearchState& state, void* opaque) {
    auto& observation = *static_cast<ExactObservation*>(opaque);
    const Fixture& f = *observation.fixture;
    std::ostringstream row;
    bool invalid = !state.has_active_intervals || state.active_intervals.empty();
    for (std::size_t q = 0; q < state.mapping.size(); ++q) {
        if (q) row << ", ";
        row << 'q' << q << '(' << labelToString(f.query.vertex_labels[q]) << ")->"
            << f.graph.externalId(state.mapping[q]);
    }
    for (std::size_t i = 0; i < state.active_intervals.size(); ++i) {
        const auto& interval = state.active_intervals[i];
        invalid = invalid || interval.end < interval.start || interval.length() < f.k;
        if (i) invalid = invalid || static_cast<std::int64_t>(interval.start) <=
            static_cast<std::int64_t>(state.active_intervals[i - 1].end) + 1;
    }
    // The oracle constructs intervals independently from bits. This observer
    // serializes the native intervals AS IS, never sorting/normalizing them.
    row << " | active=" << formatIntervals(state.active_intervals);
    std::lock_guard<std::mutex> lock(observation.mutex);
    observation.invalid_intervals = observation.invalid_intervals || invalid;
    observation.duplicate = !observation.answers.insert(row.str()).second || observation.duplicate;
    observation.callback_threads.insert(std::this_thread::get_id());
}

struct AdaptiveRun {
    Answers answers;
    GopherMatchSummary summary;
    std::uint64_t extensions = 0;
    std::uint64_t candidates = 0;
    std::size_t callback_threads = 0;
};

AdaptiveRun adaptiveNativeRun(
    const Fixture& f, const QueryPlan& plan, const TDTree& tree,
    const CandidateDag* dag, OrderSimulationMode mode = OrderSimulationMode::Disabled,
    std::size_t threads = 1, bool legacy_ops = false, AdaptiveDiagnostics* diagnostics = nullptr) {
    ExactObservation observation;
    observation.fixture = &f;
    OperatorCounters ops;
    GopherMatcherOptions options;
    options.threads = threads;
    options.parallel_mode = ParallelMode::Roots;
    options.early_prune = true;
    options.result_digest = true;
    options.order_simulation = mode;
    options.executor.memo_mode = MemoMode::None;
    options.executor.collect_statistics = false;
    options.executor.ops = legacy_ops ? &ops : nullptr;
    options.executor.adaptive = diagnostics;
    options.exact_match_observer = observeExact;
    options.exact_match_observer_context = &observation;
    GopherMatchSummary summary;
    if (dag) {
        GopherMatcher matcher(f.graph, f.query, plan, *dag, tree, f.k, options);
        summary = matcher.run(MatchOutputMode::CountOnly);
    } else {
        GopherMatcher matcher(f.graph, f.query, plan, tree, f.k, options);
        summary = matcher.run(MatchOutputMode::CountOnly);
    }
    require(!observation.duplicate, "duplicate native exact answer");
    require(!observation.invalid_intervals, "native active intervals are not normalized nonempty k-support");
    require(!summary.count_overflow && !summary.order_simulation.overflow, "small-graph counter overflow");
    require(observation.answers.size() == summary.match_count, "observer full-set size disagrees with native count");
    AdaptiveRun result;
    result.answers = std::move(observation.answers);
    result.callback_threads = observation.callback_threads.size();
    result.extensions = mode == OrderSimulationMode::Disabled
        ? ops.ops_logical_extensions.load(std::memory_order_relaxed) : summary.order_simulation.logical_extensions;
    result.candidates = mode == OrderSimulationMode::Disabled
        ? ops.ops_candidates.load(std::memory_order_relaxed) : summary.order_simulation.actual_candidates;
    result.summary = std::move(summary);
    return result;
}

void compareAdaptiveAnswers(const fs::path& out, const Fixture& f,
                           const std::string& arm, const Answers& expected, const AdaptiveRun& actual) {
    if (actual.answers != expected) {
        counterexample(out, f, arm, "exact mapping AND normalized active intervals differ", expected, actual.answers);
        throw std::runtime_error("Stage A exact answer-set mismatch: " + arm);
    }
}

Fixture fixedFixture(const std::string& name, const std::vector<std::pair<int, int>>& arcs,
                     const std::vector<Label>& query_labels, const std::vector<Label>& data_labels,
                     const std::vector<std::tuple<int, int, Bits>>& edges) {
    Fixture f;
    f.shape = name; f.k = 3; f.horizon = 8; f.instance = -1;
    f.arcs = arcs;
    initializeQuery(f, query_labels);
    f.graph.num_vertices = static_cast<int>(data_labels.size());
    f.graph.vertex_labels = data_labels;
    f.histories.assign(data_labels.size(), std::vector<Bits>(data_labels.size(), 0));
    for (auto [u, v, mask] : edges) f.histories[static_cast<std::size_t>(u)][static_cast<std::size_t>(v)] = mask;
    finishGraph(f.graph, f.histories, f.horizon);
    return f;
}

QueryDecomposition fixedDecomposition(const Fixture& f, const std::vector<int>& parent,
                                     const std::vector<int>& order) {
    QueryDecomposition qd;
    qd.root = order.front(); qd.connected = true; qd.parent = parent; qd.dfs_order = order;
    qd.vertex_labels = f.query.vertex_labels;
    qd.temporal_selectivity.assign(parent.size(), 1.0);
    qd.spanning_tree_adj.resize(parent.size());
    for (std::size_t q = 0; q < parent.size(); ++q) if (parent[q] >= 0) {
        // QueryDecomposition stores directed parent->children, not an
        // undirected tree adjacency (TD bottom-up/top-down pruning relies on it).
        qd.spanning_tree_adj[static_cast<std::size_t>(parent[q])].push_back(static_cast<int>(q));
    }
    for (auto [a, b] : f.arcs) if (parent[static_cast<std::size_t>(a)] != b && parent[static_cast<std::size_t>(b)] != a) {
        qd.non_tree_edges.emplace_back(a, b);
    }
    return qd;
}

void unitGateAdaptive(const fs::path& out) {
    using D = AdaptiveDiagnostics;
    require(D::ceilLog2(0) == 0 && D::ceilLog2(1) == 0 && D::ceilLog2(2) == 1 && D::ceilLog2(3) == 2,
            "A1 logarithm zero/singleton boundaries");
    for (unsigned power = 2; power < 64; ++power) {
        const auto exact = std::uint64_t{1} << power;
        require(D::ceilLog2(exact - 1) == power && D::ceilLog2(exact) == power && D::ceilLog2(exact + 1) == power + 1,
                "A1 logarithm power-of-two boundary");
    }
    require(D::ceilLog2(std::numeric_limits<std::uint64_t>::max()) == 64, "A1 largest ceilLog2");
    const Bits full = fullBits(8);
    // q1/q2 each have one injective candidate. Only q2 narrows the support.
    const Fixture star = fixedFixture("unit_singleton_width_tie", {{0, 1}, {0, 2}},
        {0, 1, 2}, {0, 1, 2, 4, 4, 4, 4, 4}, {{0, 1, full}, {0, 2, Bits{0x1c}}});
    const auto qd = fixedDecomposition(star, {-1, 0, 0}, {0, 1, 2});
    const TDTree tree(star.graph, star.query, qd, star.k);
    const auto plan = makeQueryPlan(star.query, qd, tree, OrderMode::Paper);
    SearchStatisticsCollector statistics(false);
    DirectEvaluator evaluator(star.graph, star.query, plan, tree, star.k, statistics);
    SearchState state;
    state.mapping = {0, -1, -1};
    OrderSimulator stat(star.graph, star.query, plan, tree, star.k, OrderSimulationMode::Static);
    OrderSimulator dyn(star.graph, star.query, plan, tree, star.k, OrderSimulationMode::DynMinCand);
    OrderSimulator dynj(star.graph, star.query, plan, tree, star.k, OrderSimulationMode::DynMinCandJ);
    require(stat.select(1, state, evaluator).query_vertex == 1, "static plan-rank selection");
    require(dyn.select(1, state, evaluator).query_vertex == 1, "equal-size candidate tie must use plan rank");
    const auto choice = dynj.select(1, state, evaluator);
    require(choice.query_vertex == 2 && *choice.candidates == std::vector<int>{2}, "min-J singleton support-width tie-break");
    require(dynj.snapshot().ordering_temporal_evaluations == 2, "min-J must score exactly the two tied injective candidates");
    require(stat.snapshot().ordering_candidate_evaluations == 0 && stat.snapshot().ordering_temporal_evaluations == 0,
            "static must have zero ordering-evaluation work");
    state.active_intervals = {{6, 8}}; state.has_active_intervals = true;
    require(dynj.select(1, state, evaluator).query_vertex == 2, "empty cumulative support contributes zero to min-J score");
    state.active_intervals = {{3, 5}};
    require(dynj.select(1, state, evaluator).query_vertex == 1, "equal cumulative widths must use plan rank");

    // q3 is adjacent to mapped q0, but its fixed TD parent q2 is UNMAPPED.
    const Fixture cycle = fixedFixture("unit_unmapped_parent", {{0, 1}, {1, 2}, {2, 3}, {3, 0}},
        {0, 1, 2, 3}, {0, 1, 2, 3, 1, 2, 4, 4},
        {{0, 1, full}, {0, 4, full}, {1, 2, full}, {4, 5, full}, {2, 3, full}, {5, 3, full}, {3, 0, full}});
    const auto cycle_qd = fixedDecomposition(cycle, {-1, 0, 1, 2}, {0, 1, 2, 3});
    const TDTree cycle_tree(cycle.graph, cycle.query, cycle_qd, cycle.k);
    const auto cycle_plan = makeQueryPlan(cycle.query, cycle_qd, cycle_tree, OrderMode::Paper);
    DirectEvaluator cycle_evaluator(cycle.graph, cycle.query, cycle_plan, cycle_tree, cycle.k, statistics);
    OrderSimulator cycle_sim(cycle.graph, cycle.query, cycle_plan, cycle_tree, cycle.k, OrderSimulationMode::DynMinCand);
    SearchState cycle_state;
    cycle_state.mapping = {0, -1, -1, -1};
    const auto frontier_choice = cycle_sim.select(1, cycle_state, cycle_evaluator);
    require(frontier_choice.query_vertex == 3 && *frontier_choice.candidates == std::vector<int>{3},
            "unmapped fixed parent must use TD-domain union, not an empty -1 parent block");
    require(cycle_sim.snapshot().union_domain_seeds > 0, "unmapped-parent unit did not cover domain fallback");
    cycle_state.mapping = {0, 1, -1, 3};
    const auto reverse_choice = cycle_sim.select(3, cycle_state, cycle_evaluator);
    require(reverse_choice.query_vertex == 2 && *reverse_choice.candidates == std::vector<int>{2}, "mapped TD child reverse relation");
    require(cycle_sim.snapshot().closed_td_child_checks > 0, "reverse TD relation was not checked");

    const Fixture empty = fixedFixture("unit_empty_frontier_candidates", {{0, 1}, {1, 2}, {2, 3}, {3, 0}},
        {0, 1, 2, 3}, {0, 1, 2, 3, 1, 2, 3, 4},
        {{0, 1, full}, {1, 2, full}, {2, 3, full}, {3, 0, full},
         {0, 4, full}, {4, 5, full}, {5, 6, full}, {6, 0, full}});
    const auto empty_qd = fixedDecomposition(empty, {-1, 0, 1, 2}, {0, 1, 2, 3});
    const TDTree empty_tree(empty.graph, empty.query, empty_qd, empty.k);
    const auto empty_plan = makeQueryPlan(empty.query, empty_qd, empty_tree, OrderMode::Paper);
    DirectEvaluator empty_evaluator(empty.graph, empty.query, empty_plan, empty_tree, empty.k, statistics);
    OrderSimulator empty_sim(empty.graph, empty.query, empty_plan, empty_tree, empty.k, OrderSimulationMode::DynMinCandJ);
    SearchState empty_state;
    empty_state.mapping = {0, 1, -1, 6};
    const auto empty_choice = empty_sim.select(3, empty_state, empty_evaluator);
    require(empty_choice.query_vertex == 2 && empty_choice.candidates->empty(), "zero candidate vertex must terminate its branch");
    require(empty_sim.snapshot().ordering_temporal_evaluations == 0, "empty candidates require no temporal tie evaluation");
    writeText(out / "stage_a_unit_receipt.json",
        "{\"status\":\"PASS\",\"ceil_log2_boundaries\":true,\"singleton\":true,\"empty\":true,\"equal_E_plan_rank\":true,\"min_j_inclusive_cardinality\":true,\"min_j_empty_support_zero\":true,\"min_j_final_rank_tie\":true,\"unmapped_parent_domain\":true,\"closed_child_relation\":true,\"static_zero_ordering_work\":true}\n");
}

void differentialGateAdaptive(const fs::path& out) {
    std::mt19937_64 rng(kGraphSeed);
    std::map<std::pair<int, std::string>, Stratum> strata;
    std::ofstream log(out / "stage_a_differential_cases.csv");
    log << "k,shape,instance,vertices,edges,horizon,planted,answers,direct_extensions,static_extensions,dynamic_extensions,dynamic_j_extensions,direct_candidates,static_candidates,dynamic_order_evaluations,dynamic_j_order_evaluations,all_exact_sets_pass\n";
    std::uint64_t comparisons = 0;
    for (int k : {2, 3, 4}) for (const auto& shape : kAdaptiveShapes) {
        for (int instance = 0; instance < kInstancesPerStratum; ++instance) {
            const Fixture f = makeAdaptiveFixture(rng, k, shape, instance);
            Answers expected;
            try {
                expected = oracle(f);
                require(!f.planted || !expected.empty(), "planted independent-oracle witness missing");
                const auto qd = decomposition(f);
                require(qd.connected, "random connected shape decomposition failed");
                const TDTree tree(f.graph, f.query, qd, k);
                const auto plan = makeQueryPlan(f.query, qd, tree, OrderMode::Paper);
                CandidateDagCompiler compiler;
                const auto base = compiler.compile(f.query, plan, qd);
                const auto all_off = optimizeCandidateDag(base, f.graph, f.query, plan, tree, {});
                require(dagIdentity(base) == dagIdentity(all_off), "all-off DAG IR changed");
                const auto direct = adaptiveNativeRun(f, plan, tree, nullptr);
                compareAdaptiveAnswers(out, f, "direct_all_diagnostics_off", expected, direct); ++comparisons;
                const auto direct_counts = adaptiveNativeRun(f, plan, tree, nullptr, OrderSimulationMode::Disabled, 1, true);
                compareAdaptiveAnswers(out, f, "direct_legacy_counters_only", expected, direct_counts); ++comparisons;
                const auto dag = adaptiveNativeRun(f, plan, tree, &all_off);
                compareAdaptiveAnswers(out, f, "dag_p0_all_off", expected, dag); ++comparisons;
                const auto stat = adaptiveNativeRun(f, plan, tree, nullptr, OrderSimulationMode::Static);
                compareAdaptiveAnswers(out, f, "order_sim_static", expected, stat); ++comparisons;
                require(stat.extensions == direct_counts.extensions && stat.candidates == direct_counts.candidates,
                        "static simulation changed historical logical_extensions or candidates");
                const auto dyn = adaptiveNativeRun(f, plan, tree, nullptr, OrderSimulationMode::DynMinCand);
                compareAdaptiveAnswers(out, f, "order_sim_dyn_min_cand", expected, dyn); ++comparisons;
                const auto dynj = adaptiveNativeRun(f, plan, tree, nullptr, OrderSimulationMode::DynMinCandJ);
                compareAdaptiveAnswers(out, f, "order_sim_dyn_min_cand_j", expected, dynj); ++comparisons;
                require(direct.summary.result_digest == dag.summary.result_digest &&
                        direct.summary.result_digest == stat.summary.result_digest &&
                        direct.summary.result_digest == dyn.summary.result_digest &&
                        direct.summary.result_digest == dynj.summary.result_digest,
                        "mapping digest disagrees despite equal exact answer sets");
                require(stat.summary.order_simulation.decisions == 0 && stat.summary.order_simulation.ordering_candidate_evaluations == 0,
                        "static unexpectedly records dynamic ordering work");
                auto& counts = strata[{k, shape}];
                ++counts.instances;
                counts.positive += !expected.empty(); counts.zero += expected.empty();
                counts.planted += f.planted; counts.answer_total += expected.size();
                log << k << ',' << shape << ',' << instance << ',' << f.graph.num_vertices << ',' << f.graph.temporal_edges.size()
                    << ',' << f.horizon << ',' << f.planted << ',' << expected.size() << ',' << direct_counts.extensions
                    << ',' << stat.extensions << ',' << dyn.extensions << ',' << dynj.extensions << ',' << direct_counts.candidates
                    << ',' << stat.candidates << ',' << dyn.summary.order_simulation.ordering_candidate_evaluations
                    << ',' << dynj.summary.order_simulation.ordering_candidate_evaluations << ",true\n";
                log.flush();
            } catch (const std::exception& error) {
                if (!fs::exists(out / "counterexample.json")) counterexample(out, f, "exception_or_counter_invariant", error.what(), expected, {});
                throw;
            }
        }
        std::cout << "STAGE A DIFFERENTIAL PASS k=" << k << " shape=" << shape << " instances=" << kInstancesPerStratum << std::endl;
    }
    std::ostringstream receipt;
    receipt << "{\"status\":\"PASS\",\"seed\":" << kGraphSeed << ",\"instances\":4200,\"native_exact_set_comparisons\":" << comparisons
            << ",\"static_counter_checks\":4200,\"all_off_ir_checks\":4200,\"strata\":[";
    bool comma = false;
    for (const auto& entry : strata) {
        const auto& s = entry.second;
        require(s.instances == 200 && s.positive > 0 && s.zero > 0, "insufficient positive/zero stratum coverage");
        if (comma) receipt << ',';
        comma = true;
        receipt << "{\"k\":" << entry.first.first << ",\"shape\":" << quote(entry.first.second)
                << ",\"instances\":" << s.instances << ",\"positive\":" << s.positive << ",\"zero\":" << s.zero
                << ",\"planted\":" << s.planted << ",\"answer_total\":" << s.answer_total << '}';
    }
    receipt << "]}\n";
    writeText(out / "stage_a_differential_receipt.json", receipt.str());
}

void parallelGateAdaptive(const fs::path& out) {
    std::vector<std::string> shapes(kAdaptiveShapes.begin(), kAdaptiveShapes.end());
    shapes.push_back("reciprocal_selfloop");
    std::ofstream log(out / "stage_a_parallel_checks.csv");
    log << "shape,arm,requested_threads,threads_used,callback_threads,answers,exact_sets_equal,count_equal,mapping_digest_equal,ordering_counters_equal\n";
    std::uint64_t checks = 0, max_callback_threads = 0;
    for (const auto& shape : shapes) {
        const auto arcs = shape == "reciprocal_selfloop"
            ? std::vector<std::pair<int, int>>{{0, 0}, {0, 1}, {1, 0}, {1, 2}, {2, 1}}
            : adaptiveArcs(shape);
        int qn = 0;
        for (auto [a, b] : arcs) qn = std::max(qn, std::max(a, b) + 1);
        std::vector<std::tuple<int, int, Bits>> edges;
        for (int a = 0; a < 8; ++a) for (int b = 0; b < 8; ++b) {
            if (a != b || shape == "reciprocal_selfloop") edges.emplace_back(a, b, Bits{0xe7});
        }
        const Fixture f = fixedFixture("parallel_" + shape, arcs, std::vector<Label>(static_cast<std::size_t>(qn), 0), std::vector<Label>(8, 0), edges);
        Answers expected;
        try {
            expected = oracle(f);
            require(!expected.empty(), "parallel fixed witness is empty");
            const auto qd = decomposition(f);
            const TDTree tree(f.graph, f.query, qd, f.k);
            const auto plan = makeQueryPlan(f.query, qd, tree, OrderMode::Paper);
            CandidateDagCompiler compiler;
            const auto dag = compiler.compile(f.query, plan, qd);
            for (int arm = 0; arm < 5; ++arm) {
                const auto mode = arm < 2 ? OrderSimulationMode::Disabled : arm == 2 ? OrderSimulationMode::Static
                    : arm == 3 ? OrderSimulationMode::DynMinCand : OrderSimulationMode::DynMinCandJ;
                const std::string name = arm == 0 ? "direct" : arm == 1 ? "dag_p0" : orderSimulationModeName(mode);
                const CandidateDag* selected_dag = arm == 1 ? &dag : nullptr;
                const auto serial = adaptiveNativeRun(f, plan, tree, selected_dag, mode, 1);
                compareAdaptiveAnswers(out, f, name + "_1t", expected, serial);
                const auto parallel = adaptiveNativeRun(f, plan, tree, selected_dag, mode, 16);
                compareAdaptiveAnswers(out, f, name + "_16t", expected, parallel);
                require(serial.summary.match_count == parallel.summary.match_count && serial.summary.result_digest == parallel.summary.result_digest,
                        "parallel count/mapping digest differs from serial");
                require(parallel.summary.threads_used > 1, "parallel observer fixture used only a serial path");
                if (mode != OrderSimulationMode::Disabled) {
                    std::ostringstream a, b;
                    serial.summary.order_simulation.writeJson(a); parallel.summary.order_simulation.writeJson(b);
                    require(a.str() == b.str(), "order simulation counters depend on worker scheduling");
                }
                max_callback_threads = std::max<std::uint64_t>(max_callback_threads, parallel.callback_threads);
                log << shape << ',' << name << ",16," << parallel.summary.threads_used << ',' << parallel.callback_threads
                    << ',' << expected.size() << ",true,true,true,true\n";
                ++checks;
            }
            // New A1 state/timer diagnostics must be observational only. They
            // run on fixed tiny inputs, never as performance measurements.
            for (bool dag_backend : {false, true}) {
                AdaptiveDiagnostics diagnostic(true, false);
                const auto observed = adaptiveNativeRun(f, plan, tree, dag_backend ? &dag : nullptr,
                    OrderSimulationMode::Disabled, 1, true, &diagnostic);
                compareAdaptiveAnswers(out, f, dag_backend ? "dag_a1_state_diagnostics" : "direct_a1_state_diagnostics", expected, observed);
                std::ostringstream evidence;
                diagnostic.writeJson(evidence);
                require(evidence.str().find("\"funnel_complete\":true") != std::string::npos, "A1 funnel is incomplete on a fixed exact-answer fixture");
                const auto totals_start = evidence.str().find("\"totals\":{");
                const auto extensions_start = evidence.str().find("\"extensions\":", totals_start);
                require(totals_start != std::string::npos && extensions_start != std::string::npos,
                        "A1 totals/extension field missing");
                const auto extension_value = std::stoull(evidence.str().substr(extensions_start + std::string("\"extensions\":").size()));
                require(extension_value == observed.extensions,
                        "A1 funnel extension count differs from actual historical operations");
                writeText(out / ("diagnostics_" + shape + (dag_backend ? "_dag.json" : "_direct.json")), evidence.str() + "\n");
                if (shape == kAdaptiveShapes.front()) {
                    AdaptiveDiagnostics timer_only(false, true);
                    const auto timed = adaptiveNativeRun(f, plan, tree, dag_backend ? &dag : nullptr,
                        OrderSimulationMode::Disabled, 1, false, &timer_only);
                    compareAdaptiveAnswers(out, f, dag_backend ? "dag_timer_only" : "direct_timer_only", expected, timed);
                    std::ostringstream timer_evidence;
                    timer_only.writeJson(timer_evidence);
                    require(timer_evidence.str().find("\"state_profile\":null") != std::string::npos,
                            "copy-loop timer silently activated state counters");
                    writeText(out / (dag_backend ? "timer_only_dag.json" : "timer_only_direct.json"), timer_evidence.str() + "\n");
                }
            }
        } catch (const std::exception& error) {
            if (!fs::exists(out / "counterexample.json")) counterexample(out, f, "parallel_or_diagnostic_invariant", error.what(), expected, {});
            throw;
        }
    }
    std::ostringstream receipt;
    receipt << "{\"status\":\"PASS\",\"fixed_fixtures\":8,\"serial_parallel_pairs\":" << checks
            << ",\"requested_parallel_threads\":16,\"workers_capped_by_root_count\":true,\"maximum_callback_threads_observed\":" << max_callback_threads
            << ",\"count_only_exact_observer\":true,\"mapping_and_active_interval_sets_equal\":true,\"mapping_digest_equal\":true,\"a1_diagnostic_exact_set_checks\":16,\"timer_only_exact_set_checks\":2}\n";
    writeText(out / "stage_a_parallel_receipt.json", receipt.str());
}

} // namespace

#ifndef DSM_ADAPTIVE_STAGE_A_LIBRARY
int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "Usage: test_adaptive_stage_a NEW_OUTPUT_DIRECTORY\n"; return 2; }
    const fs::path out(argv[1]);
    if (fs::exists(out)) { std::cerr << "Refusing existing correctness directory: " << out << '\n'; return 2; }
    fs::create_directories(out);
    try {
        unitGateAdaptive(out);
        differentialGateAdaptive(out);
        parallelGateAdaptive(out);
        writeText(out / "stage_a_native_receipt.json",
            "{\"status\":\"PASS\",\"random_graph_instances\":4200,\"seed\":2026101902,\"independent_answer_set_oracle\":true,\"active_intervals_compared\":true,\"all_off_ir_unchanged\":true,\"static_logical_extensions_unchanged\":true,\"static_candidates_unchanged\":true,\"parallel_count_digest_equal\":true,\"parallel_exact_interval_sets_equal\":true,\"parallel_pairs\":40,\"fixed_parallel_fixtures\":8,\"unit_receipt\":\"stage_a_unit_receipt.json\",\"differential_receipt\":\"stage_a_differential_receipt.json\",\"parallel_receipt\":\"stage_a_parallel_receipt.json\",\"failures\":0}\n");
        std::cout << "STAGE A CORRECTNESS PASS: 4200 independent full-set fixtures, static counters, parallel exact observers\n";
        return 0;
    } catch (const std::exception& error) {
        const std::string receipt = "{\"status\":\"STOPPED\",\"reason\":" + quote(error.what()) + ",\"no_further_test_instances\":true}\n";
        if (!fs::exists(out / "counterexample.json")) writeText(out / "counterexample.json", receipt);
        writeText(out / "STOP.json", receipt);
        writeText(out / "stage_a_native_receipt.json", receipt);
        std::cerr << "STOP: " << error.what() << '\n';
        return 1;
    }
}
#endif
