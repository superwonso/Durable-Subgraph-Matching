// Fixed diagnostic correctness fixtures, not a timing experiment.
// Large adjacency lists exercise real indexing rather than N_min fallback.
#include "../CandidateDagPasses.h"
#include "../GopherMatcher.h"
#include "../JIndexSimulation.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Mask = std::vector<std::uint8_t>;
using Answers = std::set<std::string>;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
std::string quoted(const std::string& input) {
    std::string result = "\"";
    for (char c : input) {
        if (c == '\\' || c == '"') result += '\\';
        if (c == '\n') result += "\\n"; else result += c;
    }
    return result + '"';
}
void writeNew(const fs::path& file, const std::string& text) {
    require(!fs::exists(file), "refusing existing test evidence file");
    std::ofstream stream(file);
    require(static_cast<bool>(stream), "cannot create correctness evidence");
    stream << text;
    require(static_cast<bool>(stream), "cannot write correctness evidence");
}

struct Fixture {
    int T = 0, origin = 0, k = 3;
    Graph graph, query;
    std::vector<std::pair<int, int>> arcs{{0, 1}, {1, 2}, {2, 1}, {2, 2}};
    std::map<std::pair<int, int>, Mask> histories;
};

std::vector<TimeInterval> intervals(const Mask& mask, int origin, int minimum_length) {
    std::vector<TimeInterval> result;
    for (std::size_t i = 0; i < mask.size();) {
        if (!mask[i]) { ++i; continue; }
        const auto first = i;
        while (i < mask.size() && mask[i]) ++i;
        if (i - first >= static_cast<std::size_t>(minimum_length))
            result.push_back({origin + static_cast<int>(first), origin + static_cast<int>(i) - 1});
    }
    return result;
}
Mask run(int T, int start, int length) {
    require(start >= 0 && length > 0 && start + length <= T, "invalid fixed history range");
    Mask mask(static_cast<std::size_t>(T), 0);
    for (int t = start; t < start + length; ++t) mask[static_cast<std::size_t>(t)] = 1;
    return mask;
}
Fixture fixture(int T) {
    Fixture f;
    f.T = T;
    f.origin = T == 33 || T == 129 ? -17 : 9;  // signed origin and nonzero offset
    f.graph.num_vertices = 96;
    f.graph.vertex_labels.assign(96, Label{3});
    for (int i = 0; i < 8; ++i) f.graph.vertex_labels[i] = Label{0};
    for (int i = 8; i < 12; ++i) f.graph.vertex_labels[i] = Label{1};
    for (int i = 12; i < 20; ++i) f.graph.vertex_labels[i] = Label{2};
    const Mask full(static_cast<std::size_t>(T), 1);
    std::array<Mask, 8> candidate_histories;
    candidate_histories[0] = full;
    candidate_histories[1] = run(T, 0, 3);
    candidate_histories[2] = run(T, T - 3, 3);
    candidate_histories[3] = run(T, T / 2, 3);
    candidate_histories[4] = candidate_histories[1];
    for (int t = T - 3; t < T; ++t) candidate_histories[4][static_cast<std::size_t>(t)] = 1;
    candidate_histories[5] = run(T, std::min(T - 3, 31), 3);
    candidate_histories[6] = run(T, std::min(T - 3, 63), 3);
    candidate_histories[7] = run(T, std::min(T - 3, 127), 3);
    for (int root = 0; root < 8; ++root)
        f.histories[{root, 8 + root % 4}] = run(T, root % 2 ? T / 2 : 0, 3);
    for (int anchor = 8; anchor < 12; ++anchor) {
        for (int candidate = 12; candidate < 20; ++candidate) {
            f.histories[{anchor, candidate}] = full;
            f.histories[{candidate, anchor}] = candidate_histories[static_cast<std::size_t>(candidate - 12)];
        }
        // Wrong-label neighbors do not enlarge the answer space, but MUST be
        // included in CSR construction/c_idx and make both degrees >= 64.
        for (int padding = 20; padding < 96; ++padding) {
            f.histories[{padding, anchor}] = full;
            f.histories[{anchor, padding}] = full;
        }
    }
    for (int candidate = 12; candidate < 20; ++candidate)
        f.histories[{candidate, candidate}] = full;

    auto& g = f.graph;
    g.adj.resize(96); g.in_adj.resize(96); g.external_ids.resize(96);
    std::iota(g.external_ids.begin(), g.external_ids.end(), 0);
    g.vertex_active_durations.assign(96, 0);
    g.neighbor_label_counts.resize(96); g.incoming_neighbor_label_counts.resize(96);
    std::vector<Mask> vertex_masks(96, Mask(static_cast<std::size_t>(T), 0));
    for (const auto& entry : f.histories) {
        const int from = entry.first.first, to = entry.first.second;
        const auto& mask = entry.second;
        const int id = static_cast<int>(g.temporal_edges.size());
        std::int64_t occurrences = 0;
        for (int t = 0; t < T; ++t) {
            if (mask[static_cast<std::size_t>(t)]) ++occurrences;
            vertex_masks[from][static_cast<std::size_t>(t)] |= mask[static_cast<std::size_t>(t)];
            vertex_masks[to][static_cast<std::size_t>(t)] |= mask[static_cast<std::size_t>(t)];
        }
        g.temporal_edges.emplace_back(from, to, intervals(mask, f.origin, 1), occurrences);
        g.adj[from].push_back({to, id}); g.in_adj[to].push_back({from, id});
        ++g.neighbor_label_counts[from][g.vertex_labels[to]];
        ++g.incoming_neighbor_label_counts[to][g.vertex_labels[from]];
        g.input_occurrence_count += static_cast<std::size_t>(occurrences);
    }
    for (std::size_t v = 0; v < 96; ++v) {
        for (auto bit : vertex_masks[v]) g.vertex_active_durations[v] += bit != 0;
        const auto by_to = [](const Edge& a, const Edge& b) { return a.to < b.to; };
        std::sort(g.adj[v].begin(), g.adj[v].end(), by_to);
        std::sort(g.in_adj[v].begin(), g.in_adj[v].end(), by_to);
    }
    g.input_unique_edge_count = g.filtered_edge_count = g.temporal_edges.size();
    for (int anchor = 8; anchor < 12; ++anchor)
        require(g.in_adj[anchor].size() >= 64 && g.adj[anchor].size() >= 64,
                "fixture did not create the real N_min indexing path");
    f.query.num_vertices = 3;
    f.query.vertex_labels = {Label{0}, Label{1}, Label{2}};
    f.query.external_ids = {0, 1, 2};
    f.query.adj.resize(3); f.query.in_adj.resize(3);
    for (auto arc : f.arcs) {
        f.query.adj[arc.first].push_back({arc.second, -1});
        f.query.in_adj[arc.second].push_back({arc.first, -1});
    }
    for (auto* adjacency : {&f.query.adj, &f.query.in_adj})
        for (auto& list : *adjacency)
            std::sort(list.begin(), list.end(), [](const Edge& a, const Edge& b) { return a.to < b.to; });
    return f;
}

