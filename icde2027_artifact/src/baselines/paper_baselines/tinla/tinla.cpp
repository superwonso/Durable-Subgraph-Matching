#include "tinla.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <sys/resource.h>
#endif

namespace tinla {
namespace {

using Clock = std::chrono::steady_clock;

double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::size_t peakRssBytes() {
#ifdef _WIN32
    return 0;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
    return static_cast<std::size_t>(usage.ru_maxrss) * 1024U;
#endif
}

std::uint64_t cellKey(int vertex, bool incoming, Label label) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(vertex)) << 4U) |
        (incoming ? UINT64_C(8) : UINT64_C(0)) |
        static_cast<std::uint64_t>(label);
}

struct RawEdge {
    int from = -1;
    int to = -1;
    int time = 0;
};

bool rawLess(const RawEdge& lhs, const RawEdge& rhs) {
    if (lhs.from != rhs.from) return lhs.from < rhs.from;
    if (lhs.to != rhs.to) return lhs.to < rhs.to;
    return lhs.time < rhs.time;
}

enum class LineStatus { Empty, Valid, Invalid };

#if !defined(DSM_E3E4_CORE_ONLY)
LineStatus parseTemporalLine(
    const std::string& line,
    int& from,
    int& to,
    int& time) {
    const char* cursor = line.data();
    const char* const end = cursor + line.size();
    auto skipSpace = [&]() {
        while (cursor != end &&
               std::isspace(static_cast<unsigned char>(*cursor)) != 0) {
            ++cursor;
        }
    };
    auto parseInt = [&](int& value) {
        skipSpace();
        if (cursor == end || *cursor == '#') return false;
        const auto parsed = std::from_chars(cursor, end, value);
        if (parsed.ec != std::errc{} || parsed.ptr == cursor) return false;
        cursor = parsed.ptr;
        return cursor == end || *cursor == '#' ||
            std::isspace(static_cast<unsigned char>(*cursor)) != 0;
    };

    skipSpace();
    if (cursor == end || *cursor == '#') return LineStatus::Empty;
    if (!parseInt(from) || !parseInt(to) || !parseInt(time)) {
        return LineStatus::Invalid;
    }
    skipSpace();
    return cursor == end || *cursor == '#'
        ? LineStatus::Valid
        : LineStatus::Invalid;
}
#endif  // !defined(DSM_E3E4_CORE_ONLY)

int compactId(const std::vector<int>& ids, int external_id) {
    const auto found = std::lower_bound(ids.begin(), ids.end(), external_id);
    return found != ids.end() && *found == external_id
        ? static_cast<int>(found - ids.begin())
        : -1;
}

std::vector<CountSegment> buildCountSegments(
    std::vector<std::pair<std::int64_t, int>> events) {
    if (events.empty()) return {};
    std::sort(events.begin(), events.end());

    std::vector<CountSegment> segments;
    int active = 0;
    std::size_t index = 0;
    while (index < events.size()) {
        const std::int64_t time = events[index].first;
        int delta = 0;
        while (index < events.size() && events[index].first == time) {
            delta += events[index].second;
            ++index;
        }
        active += delta;
        if (active < 0) return {};
        if (active == 0 || index == events.size()) continue;
        const std::int64_t next_time = events[index].first;
        if (next_time <= time || time > std::numeric_limits<int>::max() ||
            next_time - 1 < std::numeric_limits<int>::min()) {
            continue;
        }
        const int start = static_cast<int>(
            std::max<std::int64_t>(time, std::numeric_limits<int>::min()));
        const int end = static_cast<int>(
            std::min<std::int64_t>(next_time - 1, std::numeric_limits<int>::max()));
        if (end >= start) segments.push_back({start, end, active});
    }
    return segments;
}

bool containsSorted(const std::vector<int>& values, int value) {
    return std::binary_search(values.begin(), values.end(), value);
}

bool edgeHasDuration(
    const TemporalEdge& edge,
    int minimum_duration,
    DurationMode mode) {
    return duration(edge.active, mode) >= minimum_duration;
}

bool queryIsWeaklyConnected(const Query& query) {
    if (query.labels.empty() || query.out.size() != query.labels.size() ||
        query.in.size() != query.labels.size()) {
        return false;
    }
    std::vector<std::uint8_t> visited(query.labels.size(), 0);
    std::vector<int> frontier{0};
    visited[0] = 1;
    for (std::size_t head = 0; head < frontier.size(); ++head) {
        const int vertex = frontier[head];
        const auto visit = [&](const std::vector<int>& neighbors) {
            for (const int neighbor : neighbors) {
                if (neighbor < 0 ||
                    static_cast<std::size_t>(neighbor) >= visited.size()) {
                    continue;
                }
                if (visited[static_cast<std::size_t>(neighbor)] != 0) continue;
                visited[static_cast<std::size_t>(neighbor)] = 1;
                frontier.push_back(neighbor);
            }
        };
        visit(query.out[static_cast<std::size_t>(vertex)]);
        visit(query.in[static_cast<std::size_t>(vertex)]);
    }
    return frontier.size() == query.labels.size();
}

bool compatibleNeighbor(
    const Graph& graph,
    const std::vector<std::vector<int>>& candidates,
    int query_vertex,
    int query_neighbor,
    int data_vertex,
    bool incoming,
    int minimum_duration,
    DurationMode duration_mode) {
    if (query_vertex == query_neighbor) {
        const TemporalEdge* loop = graph.findEdge(data_vertex, data_vertex);
        return loop != nullptr &&
            edgeHasDuration(*loop, minimum_duration, duration_mode);
    }

    const auto& adjacency = incoming
        ? graph.in[static_cast<std::size_t>(data_vertex)]
        : graph.out[static_cast<std::size_t>(data_vertex)];
    const auto& target_candidates =
        candidates[static_cast<std::size_t>(query_neighbor)];
    for (const AdjEdge& reference : adjacency) {
        if (reference.vertex == data_vertex ||
            !containsSorted(target_candidates, reference.vertex)) {
            continue;
        }
        if (edgeHasDuration(
                graph.edges[static_cast<std::size_t>(reference.edge_id)],
                minimum_duration,
                duration_mode)) {
            return true;
        }
    }
    return false;
}

