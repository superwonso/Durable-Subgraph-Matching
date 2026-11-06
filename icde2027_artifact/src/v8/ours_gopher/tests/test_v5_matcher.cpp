// V5 correctness only: no timing evidence, no adaptive fixture/configuration
// selection, and immediate STOP on the first mismatch. The independent oracle
// below uses one byte per discrete timestamp (not the native interval/W code).
#include "../CandidateDagPasses.h"
#include "../GopherMatcher.h"
#include "../OperatorCounters.h"
#include "../TDTree.h"
#include "../Utils.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t kSeed = 20261020ULL;
constexpr int kPerStratum = 200;
const std::array<std::string, 7> kShapes{
    "path2", "path3", "cycle3", "cycle4", "star3", "diamond4", "tailed_cycle5"};
const std::array<std::pair<int, int>, 4> kHorizons{
    std::pair<int, int>{1, 10}, {60, 70}, {120, 140}, {200, 200}};
using Times = std::vector<unsigned char>;

void require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

std::string quote(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        else if (c == '\n') out << "\\n";
        else if (c == '\r') out << "\\r";
        else if (c == '\t') out << "\\t";
        else if (c < 32)
            out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                << static_cast<unsigned>(c) << std::dec;
        else out << static_cast<char>(c);
    }
    out << '"';
    return out.str();
}

void writeNew(const fs::path& path, const std::string& value) {
    require(!fs::exists(path), "refusing to overwrite " + path.string());
    std::ofstream stream(path);
    require(static_cast<bool>(stream), "cannot create " + path.string());
    stream << value;
    stream.flush();
    require(static_cast<bool>(stream), "cannot finish " + path.string());
}

struct Answer {
    std::vector<int> mapping;
    std::vector<std::pair<int, int>> active;
    bool operator<(const Answer& other) const {
        return std::tie(mapping, active) < std::tie(other.mapping, other.active);
    }
    bool operator==(const Answer& other) const {
        return mapping == other.mapping && active == other.active;
    }
};
using Answers = std::set<Answer>;

struct Fixture {
    Graph graph, query;
    int k = 3, horizon = 0, origin = 0, stratum = -1, instance = -1;
    std::string shape;
    bool planted = false, force_root_zero = false;
    std::vector<std::pair<int, int>> arcs;
    std::vector<std::vector<Times>> histories;
};

std::vector<std::pair<int, int>> arcsFor(const std::string& shape) {
    if (shape == "path2") return {{0, 1}, {1, 2}};
    if (shape == "path3") return {{0, 1}, {1, 2}, {2, 3}};
    if (shape == "cycle3") return {{0, 1}, {1, 2}, {2, 0}};
    if (shape == "cycle4") return {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
    if (shape == "star3") return {{0, 1}, {0, 2}, {0, 3}};
    if (shape == "diamond4") return {{0, 1}, {0, 2}, {1, 2}, {1, 3}, {2, 3}};
    if (shape == "tailed_cycle5") return {{0, 1}, {1, 2}, {2, 0}, {2, 3}, {3, 4}};
    if (shape == "reciprocal_selfloop")
        return {{0, 0}, {0, 1}, {1, 0}, {1, 2}, {2, 1}, {2, 2}};
    throw std::runtime_error("unknown fixed shape");
}

// Independent oracle interval construction; no native normalization helper.
std::vector<std::pair<int, int>> independentIntervals(const Times& times, int origin, int k) {
    std::vector<std::pair<int, int>> result;
    for (std::size_t begin = 0; begin < times.size();) {
        if (!times[begin]) { ++begin; continue; }
        std::size_t end = begin;
        while (end + 1 < times.size() && times[end + 1]) ++end;
        if (end - begin + 1 >= static_cast<std::size_t>(k))
            result.emplace_back(origin + static_cast<int>(begin), origin + static_cast<int>(end));
        begin = end + 1;
    }
    return result;
}

bool independentHasRun(const Times& times, int k) {
    int length = 0;
    for (unsigned char value : times) {
        length = value ? length + 1 : 0;
        if (length >= k) return true;
    }
    return false;
}

void initializeQuery(Fixture& fixture, const std::vector<Label>& labels) {
    Graph& query = fixture.query;
    query.num_vertices = static_cast<int>(labels.size());
    query.vertex_labels = labels;
    query.adj.assign(labels.size(), {});
    query.in_adj.assign(labels.size(), {});
    query.external_ids.resize(labels.size());
    std::iota(query.external_ids.begin(), query.external_ids.end(), 0);
    for (const auto [from, to] : fixture.arcs) {
        query.adj[from].push_back({to, -1});
        query.in_adj[to].push_back({from, -1});
    }
    for (auto* adjacency : {&query.adj, &query.in_adj})
        for (auto& edges : *adjacency)
            std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.to < b.to; });
}

void initializeHistories(Fixture& fixture, int vertices) {
    fixture.graph.num_vertices = vertices;
    fixture.histories.assign(vertices,
        std::vector<Times>(vertices, Times(static_cast<std::size_t>(fixture.horizon), 0)));
}

