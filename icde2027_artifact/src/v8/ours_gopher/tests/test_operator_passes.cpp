// Fixed correctness gate, never performance evidence. Stop on the first
// discrepancy, retaining the complete graph/query and answer-set counterexample.
#include "../CandidateDagPasses.h"
#include "../GopherMatcher.h"
#include "../OperatorCounters.h"
#include "../OpKernels.h"
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
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr std::uint64_t kPropertySeed = 2026101901ULL;
constexpr std::uint64_t kGraphSeed = 2026101902ULL;
constexpr std::size_t kPropertyPairs = 1000000;
constexpr int kInstancesPerStratum = 200;
using Bits = std::uint64_t;
using Answers = std::set<std::string>;
namespace fs = std::filesystem;

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

std::string quote(const std::string& s) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 32) {
                    out << "\\u" << std::hex << std::setw(4)
                        << std::setfill('0') << static_cast<unsigned int>(c)
                        << std::dec;
                } else out << static_cast<char>(c);
        }
    }
    out << '"';
    return out.str();
}

void writeText(const fs::path& path, const std::string& text) {
    std::ofstream file(path);
    require(static_cast<bool>(file), "cannot create " + path.string());
    file << text;
    require(static_cast<bool>(file), "cannot finish " + path.string());
}

Bits fullBits(int horizon) {
    return horizon == 64 ? std::numeric_limits<Bits>::max()
                         : ((Bits{1} << horizon) - 1);
}

bool bit(Bits bits, int index) { return ((bits >> index) & Bits{1}) != 0; }

// Independent discrete-time oracle: no native interval/intersection helpers.
Bits oracleKappa(Bits bits, int horizon, int k) {
    Bits output = 0;
    for (int start = 0; start < horizon;) {
        if (!bit(bits, start)) { ++start; continue; }
        int end = start;
        while (end + 1 < horizon && bit(bits, end + 1)) ++end;
        if (end - start + 1 >= k) {
            for (int t = start; t <= end; ++t) output |= Bits{1} << t;
        }
        start = end + 1;
    }
    return output;
}

std::vector<TimeInterval> intervals(Bits bits, int horizon, int origin = 1) {
    std::vector<TimeInterval> result;
    for (int start = 0; start < horizon;) {
        if (!bit(bits, start)) { ++start; continue; }
        int end = start;
        while (end + 1 < horizon && bit(bits, end + 1)) ++end;
        result.push_back({origin + start, origin + end});
        start = end + 1;
    }
    return result;
}

bool sameIntervals(const std::vector<TimeInterval>& a,
                   const std::vector<TimeInterval>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].start != b[i].start || a[i].end != b[i].end) return false;
    }
    return true;
}

Bits randomBits(std::mt19937_64& rng, int horizon, int mode) {
    if (mode == 0) return 0;
    if (mode == 1) return fullBits(horizon);
    if (mode == 2) return rng() & fullBits(horizon);
    if (mode == 3) return rng() & rng() & rng() & fullBits(horizon);
    if (mode == 4) return (rng() | rng() | rng()) & fullBits(horizon);
    if (mode == 5) return 0x5555555555555555ULL & fullBits(horizon);
    if (mode == 6) {
        const int begin = static_cast<int>(rng() % static_cast<Bits>(horizon));
        const int length = 1 + static_cast<int>(rng() % static_cast<Bits>(horizon - begin));
        Bits result = 0;
        for (int t = begin; t < begin + length; ++t) result |= Bits{1} << t;
        return result;
    }
    return (rng() & 0xAAAAAAAAAAAAAAAAULL) & fullBits(horizon);
}