bool refineCandidates(
    const Graph& graph,
    const Query& query,
    const RunOptions& options,
    std::vector<std::vector<int>>& candidates,
    Counters& counters,
    bool dynamic) {
    bool changed = true;
    while (changed) {
        changed = false;
        if (dynamic) {
            ++counters.dynamic_refine_rounds;
        } else {
            ++counters.refine_rounds;
        }
        for (std::size_t q = 0; q < query.labels.size(); ++q) {
            std::vector<int> retained;
            retained.reserve(candidates[q].size());
            for (const int data_vertex : candidates[q]) {
                bool valid = true;
                for (const int neighbor : query.out[q]) {
                    if (!compatibleNeighbor(
                            graph,
                            candidates,
                            static_cast<int>(q),
                            neighbor,
                            data_vertex,
                            false,
                            options.minimum_duration,
                            options.duration_mode)) {
                        valid = false;
                        break;
                    }
                }
                if (valid) {
                    for (const int neighbor : query.in[q]) {
                        if (!compatibleNeighbor(
                                graph,
                                candidates,
                                static_cast<int>(q),
                                neighbor,
                                data_vertex,
                                true,
                                options.minimum_duration,
                                options.duration_mode)) {
                            valid = false;
                            break;
                        }
                    }
                }
                if (valid) {
                    retained.push_back(data_vertex);
                } else {
                    if (dynamic) {
                        ++counters.dynamic_refine_pruned;
                    } else {
                        ++counters.refine_pruned;
                    }
                    changed = true;
                }
            }
            candidates[q].swap(retained);
            if (candidates[q].empty()) return false;
        }
    }
    return true;
}

std::vector<int> makeOrder(
    const Query& query,
    const std::vector<std::vector<int>>& candidates) {
    const std::size_t count = query.labels.size();
    std::vector<int> order;
    order.reserve(count);
    std::vector<bool> selected(count, false);
    while (order.size() < count) {
        int best = -1;
        int best_bound = -1;
        std::size_t best_candidates = std::numeric_limits<std::size_t>::max();
        int best_degree = -1;
        for (std::size_t q = 0; q < count; ++q) {
            if (selected[q]) continue;
            int bound = 0;
            for (int n : query.out[q]) {
                if (selected[static_cast<std::size_t>(n)]) ++bound;
            }
            for (int n : query.in[q]) {
                if (selected[static_cast<std::size_t>(n)]) ++bound;
            }
            const int degree = static_cast<int>(query.out[q].size() + query.in[q].size());
            const bool better = best < 0 || bound > best_bound ||
                (bound == best_bound && candidates[q].size() < best_candidates) ||
                (bound == best_bound && candidates[q].size() == best_candidates &&
                 degree > best_degree) ||
                (bound == best_bound && candidates[q].size() == best_candidates &&
                 degree == best_degree && static_cast<int>(q) < best);
            if (better) {
                best = static_cast<int>(q);
                best_bound = bound;
                best_candidates = candidates[q].size();
                best_degree = degree;
            }
        }
        selected[static_cast<std::size_t>(best)] = true;
        order.push_back(best);
    }
    return order;
}

bool checkedIncrement(std::uint64_t& value) {
    if (value == std::numeric_limits<std::uint64_t>::max()) return false;
    ++value;
    return true;
}

bool checkedAdd(std::uint64_t& value, std::uint64_t increment) {
    if (increment > std::numeric_limits<std::uint64_t>::max() - value) return false;
    value += increment;
    return true;
}

class Searcher {
public:
    Searcher(
        const Graph& graph,
        const Query& query,
        const RunOptions& options,
        const std::vector<int>& order,
        const std::vector<std::vector<int>>& initial_candidates)
        : graph_(graph),
          query_(query),
          options_(options),
          order_(order),
          initial_candidates_(initial_candidates) {}

    std::uint64_t runRoot(int root_candidate, std::vector<Match>* matches) {
        std::vector<int> mapping(query_.labels.size(), -1);
        std::vector<bool> used(graph_.labels.size(), false);
        std::vector<Interval> support;
        if (graph_.max_time >= graph_.min_time) {
            support.push_back({graph_.min_time, graph_.max_time});
        }
        std::vector<std::vector<int>> candidates = initial_candidates_;
        std::uint64_t count = 0;
        place(0, root_candidate, candidates, mapping, used, support, count, matches);
        return count;
    }

    const Counters& counters() const { return counters_; }
    bool overflow() const { return overflow_; }

private:
    void search(
        std::size_t depth,
        const std::vector<std::vector<int>>& candidates,
        std::vector<int>& mapping,
        std::vector<bool>& used,
        const std::vector<Interval>& support,
        std::uint64_t& count,
        std::vector<Match>* matches) {
        if (overflow_) return;
        ++counters_.dfs_states;
        if (depth == order_.size()) {
            if (!checkedIncrement(count)) {
                overflow_ = true;
                return;
            }
            if (matches != nullptr) matches->push_back({mapping, support});
            return;
        }
        const int q = order_[depth];
        for (const int candidate : candidates[static_cast<std::size_t>(q)]) {
            place(depth, candidate, candidates, mapping, used, support, count, matches);
            if (overflow_) return;
        }
    }

    void place(
        std::size_t depth,
        int candidate,
        const std::vector<std::vector<int>>& candidates,
        std::vector<int>& mapping,
        std::vector<bool>& used,
        const std::vector<Interval>& support,
        std::uint64_t& count,
        std::vector<Match>* matches) {
        const int q = order_[depth];
        ++counters_.raw_extension_candidates;
        if (candidate < 0 ||
            static_cast<std::size_t>(candidate) >= used.size() ||
            used[static_cast<std::size_t>(candidate)]) {
            ++counters_.injectivity_pruned;
            return;
        }

        mapping[static_cast<std::size_t>(q)] = candidate;
        used[static_cast<std::size_t>(candidate)] = true;
        std::vector<Interval> next_support = support;
        bool valid = true;
        for (const auto& edge : query_.edges) {
            if (edge.first != q && edge.second != q) continue;
            const int from = mapping[static_cast<std::size_t>(edge.first)];
            const int to = mapping[static_cast<std::size_t>(edge.second)];
            if (from < 0 || to < 0) continue;
            const TemporalEdge* temporal_edge = graph_.findEdge(from, to);
            if (temporal_edge == nullptr) {
                ++counters_.structural_pruned;
                valid = false;
                break;
            }
            next_support = intersectIntervals(next_support, temporal_edge->active);
            if (duration(next_support, options_.duration_mode) <
                options_.minimum_duration) {
                ++counters_.temporal_pruned;
                valid = false;
                break;
            }
        }

        if (valid) {
            std::vector<std::vector<int>> next_candidates = candidates;
            next_candidates[static_cast<std::size_t>(q)] = {candidate};
            ++counters_.dynamic_refine_calls;
            if (refineCandidates(
                    graph_, query_, options_, next_candidates, counters_, true)) {
                search(
                    depth + 1,
                    next_candidates,
                    mapping,
                    used,
                    next_support,
                    count,
                    matches);
            }
        }
        used[static_cast<std::size_t>(candidate)] = false;
        mapping[static_cast<std::size_t>(q)] = -1;
    }

    const Graph& graph_;
    const Query& query_;
    const RunOptions& options_;
    const std::vector<int>& order_;
    const std::vector<std::vector<int>>& initial_candidates_;
    Counters counters_;
    bool overflow_ = false;
};