void finishGraph(Fixture& fixture) {
    Graph& graph = fixture.graph;
    const int vertices = graph.num_vertices;
    graph.adj.assign(vertices, {});
    graph.in_adj.assign(vertices, {});
    graph.external_ids.resize(vertices);
    graph.vertex_active_durations.assign(vertices, 0);
    graph.neighbor_label_counts.assign(vertices, {});
    graph.incoming_neighbor_label_counts.assign(vertices, {});
    graph.temporal_edges.clear();
    graph.input_occurrence_count = 0;
    std::vector<Times> active(vertices, Times(fixture.horizon, 0));
    for (int from = 0; from < vertices; ++from) {
        graph.external_ids[from] = 1000 + 7 * from;
        for (int to = 0; to < vertices; ++to) {
            const auto& times = fixture.histories[from][to];
            const auto pairs = independentIntervals(times, fixture.origin, 1);
            if (pairs.empty()) continue;
            std::vector<TimeInterval> intervals;
            std::int64_t occurrences = 0;
            for (const auto [begin, end] : pairs) {
                intervals.push_back({begin, end});
                occurrences += static_cast<std::int64_t>(end) - begin + 1;
            }
            const int edge_id = static_cast<int>(graph.temporal_edges.size());
            graph.temporal_edges.emplace_back(from, to, intervals, occurrences);
            graph.adj[from].push_back({to, edge_id});
            graph.in_adj[to].push_back({from, edge_id});
            ++graph.neighbor_label_counts[from][graph.vertex_labels[to]];
            ++graph.incoming_neighbor_label_counts[to][graph.vertex_labels[from]];
            for (int t = 0; t < fixture.horizon; ++t) {
                active[from][t] |= times[t];
                active[to][t] |= times[t];
            }
            graph.input_occurrence_count += static_cast<std::size_t>(occurrences);
        }
    }
    for (int vertex = 0; vertex < vertices; ++vertex) {
        graph.vertex_active_durations[vertex] =
            std::accumulate(active[vertex].begin(), active[vertex].end(), std::int64_t{0});
        for (auto* adjacency : {&graph.adj, &graph.in_adj})
            std::sort((*adjacency)[vertex].begin(), (*adjacency)[vertex].end(),
                [](const Edge& a, const Edge& b) { return a.to < b.to; });
    }
    graph.input_unique_edge_count = graph.temporal_edges.size();
    graph.filtered_edge_count = graph.temporal_edges.size();
}

Times randomTimes(std::mt19937_64& random, int horizon, int mode) {
    Times result(horizon, 0);
    if (mode == 5) {
        const int begin = static_cast<int>(random() % static_cast<std::uint64_t>(horizon));
        const int end = begin + static_cast<int>(random() % static_cast<std::uint64_t>(horizon - begin));
        for (int t = begin; t <= end; ++t) result[t] = 1;
    } else {
        for (int t = 0; t < horizon; ++t) {
            switch (mode) {
                case 0: result[t] = 1; break;
                case 1: result[t] = random() % 2 == 0; break;
                case 2: result[t] = random() % 8 == 0; break;
                case 3: result[t] = random() % 8 != 0; break;
                case 4: result[t] = t % 2 == 0; break;
                case 6: result[t] = (t % 11 < 4); break;
                default: result[t] = (t % 2 == 1 && random() % 2 == 0); break;
            }
        }
    }
    if (std::none_of(result.begin(), result.end(), [](unsigned char value) { return value != 0; }))
        result[random() % static_cast<std::uint64_t>(horizon)] = 1;
    return result;
}

Fixture randomFixture(std::mt19937_64& random, int k, const std::string& shape,
                      int stratum, int instance) {
    Fixture f;
    f.k = k; f.shape = shape; f.stratum = stratum; f.instance = instance;
    const auto [minimum, maximum] = kHorizons[stratum];
    // Fixed balanced coverage of all horizon sizes, with random graph content.
    f.horizon = minimum + instance % (maximum - minimum + 1);
    const std::array<int, 5> origins{-129, -1, 0, 1, 1024};
    f.origin = origins[instance % origins.size()];
    f.arcs = arcsFor(shape);
    int query_vertices = 0;
    for (auto [from, to] : f.arcs) query_vertices = std::max(query_vertices, std::max(from, to) + 1);
    const int vertices = 8 + static_cast<int>(random() % 5);
    initializeHistories(f, vertices);
    const bool homogeneous = instance % 4 == 0;
    f.graph.vertex_labels.resize(vertices);
    for (auto& label : f.graph.vertex_labels)
        label = homogeneous ? Label{0} : static_cast<Label>(random() % 3);
    std::vector<Label> query_labels(query_vertices);
    for (int q = 0; q < query_vertices; ++q)
        query_labels[q] = homogeneous ? Label{0} : static_cast<Label>(q % 3);
    initializeQuery(f, query_labels);
    std::vector<std::pair<int, int>> available;
    for (int u = 0; u < vertices; ++u)
        for (int v = 0; v < vertices; ++v) if (u != v) available.emplace_back(u, v);
    std::shuffle(available.begin(), available.end(), random);
    const std::size_t target_edges = 20 + static_cast<std::size_t>(random() % 21);
    std::set<std::pair<int, int>> selected;
    const bool deliberately_zero = instance % 3 == 2;
    f.planted = !deliberately_zero && f.horizon >= k;
    int planted_begin = 0;
    if (f.planted) {
        planted_begin = static_cast<int>(random() % static_cast<std::uint64_t>(f.horizon - k + 1));
        for (int q = 0; q < query_vertices; ++q) f.graph.vertex_labels[q] = query_labels[q];
        selected.insert(f.arcs.begin(), f.arcs.end());
    }
    for (const auto edge : available) {
        if (selected.size() == target_edges) break;
        selected.insert(edge);
    }
    std::size_t edge_number = 0;
    for (const auto [from, to] : selected) {
        Times times(f.horizon, 0);
        if (deliberately_zero) {
            // Only one edge can have a durable history; every fixed query
            // requires >=2 distinct edges. Thus these are guaranteed zeros.
            // Its full history also retains the requested domain boundaries.
            if (edge_number == 0) std::fill(times.begin(), times.end(), 1);
            else times[random() % static_cast<std::uint64_t>(f.horizon)] = 1;
        } else {
            times = randomTimes(random, f.horizon, static_cast<int>(random() % 8));
            if (f.planted && std::find(f.arcs.begin(), f.arcs.end(), std::pair<int, int>{from, to}) != f.arcs.end())
                for (int t = planted_begin; t < planted_begin + k; ++t) times[t] = 1;
        }
        f.histories[from][to] = std::move(times);
        ++edge_number;
    }
    finishGraph(f);
    require(f.graph.temporal_edges.size() == target_edges, "random edge bounds/count mismatch");
    return f;
}