void propertyGate(const fs::path& out) {
    std::mt19937_64 rng(kPropertySeed);
    std::array<std::size_t, 65> horizons{};
    std::array<std::size_t, 81> ks{};
    std::array<std::size_t, 8> modes{};
    std::size_t beyond = 0;
    for (std::size_t i = 0; i < kPropertyPairs; ++i) {
        const int horizon = 1 + static_cast<int>(rng() % 64);
        const int k = 1 + static_cast<int>(rng() % 80);
        const int origin = -32 + static_cast<int>(rng() % 65);
        const int mode_j = static_cast<int>(i % 8);
        const int mode_x = static_cast<int>((i / 8) % 8);
        const Bits j = randomBits(rng, horizon, mode_j);
        const Bits x = randomBits(rng, horizon, mode_x);
        const auto ji = intervals(j, horizon, origin);
        const auto xi = intervals(x, horizon, origin);
        const auto inner = intersectTimeIntervals(xi, xi, k);
        const auto lhs = intersectTimeIntervals(ji, inner, k);
        const auto rhs = intersectTimeIntervals(ji, xi, k);
        const auto independent = intervals(oracleKappa(j & x, horizon, k), horizon, origin);
        if (!sameIntervals(lhs, rhs) || !sameIntervals(rhs, independent)) {
            std::ostringstream failure;
            failure << "{\"kind\":\"kappa_property\",\"seed\":" << kPropertySeed
                    << ",\"pair_index\":" << i << ",\"horizon\":" << horizon
                    << ",\"k\":" << k << ",\"origin\":" << origin
                    << ",\"J\":" << quote(formatIntervals(ji))
                    << ",\"X\":" << quote(formatIntervals(xi))
                    << ",\"lhs\":" << quote(formatIntervals(lhs))
                    << ",\"rhs\":" << quote(formatIntervals(rhs))
                    << ",\"independent\":" << quote(formatIntervals(independent)) << "}\n";
            writeText(out / "counterexample.json", failure.str());
            throw std::runtime_error("native kappa fusion identity/independent oracle mismatch");
        }
        ++horizons[static_cast<std::size_t>(horizon)];
        ++ks[static_cast<std::size_t>(k)];
        ++modes[static_cast<std::size_t>(mode_j)];
        beyond += k > horizon ? 1U : 0U;
    }
    std::ofstream log(out / "property_distribution.csv");
    log << "axis,value,count\n";
    for (std::size_t i = 1; i < horizons.size(); ++i) log << "horizon," << i << ',' << horizons[i] << '\n';
    for (std::size_t i = 1; i < ks.size(); ++i) log << "k," << i << ',' << ks[i] << '\n';
    for (std::size_t i = 0; i < modes.size(); ++i) log << "J_distribution_mode," << i << ',' << modes[i] << '\n';
    std::ostringstream receipt;
    receipt << "{\"status\":\"PASS\",\"pairs\":" << kPropertyPairs
            << ",\"seed\":" << kPropertySeed << ",\"native_identity\":true,\"independent_bitmask_oracle\":true,"
            << "\"horizon_min\":1,\"horizon_max\":64,\"k_min\":1,\"k_max\":80,\"k_greater_than_horizon\":" << beyond
            << ",\"distribution_modes\":[\"empty\",\"full\",\"bernoulli_half\",\"sparse_eighth\",\"dense_seven_eighths\",\"alternating\",\"one_interval\",\"fragmented_random\"]}\n";
    writeText(out / "property_receipt.json", receipt.str());
    std::cout << "PROPERTY PASS pairs=" << kPropertyPairs << " seed=" << kPropertySeed << std::endl;
}