QueryDecomposition fixedPlanInput(const Fixture& f) {
    QueryDecomposition result;
    result.root = 0; result.connected = true;
    result.parent = {-1, 0, 1}; result.dfs_order = {0, 1, 2};
    result.vertex_labels = f.query.vertex_labels;
    result.temporal_selectivity.assign(3, 1.0);
    result.spanning_tree_adj = {{1}, {2}, {}};
    result.non_tree_edges = {{2, 2}};
    return result;
}
std::string key(const std::vector<int>& mapping, const std::vector<TimeInterval>& support) {
    std::ostringstream out;
    for (int vertex : mapping) out << vertex << ',';
    out << '|';
    for (const auto& interval : support) out << '[' << interval.start << ',' << interval.end << ']';
    return out.str();
}
Answers oracle(const Fixture& f) {
    Answers result;
    // Complete label-compatible injective enumeration, independently using
    // a T-element boolean array rather than native interval/Window kernels.
    for (int root = 0; root < 8; ++root)
        for (int anchor = 8; anchor < 12; ++anchor)
            for (int candidate = 12; candidate < 20; ++candidate) {
                const std::vector<int> mapping{root, anchor, candidate};
                Mask active(static_cast<std::size_t>(f.T), 1);
                for (auto arc : f.arcs) {
                    const auto found = f.histories.find({mapping[arc.first], mapping[arc.second]});
                    if (found == f.histories.end()) { std::fill(active.begin(), active.end(), 0); break; }
                    for (int t = 0; t < f.T; ++t)
                        active[static_cast<std::size_t>(t)] &= found->second[static_cast<std::size_t>(t)];
                }
                const auto support = intervals(active, f.origin, f.k);
                if (!support.empty()) result.insert(key(mapping, support));
            }
    return result;
}