Answers independentOracle(const Fixture& fixture) {
    Answers result;
    std::vector<int> mapping(fixture.query.num_vertices, -1);
    std::vector<bool> used(fixture.graph.num_vertices, false);
    std::function<void(int, const Times&)> visit = [&](int query_vertex, const Times& common) {
        if (query_vertex == fixture.query.num_vertices) {
            const auto active = independentIntervals(common, fixture.origin, fixture.k);
            if (active.empty()) return;
            Answer answer;
            for (int vertex : mapping) answer.mapping.push_back(fixture.graph.externalId(vertex));
            answer.active = active;
            result.insert(std::move(answer));
            return;
        }
        for (int vertex = 0; vertex < fixture.graph.num_vertices; ++vertex) {
            if (used[vertex] || fixture.graph.vertex_labels[vertex] != fixture.query.vertex_labels[query_vertex])
                continue;
            mapping[query_vertex] = vertex;
            Times next = common;
            for (const auto [from, to] : fixture.arcs) {
                if (std::max(from, to) != query_vertex) continue;
                const Times& edge = fixture.histories[mapping[from]][mapping[to]];
                for (int t = 0; t < fixture.horizon; ++t) next[t] &= edge[t];
            }
            if (independentHasRun(next, fixture.k)) {
                used[vertex] = true;
                visit(query_vertex + 1, next);
                used[vertex] = false;
            }
            mapping[query_vertex] = -1;
        }
    };
    visit(0, Times(fixture.horizon, 1));
    return result;
}

QueryDecomposition decompositionFor(const Fixture& fixture) {
    if (fixture.force_root_zero) {
        QueryDecomposition qd;
        qd.root = 0; qd.connected = true; qd.parent = {-1, 0, 1}; qd.dfs_order = {0, 1, 2};
        qd.vertex_labels = fixture.query.vertex_labels;
        qd.temporal_selectivity.assign(3, 1.0);
        qd.spanning_tree_adj = {{1}, {2}, {}};
        for (const auto [from, to] : fixture.arcs)
            if (qd.parent[from] != to && qd.parent[to] != from) qd.non_tree_edges.emplace_back(from, to);
        return qd;
    }
    std::array<std::size_t, kLabelCount> counts{};
    std::array<double, kLabelCount> means{};
    for (int vertex = 0; vertex < fixture.graph.num_vertices; ++vertex) {
        const auto label = fixture.graph.vertex_labels[vertex];
        ++counts[label];
        means[label] += fixture.graph.vertex_active_durations[vertex];
    }
    for (std::size_t label = 0; label < counts.size(); ++label)
        if (counts[label]) means[label] /= static_cast<double>(counts[label]);
    return decomposeQuery(fixture.query, counts, means, SelectivityMode::Paper);
}

struct Configuration {
    std::string name;
    int physical_mask = -1; // -1 means Direct, not a DAG.
    bool direct_p4 = false, window = false, lazy = false;
};

std::vector<Configuration> configurations() {
    std::vector<Configuration> result{
        {"direct", -1, false, false, false},
        {"direct_p4", -1, true, false, false},
        {"direct_p4_w_eager", -1, true, true, false},
        {"direct_p4_w_lazy", -1, true, true, true}};
    for (const auto [mask, name] :
         std::array<std::pair<int, const char*>, 5>{{{0, "dag_p0"}, {1, "dag_p4"},
             {3, "dag_p4p5"}, {5, "dag_p4p6"}, {7, "dag_s1"}}}) {
        result.push_back({name, mask, false, false, false});
        if (mask & 1) {
            result.push_back({std::string(name) + "_w_eager", mask, false, true, false});
            result.push_back({std::string(name) + "_w_lazy", mask, false, true, true});
        }
    }
    require(result.size() == 17, "fixed configuration coverage differs");
    return result;
}

DagPasses passes(int mask) {
    DagPasses value;
    value.p4 = mask & 1; value.p5 = mask & 2; value.p6 = mask & 4;
    return value;
}