void kernelGate(const fs::path& out) {
    std::mt19937_64 rng(kPropertySeed + 1);
    std::size_t candidate_checks = 0;
    std::size_t interval_checks = 0;
    std::size_t adaptive_branches = 0;
    auto candidates = [&](const std::vector<int>& left, const std::vector<int>& right) {
        std::vector<int> expected;
        std::set_intersection(left.begin(), left.end(), right.begin(), right.end(), std::back_inserter(expected));
        OperatorCounters counters;
        const auto actual = OperatorKernels::intersectCandidates(left, right, true, &counters);
        if (actual != expected) {
            std::ostringstream message;
            message << "{\"kind\":\"adaptive_candidate_kernel\",\"seed\":" << kPropertySeed + 1
                    << ",\"check\":" << candidate_checks << ",\"left\":[";
            for (std::size_t i = 0; i < left.size(); ++i) { if (i) message << ','; message << left[i]; }
            message << "],\"right\":[";
            for (std::size_t i = 0; i < right.size(); ++i) { if (i) message << ','; message << right[i]; }
            message << "]}\n";
            writeText(out / "counterexample.json", message.str());
            throw std::runtime_error("adaptive candidate kernel differs from std::set_intersection");
        }
        adaptive_branches += static_cast<std::size_t>(counters.ops_galloping_intersections.load());
        ++candidate_checks;
    };
    auto temporal = [&](const std::vector<TimeInterval>& left, const std::vector<TimeInterval>& right, int k) {
        const auto expected = intersectTimeIntervals(left, right, k);
        OperatorCounters counters;
        const auto actual = OperatorKernels::intersectIntervals(left, right, k, true, &counters);
        if (!sameIntervals(actual, expected)) {
            std::ostringstream message;
            message << "{\"kind\":\"adaptive_interval_kernel\",\"seed\":" << kPropertySeed + 1
                    << ",\"check\":" << interval_checks << ",\"k\":" << k
                    << ",\"left\":" << quote(formatIntervals(left))
                    << ",\"right\":" << quote(formatIntervals(right))
                    << ",\"expected\":" << quote(formatIntervals(expected))
                    << ",\"actual\":" << quote(formatIntervals(actual)) << "}\n";
            writeText(out / "counterexample.json", message.str());
            throw std::runtime_error("adaptive interval kernel differs from native merge reference");
        }
        adaptive_branches += static_cast<std::size_t>(counters.ops_galloping_intersections.load());
        ++interval_checks;
    };
    candidates({}, {}); candidates({}, {1, 2}); candidates({1, 2}, {});
    temporal({}, {}, 1); temporal({}, {{1, 4}}, 3); temporal({{1, 4}}, {}, 3);
    for (const int ratio : {1, 2, 31, 32, 33, 100, 128}) {
        for (int trial = 0; trial < 100; ++trial) {
            const int small_size = 1 + static_cast<int>(rng() % 8);
            const int large_size = ratio * small_size;
            std::vector<int> small, large;
            int current = -100;
            for (int i = 0; i < small_size; ++i) {
                current += 1 + static_cast<int>(rng() % 17);
                small.push_back(current);
            }
            current = -120;
            for (int i = 0; i < large_size; ++i) {
                current += 1 + static_cast<int>(rng() % 5);
                large.push_back(current);
            }
            candidates(small, large); candidates(large, small);
            std::vector<TimeInterval> short_list, long_list;
            int end = -200;
            for (int i = 0; i < small_size; ++i) {
                const int start = end + 2 + static_cast<int>(rng() % 20);
                end = start + 1 + static_cast<int>(rng() % 250);
                short_list.push_back({start, end});
            }
            end = -210;
            for (int i = 0; i < large_size; ++i) {
                const int start = end + 2 + static_cast<int>(rng() % 4);
                end = start + static_cast<int>(rng() % 8);
                long_list.push_back({start, end});
            }
            for (int k : {1, 2, 3, 7}) {
                temporal(short_list, long_list, k); temporal(long_list, short_list, k);
            }
        }
    }
    // One broad interval intersects many separated intervals; the result may
    // exceed min(lhs.size(),rhs.size()), so reserve is not an output bound.
    std::vector<TimeInterval> many;
    for (int i = 0; i < 128; ++i) many.push_back({10 * i, 10 * i + 5});
    for (int k : {1, 3, 6, 7}) {
        temporal({{-100, 2000}}, many, k); temporal(many, {{-100, 2000}}, k);
    }
    std::ostringstream receipt;
    receipt << "{\"status\":\"PASS\",\"seed\":" << kPropertySeed + 1
            << ",\"candidate_checks\":" << candidate_checks << ",\"interval_checks\":" << interval_checks
            << ",\"adaptive_branch_executions\":" << adaptive_branches
            << ",\"fixed_ratios\":[1,2,31,32,33,100,128],\"empty_and_spanning_interval_cases\":true,\"threshold\":32}\n";
    writeText(out / "kernel_receipt.json", receipt.str());
    std::cout << "KERNEL PASS candidates=" << candidate_checks << " intervals=" << interval_checks << std::endl;
}

struct Fixture {
    Graph graph;
    Graph query;
    int horizon = 0;
    int k = 0;
    std::string shape;
    int instance = 0;
    bool planted = false;
    std::vector<std::vector<Bits>> histories;
    std::vector<std::pair<int, int>> arcs;
};