bool materializeRawGraph(
    std::vector<RawEdge> raw,
    std::uint32_t seed,
    LabelMode label_mode,
    const std::vector<ValidatedRuntimeLabel>* explicit_labels,
    Graph& graph,
    double* preprocess_ms,
    std::string& error,
    bool apply_consecutive_pair_filter = false,
    V8LoadTimings* v8_timings = nullptr) {
    if (preprocess_ms != nullptr) *preprocess_ms = 0.0;
    if (raw.empty()) {
        error = "temporal graph contains no edge occurrences";
        return false;
    }
    const auto preprocess_start = Clock::now();
    graph = {};
    graph.input_occurrences = raw.size();
    std::sort(raw.begin(), raw.end(), rawLess);
    std::vector<int> external_ids;
    external_ids.reserve(std::min<std::size_t>(raw.size(), 8'000'000));
    graph.edges.reserve(std::min<std::size_t>(raw.size() / 4 + 1, 4'000'000));
    graph.min_time = std::numeric_limits<int>::max();
    graph.max_time = std::numeric_limits<int>::min();
    const auto history_start = Clock::now();
    std::size_t index = 0;
    while (index < raw.size()) {
        const int from = raw[index].from;
        const int to = raw[index].to;
        external_ids.push_back(from);
        external_ids.push_back(to);
        std::vector<Interval> active;
        bool have_time = false;
        bool has_consecutive_pair = false;
        int run_start = 0;
        int previous = 0;
        while (index < raw.size() && raw[index].from == from &&
               raw[index].to == to) {
            const int current = raw[index].time;
            ++index;
            if (have_time && current == previous) continue;
            ++graph.unique_occurrences;
            graph.min_time = std::min(graph.min_time, current);
            graph.max_time = std::max(graph.max_time, current);
            if (!have_time) {
                run_start = previous = current;
                have_time = true;
            } else if (previous != std::numeric_limits<int>::max() &&
                       current == previous + 1) {
                has_consecutive_pair = true;
                previous = current;
            } else {
                active.push_back({run_start, previous});
                run_start = previous = current;
            }
        }
        if (have_time) active.push_back({run_start, previous});
        // v8 input-only F2: retain complete histories, not eroded runs.
        if (!apply_consecutive_pair_filter || has_consecutive_pair) {
            graph.edges.push_back({from, to, std::move(active)});
        }
    }
    const auto history_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - history_start).count());
    std::vector<RawEdge>().swap(raw);

    const auto label_start = Clock::now();
    std::sort(external_ids.begin(), external_ids.end());
    external_ids.erase(
        std::unique(external_ids.begin(), external_ids.end()),
        external_ids.end());
    graph.external_ids = std::move(external_ids);
    graph.labels.resize(graph.external_ids.size(), kInvalidLabel);
    if (explicit_labels != nullptr) {
        if (explicit_labels->size() != graph.external_ids.size()) {
            error = "explicit runtime labels do not exactly cover graph vertices";
            return false;
        }
        int previous_external_id = -1;
        for (std::size_t i = 0; i < explicit_labels->size(); ++i) {
            const ValidatedRuntimeLabel& row = explicit_labels->at(i);
            if (row.external_id < 0 || row.external_id <= previous_external_id ||
                row.external_id != graph.external_ids[i] ||
                row.runtime_label > kInvalidLabel) {
                error = "explicit runtime labels are not strict full graph coverage";
                return false;
            }
            previous_external_id = row.external_id;
            graph.labels[i] = row.runtime_label;
        }
    } else if (label_mode == LabelMode::StableId) {
        for (std::size_t i = 0; i < graph.external_ids.size(); ++i) {
            graph.labels[i] = stableIdLabel(graph.external_ids[i], seed);
        }
    } else {
        std::mt19937 generator(seed);
        std::uniform_int_distribution<int> distribution(0, kLabelCount - 1);
        for (Label& label : graph.labels) {
            label = static_cast<Label>(distribution(generator));
        }
    }

    const auto label_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - label_start).count());
    if (apply_consecutive_pair_filter) {
        // Validate full raw-map coverage first, then retain the surviving
        // external-ID subset without recomputing any label.
        std::vector<int> retained_ids;
        retained_ids.reserve(graph.edges.size() * 2);
        graph.min_time = std::numeric_limits<int>::max();
        graph.max_time = std::numeric_limits<int>::min();
        for (const auto& edge : graph.edges) {
            retained_ids.push_back(edge.from);
            retained_ids.push_back(edge.to);
            for (const auto& interval : edge.active) {
                graph.min_time = std::min(graph.min_time, interval.start);
                graph.max_time = std::max(graph.max_time, interval.end);
            }
        }
        std::sort(retained_ids.begin(), retained_ids.end());
        retained_ids.erase(std::unique(retained_ids.begin(), retained_ids.end()), retained_ids.end());
        std::vector<Label> retained_labels;
        retained_labels.reserve(retained_ids.size());
        for (const int external_id : retained_ids) {
            const auto old = std::lower_bound(graph.external_ids.begin(), graph.external_ids.end(), external_id);
            retained_labels.push_back(graph.labels[static_cast<std::size_t>(old - graph.external_ids.begin())]);
        }
        graph.external_ids = std::move(retained_ids);
        graph.labels = std::move(retained_labels);
        if (graph.edges.empty()) { graph.min_time = 0; graph.max_time = -1; }
    }
    graph.out.resize(graph.external_ids.size());
    graph.in.resize(graph.external_ids.size());
    std::vector<std::size_t> out_degrees(graph.external_ids.size(), 0);
    std::vector<std::size_t> in_degrees(graph.external_ids.size(), 0);
    for (const TemporalEdge& edge : graph.edges) {
        const int from = compactId(graph.external_ids, edge.from);
        const int to = compactId(graph.external_ids, edge.to);
        if (from >= 0 && to >= 0) {
            ++out_degrees[static_cast<std::size_t>(from)];
            ++in_degrees[static_cast<std::size_t>(to)];
        }
    }
    for (std::size_t vertex = 0; vertex < graph.external_ids.size(); ++vertex) {
        graph.out[vertex].reserve(out_degrees[vertex]);
        graph.in[vertex].reserve(in_degrees[vertex]);
    }
    for (std::size_t edge_id = 0; edge_id < graph.edges.size(); ++edge_id) {
        TemporalEdge& edge = graph.edges[edge_id];
        edge.from = compactId(graph.external_ids, edge.from);
        edge.to = compactId(graph.external_ids, edge.to);
        if (edge.from < 0 || edge.to < 0) {
            error = "internal error while compacting vertex IDs";
            return false;
        }
        graph.out[static_cast<std::size_t>(edge.from)].push_back(
            {edge.to, static_cast<int>(edge_id)});
        graph.in[static_cast<std::size_t>(edge.to)].push_back(
            {edge.from, static_cast<int>(edge_id)});
    }
    const auto adjacencyLess = [](const AdjEdge& lhs, const AdjEdge& rhs) {
        return lhs.vertex < rhs.vertex;
    };
    for (auto& adjacency : graph.out) {
        std::sort(adjacency.begin(), adjacency.end(), adjacencyLess);
    }
    for (auto& adjacency : graph.in) {
        std::sort(adjacency.begin(), adjacency.end(), adjacencyLess);
    }
    if (preprocess_ms != nullptr) {
        *preprocess_ms = milliseconds(preprocess_start);
    }
    if (v8_timings != nullptr) {
        const auto preprocess_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - preprocess_start).count());
        v8_timings->label_ns = label_ns;
        v8_timings->filter_ns = apply_consecutive_pair_filter ? history_ns : 0;
        v8_timings->graph_build_ns = preprocess_ns - label_ns - v8_timings->filter_ns;
    }
    return true;
}