std::string dagIdentity(const CandidateDag& dag) {
    std::ostringstream out;
    out << std::setprecision(17) << dag.nodes_attempted << '/' << dag.nodes_merged << '/'
        << dag.nodes_eliminated_by_passes << '/' << dag.cumulative_support
        << dag.passes.p1 << dag.passes.p2 << dag.passes.p3
        << dag.passes.p4 << dag.passes.p5 << dag.passes.p6;
    for (auto count : dag.op_node_attempt_counts) out << ':' << count;
    for (const auto* roots : {&dag.structural_roots, &dag.temporal_roots, &dag.longest_run_roots}) {
        out << " roots"; for (auto root : *roots) out << ':' << root;
    }
    for (const auto& node : dag.nodes) {
        out << " node:" << node.id << ':' << static_cast<int>(node.op) << ':' << static_cast<int>(node.result_kind)
            << ':' << node.parameter.first_query_vertex << ':' << node.parameter.second_query_vertex
            << ':' << node.canonical_expression << ':' << node.static_fingerprint << ':' << node.depth
            << ':' << node.raw_intervals << ':' << node.short_circuit << ':' << node.adaptive_intersection
            << ':' << node.estimated_cardinality << ':' << static_cast<int>(node.structural_policy)
            << ':' << node.physical_alpha << ':' << node.bound_producer << ':' << node.bound_slot
            << ':' << node.lmax_proven << " parents";
        for (auto parent : node.parents) out << ':' << parent;
        out << " deps"; for (auto dependency : node.dependencies) out << ':' << dependency;
        out << " slots";
        for (const auto slot : node.history_slots) out << ':' << slot.from_query_vertex << '>' << slot.to_query_vertex;
    }
    return out.str();
}

struct Observation {
    const Fixture* fixture = nullptr;
    std::mutex mutex;
    Answers answers;
    std::set<std::thread::id> threads;
    bool duplicate = false, invalid = false;
};

void observe(const SearchState& state, void* opaque) {
    auto& observation = *static_cast<Observation*>(opaque);
    const Fixture& fixture = *observation.fixture;
    Answer answer;
    bool invalid = state.mapping.size() != static_cast<std::size_t>(fixture.query.num_vertices)
        || !state.has_active_intervals || state.active_intervals.empty();
    for (int vertex : state.mapping) {
        invalid = invalid || vertex < 0 || vertex >= fixture.graph.num_vertices;
        answer.mapping.push_back(vertex < 0 || vertex >= fixture.graph.num_vertices
            ? -1 : fixture.graph.externalId(vertex));
    }
    for (const auto& interval : state.active_intervals) {
        invalid = invalid || interval.end < interval.start
            || static_cast<std::int64_t>(interval.end) - interval.start + 1 < fixture.k;
        if (!answer.active.empty())
            invalid = invalid || static_cast<std::int64_t>(interval.start) <=
                static_cast<std::int64_t>(answer.active.back().second) + 1;
        // Do not sort, normalize, merge, trim or reconstruct native intervals.
        answer.active.emplace_back(interval.start, interval.end);
    }
    std::lock_guard<std::mutex> lock(observation.mutex);
    observation.duplicate = !observation.answers.insert(std::move(answer)).second || observation.duplicate;
    observation.invalid = observation.invalid || invalid;
    observation.threads.insert(std::this_thread::get_id());
}

struct NativeResult {
    Answers answers;
    GopherMatchSummary summary;
    std::map<std::string, std::uint64_t> ops;
    std::size_t callback_threads = 0;
    bool duplicate = false, invalid = false, counted = true;
};

void nativeRun(const Fixture& fixture, const QueryPlan& plan, const TDTree& tree,
               const CandidateDag* dag, const Configuration& config, std::size_t threads,
               bool counted, NativeResult& result) {
    result = NativeResult{};
    result.counted = counted;
    Observation observation;
    observation.fixture = &fixture;
    OperatorCounters counters;
    GopherMatcherOptions options;
    options.threads = threads;
    options.parallel_mode = ParallelMode::Roots;
    options.early_prune = true; options.result_digest = true;
    options.window_algebra = config.window;
    options.window_lazy = config.lazy;
    options.direct_p4 = config.direct_p4;
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
#define SAVE_OP(name) result.ops[#name] = counters.ops_##name.load(std::memory_order_relaxed);
        DSM_OPS_FIELDS(SAVE_OP)
#undef SAVE_OP
    };
    try {
        // Each constructor creates its own production WindowStore. Its eager
        // build worker policy is intentionally unchanged, including 1T search.
        if (dag) {
            GopherMatcher matcher(fixture.graph, fixture.query, plan, *dag, tree, fixture.k, options);
            result.summary = matcher.run(MatchOutputMode::CountOnly);
        } else {
            GopherMatcher matcher(fixture.graph, fixture.query, plan, tree, fixture.k, options);
            result.summary = matcher.run(MatchOutputMode::CountOnly);
        }
    } catch (...) {
        capture(); // Retain any observed prefix if native execution throws.
        throw;
    }
    capture();
}

void writeAnswer(std::ostream& out, const Answer& answer) {
    out << "{\"mapping\":[";
    for (std::size_t i = 0; i < answer.mapping.size(); ++i) { if (i) out << ','; out << answer.mapping[i]; }
    out << "],\"active\":[";
    for (std::size_t i = 0; i < answer.active.size(); ++i) {
        if (i) out << ',';
        out << '[' << answer.active[i].first << ',' << answer.active[i].second << ']';
    }
    out << "]}";
}

void writeAnswers(const fs::path& path, const Answers& answers) {
    require(!fs::exists(path), "refusing answer evidence overwrite");
    std::ofstream stream(path);
    require(static_cast<bool>(stream), "cannot create answer evidence");
    for (const auto& answer : answers) { writeAnswer(stream, answer); stream << '\n'; }
    stream.flush();
    require(static_cast<bool>(stream), "cannot finish answer evidence");
}