struct Observation {
    std::mutex mutex;
    Answers answers;
    std::set<std::thread::id> threads;
    bool duplicate = false, invalid = false;
    int k = 3;
};
void exactObserver(const SearchState& state, void* opaque) {
    auto& observed = *static_cast<Observation*>(opaque);
    bool invalid = !state.has_active_intervals || state.active_intervals.empty();
    for (std::size_t i = 0; i < state.active_intervals.size(); ++i) {
        const auto& run = state.active_intervals[i];
        invalid = invalid || run.end < run.start || run.length() < observed.k;
        if (i) invalid = invalid ||
            static_cast<std::int64_t>(run.start) <= static_cast<std::int64_t>(state.active_intervals[i - 1].end) + 1;
    }
    std::lock_guard<std::mutex> lock(observed.mutex);
    observed.invalid = observed.invalid || invalid;
    observed.duplicate = !observed.answers.insert(key(state.mapping, state.active_intervals)).second || observed.duplicate;
    observed.threads.insert(std::this_thread::get_id());
}
struct Run {
    Answers answers;
    std::string digest, ops, index;
    std::uint64_t count = 0, extensions = 0, temporal_rejections = 0;
    std::size_t threads_used = 0, callback_threads = 0;
};
Run native(const Fixture& f, const QueryPlan& plan, const CandidateDag& dag,
           const TDTree& tree, std::size_t threads, bool shadow) {
    Observation observed;
    observed.k = f.k;
    OperatorCounters ops;
    std::unique_ptr<JIndexSimulation> simulation;
    if (shadow) simulation = std::make_unique<JIndexSimulation>(f.graph, f.query, plan, dag, tree, f.k);
    GopherMatcherOptions options;
    options.threads = threads; options.parallel_mode = ParallelMode::Roots;
    options.early_prune = true; options.result_digest = true;
    options.executor.memo_mode = MemoMode::None;
    options.executor.collect_statistics = false; options.executor.ops = &ops;
    options.jindex_simulation = simulation.get();
    options.exact_match_observer = exactObserver;
    options.exact_match_observer_context = &observed;
    GopherMatcher matcher(f.graph, f.query, plan, dag, tree, f.k, options);
    const auto summary = matcher.run(MatchOutputMode::CountOnly);
    require(!observed.duplicate && !observed.invalid, "invalid or duplicate exact native interval answer");
    require(!summary.count_overflow && summary.match_count == observed.answers.size(),
            "native count differs from exact callback set");
    Run result;
    result.answers = std::move(observed.answers);
    result.count = summary.match_count; result.digest = summary.result_digest;
    result.threads_used = summary.threads_used; result.callback_threads = observed.threads.size();
    result.extensions = ops.ops_logical_extensions.load();
    result.temporal_rejections = ops.ops_physical_temporal_rejections.load();
    std::ostringstream counter_text; ops.writeJsonMembers(counter_text, true);
    result.ops = counter_text.str();
    if (simulation) { std::ostringstream json; simulation->writeJson(json); result.index = json.str(); }
    return result;
}