std::vector<std::pair<int, int>> shapeArcs(const std::string& shape) {
    if (shape == "path2") return {{0, 1}, {1, 2}};
    if (shape == "path3") return {{0, 1}, {1, 2}, {2, 3}};
    if (shape == "cycle3") return {{0, 1}, {1, 2}, {2, 0}};
    if (shape == "cycle4") return {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
    if (shape == "star3") return {{0, 1}, {0, 2}, {0, 3}};
    throw std::runtime_error("unknown fixed shape");
}

void finishGraph(Graph& graph, const std::vector<std::vector<Bits>>& masks, int horizon) {
    const auto size = static_cast<std::size_t>(graph.num_vertices);
    graph.adj.assign(size, {});
    graph.in_adj.assign(size, {});
    graph.external_ids.resize(size);
    graph.vertex_active_durations.assign(size, 0);
    graph.neighbor_label_counts.assign(size, {});
    graph.incoming_neighbor_label_counts.assign(size, {});
    std::vector<Bits> active(size, 0);
    for (int u = 0; u < graph.num_vertices; ++u) {
        graph.external_ids[static_cast<std::size_t>(u)] = 1000 + 7 * u;
        for (int v = 0; v < graph.num_vertices; ++v) {
            const Bits mask = masks[static_cast<std::size_t>(u)][static_cast<std::size_t>(v)];
            if (!mask) continue;
            const int edge_id = static_cast<int>(graph.temporal_edges.size());
            const auto history = intervals(mask, horizon);
            std::int64_t occurrences = 0;
            for (const auto& interval : history) occurrences += interval.length();
            graph.temporal_edges.emplace_back(u, v, history, occurrences);
            graph.adj[static_cast<std::size_t>(u)].push_back({v, edge_id});
            graph.in_adj[static_cast<std::size_t>(v)].push_back({u, edge_id});
            ++graph.neighbor_label_counts[static_cast<std::size_t>(u)][graph.vertex_labels[static_cast<std::size_t>(v)]];
            ++graph.incoming_neighbor_label_counts[static_cast<std::size_t>(v)][graph.vertex_labels[static_cast<std::size_t>(u)]];
            active[static_cast<std::size_t>(u)] |= mask;
            active[static_cast<std::size_t>(v)] |= mask;
            graph.input_occurrence_count += static_cast<std::size_t>(occurrences);
        }
    }
    for (std::size_t u = 0; u < size; ++u) {
        for (int t = 0; t < horizon; ++t) graph.vertex_active_durations[u] += bit(active[u], t) ? 1 : 0;
        const auto by_target = [](const Edge& a, const Edge& b) { return a.to < b.to; };
        std::sort(graph.adj[u].begin(), graph.adj[u].end(), by_target);
        std::sort(graph.in_adj[u].begin(), graph.in_adj[u].end(), by_target);
    }
    graph.input_unique_edge_count = graph.temporal_edges.size();
    graph.filtered_edge_count = graph.temporal_edges.size();
}

Fixture makeFixture(std::mt19937_64& rng, int k, const std::string& shape, int instance) {
    Fixture f;
    f.k = k; f.shape = shape; f.instance = instance;
    f.horizon = 1 + static_cast<int>(rng() % 10);
    f.graph.num_vertices = 8 + static_cast<int>(rng() % 5);
    f.arcs = shapeArcs(shape);
    f.query.num_vertices = shape == "path2" || shape == "cycle3" ? 3 : 4;
    const auto n = static_cast<std::size_t>(f.graph.num_vertices);
    const auto qn = static_cast<std::size_t>(f.query.num_vertices);
    f.graph.vertex_labels.resize(n);
    f.query.vertex_labels.resize(qn);
    const bool all_same_label = instance % 4 == 0;
    for (auto& label : f.graph.vertex_labels) label = all_same_label ? Label{0} : static_cast<Label>(rng() % 3);
    for (std::size_t u = 0; u < qn; ++u) f.query.vertex_labels[u] = all_same_label ? Label{0} : static_cast<Label>(u % 3);
    f.query.adj.resize(qn); f.query.in_adj.resize(qn); f.query.external_ids.resize(qn);
    std::iota(f.query.external_ids.begin(), f.query.external_ids.end(), 0);
    for (auto arc : f.arcs) {
        f.query.adj[static_cast<std::size_t>(arc.first)].push_back({arc.second, -1});
        f.query.in_adj[static_cast<std::size_t>(arc.second)].push_back({arc.first, -1});
    }
    f.histories.assign(n, std::vector<Bits>(n, 0));
    std::vector<std::pair<int, int>> available;
    for (int u = 0; u < f.graph.num_vertices; ++u) {
        for (int v = 0; v < f.graph.num_vertices; ++v) if (u != v) available.emplace_back(u, v);
    }
    std::shuffle(available.begin(), available.end(), rng);
    const std::size_t target_edges = 20 + static_cast<std::size_t>(rng() % 21);
    std::set<std::pair<int, int>> selected;
    f.planted = f.horizon >= k && instance % 3 != 2;
    Bits planted_support = 0;
    if (f.planted) {
        const int begin = static_cast<int>(rng() % static_cast<Bits>(f.horizon - k + 1));
        for (int t = begin; t < begin + k; ++t) planted_support |= Bits{1} << t;
        for (std::size_t u = 0; u < qn; ++u) f.graph.vertex_labels[u] = f.query.vertex_labels[u];
        selected.insert(f.arcs.begin(), f.arcs.end());
    }
    for (auto edge : available) {
        if (selected.size() == target_edges) break;
        selected.insert(edge);
    }
    for (auto edge : selected) {
        Bits mask = randomBits(rng, f.horizon, 1 + static_cast<int>(rng() % 7));
        if (!mask) mask = Bits{1} << (rng() % static_cast<Bits>(f.horizon));
        if (f.planted && std::find(f.arcs.begin(), f.arcs.end(), edge) != f.arcs.end()) mask |= planted_support;
        f.histories[static_cast<std::size_t>(edge.first)][static_cast<std::size_t>(edge.second)] = mask;
    }
    finishGraph(f.graph, f.histories, f.horizon);
    require(f.graph.temporal_edges.size() == target_edges, "graph edge-count construction failure");
    return f;
}

std::string answerText(const Fixture& f, const std::vector<int>& mapping, Bits active) {
    std::ostringstream row;
    for (std::size_t u = 0; u < mapping.size(); ++u) {
        if (u) row << ", ";
        row << 'q' << u << '(' << labelToString(f.query.vertex_labels[u]) << ")->"
            << f.graph.externalId(mapping[u]);
    }
    row << " | active=" << formatIntervals(intervals(active, f.horizon));
    return row.str();
}

Answers oracle(const Fixture& f) {
    Answers answers;
    std::vector<int> mapping(static_cast<std::size_t>(f.query.num_vertices), -1);
    std::vector<bool> used(static_cast<std::size_t>(f.graph.num_vertices), false);
    std::function<void(int, Bits)> enumerate = [&](int u, Bits common) {
        if (u == f.query.num_vertices) {
            const Bits normalized = oracleKappa(common, f.horizon, f.k);
            if (normalized) answers.insert(answerText(f, mapping, normalized));
            return;
        }
        for (int v = 0; v < f.graph.num_vertices; ++v) {
            if (used[static_cast<std::size_t>(v)] || f.query.vertex_labels[static_cast<std::size_t>(u)] != f.graph.vertex_labels[static_cast<std::size_t>(v)]) continue;
            mapping[static_cast<std::size_t>(u)] = v;
            Bits next = common;
            for (auto arc : f.arcs) {
                if (std::max(arc.first, arc.second) != u) continue;
                const int a = mapping[static_cast<std::size_t>(arc.first)];
                const int b = mapping[static_cast<std::size_t>(arc.second)];
                next &= f.histories[static_cast<std::size_t>(a)][static_cast<std::size_t>(b)];
            }
            if (oracleKappa(next, f.horizon, f.k)) {
                used[static_cast<std::size_t>(v)] = true;
                enumerate(u + 1, next);
                used[static_cast<std::size_t>(v)] = false;
            }
            mapping[static_cast<std::size_t>(u)] = -1;
        }
    };
    enumerate(0, fullBits(f.horizon));
    return answers;
}

QueryDecomposition decomposition(const Fixture& f) {
    std::array<std::size_t, kLabelCount> counts{};
    std::array<double, kLabelCount> means{};
    for (std::size_t u = 0; u < f.graph.vertex_labels.size(); ++u) {
        const Label label = f.graph.vertex_labels[u];
        ++counts[label];
        means[label] += f.graph.vertex_active_durations[u];
    }
    for (std::size_t i = 0; i < counts.size(); ++i) if (counts[i]) means[i] /= static_cast<double>(counts[i]);
    return decomposeQuery(f.query, counts, means, SelectivityMode::Paper);
}

Answers parseAnswers(const std::string& output) {
    Answers answers;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.rfind("Match ", 0) != 0) continue;
        const auto pos = line.find(": ");
        require(pos != std::string::npos, "malformed native match row");
        require(answers.insert(line.substr(pos + 2)).second, "duplicate native full answer");
    }
    return answers;
}