void counterexample(const fs::path& out, const Fixture& fixture, const std::string& arm,
                    const std::string& reason, const Answers& expected, const NativeResult& actual) {
    if (fs::exists(out / "counterexample.json")) return;
    std::ostringstream detail;
    detail << "{\"status\":\"STOPPED\",\"seed\":" << kSeed << ",\"arm\":" << quote(arm)
           << ",\"reason\":" << quote(reason) << ",\"shape\":" << quote(fixture.shape)
           << ",\"stratum\":" << fixture.stratum << ",\"instance\":" << fixture.instance
           << ",\"k\":" << fixture.k << ",\"horizon\":" << fixture.horizon
           << ",\"origin\":" << fixture.origin << ",\"planted\":" << (fixture.planted ? "true" : "false")
           << ",\"force_root_zero\":" << (fixture.force_root_zero ? "true" : "false")
           << ",\"graph_labels\":[";
    for (std::size_t i = 0; i < fixture.graph.vertex_labels.size(); ++i) {
        if (i) detail << ','; detail << static_cast<unsigned>(fixture.graph.vertex_labels[i]);
    }
    detail << "],\"query_labels\":[";
    for (std::size_t i = 0; i < fixture.query.vertex_labels.size(); ++i) {
        if (i) detail << ','; detail << static_cast<unsigned>(fixture.query.vertex_labels[i]);
    }
    detail << "],\"query_arcs\":[";
    for (std::size_t i = 0; i < fixture.arcs.size(); ++i) {
        if (i) detail << ','; detail << '[' << fixture.arcs[i].first << ',' << fixture.arcs[i].second << ']';
    }
    detail << "],\"graph_histories\":[";
    bool comma = false;
    for (int from = 0; from < fixture.graph.num_vertices; ++from)
        for (int to = 0; to < fixture.graph.num_vertices; ++to) {
            const auto history = independentIntervals(fixture.histories[from][to], fixture.origin, 1);
            if (history.empty()) continue;
            if (comma) detail << ','; comma = true;
            detail << "{\"from\":" << from << ",\"to\":" << to << ",\"intervals\":[";
            for (std::size_t i = 0; i < history.size(); ++i) {
                if (i) detail << ','; detail << '[' << history[i].first << ',' << history[i].second << ']';
            }
            detail << "]}";
        }
    detail << "],\"expected_answers\":" << expected.size() << ",\"actual_answers\":" << actual.answers.size()
           << ",\"native_count\":" << actual.summary.match_count
           << ",\"native_digest\":" << quote(actual.summary.result_digest) << ",\"ops\":{";
    comma = false;
    for (const auto& [key, value] : actual.ops) { if (comma) detail << ','; comma = true; detail << quote(key) << ':' << value; }
    detail << "},\"answer_files\":[\"expected_answers.jsonl\",\"actual_answers.jsonl\"],\"no_further_instances\":true}\n";
    writeNew(out / "counterexample.json", detail.str());
    writeAnswers(out / "expected_answers.jsonl", expected);
    writeAnswers(out / "actual_answers.jsonl", actual.answers);
}

void checkResult(const Answers& expected, const NativeResult& actual, const Configuration& config) {
    require(!actual.duplicate && !actual.invalid, "duplicate mapping or invalid/un-normalized native intervals");
    require(!actual.summary.count_overflow, "native answer count overflow");
    require(actual.summary.match_count == actual.answers.size(), "native count disagrees with exact observer");
    require(actual.answers == expected, "mapping AND unmodified active interval set differs from independent oracle");
    if (config.window) {
        require(actual.summary.v5_window_built_edges <= actual.summary.v5_window_relevant_edges,
                "W store built more edges than its declared query domain");
        if (!config.lazy)
            require(actual.summary.v5_window_built_edges == actual.summary.v5_window_relevant_edges,
                    "eager W store did not build its complete fixed domain");
    } else {
        require(actual.summary.v5_window_relevant_edges == 0 &&
                actual.summary.v5_window_built_edges == 0, "W-off configuration created a W store");
    }
}

std::map<int, CandidateDag> compileDags(const Fixture& fixture, const QueryPlan& plan,
                                      const TDTree& tree, const QueryDecomposition& decomposition) {
    CandidateDagCompiler compiler;
    const auto base = compiler.compile(fixture.query, plan, decomposition);
    std::map<int, CandidateDag> result;
    for (int mask : {0, 1, 3, 5, 7})
        result.emplace(mask, optimizeCandidateDag(base, fixture.graph, fixture.query, plan, tree, passes(mask)));
    require(dagIdentity(base) == dagIdentity(result.at(0)), "new-feature-OFF no-op rewrite changed P0 IR");
    return result;
}

void checkSameWork(const NativeResult& direct_p4, const NativeResult& dag_p4) {
    require(direct_p4.ops.at("logical_extensions") == dag_p4.ops.at("logical_extensions"),
            "DirectP4 and DAG P4 logical extensions differ");
    require(direct_p4.ops.at("candidates") == dag_p4.ops.at("candidates"),
            "DirectP4 and DAG P4 candidate counts differ");
}

void preserveWorkFailure(const fs::path& out, const NativeResult& reference) {
    std::ostringstream json;
    json << "{\"reference\":\"direct_p4\",\"ops\":{";
    bool comma = false;
    for (const auto& [key, value] : reference.ops) {
        if (comma) json << ','; comma = true;
        json << quote(key) << ':' << value;
    }
    json << "}}\n";
    writeNew(out / "direct_p4_reference_ops.json", json.str());
}

void writeWorkRow(std::ostream& log, const Fixture& fixture, const Configuration& config,
                  const NativeResult& actual) {
    log << fixture.k << ',' << fixture.shape << ',' << fixture.stratum << ',' << fixture.instance
        << ',' << config.name << ',' << actual.answers.size();
    for (const auto* name : {"logical_extensions", "candidates", "physical_structural_probes",
                            "time_intersections_executed", "kappa_executed",
                            "physical_history_handle_reads", "window_candidate_checks"})
        log << ',' << actual.ops.at(name);
    log << '\n';
}