bool observeNativePhase(
    const NativePhaseObserver* legacy_observer,
    const RuntimeHookedNativePhaseObserver* runtime_observer,
    const dsm::e3e4::adapter_v1::runtime_hooks_v1::MonotonicNowSource* clock,
    NativeCountPhase phase,
    std::string& error) {
    if (legacy_observer == nullptr && runtime_observer == nullptr) return true;
    if (clock == nullptr ||
        (legacy_observer != nullptr && runtime_observer != nullptr)) {
        error = "native phase hook configuration is invalid";
        return false;
    }
    const auto reading = clock->now();
    const bool accepted = runtime_observer != nullptr
        ? (*runtime_observer)(phase, reading, error)
        : (*legacy_observer)(phase, reading.nanoseconds(), error);
    if (!accepted) {
        if (error.empty()) error = "native phase observer rejected transition";
        return false;
    }
    return true;
}

}  // namespace

std::int64_t Interval::length() const {
    return end >= start
        ? static_cast<std::int64_t>(end) - static_cast<std::int64_t>(start) + 1
        : 0;
}

bool Interval::operator==(const Interval& other) const {
    return start == other.start && end == other.end;
}

const char* labelModeName(LabelMode mode) {
    return mode == LabelMode::StableId ? "stable-id" : "sequential-rng";
}

const char* durationModeName(DurationMode mode) {
    return mode == DurationMode::Consecutive ? "consecutive" : "cardinality";
}

const char* indexModeName(IndexMode mode) {
    return mode == IndexMode::Ctinla ? "ctinla" : "tinla";
}