struct Run {
    Answers answers;
    std::uint64_t count = 0;
    std::uint64_t extensions = 0;
};

Run nativeRun(const Fixture& f, const QueryPlan& plan, const TDTree& tree,
              const CandidateDag* dag, DagPasses direct_passes = {}) {
    OperatorCounters ops;
    GopherMatcherOptions options;
    options.threads = 1;
    options.parallel_mode = ParallelMode::Roots;
    options.early_prune = true;
    options.result_digest = false;
    options.executor.memo_mode = MemoMode::None;
    options.executor.collect_statistics = false;
    options.executor.ops = &ops;
    options.executor.direct_passes = direct_passes;
    std::ostringstream output;
    GopherMatchSummary summary;
    if (dag) {
        GopherMatcher matcher(f.graph, f.query, plan, *dag, tree, f.k, options);
        summary = matcher.run(MatchOutputMode::Full, &output);
    } else {
        GopherMatcher matcher(f.graph, f.query, plan, tree, f.k, options);
        summary = matcher.run(MatchOutputMode::Full, &output);
    }
    require(!summary.count_overflow, "native count overflow in small-graph gate");
    Run result{parseAnswers(output.str()), summary.match_count,
               ops.ops_logical_extensions.load(std::memory_order_relaxed)};
    require(result.answers.size() == result.count, "full answer rows disagree with native count");
    return result;
}