void differential(const fs::path& out) {
    const auto configs = configurations();
    std::mt19937_64 random(kSeed);
    std::uint64_t instances = 0, comparisons = 0, work_checks = 0;
    std::ofstream log(out / "v5_differential_cases.csv");
    std::ofstream operations(out / "v5_differential_ops.csv");
    log << "k,shape,stratum,instance,vertices,edges,horizon,origin,planted,answers,configurations,exact_sets_equal\n";
    operations << "k,shape,stratum,instance,configuration,answers,logical_extensions,candidates,structural_probes,"
                  "time_intersections,kappa,handle_reads,window_candidate_checks\n";
    std::ostringstream strata;
    bool stratum_comma = false;
    for (int k : {2, 3, 4}) for (const auto& shape : kShapes) for (int stratum = 0; stratum < 4; ++stratum) {
        std::uint64_t positive = 0, zero = 0, planted = 0;
        for (int instance = 0; instance < kPerStratum; ++instance) {
            const Fixture fixture = randomFixture(random, k, shape, stratum, instance);
            Answers expected;
            NativeResult current;
            std::string arm = "oracle_or_plan";
            try {
                expected = independentOracle(fixture);
                require(!fixture.planted || !expected.empty(), "independent planted witness missing");
                if (instance % 3 == 2) require(expected.empty(), "fixed zero fixture unexpectedly has an answer");
                const auto decomposition = decompositionFor(fixture);
                const TDTree tree(fixture.graph, fixture.query, decomposition, fixture.k);
                const auto plan = makeQueryPlan(fixture.query, decomposition, tree, OrderMode::Paper);
                const auto dags = compileDags(fixture, plan, tree, decomposition);
                std::map<std::string, NativeResult> control;
                std::uint64_t direct_extensions = 0;
                std::string direct_digest;
                for (const auto& config : configs) {
                    arm = config.name;
                    const auto* dag = config.physical_mask < 0 ? nullptr : &dags.at(config.physical_mask);
                    const std::string before = dag ? dagIdentity(*dag) : "";
                    nativeRun(fixture, plan, tree, dag, config, 1, true, current);
                    checkResult(expected, current, config);
                    if (config.name == "direct") {
                        direct_extensions = current.ops.at("logical_extensions");
                        direct_digest = current.summary.result_digest;
                    }
                    require(current.ops.at("logical_extensions") == direct_extensions,
                            "fixed-plan accepted logical extensions differ from legacy Direct");
                    require(current.summary.result_digest == direct_digest, "mapping digest differs for equal exact answers");
                    require(!dag || before == dagIdentity(*dag), "matcher changed its input Candidate-DAG");
                    writeWorkRow(operations, fixture, config, current);
                    if (config.name == "direct_p4" || config.name == "dag_p4")
                        control.emplace(config.name, std::move(current));
                    ++comparisons;
                }
                try {
                    checkSameWork(control.at("direct_p4"), control.at("dag_p4"));
                } catch (...) {
                    arm = "direct_p4_vs_dag_p4";
                    preserveWorkFailure(out, control.at("direct_p4"));
                    current = std::move(control.at("dag_p4"));
                    throw;
                }
                ++work_checks; ++instances;
                positive += !expected.empty(); zero += expected.empty(); planted += fixture.planted;
                log << k << ',' << shape << ',' << stratum << ',' << instance << ',' << fixture.graph.num_vertices
                    << ',' << fixture.graph.temporal_edges.size() << ',' << fixture.horizon << ',' << fixture.origin
                    << ',' << (fixture.planted ? 1 : 0) << ',' << expected.size() << ",17,true\n";
                log.flush(); operations.flush();
                require(static_cast<bool>(log) && static_cast<bool>(operations), "differential log write failed");
            } catch (const std::exception& error) {
                counterexample(out, fixture, arm, error.what(), expected, current);
                throw;
            }
        }
        require(positive > 0 && zero > 0, "missing positive/negative stratum coverage");
        if (stratum_comma) strata << ','; stratum_comma = true;
        strata << "{\"k\":" << k << ",\"shape\":" << quote(shape) << ",\"stratum\":" << stratum
               << ",\"instances\":200,\"positive\":" << positive << ",\"zero\":" << zero << ",\"planted\":" << planted << '}';
        std::cout << "V5 DIFFERENTIAL PASS k=" << k << " shape=" << shape
                  << " stratum=" << stratum << " instances=200" << std::endl;
    }
    require(instances == 16800 && comparisons == 285600 && work_checks == 16800,
            "mandatory v5 random differential coverage incomplete");
    std::ostringstream receipt;
    receipt << "{\"status\":\"PASS\",\"seed\":" << kSeed
            << ",\"random_graphs\":16800,\"configurations_per_graph\":17,\"native_exact_set_comparisons\":285600,"
               "\"direct_p4_vs_dag_p4_work_checks\":16800,\"oracle\":\"independent discrete-time byte vector\","
               "\"native_intervals_not_normalized_by_test\":true,\"legacy_off_scope\":\"preserved paths in new binary, not independent old executable\","
               "\"strata\":[" << strata.str() << "]}\n";
    writeNew(out / "v5_differential_receipt.json", receipt.str());
}