Label stableIdLabel(int external_id, std::uint32_t label_seed) {
    if (external_id < 0) return kInvalidLabel;
    std::uint64_t mixed =
        (static_cast<std::uint64_t>(label_seed) << 32U) |
        static_cast<std::uint32_t>(external_id);
    mixed += UINT64_C(0x9e3779b97f4a7c15);
    mixed = (mixed ^ (mixed >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    mixed = (mixed ^ (mixed >> 27U)) * UINT64_C(0x94d049bb133111eb);
    mixed ^= mixed >> 31U;
    return static_cast<Label>(mixed % kLabelCount);
}

Label labelFromString(const std::string& text) {
    return text.size() == 1 && text[0] >= 'A' && text[0] <= 'E'
        ? static_cast<Label>(text[0] - 'A')
        : kInvalidLabel;
}

std::string labelToString(Label label) {
    return label < kLabelCount
        ? std::string(1, static_cast<char>('A' + label))
        : "?";
}

const TemporalEdge* Graph::findEdge(int from, int to) const {
    if (from < 0 || to < 0 ||
        static_cast<std::size_t>(from) >= out.size()) {
        return nullptr;
    }
    const auto& adjacency = out[static_cast<std::size_t>(from)];
    const auto found = std::lower_bound(
        adjacency.begin(), adjacency.end(), to,
        [](const AdjEdge& edge, int vertex) { return edge.vertex < vertex; });
    return found != adjacency.end() && found->vertex == to
        ? &edges[static_cast<std::size_t>(found->edge_id)]
        : nullptr;
}

int Graph::externalId(int internal_id) const {
    return internal_id >= 0 &&
        static_cast<std::size_t>(internal_id) < external_ids.size()
        ? external_ids[static_cast<std::size_t>(internal_id)]
        : internal_id;
}

std::size_t Graph::memoryBytes() const {
    std::size_t bytes = sizeof(Graph);
    bytes += external_ids.capacity() * sizeof(int);
    bytes += labels.capacity() * sizeof(Label);
    bytes += edges.capacity() * sizeof(TemporalEdge);
    for (const auto& edge : edges) {
        bytes += edge.active.capacity() * sizeof(Interval);
    }
    bytes += out.capacity() * sizeof(std::vector<AdjEdge>);
    bytes += in.capacity() * sizeof(std::vector<AdjEdge>);
    for (const auto& adjacency : out) {
        bytes += adjacency.capacity() * sizeof(AdjEdge);
    }
    for (const auto& adjacency : in) {
        bytes += adjacency.capacity() * sizeof(AdjEdge);
    }
    return bytes;
}

NeighborLabelIndex::NeighborLabelIndex(IndexMode mode) : mode_(mode) {}

bool NeighborLabelIndex::build(const Graph& graph, std::string& error) {
    presence_.clear();
    counters_.clear();
    for (std::size_t vertex = 0; vertex < graph.labels.size(); ++vertex) {
        for (int direction = 0; direction < 2; ++direction) {
            const bool incoming = direction != 0;
            std::array<std::vector<std::pair<std::int64_t, int>>, kLabelCount>
                events_by_label;
            const auto& adjacency = incoming ? graph.in[vertex] : graph.out[vertex];
            for (const AdjEdge& reference : adjacency) {
                const Label label =
                    graph.labels[static_cast<std::size_t>(reference.vertex)];
                if (label >= kLabelCount) continue;
                auto& events = events_by_label[label];
                const auto& edge =
                    graph.edges[static_cast<std::size_t>(reference.edge_id)];
                for (const Interval& interval : edge.active) {
                    events.emplace_back(
                        static_cast<std::int64_t>(interval.start), 1);
                    events.emplace_back(
                        static_cast<std::int64_t>(interval.end) + 1, -1);
                }
            }
            for (std::size_t label = 0; label < kLabelCount; ++label) {
                std::vector<CountSegment> segments =
                    buildCountSegments(std::move(events_by_label[label]));
                if (segments.empty()) continue;
                const std::uint64_t key = cellKey(
                    static_cast<int>(vertex), incoming, static_cast<Label>(label));
                if (mode_ == IndexMode::Ctinla) {
                    counters_.push_back({key, std::move(segments)});
                } else {
                    std::vector<Interval> active;
                    for (const CountSegment& segment : segments) {
                        if (!active.empty() &&
                            static_cast<std::int64_t>(active.back().end) + 1 ==
                                segment.start) {
                            active.back().end = segment.end;
                        } else {
                            active.push_back({segment.start, segment.end});
                        }
                    }
                    presence_.push_back({key, std::move(active)});
                }
            }
        }
    }
    const auto sortedByKey = [](const auto& lhs, const auto& rhs) {
        return lhs.key < rhs.key;
    };
    if (!std::is_sorted(presence_.begin(), presence_.end(), sortedByKey) ||
        !std::is_sorted(counters_.begin(), counters_.end(), sortedByKey)) {
        error = "internal error: neighbor-label index keys are not sorted";
        return false;
    }
    return true;
}

bool NeighborLabelIndex::supports(
    int vertex,
    bool incoming,
    Label neighbor_label,
    int required_count,
    int minimum_duration,
    DurationMode duration_mode) const {
    if (required_count <= 0) return true;
    const std::uint64_t key = cellKey(vertex, incoming, neighbor_label);
    if (mode_ == IndexMode::Tinla) {
        const auto found = std::lower_bound(
            presence_.begin(), presence_.end(), key,
            [](const PresenceCell& cell, std::uint64_t value) {
                return cell.key < value;
            });
        if (found == presence_.end() || found->key != key) return false;
        return duration(found->active, duration_mode) >= minimum_duration;
    }
    const auto found = std::lower_bound(
        counters_.begin(), counters_.end(), key,
        [](const CounterCell& cell, std::uint64_t value) {
            return cell.key < value;
        });
    if (found == counters_.end() || found->key != key) return false;
    std::vector<Interval> qualifying;
    for (const CountSegment& segment : found->active) {
        if (segment.count < required_count) continue;
        if (!qualifying.empty() &&
            static_cast<std::int64_t>(qualifying.back().end) + 1 ==
                segment.start) {
            qualifying.back().end = segment.end;
        } else {
            qualifying.push_back({segment.start, segment.end});
        }
    }
    return duration(qualifying, duration_mode) >= minimum_duration;
}

std::size_t NeighborLabelIndex::memoryBytes() const {
    std::size_t bytes = sizeof(NeighborLabelIndex);
    bytes += presence_.capacity() * sizeof(PresenceCell);
    for (const auto& cell : presence_) {
        bytes += cell.active.capacity() * sizeof(Interval);
    }
    bytes += counters_.capacity() * sizeof(CounterCell);
    for (const auto& cell : counters_) {
        bytes += cell.active.capacity() * sizeof(CountSegment);
    }
    return bytes;
}

std::size_t NeighborLabelIndex::cellCount() const {
    return presence_.size() + counters_.size();
}

std::size_t NeighborLabelIndex::segmentCount() const {
    std::size_t count = 0;
    for (const auto& cell : presence_) count += cell.active.size();
    for (const auto& cell : counters_) count += cell.active.size();
    return count;
}

IndexMode NeighborLabelIndex::mode() const { return mode_; }

Counters& Counters::operator+=(const Counters& other) {
    initial_candidates += other.initial_candidates;
    post_index_candidates += other.post_index_candidates;
    post_refine_candidates += other.post_refine_candidates;
    index_pruned += other.index_pruned;
    refine_pruned += other.refine_pruned;
    refine_rounds += other.refine_rounds;
    root_candidates += other.root_candidates;
    dfs_states += other.dfs_states;
    raw_extension_candidates += other.raw_extension_candidates;
    injectivity_pruned += other.injectivity_pruned;
    structural_pruned += other.structural_pruned;
    temporal_pruned += other.temporal_pruned;
    dynamic_refine_calls += other.dynamic_refine_calls;
    dynamic_refine_pruned += other.dynamic_refine_pruned;
    dynamic_refine_rounds += other.dynamic_refine_rounds;
    return *this;
}

std::vector<Interval> intersectIntervals(
    const std::vector<Interval>& lhs,
    const std::vector<Interval>& rhs) {
    std::vector<Interval> result;
    result.reserve(std::min(lhs.size(), rhs.size()));
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < lhs.size() && j < rhs.size()) {
        const int start = std::max(lhs[i].start, rhs[j].start);
        const int end = std::min(lhs[i].end, rhs[j].end);
        if (end >= start) result.push_back({start, end});
        if (lhs[i].end < rhs[j].end) {
            ++i;
        } else {
            ++j;
        }
    }
    return result;
}

std::int64_t duration(
    const std::vector<Interval>& intervals,
    DurationMode mode) {
    std::int64_t result = 0;
    for (const Interval& interval : intervals) {
        if (mode == DurationMode::Consecutive) {
            result = std::max(result, interval.length());
        } else {
            if (interval.length() > std::numeric_limits<std::int64_t>::max() - result) {
                return std::numeric_limits<std::int64_t>::max();
            }
            result += interval.length();
        }
    }
    return result;
}

#if !defined(DSM_E3E4_CORE_ONLY)
bool readTemporalGraph(
    const std::string& path,
    std::uint32_t seed,
    LabelMode label_mode,
    Graph& graph,
    double* read_ms,
    double* preprocess_ms,
    std::string& error,
    const std::vector<ValidatedRuntimeLabel>* explicit_labels,
    bool apply_consecutive_pair_filter,
    V8LoadTimings* v8_timings) {
    if (read_ms != nullptr) *read_ms = 0.0;
    if (preprocess_ms != nullptr) *preprocess_ms = 0.0;
    graph = {};
    const auto read_start = Clock::now();
    std::ifstream input(path);
    if (!input) {
        error = "cannot open temporal graph file: " + path;
        return false;
    }
    std::vector<RawEdge> raw;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        int from = 0;
        int to = 0;
        int time = 0;
        const LineStatus status = parseTemporalLine(line, from, to, time);
        if (status == LineStatus::Empty) continue;
        if (status == LineStatus::Invalid) {
            error = "temporal graph line " + std::to_string(line_number) +
                " must contain exactly three integers";
            return false;
        }
        if (from < 0 || to < 0) {
            error = "negative vertex ID on temporal graph line " +
                std::to_string(line_number);
            return false;
        }
        raw.push_back({from, to, time});
    }
    if (input.bad()) {
        error = "failed while reading temporal graph file: " + path;
        return false;
    }
    if (read_ms != nullptr) *read_ms = milliseconds(read_start);
    if (v8_timings != nullptr) {
        v8_timings->read_parse_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - read_start).count());
    }
    return materializeRawGraph(
        std::move(raw), seed, label_mode, explicit_labels,
        graph, preprocess_ms, error, apply_consecutive_pair_filter, v8_timings);
}
#endif  // !defined(DSM_E3E4_CORE_ONLY)

bool buildTemporalGraphFromValidatedInput(
    const std::vector<ValidatedTemporalOccurrence>& occurrences,
    const std::vector<ValidatedRuntimeLabel>& labels_by_external_id,
    Graph& graph,
    std::string& error) {
    std::vector<RawEdge> raw;
    raw.reserve(occurrences.size());
    for (const ValidatedTemporalOccurrence& row : occurrences) {
        if (row.from < 0 || row.to < 0) {
            error = "validated temporal occurrence contains a negative vertex ID";
            return false;
        }
        raw.push_back({row.from, row.to, row.time});
    }
    return materializeRawGraph(
        std::move(raw), 0U, LabelMode::SequentialRng,
        &labels_by_external_id, graph, nullptr, error);
}