std::string dagIdentity(const CandidateDag& dag) {
    std::ostringstream out;
    out << std::setprecision(17) << dag.nodes_attempted << '/' << dag.nodes_merged
        << '/' << dag.nodes_eliminated_by_passes
        << '/' << dag.cumulative_support << '/' << dag.passes.p1 << dag.passes.p2 << dag.passes.p3;
    for (const auto count : dag.op_node_attempt_counts) out << ':' << count;
    for (const auto* roots : {&dag.structural_roots, &dag.temporal_roots, &dag.longest_run_roots}) {
        out << " roots"; for (const auto id : *roots) out << ':' << id;
    }
    for (const auto& n : dag.nodes) {
        out << " node:" << n.id << ':' << static_cast<int>(n.op) << ':' << static_cast<int>(n.result_kind)
            << ':' << n.parameter.first_query_vertex << ':' << n.parameter.second_query_vertex
            << ':' << n.canonical_expression << ':' << n.static_fingerprint << ':' << n.depth
            << ':' << n.raw_intervals << ':' << n.short_circuit << ':' << n.adaptive_intersection
            << ':' << n.estimated_cardinality << " parents";
        for (auto id : n.parents) out << ':' << id;
        out << " dependencies"; for (auto id : n.dependencies) out << ':' << id;
    }
    return out.str();
}

void verifyPassStructure(const CandidateDag& dag, DagPasses passes) {
    require(dag.nodes_attempted == dag.nodes.size() + dag.nodes_merged + dag.nodes_eliminated_by_passes,
            "IR attempted/materialized/merged/eliminated accounting mismatch");
    for (const auto& node : dag.nodes) {
        for (auto parent : node.parents) require(parent < node.id, "IR dependency is not topological");
        if (passes.p2) require(node.op != DagOp::EdgeTimeLookup, "P2 left an unguarded edge-history node");
        if (passes.p3 && (node.op == DagOp::StructuralIntersection || node.op == DagOp::TemporalIntersection)) {
            require(node.short_circuit && node.adaptive_intersection, "P3 intersection lacks compiled evaluation flags");
        }
    }
    if (passes.p1) {
        require(dag.cumulative_support, "P1 lacks explicit cumulative-support semantics");
        for (auto id : dag.longest_run_roots) require(id == kInvalidDagNode, "P1 retained scalar-longest root");
        for (auto id : dag.temporal_roots) {
            if (id == kInvalidDagNode) continue;
            const auto& kappa = dag.node(id);
            require(kappa.op == DagOp::KappaNormalize && kappa.parents.size() == 1, "P1 time root is not explicit kappa");
            const auto& join = dag.node(kappa.parents.front());
            require(join.op == DagOp::TemporalIntersection && join.raw_intervals && join.short_circuit && !join.parents.empty(),
                    "P1 normalized root does not consume raw short-circuit intersection");
            require(dag.node(join.parents.front()).op == DagOp::BranchSupportInput,
                    "P1 branch-support input is not first temporal operand");
        }
    }
}

void answerFile(const fs::path& path, const Answers& answers) {
    std::ostringstream text;
    for (const auto& answer : answers) text << answer << '\n';
    writeText(path, text.str());
}

void counterexample(const fs::path& out, const Fixture& f, const std::string& arm,
                    const std::string& reason, const Answers& expected, const Answers& actual) {
    answerFile(out / "expected_answers.txt", expected);
    answerFile(out / "actual_answers.txt", actual);
    std::ostringstream json;
    json << "{\"kind\":\"differential\",\"seed\":" << kGraphSeed
         << ",\"k\":" << f.k << ",\"shape\":" << quote(f.shape)
         << ",\"instance\":" << f.instance << ",\"horizon\":" << f.horizon
         << ",\"arm\":" << quote(arm) << ",\"reason\":" << quote(reason)
         << ",\"planted\":" << (f.planted ? "true" : "false") << ",\"graph_labels\":[";
    for (std::size_t i = 0; i < f.graph.vertex_labels.size(); ++i) {
        if (i) json << ',';
        json << quote(labelToString(f.graph.vertex_labels[i]));
    }
    json << "],\"query_labels\":[";
    for (std::size_t i = 0; i < f.query.vertex_labels.size(); ++i) {
        if (i) json << ',';
        json << quote(labelToString(f.query.vertex_labels[i]));
    }
    json << "],\"query_arcs\":[";
    for (std::size_t i = 0; i < f.arcs.size(); ++i) {
        if (i) json << ',';
        json << '[' << f.arcs[i].first << ',' << f.arcs[i].second << ']';
    }
    json << "],\"edges\":[";
    bool comma = false;
    std::ostringstream raw;
    for (const auto& edge : f.graph.temporal_edges) {
        if (comma) json << ',';
        comma = true;
        json << "{\"u\":" << edge.u << ",\"v\":" << edge.v
             << ",\"intervals\":" << quote(formatIntervals(edge.activeIntervals())) << '}';
        for (const auto& interval : edge.activeIntervals()) {
            for (int t = interval.start; t <= interval.end; ++t) raw << edge.u << ' ' << edge.v << ' ' << t << '\n';
        }
    }
    json << "],\"expected_count\":" << expected.size() << ",\"actual_count\":" << actual.size() << "}\n";
    writeText(out / "counterexample.json", json.str());
    writeText(out / "counterexample_graph_occurrences.txt", raw.str());
}