Fixture loopFixture(const std::string& name, int horizon) {
    Fixture fixture;
    fixture.shape = name; fixture.horizon = horizon; fixture.origin = -65;
    fixture.k = 3; fixture.force_root_zero = true;
    fixture.arcs = {{0, 1}, {1, 2}};
    if (name.find("root_loop") != std::string::npos && name.find("nonroot") == std::string::npos)
        fixture.arcs.emplace_back(0, 0);
    if (name.find("nonroot_loop") != std::string::npos)
        fixture.arcs.emplace_back(1, 1);
    if (name == "positive_both_loops") {
        fixture.arcs.emplace_back(0, 0);
        fixture.arcs.emplace_back(1, 1);
    }
    initializeQuery(fixture, {0, 1, 2});
    initializeHistories(fixture, 8);
    fixture.graph.vertex_labels = {0, 1, 2, 3, 3, 3, 3, 3};
    auto fill = [&](int from, int to, int begin, int end) {
        for (int t = begin; t <= end; ++t) fixture.histories[from][to][t] = 1;
    };
    if (name == "disjoint_cumulative_support") {
        fill(0, 1, 0, 2);
        fill(1, 2, horizon - 3, horizon - 1);
    } else {
        fill(0, 1, 0, horizon - 1);
        fill(1, 2, 0, horizon - 1);
        if (name == "short_root_loop") fill(0, 0, 0, 1);
        if (name == "short_nonroot_loop") fill(1, 1, 0, 1);
        if (name == "positive_both_loops") {
            fill(0, 0, 0, horizon - 1);
            fill(1, 1, 0, 2);
            fill(1, 1, horizon - 3, horizon - 1);
        }
    }
    finishGraph(fixture);
    return fixture;
}

void boundaryAndLoopUnits(const fs::path& out) {
    const auto configs = configurations();
    const std::array<std::string, 6> names{
        "disjoint_cumulative_support", "missing_root_loop", "short_root_loop",
        "missing_nonroot_loop", "short_nonroot_loop", "positive_both_loops"};
    std::uint64_t checks = 0;
    std::ofstream log(out / "v5_boundary_loop_checks.csv");
    log << "fixture,horizon,configuration,answers,expected_empty,exact_sets_equal\n";
    for (const auto& name : names) for (int horizon : {8, 65, 130, 200}) {
        const Fixture fixture = loopFixture(name, horizon);
        Answers expected;
        NativeResult current;
        std::string arm = "fixed_oracle";
        try {
            expected = independentOracle(fixture);
            const bool negative = name != "positive_both_loops";
            require(expected.empty() == negative, "fixed root/nonroot loop witness has wrong truth");
            const auto decomposition = decompositionFor(fixture);
            const TDTree tree(fixture.graph, fixture.query, decomposition, fixture.k);
            const auto plan = makeQueryPlan(fixture.query, decomposition, tree, OrderMode::Paper);
            require(plan.root == 0, "fixed root-self-loop fixture changed root");
            const auto dags = compileDags(fixture, plan, tree, decomposition);
            std::uint64_t direct_extensions = 0;
            std::string direct_digest;
            std::map<std::string, NativeResult> controls;
            for (const auto& config : configs) {
                arm = config.name;
                const auto* dag = config.physical_mask < 0 ? nullptr : &dags.at(config.physical_mask);
                nativeRun(fixture, plan, tree, dag, config, 1, true, current);
                checkResult(expected, current, config);
                if (config.name == "direct") {
                    direct_extensions = current.ops.at("logical_extensions");
                    direct_digest = current.summary.result_digest;
                }
                require(current.ops.at("logical_extensions") == direct_extensions &&
                        current.summary.result_digest == direct_digest, "loop fixture work/digest differs");
                log << name << ',' << horizon << ',' << config.name << ',' << expected.size()
                    << ',' << (negative ? "true" : "false") << ",true\n";
                if (config.name == "direct_p4" || config.name == "dag_p4")
                    controls.emplace(config.name, std::move(current));
                ++checks;
            }
            try {
                checkSameWork(controls.at("direct_p4"), controls.at("dag_p4"));
            } catch (...) {
                arm = "direct_p4_vs_dag_p4";
                preserveWorkFailure(out, controls.at("direct_p4"));
                current = std::move(controls.at("dag_p4"));
                throw;
            }
            log.flush();
        } catch (const std::exception& error) {
            counterexample(out, fixture, arm, error.what(), expected, current);
            throw;
        }
    }
    require(checks == 408, "fixed boundary/self-loop coverage incomplete");
    writeNew(out / "v5_boundary_loop_receipt.json",
        "{\"status\":\"PASS\",\"fixtures\":24,\"negative_fixtures\":20,\"positive_fixtures\":4,"
        "\"native_exact_set_comparisons\":408,\"fixed_root_zero\":true,"
        "\"root_nonroot_missing_short_loops\":true,\"disjoint_cumulative_support\":true}\n");
}

Fixture parallelFixture(const std::string& shape, int horizon) {
    Fixture fixture;
    fixture.shape = "parallel_" + shape;
    fixture.horizon = horizon; fixture.origin = -65; fixture.k = 3;
    fixture.arcs = arcsFor(shape);
    int query_vertices = 0;
    for (auto [from, to] : fixture.arcs) query_vertices = std::max(query_vertices, std::max(from, to) + 1);
    initializeQuery(fixture, std::vector<Label>(query_vertices, 0));
    initializeHistories(fixture, 8);
    fixture.graph.vertex_labels.assign(8, 0);
    for (int from = 0; from < 8; ++from) for (int to = 0; to < 8; ++to) {
        if (from == to && shape != "reciprocal_selfloop") continue;
        auto& times = fixture.histories[from][to];
        for (int t = 0; t <= 3; ++t) times[t] = 1;
        const int variant = (from * 8 + to) % 4;
        const std::array<std::pair<int, int>, 3> intervals{{{60, 67}, {123, 132}, {horizon - 5, horizon - 1}}};
        if (variant) {
            const auto [begin, end] = intervals[variant - 1];
            for (int t = std::max(0, begin); t <= std::min(horizon - 1, end); ++t) times[t] = 1;
        }
    }
    finishGraph(fixture);
    return fixture;
}