#if !defined(DSM_E3E4_CORE_ONLY)
bool readQuery(const std::string& path, Query& query, std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "cannot open query graph file: " + path;
        return false;
    }
    query = {};
    std::vector<std::pair<std::string, int>> vertex_by_name;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        std::istringstream parser(line);
        std::string lhs;
        std::string rhs;
        std::string extra;
        if (!(parser >> lhs)) continue;
        if (!(parser >> rhs) || (parser >> extra)) {
            error = "query graph line " + std::to_string(line_number) +
                " must contain exactly two vertex tokens";
            return false;
        }
        auto parseToken = [&](const std::string& token, std::string& name, Label& label) {
            const std::size_t separator = token.rfind(':');
            name = separator == std::string::npos ? token : token.substr(0, separator);
            const std::string label_text = separator == std::string::npos
                ? token
                : token.substr(separator + 1);
            label = labelFromString(label_text);
            return !name.empty() && label != kInvalidLabel;
        };
        std::string lhs_name;
        std::string rhs_name;
        Label lhs_label = kInvalidLabel;
        Label rhs_label = kInvalidLabel;
        if (!parseToken(lhs, lhs_name, lhs_label) ||
            !parseToken(rhs, rhs_name, rhs_label)) {
            error = "query labels must be one of A, B, C, D, E";
            return false;
        }
        auto vertex = [&](const std::string& name, Label label) {
            const auto found = std::find_if(
                vertex_by_name.begin(), vertex_by_name.end(),
                [&](const auto& entry) { return entry.first == name; });
            if (found != vertex_by_name.end()) {
                return query.labels[static_cast<std::size_t>(found->second)] == label
                    ? found->second
                    : -1;
            }
            const int id = static_cast<int>(query.labels.size());
            vertex_by_name.emplace_back(name, id);
            query.names.push_back(name);
            query.labels.push_back(label);
            return id;
        };
        const int from = vertex(lhs_name, lhs_label);
        const int to = vertex(rhs_name, rhs_label);
        if (from < 0 || to < 0) {
            error = "a query vertex ID was used with conflicting labels";
            return false;
        }
        query.edges.emplace_back(from, to);
    }
    if (input.bad()) {
        error = "failed while reading query graph file: " + path;
        return false;
    }
    std::sort(query.edges.begin(), query.edges.end());
    query.edges.erase(std::unique(query.edges.begin(), query.edges.end()), query.edges.end());
    if (query.labels.empty() || query.edges.empty()) {
        error = "query graph contains no edge";
        return false;
    }
    query.out.resize(query.labels.size());
    query.in.resize(query.labels.size());
    for (const auto& edge : query.edges) {
        query.out[static_cast<std::size_t>(edge.first)].push_back(edge.second);
        query.in[static_cast<std::size_t>(edge.second)].push_back(edge.first);
    }
    return true;
}
#endif  // !defined(DSM_E3E4_CORE_ONLY)