struct Stratum {
    std::size_t instances = 0;
    std::size_t positive = 0;
    std::size_t zero = 0;
    std::size_t planted = 0;
    std::size_t answer_total = 0;
};

void differentialGate(const fs::path& out) {
    std::mt19937_64 rng(kGraphSeed);
    const std::array<std::string, 5> shapes{{"path2", "path3", "cycle3", "cycle4", "star3"}};
    std::map<std::pair<int, std::string>, Stratum> strata;
    std::size_t comparisons = 0;
    std::size_t oracle_comparisons = 0;
    std::size_t p1_expansion_checks = 0;
    std::size_t all_off_checks = 0;
    std::ofstream cases_log(out / "differential_cases.csv");
    cases_log << "k,shape,instance,vertices,edges,horizon,planted,answer_count,direct_extensions,dag_base_extensions,dag_p1_extensions,all_8_subsets_pass,direct_opt_pass,independent_oracle_pass\n";
    for (int k : {2, 3, 4}) for (const auto& shape : shapes) {
        for (int instance = 0; instance < kInstancesPerStratum; ++instance) {
            const Fixture f = makeFixture(rng, k, shape, instance);
            Answers expected;
            try {
            const auto qd = decomposition(f);
            require(qd.connected, "connected shape decomposition failed");
            const TDTree tree(f.graph, f.query, qd, k);
            const QueryPlan plan = makeQueryPlan(f.query, qd, tree, OrderMode::Paper);
            CandidateDagCompiler compiler;
            const CandidateDag base = compiler.compile(f.query, plan, qd);
            expected = oracle(f);
            const Run direct = nativeRun(f, plan, tree, nullptr);
            if (direct.answers != expected || (f.planted && expected.empty())) {
                counterexample(out, f, "direct", "unoptimized Direct differs from independent full-set oracle or planted witness missing", expected, direct.answers);
                throw std::runtime_error("Direct / independent oracle mismatch");
            }
            ++oracle_comparisons;
            std::uint64_t base_extensions = 0, p1_extensions = 0;
            for (unsigned int mask = 0; mask < 8; ++mask) {
                const DagPasses passes{(mask & 1U) != 0, (mask & 2U) != 0, (mask & 4U) != 0};
                const CandidateDag optimized = optimizeCandidateDag(base, f.graph, f.query, plan, tree, passes);
                verifyPassStructure(optimized, passes);
                if (mask == 0) {
                    if (dagIdentity(optimized) != dagIdentity(base)) {
                        counterexample(out, f, "none", "all-off IR differs from original P0", expected, {});
                        throw std::runtime_error("all-off DAG topology/identity mismatch");
                    }
                    ++all_off_checks;
                }
                const Run candidate = nativeRun(f, plan, tree, &optimized);
                if (candidate.answers != expected) {
                    counterexample(out, f, dagPassesName(passes), "DAG full set (mapping and active intervals) mismatch", expected, candidate.answers);
                    throw std::runtime_error("DAG subset full answer-set mismatch");
                }
                if (mask == 0) base_extensions = candidate.extensions;
                if (mask == 1) p1_extensions = candidate.extensions;
                ++comparisons;
            }
            if (base_extensions != p1_extensions) {
                counterexample(out, f, "p1", "logical_extensions differs from dag_base: " + std::to_string(base_extensions) + " vs " + std::to_string(p1_extensions), expected, expected);
                throw std::runtime_error("P1 changed logical search extensions");
            }
            ++p1_expansion_checks;
            const Run direct_opt = nativeRun(f, plan, tree, nullptr, DagPasses{true, true, true});
            if (direct_opt.answers != expected) {
                counterexample(out, f, "direct_opt", "optimized Direct full-set mismatch", expected, direct_opt.answers);
                throw std::runtime_error("direct_opt full answer-set mismatch");
            }
            ++comparisons;
            Stratum& stats = strata[{k, shape}];
            ++stats.instances;
            stats.positive += expected.empty() ? 0U : 1U;
            stats.zero += expected.empty() ? 1U : 0U;
            stats.planted += f.planted ? 1U : 0U;
            stats.answer_total += expected.size();
            cases_log << k << ',' << shape << ',' << instance << ',' << f.graph.num_vertices << ','
                      << f.graph.temporal_edges.size() << ',' << f.horizon << ',' << f.planted << ',' << expected.size()
                      << ',' << direct.extensions << ',' << base_extensions << ',' << p1_extensions << ",true,true,true\n";
            cases_log.flush();
            } catch (const std::exception& error) {
                if (!fs::exists(out / "counterexample.json")) {
                    counterexample(out, f, "exception", error.what(), expected, {});
                }
                throw;
            }
        }
        std::cout << "DIFFERENTIAL PASS k=" << k << " shape=" << shape << " instances=" << kInstancesPerStratum << std::endl;
    }
    std::ostringstream receipt;
    receipt << "{\"status\":\"PASS\",\"seed\":" << kGraphSeed
            << ",\"instances\":3000,\"dag_subset_comparisons\":24000,\"direct_opt_comparisons\":3000,\"total_comparisons\":" << comparisons
            << ",\"independent_oracle_comparisons\":" << oracle_comparisons
            << ",\"p1_logical_extension_checks\":" << p1_expansion_checks
            << ",\"all_off_exact_ir_checks\":" << all_off_checks
            << ",\"answer_semantics\":\"complete mapping plus normalized active intervals, no digest substitution\",\"strata\":[";
    bool comma = false;
    for (const auto& item : strata) {
        const auto& stats = item.second;
        require(stats.instances == kInstancesPerStratum && stats.positive > 0 && stats.zero > 0, "insufficient nontrivial differential stratum coverage");
        if (comma) receipt << ',';
        comma = true;
        receipt << "{\"k\":" << item.first.first << ",\"shape\":" << quote(item.first.second)
                << ",\"instances\":" << stats.instances << ",\"positive\":" << stats.positive << ",\"zero\":" << stats.zero
                << ",\"planted\":" << stats.planted << ",\"answer_total\":" << stats.answer_total << '}';
    }
    receipt << "]}\n";
    writeText(out / "differential_receipt.json", receipt.str());
}