void parallel(const fs::path& out) {
    const auto configs = configurations();
    std::vector<std::string> shapes(kShapes.begin(), kShapes.end());
    shapes.push_back("reciprocal_selfloop");
    std::uint64_t pairs = 0, ops_off = 0, max_callbacks = 0;
    std::ofstream log(out / "v5_parallel_checks.csv");
    log << "shape,horizon,configuration,answers,requested_threads,threads_used,callback_threads,exact_sets_equal,digest_equal,extensions_equal\n";
    for (const auto& shape : shapes) for (int horizon : {8, 65, 130, 200}) {
        const Fixture fixture = parallelFixture(shape, horizon);
        Answers expected;
        NativeResult current;
        std::string arm = "parallel_oracle";
        try {
            expected = independentOracle(fixture);
            require(!expected.empty(), "parallel fixed fixture lacks its planted support");
            const auto decomposition = decompositionFor(fixture);
            const TDTree tree(fixture.graph, fixture.query, decomposition, fixture.k);
            const auto plan = makeQueryPlan(fixture.query, decomposition, tree, OrderMode::Paper);
            const auto dags = compileDags(fixture, plan, tree, decomposition);
            for (const auto& config : configs) {
                const auto* dag = config.physical_mask < 0 ? nullptr : &dags.at(config.physical_mask);
                arm = config.name + "_1t";
                nativeRun(fixture, plan, tree, dag, config, 1, true, current);
                checkResult(expected, current, config);
                const auto serial_digest = current.summary.result_digest;
                const auto serial_extensions = current.ops.at("logical_extensions");
                const auto serial_candidates = current.ops.at("candidates");
                arm = config.name + "_16t";
                nativeRun(fixture, plan, tree, dag, config, 16, true, current);
                checkResult(expected, current, config);
                require(current.summary.result_digest == serial_digest
                        && current.ops.at("logical_extensions") == serial_extensions
                        && current.ops.at("candidates") == serial_candidates,
                        "1T/16T digest or fixed-plan work differs");
                require(current.summary.threads_used > 1, "parallel fixture silently used one worker");
                max_callbacks = std::max<std::uint64_t>(max_callbacks, current.callback_threads);
                ++pairs;
                log << shape << ',' << horizon << ',' << config.name << ',' << expected.size()
                    << ",16," << current.summary.threads_used << ',' << current.callback_threads << ",true,true,true\n";
                log.flush();
                if (horizon == 8) {
                    arm = config.name + "_ops_off";
                    nativeRun(fixture, plan, tree, dag, config, 1, false, current);
                    checkResult(expected, current, config);
                    require(current.summary.result_digest == serial_digest, "ops OFF changed exact mapping digest");
                    ++ops_off;
                }
            }
        } catch (const std::exception& error) {
            counterexample(out, fixture, arm, error.what(), expected, current);
            throw;
        }
    }
    require(pairs == 544 && ops_off == 136 && max_callbacks > 1, "parallel coverage incomplete");
    std::ostringstream receipt;
    receipt << "{\"status\":\"PASS\",\"fixtures\":32,\"serial_parallel_pairs\":544,\"ops_off_checks\":136,"
               "\"requested_threads\":16,\"actual_workers_capped_by_roots\":true,"
               "\"max_callback_threads_observed\":" << max_callbacks
            << ",\"full_mapping_and_active_interval_sets_equal\":true,\"mapping_digest_equal\":true}\n";
    writeNew(out / "v5_parallel_receipt.json", receipt.str());
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: test_v5_matcher NEW_OUTPUT_DIRECTORY\n";
        return 2;
    }
    const fs::path out(argv[1]);
    if (fs::exists(out)) {
        std::cerr << "Refusing existing correctness output " << out << '\n';
        return 2;
    }
    fs::create_directories(out);
    try {
        boundaryAndLoopUnits(out);
        differential(out);
        parallel(out);
        writeNew(out / "v5_matcher_receipt.json",
            "{\"status\":\"PASS\",\"seed\":20261020,\"random_graphs\":16800,"
            "\"configurations\":17,\"random_exact_set_comparisons\":285600,"
            "\"boundary_loop_fixtures\":24,\"boundary_loop_comparisons\":408,"
            "\"parallel_pairs\":544,\"ops_off_checks\":136,"
            "\"independent_mapping_and_active_interval_oracle\":true,"
            "\"P8_tested\":false,\"P9_tested\":false,\"performance_runs\":0,"
            "\"old_executable_parity_requires_separate_evidence\":true,\"failures\":0}\n");
        std::cout << "V5 MATCHER CORRECTNESS PASS" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        const std::string receipt = "{\"status\":\"STOPPED\",\"reason\":" + quote(error.what())
            + ",\"no_further_test_instances\":true,\"performance_runs\":0}\n";
        if (!fs::exists(out / "counterexample.json")) writeNew(out / "counterexample.json", receipt);
        writeNew(out / "STOP.json", receipt);
        writeNew(out / "v5_matcher_receipt.json", receipt);
        std::cerr << "STOP V5 CORRECTNESS: " << error.what() << '\n';
        return 1;
    }
}