std::string objectAfter(const std::string& input, const std::string& marker) {
    const auto position = input.find(marker);
    require(position != std::string::npos, "missing JSON object marker: " + marker);
    const auto begin = input.find('{', position + marker.size());
    require(begin != std::string::npos, "missing JSON object opening");
    int depth = 0;
    bool string = false, escaped = false;
    for (std::size_t i = begin; i < input.size(); ++i) {
        const char c = input[i];
        if (string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') string = false;
        } else {
            if (c == '"') string = true;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) return input.substr(begin, i - begin + 1);
        }
    }
    throw std::runtime_error("unterminated JSON object");
}
std::uint64_t number(const std::string& object, const std::string& name) {
    const auto marker = '"' + name + "\":";
    const auto begin = object.find(marker);
    require(begin != std::string::npos, "missing JSON integer: " + name);
    std::size_t at = begin + marker.size();
    const auto end = object.find_first_not_of("0123456789", at);
    require(end != at, "JSON value is not an unsigned integer: " + name);
    return std::stoull(object.substr(at, end - at));
}
std::string blockObject(const std::string& json, std::size_t B) {
    const auto marker = "{\"B\":" + std::to_string(B) + ',';
    const auto start = json.find(marker);
    require(start != std::string::npos, "missing B model");
    return objectAfter("MARKER" + json.substr(start), "MARKER");
}
void checkIndex(const Fixture& f, const Run& result) {
    require(result.index.find("\"thread_totals_equal_node_sums\":true") != std::string::npos,
            "local counter/node sum consistency assertion absent");
    require(number(result.index, "window_start_count") == static_cast<std::uint64_t>(f.T - f.k + 1),
            "signed domain offset/span differs");
    for (auto B : JIndexSimulation::kBlockSizes) {
        const auto block = blockObject(result.index, B);
        require(block.find("\"status\":\"OK\"") != std::string::npos, "fixed index model unavailable");
        const auto totals = objectAfter(block, "\"totals\":");
        const auto scan = number(totals, "scan_touched");
        const auto indexed = number(totals, "idx_touched");
        const auto passed = number(totals, "passed");
        require(number(totals, "indexed_states") > 0 && number(block, "built_indexes") > 0,
                "fixture reached only N_min fallback, not real indexing");
        require(indexed <= scan && passed <= indexed && passed > 0,
                "index/scan/exact-pass cardinality relation violated");
        require(passed == result.extensions, "accepted callbacks differ from exact logical extensions");
        require(scan > passed && result.temporal_rejections > 0,
                "fixture did not exercise exact temporal rejection");
        require(number(totals, "fallback_no_j") > 0, "unconstrained initial-J fallback untested");
        require(number(block, "peak_index_plus_build_scratch_bytes") <= number(block, "cap_bytes"),
                "index plus live scratch exceeded its declared memory cap");
        if (f.T == 200) require(indexed < scan, "large-domain fixture did not remove any indexed candidate");
    }
}
void exerciseRejectingAssertion(const Fixture& f, const QueryPlan& plan,
                                const CandidateDag& dag, const TDTree& tree) {
    SearchStatisticsCollector statistics(false);
    DagExecutorOptions options;
    options.memo_mode = MemoMode::None; options.collect_statistics = false;
    DagExecutor executor(f.graph, f.query, plan, dag, tree, f.k, options, statistics);
    SearchState state;
    state.mapping = {0, 8, -1};
    state.has_active_intervals = true;
    state.active_intervals = {{f.origin, f.origin + 2}};
    auto batch = executor.physicalCandidates(2, state);
    JIndexSimulation simulation(f.graph, f.query, plan, dag, tree, f.k);
    auto token = simulation.observe(2, state, batch);
    bool exercised = false;
    for (std::size_t i = 0; i < batch.size(); ++i) {
        if (!token.available[0] || !token.scan_mask[i] || token.index_masks[0][i]) continue;
        // Deliberately lie to the test observer (NOT an actual DFS answer).
        // This proves the superset guard actively rejects a missing candidate.
        bool rejected = false;
        try { simulation.recordAccepted(token, i); }
        catch (const std::logic_error&) { rejected = true; }
        require(rejected, "superset counterexample assertion did not fire");
        exercised = true;
        break;
    }
    require(exercised, "no fixed excluded candidate available for negative assertion test");
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "Usage: test_jindex_simulation NEW_OUTPUT_DIRECTORY\n"; return 2; }
    const fs::path out(argv[1]);
    if (fs::exists(out)) { std::cerr << "Refusing existing correctness output\n"; return 2; }
    fs::create_directories(out);
    try {
        std::uint64_t comparisons = 0;
        for (int T : {33, 65, 129, 200}) {
            const auto f = fixture(T);
            const auto expected = oracle(f);
            require(!expected.empty(), "independent fixed oracle unexpectedly empty");
            const auto decomposition = fixedPlanInput(f);
            const TDTree tree(f.graph, f.query, decomposition, f.k);
            const auto plan = makeQueryPlan(f.query, decomposition, tree, OrderMode::Paper);
            require(plan.root == 0 && plan.order == std::vector<int>({0, 1, 2}), "fixed plan changed");
            CandidateDagCompiler compiler;
            const auto base = compiler.compile(f.query, plan, decomposition);
            DagPasses passes; passes.p4 = passes.p5 = passes.p6 = true;
            const auto dag = optimizeCandidateDag(base, f.graph, f.query, plan, tree, passes);
            Run serial;
            for (std::size_t threads : {std::size_t{1}, std::size_t{16}}) {
                const auto off = native(f, plan, dag, tree, threads, false);
                const auto on = native(f, plan, dag, tree, threads, true);
                const std::string prefix = "T" + std::to_string(T) + "_t" + std::to_string(threads);
                writeNew(out / (prefix + "_jindex.json"), on.index + '\n');
                writeNew(out / (prefix + "_ops_off.txt"), off.ops);
                writeNew(out / (prefix + "_ops_on.txt"), on.ops);
                require(off.answers == expected && on.answers == expected,
                        "exact mapping/active-interval set differs from independent oracle: " + prefix);
                require(off.count == on.count && off.digest == on.digest && off.ops == on.ops,
                        "shadow ON changed count, digest, or operation counters: " + prefix);
                require(!on.digest.empty(), "native digest absent");
                checkIndex(f, on);
                if (threads == 1) serial = off;
                else {
                    require(off.threads_used > 1 && on.threads_used > 1, "parallel matcher path was not exercised");
                    require(serial.answers == off.answers && serial.digest == off.digest && serial.ops == off.ops,
                            "serial/parallel count, exact intervals, digest or counters differ");
                }
                ++comparisons;
            }
            exerciseRejectingAssertion(f, plan, dag, tree);
        }
        writeNew(out / "jindex_native_receipt.json",
            "{\"status\":\"PASS\",\"fixed_graphs\":4,\"vertices_per_graph\":96,"
            "\"time_spans\":[33,65,129,200],\"signed_and_nonzero_origins\":true,"
            "\"requested_threads\":[1,16],\"shadow_on_off_pairs\":" + std::to_string(comparisons) + ","
            "\"all_four_B_really_indexed\":true,\"driver_degree_at_least_64\":true,"
            "\"independent_boolean_array_oracle\":true,\"exact_active_intervals_equal\":true,"
            "\"mapping_digest_equal\":true,\"all_operation_counters_equal\":true,"
            "\"reciprocal_and_nonroot_selfloop\":true,\"positive_and_temporally_rejected_candidates\":true,"
            "\"superset_assertion_negative_checks\":4,\"failures\":0}\n");
        std::cout << "JINDEX SIMULATION CORRECTNESS PASS\n";
        return 0;
    } catch (const std::exception& error) {
        const auto stopped = "{\"status\":\"STOPPED\",\"reason\":" + quoted(error.what()) +
            ",\"no_further_test_instances\":true}\n";
        writeNew(out / "STOP.json", stopped);
        writeNew(out / "jindex_native_receipt.json", stopped);
        std::cerr << "STOP: " << error.what() << '\n';
        return 1;
    }
}