namespace {

bool runMatcherImpl(
    const Graph& graph,
    const Query& query,
    const RunOptions& options,
    const NativePhaseObserver* legacy_observer,
    const RuntimeHookedNativePhaseObserver* runtime_observer,
    const dsm::e3e4::adapter_v1::runtime_hooks_v1::MonotonicNowSource* clock,
    const dsm::e3e4::adapter_v1::runtime_hooks_v1::WorkerAffinityBinder*
        affinity_binder,
    RunResult& result,
    std::string& error) {
    const auto total_start = Clock::now();
    result = {};
    result.threads_requested = options.threads;
    result.primary_metrics.scope =
        options.index_mode == IndexMode::Ctinla
        ? "ctinla_enumeration_and_count_reduction"
        : "tinla_enumeration_and_count_reduction";
    if (options.minimum_duration < 1) {
        error = "minimum duration must be at least 1";
        return false;
    }
    if (options.threads < 1 || options.threads > 1024) {
        error = "threads must be between 1 and 1024";
        return false;
    }
    if (query.labels.empty() || graph.labels.empty()) {
        error = "graph and query must be non-empty";
        return false;
    }
    if (!queryIsWeaklyConnected(query)) {
        error = "query graph must be weakly connected";
        return false;
    }
    const bool observed =
        legacy_observer != nullptr || runtime_observer != nullptr;
    if (observed && !options.count_only) {
        error = "native observed entry point requires count-only mode";
        return false;
    }
    if (observed && (clock == nullptr || affinity_binder == nullptr)) {
        error = "native observed entry point requires runtime hooks";
        return false;
    }
    auto record_empty_primary = [&]() {
        dsm::pure_online_v2::captureEnvironmentStart(
            result.primary_metrics);
        const auto start = dsm::pure_online_v2::beginInterval();
        const auto delta = dsm::pure_online_v2::endInterval(start);
        dsm::pure_online_v2::captureEnvironmentEnd(
            result.primary_metrics);
        dsm::pure_online_v2::addInterval(
            result.primary_metrics, delta);
        result.primary_metrics.parallel_interval_count = 1;
        result.primary_metrics.parallel_workers_with_tasks_min = 0;
        result.primary_metrics.parallel_workers_with_tasks_max = 0;
        result.primary_metrics.parallel_workers_with_tasks_sum = 0;
        result.primary_metrics.parallelism_eligible = false;
        result.timings.enumerate_ns = result.primary_metrics.wall_ns;
        result.timings.enumerate_ms =
            static_cast<double>(result.timings.enumerate_ns) /
            1'000'000.0;
    };

    std::vector<std::array<int, kLabelCount>> required_out;
    std::vector<std::array<int, kLabelCount>> required_in;
    const auto buildRequirements = [&]() {
        required_out.resize(query.labels.size());
        required_in.resize(query.labels.size());
        for (std::size_t q = 0; q < query.labels.size(); ++q) {
            required_out[q].fill(0);
            required_in[q].fill(0);
        }
        for (const auto& edge : query.edges) {
            ++required_out[static_cast<std::size_t>(edge.first)]
                [query.labels[static_cast<std::size_t>(edge.second)]];
            ++required_in[static_cast<std::size_t>(edge.second)]
                [query.labels[static_cast<std::size_t>(edge.first)]];
        }
    };
    if (observed) {
        if (!observeNativePhase(
                legacy_observer,
                runtime_observer,
                clock,
                NativeCountPhase::Requirements,
                error)) {
            return false;
        }
        buildRequirements();
        if (!observeNativePhase(
                legacy_observer,
                runtime_observer,
                clock,
                NativeCountPhase::IndexConstruction,
                error)) {
            return false;
        }
    }

    const auto index_start = Clock::now();
    NeighborLabelIndex index(options.index_mode);
    if (!index.build(graph, error)) return false;
    result.timings.build_index_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - index_start).count());
    result.timings.build_index_ms = milliseconds(index_start);
    result.graph_memory_bytes = graph.memoryBytes();
    result.index_memory_bytes = index.memoryBytes();
    result.index_cells = index.cellCount();
    result.index_segments = index.segmentCount();

    if (!observeNativePhase(
            legacy_observer,
            runtime_observer,
            clock,
            NativeCountPhase::CandidateFiltering,
            error)) {
        return false;
    }
    const auto filter_start = Clock::now();
    if (!observed) buildRequirements();
    std::vector<std::vector<int>> candidates(query.labels.size());
    for (std::size_t q = 0; q < query.labels.size(); ++q) {
        for (std::size_t vertex = 0; vertex < graph.labels.size(); ++vertex) {
            if (graph.labels[vertex] != query.labels[q]) continue;
            ++result.counters.initial_candidates;
            bool valid = true;
            for (std::size_t label = 0; label < kLabelCount; ++label) {
                if (!index.supports(
                        static_cast<int>(vertex),
                        false,
                        static_cast<Label>(label),
                        required_out[q][label],
                        options.minimum_duration,
                        options.duration_mode) ||
                    !index.supports(
                        static_cast<int>(vertex),
                        true,
                        static_cast<Label>(label),
                        required_in[q][label],
                        options.minimum_duration,
                        options.duration_mode)) {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                candidates[q].push_back(static_cast<int>(vertex));
                ++result.counters.post_index_candidates;
            } else {
                ++result.counters.index_pruned;
            }
        }
    }
    result.timings.filter_candidates_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - filter_start).count());
    result.timings.filter_candidates_ms = milliseconds(filter_start);
    const bool empty_after_filter = std::any_of(
        candidates.begin(), candidates.end(),
        [](const auto& values) { return values.empty(); });
    if (!observed && empty_after_filter) {
        // Preserve the historical API's actual worker-use accounting.
        result.threads_used = 1;
        record_empty_primary();
        result.peak_rss_bytes = peakRssBytes();
        result.timings.end_to_end_ms = milliseconds(total_start);
        return true;
    }

    if (!observeNativePhase(
            legacy_observer,
            runtime_observer,
            clock,
            NativeCountPhase::CandidateRefinement,
            error)) {
        return false;
    }
    const auto refine_start = Clock::now();
    const bool survived_refinement = !empty_after_filter &&
        refineCandidates(graph, query, options, candidates, result.counters, false);
    for (const auto& values : candidates) {
        result.counters.post_refine_candidates += values.size();
    }
    result.timings.refine_candidates_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - refine_start).count());
    result.timings.refine_candidates_ms = milliseconds(refine_start);
    if (!observed && !survived_refinement) {
        result.threads_used = 1;
        record_empty_primary();
        result.peak_rss_bytes = peakRssBytes();
        result.timings.end_to_end_ms = milliseconds(total_start);
        return true;
    }

    if (!observeNativePhase(
            legacy_observer,
            runtime_observer,
            clock,
            NativeCountPhase::OrderConstruction,
            error)) {
        return false;
    }
    const auto order_start = Clock::now();
    const std::vector<int> order = makeOrder(query, candidates);
    result.timings.order_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - order_start).count());
    const auto& roots = candidates[static_cast<std::size_t>(order.front())];
    result.counters.root_candidates = roots.size();
    if (!observeNativePhase(
            legacy_observer,
            runtime_observer,
            clock,
            NativeCountPhase::Enumeration,
            error)) {
        return false;
    }
    dsm::pure_online_v2::captureEnvironmentStart(
        result.primary_metrics);
    const auto primary_start = dsm::pure_online_v2::beginInterval();
    if (!options.count_only) {
        result.threads_used = 1;
        Searcher searcher(graph, query, options, order, candidates);
        for (const int root : roots) {
            ++result.primary_metrics.parallel_work_units_claimed;
            const std::uint64_t increment = searcher.runRoot(root, &result.matches);
            if (!checkedAdd(result.match_count, increment) || searcher.overflow()) {
                error = "match count exceeds uint64_t";
                return false;
            }
        }
        result.counters += searcher.counters();
        std::sort(
            result.matches.begin(), result.matches.end(),
            [](const Match& lhs, const Match& rhs) {
                return lhs.mapping < rhs.mapping;
            });
    } else {
        result.threads_used = options.threads;
        struct WorkerResult {
            std::uint64_t count = 0;
            Counters counters;
            bool overflow = false;
            std::uint64_t work_units_claimed = 0;
            std::optional<dsm::e3e4::adapter_v1::runtime_hooks_v1::
                WorkerAffinityObservation> affinity_observation;
        };
        std::vector<WorkerResult> workers(options.threads);
        std::vector<std::thread> threads;
        threads.reserve(options.threads);
        std::atomic<std::size_t> live_workers{0};
        std::atomic<std::size_t> peak_live_workers{0};
        std::atomic<std::size_t> productive_workers{0};
        std::atomic<std::size_t> peak_productive_workers{0};
        try {
            for (std::size_t worker = 0; worker < options.threads; ++worker) {
                threads.emplace_back([&, worker]() {
                    const std::size_t live_now =
                        live_workers.fetch_add(
                            1, std::memory_order_relaxed) + 1;
                    dsm::pure_online_v2::updateAtomicPeak(
                        peak_live_workers, live_now);
                    bool executing_work_unit = false;
                    auto begin_work_unit = [&]() {
                        executing_work_unit = true;
                        const std::size_t productive_now =
                            productive_workers.fetch_add(
                                1, std::memory_order_relaxed) + 1;
                        dsm::pure_online_v2::updateAtomicPeak(
                            peak_productive_workers,
                            productive_now);
                    };
                    auto end_work_unit = [&]() {
                        if (!executing_work_unit) return;
                        executing_work_unit = false;
                        productive_workers.fetch_sub(
                            1, std::memory_order_relaxed);
                    };
                    if (affinity_binder != nullptr) {
                        workers[worker].affinity_observation.emplace(
                            affinity_binder->bindCurrentWorkerToCpu(
                                static_cast<std::uint32_t>(worker)));
                        if (!workers[worker].affinity_observation
                                 ->bindSucceeded()) {
                            live_workers.fetch_sub(
                                1, std::memory_order_relaxed);
                            return;
                        }
                    }
                    Searcher searcher(graph, query, options, order, candidates);
                    for (std::size_t root_index = worker;
                         root_index < roots.size();
                         root_index += options.threads) {
                        begin_work_unit();
                        ++workers[worker].work_units_claimed;
                        const std::uint64_t increment =
                            searcher.runRoot(roots[root_index], nullptr);
                        end_work_unit();
                        if (!checkedAdd(workers[worker].count, increment)) {
                            workers[worker].overflow = true;
                            break;
                        }
                        if (searcher.overflow()) {
                            workers[worker].overflow = true;
                            break;
                        }
                    }
                    end_work_unit();
                    workers[worker].counters = searcher.counters();
                    live_workers.fetch_sub(1, std::memory_order_relaxed);
                });
                result.primary_metrics.worker_threads_created =
                    threads.size();
            }
        } catch (const std::system_error& exception) {
            for (auto& thread : threads) thread.join();
            error = "worker creation failed: " + std::string(exception.what());
            return false;
        }
        for (auto& thread : threads) thread.join();
        result.primary_metrics.peak_live_worker_threads =
            peak_live_workers.load(std::memory_order_relaxed);
        result.primary_metrics.workers_with_tasks =
            peak_productive_workers.load(std::memory_order_relaxed);
        if (!observeNativePhase(
                legacy_observer,
                runtime_observer,
                clock,
                NativeCountPhase::CountReduction,
                error)) {
            return false;
        }
        if (affinity_binder != nullptr) {
            result.affinity_cpu_set.reserve(workers.size());
            result.worker_affinity_observations.reserve(workers.size());
            for (std::size_t worker = 0; worker < workers.size(); ++worker) {
                if (!workers[worker].affinity_observation.has_value()) {
                    error = "worker " + std::to_string(worker) +
                        ": affinity observation missing";
                    return false;
                }
                const auto& observation =
                    *workers[worker].affinity_observation;
                if (!observation.bindSucceeded() ||
                    observation.observedCpuSet().size() != 1U ||
                    observation.observedCpuSet().front() !=
                        static_cast<std::uint32_t>(worker)) {
                    error = "worker " + std::to_string(worker) + ": " +
                        (observation.error().empty()
                             ? "observed affinity is not the requested singleton"
                             : observation.error());
                    return false;
                }
                result.affinity_cpu_set.push_back(
                    observation.observedCpuSet().front());
                result.worker_affinity_observations.push_back(
                    std::move(*workers[worker].affinity_observation));
            }
        }
        for (const WorkerResult& worker : workers) {
            dsm::pure_online_v2::checkedAdd(
                result.primary_metrics.parallel_work_units_claimed,
                worker.work_units_claimed);
            if (worker.overflow ||
                !checkedAdd(result.match_count, worker.count)) {
                result.match_count =
                    std::numeric_limits<std::uint64_t>::max();
                result.count_overflow = true;
            }
            result.counters += worker.counters;
        }
    }
    const auto primary_delta =
        dsm::pure_online_v2::endInterval(primary_start);
    dsm::pure_online_v2::captureEnvironmentEnd(result.primary_metrics);
    dsm::pure_online_v2::addInterval(
        result.primary_metrics, primary_delta);
    result.primary_metrics.parallel_interval_count = 1;
    result.primary_metrics.parallel_work_units_min = roots.size();
    result.primary_metrics.parallel_work_units_max = roots.size();
    result.primary_metrics.parallel_work_units_total = roots.size();
    if (roots.size() >= options.threads) {
        result.primary_metrics
            .parallel_intervals_with_at_least_requested_work = 1;
    }
    result.primary_metrics.parallel_workers_with_tasks_min =
        result.primary_metrics.workers_with_tasks;
    result.primary_metrics.parallel_workers_with_tasks_max =
        result.primary_metrics.workers_with_tasks;
    result.primary_metrics.parallel_workers_with_tasks_sum =
        result.primary_metrics.workers_with_tasks;
    result.primary_metrics.parallelism_eligible =
        result.primary_metrics
            .parallel_intervals_with_at_least_requested_work ==
        result.primary_metrics.parallel_interval_count;
    result.timings.enumerate_ns = result.primary_metrics.wall_ns;
    result.timings.enumerate_ms =
        static_cast<double>(result.timings.enumerate_ns) / 1'000'000.0;
    result.peak_rss_bytes = peakRssBytes();
    result.timings.end_to_end_ms = milliseconds(total_start);
    return true;
}

}  // namespace