void dumps(const fs::path& out) {
    fs::create_directories(out / "dag_dumps");
    std::mt19937_64 rng(kGraphSeed + 1);
    for (const auto& shape : {std::string("path2"), std::string("cycle3")}) {
        Fixture f;
        do { f = makeFixture(rng, 3, shape, 0); } while (!f.planted);
        const auto qd = decomposition(f);
        const TDTree tree(f.graph, f.query, qd, 3);
        const QueryPlan plan = makeQueryPlan(f.query, qd, tree, OrderMode::Paper);
        CandidateDagCompiler compiler;
        const CandidateDag base = compiler.compile(f.query, plan, qd);
        for (const auto& named : std::vector<std::pair<std::string, DagPasses>>{
            {"p0", {}}, {"p0_p1", {true, false, false}}, {"p0_p2", {false, true, false}},
            {"p0_p3", {false, false, true}}, {"p0_p1_p2", {true, true, false}}, {"p0_p1_p2_p3", {true, true, true}}}) {
            const auto dag = optimizeCandidateDag(base, f.graph, f.query, plan, tree, named.second);
            dumpCandidateDag(dag, plan, (out / "dag_dumps" / (shape + "_k3_" + named.first)).string(), named.first);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: test_operator_passes NEW_OUTPUT_DIRECTORY\n";
        return 2;
    }
    const fs::path out(argv[1]);
    if (fs::exists(out)) {
        std::cerr << "Refusing existing correctness output directory: " << out << '\n';
        return 2;
    }
    fs::create_directories(out);
    try {
        propertyGate(out);
        kernelGate(out);
        differentialGate(out);
        dumps(out);
        writeText(out / "native_receipt.json", "{\"status\":\"PASS\",\"property_pairs\":1000000,\"random_graph_instances\":3000,\"dag_subsets\":8,\"dag_subset_comparisons\":24000,\"direct_opt_comparisons\":3000,\"independent_answer_set_oracle\":true,\"active_intervals_compared\":true,\"p1_logical_extensions_unchanged\":true,\"all_off_ir_unchanged\":true,\"failures\":0}\n");
        std::cout << "CORRECTNESS PASS: property + all subsets + direct_opt + independent full sets\n";
        return 0;
    } catch (const std::exception& error) {
        const std::string receipt = "{\"status\":\"STOPPED\",\"reason\":" + quote(error.what()) + ",\"no_further_test_instances\":true}\n";
        writeText(out / "STOP.json", receipt);
        writeText(out / "native_receipt.json", receipt);
        std::cerr << "STOP: " << error.what() << '\n';
        return 1;
    }
}