#if !defined(DSM_E3E4_CORE_ONLY)
bool runMatcher(
    const Graph& graph,
    const Query& query,
    const RunOptions& options,
    RunResult& result,
    std::string& error) {
    return runMatcherImpl(
        graph,
        query,
        options,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        result,
        error);
}
#endif  // !defined(DSM_E3E4_CORE_ONLY)

#if !defined(DSM_E3E4_CORE_ONLY)
bool runCountOnlyObserved(
    const Graph& graph,
    const Query& query,
    const CountOnlyOptions& options,
    const NativePhaseObserver& observer,
    RunResult& result,
    std::string& error) {
    if (!observer) {
        error = "native phase observer is required";
        return false;
    }
    RunOptions count_options;
    count_options.minimum_duration = options.minimum_duration;
    count_options.duration_mode = options.duration_mode;
    count_options.index_mode = options.index_mode;
    count_options.threads = options.threads;
    count_options.count_only = true;
    const auto& clock =
        dsm::e3e4::adapter_v1::runtime_hooks_v1::
            productionMonotonicNowSource();
    const auto& affinity_binder =
        dsm::e3e4::adapter_v1::runtime_hooks_v1::
            productionWorkerAffinityBinder();
    return runMatcherImpl(
        graph,
        query,
        count_options,
        &observer,
        nullptr,
        &clock,
        &affinity_binder,
        result,
        error);
}
#endif  // !defined(DSM_E3E4_CORE_ONLY)

bool RuntimeHookedCountOnlyCore::runAdapterOwned(
    const Graph& graph,
    const Query& query,
    const CountOnlyOptions& options,
    const dsm::e3e4::adapter_v1::runtime_hooks_v1::MonotonicNowSource& clock,
    const dsm::e3e4::adapter_v1::runtime_hooks_v1::WorkerAffinityBinder&
        affinity_binder,
    const RuntimeHookedNativePhaseObserver& observer,
    RunResult& result,
    std::string& error) {
    if (!observer) {
        error = "runtime-hooked native phase observer is required";
        return false;
    }
    RunOptions count_options;
    count_options.minimum_duration = options.minimum_duration;
    count_options.duration_mode = options.duration_mode;
    count_options.index_mode = options.index_mode;
    count_options.threads = options.threads;
    count_options.count_only = true;
    return runMatcherImpl(
        graph,
        query,
        count_options,
        nullptr,
        &observer,
        &clock,
        &affinity_binder,
        result,
        error);
}

#if defined(DSM_E3E4_TEST_HOOKS)
bool e3e4_adapter_v1::test_support::TestOnlyCoreAccess::run(
    const Graph& graph,
    const Query& query,
    const CountOnlyOptions& options,
    const dsm::e3e4::adapter_v1::runtime_hooks_v1::MonotonicNowSource& clock,
    const dsm::e3e4::adapter_v1::runtime_hooks_v1::WorkerAffinityBinder&
        affinity_binder,
    const RuntimeHookedNativePhaseObserver& observer,
    RunResult& result,
    std::string& error) {
    return RuntimeHookedCountOnlyCore::runAdapterOwned(
        graph, query, options, clock, affinity_binder, observer,
        result, error);
}
#endif  // DSM_E3E4_TEST_HOOKS

}  // namespace tinla
