#include "JIndexExecution.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include "PhysicalKernels.h"
#include "WindowStore.h"

namespace {
using U64 = std::uint64_t;
constexpr U64 kIndexCap = 2ULL * 1024 * 1024 * 1024;
constexpr std::size_t kMinimumNeighbors = 64;
U64 addSize(U64 a, U64 b) {
    if (b > std::numeric_limits<U64>::max() - a)
        throw std::overflow_error("production J-index size overflow");
    return a + b;
}
U64 multiplySize(U64 a, U64 b) {
    if (b && a > std::numeric_limits<U64>::max() / b)
        throw std::overflow_error("production J-index size overflow");
    return a * b;
}
U64 elapsed(std::chrono::steady_clock::time_point begin) {
    return static_cast<U64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - begin).count());
}
U64 threadTime() {
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value))
        throw std::runtime_error("production J-index thread clock unavailable");
    return addSize(multiplySize(static_cast<U64>(value.tv_sec), 1000000000ULL),
                   static_cast<U64>(value.tv_nsec));
}
U64 baselineBytes(const Graph& graph) {
    U64 result = multiplySize(addSize(graph.adj.capacity(), graph.in_adj.capacity()),
                              sizeof(std::vector<Edge>));
    for (const auto& list : graph.adj) result = addSize(result, multiplySize(list.capacity(), sizeof(Edge)));
    for (const auto& list : graph.in_adj) result = addSize(result, multiplySize(list.capacity(), sizeof(Edge)));
    result = addSize(result, multiplySize(graph.temporal_edges.capacity(), sizeof(TemporalEdge)));
    for (const auto& edge : graph.temporal_edges)
        result = addSize(result, multiplySize(edge.activeIntervals().capacity(), sizeof(TimeInterval)));
    return result;
}
void accumulate(U64& value, U64 amount, bool& overflow) {
    if (amount > std::numeric_limits<U64>::max() - value) {
        value = std::numeric_limits<U64>::max();
        overflow = true;
    } else value += amount;
}
void atomicAdd(std::atomic<U64>& value, U64 amount, std::atomic<bool>& overflow) {
    U64 prior = value.load(std::memory_order_relaxed);
    for (;;) {
        const bool invalid = amount > std::numeric_limits<U64>::max() - prior;
        const U64 next = invalid ? std::numeric_limits<U64>::max() : prior + amount;
        if (value.compare_exchange_weak(prior, next, std::memory_order_relaxed)) {
            if (invalid) overflow.store(true, std::memory_order_relaxed);
            return;
        }
    }
}
void maximum(std::atomic<U64>& value, U64 next) {
    U64 prior = value.load(std::memory_order_relaxed);
    while (next > prior && !value.compare_exchange_weak(prior, next, std::memory_order_relaxed)) {}
}

#define EXECUTION_FIELDS(X) \
    X(states) X(index_selected) X(scan_selected) X(eligible_states) \
    X(fallback_no_driver) X(fallback_universal) X(fallback_small_degree) \
    X(empty_seed_states) X(empty_prior_states) X(cost_evaluated_states) X(cost_rejected_states) \
    X(c_idx_sum) X(c_scan_sum) X(selected_buckets) X(bucket_entries_consumed) \
    X(unique_driver_positions) X(td_membership_probes) X(selected_td_candidates)
struct Counters {
#define DECLARE(name) U64 name = 0;
    EXECUTION_FIELDS(DECLARE)
#undef DECLARE
};
void merge(Counters& output, const Counters& input, bool& overflow) {
#define MERGE(name) accumulate(output.name, input.name, overflow);
    EXECUTION_FIELDS(MERGE)
#undef MERGE
}
bool equal(const Counters& a, const Counters& b) {
#define EQUAL(name) if (a.name != b.name) return false;
    EXECUTION_FIELDS(EQUAL)
#undef EQUAL
    return true;
}
void writeCounters(std::ostream& out, const Counters& counters) {
    out << '{';
    bool first = true;
#define WRITE(name) if (!first) out << ','; first = false; out << "\"" #name "\":" << counters.name;
    EXECUTION_FIELDS(WRITE)
#undef WRITE
    out << '}';
}
} // namespace

struct JIndexExecution::Impl {
    struct Recipe {
        bool registered = false;
        std::uint32_t node = 0;
        int anchor = -1;
        bool outgoing = false;
        DriverOrigin origin = DriverOrigin::CompiledDag;
    };
    struct Index {
        U64 blocks = 0, entries = 0;
        std::unique_ptr<U64[]> offsets;
        std::unique_ptr<std::uint32_t[]> positions;
    };
    struct Local {
        Counters total;
        std::vector<Counters> nodes;
        bool overflow = false;
        explicit Local(std::size_t count) : nodes(count) {}
    };
    struct Aggregate {
        Counters total;
        std::vector<Counters> nodes;
        bool overflow = false;
        std::size_t workers = 0;
        explicit Aggregate(std::size_t count) : nodes(count) {}
    };
    const Graph& graph;
    const window_algebra::Domain domain;
    const int k;
    const std::size_t B;
    const Mode mode;
    const double alpha;
    const bool counters_enabled;
    const bool verify_superset;
    const WindowStore* window_source;
    const U64 baseline_bytes, cap, block_count, slots, generation;
    std::vector<Recipe> recipes;
    bool frozen = false;
    U64 setup_ns = 0;
    std::unique_ptr<std::atomic<Index*>[]> cache;
    mutable std::mutex build_mutex, registry_mutex;
    mutable std::vector<std::unique_ptr<Local>> registry;
    mutable std::atomic<bool> unavailable{false}, telemetry_overflow{false};
    mutable std::string unavailable_reason;
    mutable std::atomic<U64> persistent_bytes{0}, peak_bytes{0}, build_attempts{0}, built_indexes{0};
    mutable std::atomic<U64> build_wall_ns{0}, build_thread_ns{0};

    static U64 newGeneration() {
        static std::atomic<U64> next{1};
        const U64 result = next.fetch_add(1, std::memory_order_relaxed);
        if (!result || result == std::numeric_limits<U64>::max())
            throw std::overflow_error("production J-index generation exhausted");
        return result;
    }
    Impl(const Graph& g, const window_algebra::Domain& d, int duration,
         std::size_t block, Mode policy, double factor, std::size_t query_vertices, bool counts,
         bool verify, const WindowStore* source)
        : graph(g), domain(d), k(duration), B(block), mode(policy), alpha(factor), counters_enabled(counts),
          verify_superset(verify), window_source(source),
          baseline_bytes(baselineBytes(g)), cap(std::min(kIndexCap, baseline_bytes)),
          block_count(block ? d.startCount() / block + (d.startCount() % block != 0) : 0),
          slots(multiplySize(g.num_vertices < 0 ? 0 : static_cast<U64>(g.num_vertices), 2)),
          generation(newGeneration()), recipes(query_vertices) {
        if (k < 1 || d.k() != k || (B != 8 && B != 16 && B != 32 && B != 64 && B != 128))
            throw std::invalid_argument("production J-index requires matching k and B=8/16/32/64/128");
        if (alpha != .5 && alpha != 1. && alpha != 2.)
            throw std::invalid_argument("production J-index alpha must be 0.5, 1, or 2");
        if (window_source && !domain.sameAs(window_source->domain()))
            throw std::invalid_argument("adjacency index WindowStore domain differs");
        if (g.num_vertices < 0 || graph.adj.size() != static_cast<std::size_t>(g.num_vertices)
            || graph.in_adj.size() != static_cast<std::size_t>(g.num_vertices))
            throw std::invalid_argument("production J-index graph adjacency size differs");
        const U64 pointer_bytes = addSize(sizeof(cache), multiplySize(slots, sizeof(std::atomic<Index*>)));
        if (pointer_bytes > cap || slots > std::numeric_limits<std::size_t>::max()) {
            unavailable.store(true);
            unavailable_reason = "cache_pointer_array_exceeds_memory_cap";
            throw std::runtime_error("out of memory: production J-index cache pointer array exceeds index budget");
        }
        cache.reset(new std::atomic<Index*>[static_cast<std::size_t>(slots)]);
        for (U64 i = 0; i < slots; ++i) cache[static_cast<std::size_t>(i)].store(nullptr, std::memory_order_relaxed);
        persistent_bytes.store(pointer_bytes);
        peak_bytes.store(pointer_bytes);
    }
    ~Impl() {
        if (cache) for (U64 i = 0; i < slots; ++i)
            delete cache[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
    }
    U64 offset(std::int64_t timestamp) const {
        // Unsigned subtraction is exact for ordered endpoints even when the
        // original signed time axis straddles zero or is wider than INT64_MAX.
        return static_cast<U64>(timestamp) - static_cast<U64>(domain.minimum());
    }
    template<class Function>
    void historyBlocks(const std::vector<TimeInterval>& intervals, Function function) const {
        if (!domain.startCount()) return;
        bool prior = false;
        U64 last_seen = 0;
        for (const auto& interval : intervals) {
            const std::int64_t last_start = static_cast<std::int64_t>(interval.end) - k + 1;
            if (last_start < interval.start) continue;
            const auto first_time = std::max<std::int64_t>(interval.start, domain.minimum());
            const auto last_time = std::min(last_start, domain.lastStart());
            if (first_time > last_time) continue;
            U64 first = offset(first_time) / B;
            const U64 last = offset(last_time) / B;
            if (prior && first <= last_seen) first = last_seen + 1;
            if (first > last) continue;
            function(first, last);
            last_seen = last;
            prior = true;
        }
    }
    template<class Function>
    void edgeBlocks(int id, Function function) const {
        if (id < 0 || static_cast<std::size_t>(id) >= graph.temporal_edges.size())
            throw std::logic_error("adjacency index invalid temporal edge ID");
        if (!window_source) {
            historyBlocks(graph.temporal_edges[static_cast<std::size_t>(id)].activeIntervals(), function);
            return;
        }
        // v6 adjacency-index control: retain the v5 adjacency CSR and its TD
        // membership probe. Only the edge bucket-membership source changes to
        // the immutable, edge-ID-addressed W-store; no history is re-trimmed.
        const auto view = window_source->get(id);
        if (!domain.sameAs(view.domain())) throw std::logic_error("adjacency index W domain mismatch");
        bool seen = false;
        U64 last = 0;
        auto emit = [&](U64 first, U64 end) {
            if (end >= block_count) throw std::logic_error("adjacency index W block outside domain");
            if (seen && first <= last) first = last + 1;
            if (first <= end) function(first, end);
            seen = true;
            last = end;
        };
        if (view.isEmpty()) return;
        if (view.isUniversal()) throw std::logic_error("edge W-store cannot be universal");
        if (view.kind() == window_algebra::Kind::Dense) {
            for (std::size_t word = 0; word < view.wordCount(); ++word) {
                U64 bits = view.words()[word];
                while (bits) {
                    const auto bit = static_cast<unsigned>(__builtin_ctzll(bits));
                    const U64 start = addSize(multiplySize(word, 64), bit);
                    if (start >= domain.startCount()) throw std::logic_error("adjacency index W padding");
                    emit(start / B, start / B);
                    bits &= bits - 1;
                }
            }
        } else {
            const auto intervals = view.starts();
            for (std::size_t i = 0; i < intervals.size; ++i)
                emit(offset(intervals.data[i].start) / B, offset(intervals.data[i].end) / B);
        }
    }
    std::vector<U64> priorBlocks(window_algebra::WindowView prior) const {
        std::vector<U64> result;
        auto append = [&](U64 block) {
            if (block >= block_count) throw std::logic_error("production J-index block outside time domain");
            if (result.empty() || result.back() != block) result.push_back(block);
        };
        if (prior.kind() == window_algebra::Kind::Dense) {
            for (std::size_t word = 0; word < prior.wordCount(); ++word) {
                U64 bits = prior.words()[word];
                while (bits) {
                    const auto bit = static_cast<unsigned>(__builtin_ctzll(bits));
                    const U64 start = addSize(multiplySize(word, 64), bit);
                    if (start >= domain.startCount()) throw std::logic_error("production J-index nonzero padding");
                    append(start / B);
                    bits &= bits - 1;
                }
            }
        } else if (prior.kind() == window_algebra::Kind::Intervals) {
            const auto intervals = prior.starts();
            for (std::size_t i = 0; i < intervals.size; ++i) {
                const U64 first = offset(intervals.data[i].start) / B;
                const U64 last = offset(intervals.data[i].end) / B;
                for (U64 b = first; b <= last; ++b) append(b);
            }
        } else if (!prior.isEmpty()) {
            throw std::logic_error("production J-index universal prior should scan");
        }
        return result;
    }
    // Independent test/debug oracle: intersect true window-start intervals with
    // the incoming J, without using CSR buckets or the production W kernels.
    // A bucket hit is allowed to be a false positive, never a false negative.
    bool historyOverlapsPrior(const std::vector<TimeInterval>& history,
                              window_algebra::WindowView prior) const {
        if (prior.isEmpty()) return false;
        for (const auto& interval : history) {
            const std::int64_t last_start = static_cast<std::int64_t>(interval.end) - k + 1;
            const auto first = std::max<std::int64_t>(interval.start, domain.minimum());
            const auto last = std::min(last_start, domain.lastStart());
            if (first > last) continue;
            if (prior.isUniversal()) return true;
            if (prior.kind() == window_algebra::Kind::Intervals) {
                const auto starts = prior.starts();
                for (std::size_t i = 0; i < starts.size; ++i) {
                    if (starts.data[i].start > last) break;
                    if (starts.data[i].end >= first) return true;
                }
            } else if (prior.kind() == window_algebra::Kind::Dense) {
                const U64 lo = offset(first), hi = offset(last);
                const U64 lo_word = lo / 64, hi_word = hi / 64;
                for (U64 word = lo_word; word <= hi_word; ++word) {
                    if (word >= prior.wordCount())
                        throw std::logic_error("production J-index debug prior word outside domain");
                    U64 mask = std::numeric_limits<U64>::max();
                    if (word == lo_word) mask &= std::numeric_limits<U64>::max() << (lo % 64);
                    if (word == hi_word && hi % 64 != 63) mask &= (U64{1} << (hi % 64 + 1)) - 1;
                    if (prior.words()[word] & mask) return true;
                }
            } else {
                throw std::logic_error("production J-index debug unexpected prior representation");
            }
        }
        return false;
    }
    void verifySelection(int q, CandidateReadView seed, const std::vector<Edge>& adjacent,
                         window_algebra::WindowView prior, const Selection& selected) const {
        if (!verify_superset || !selected.used) return;
        if (selected.candidates.size() != selected.driver_entries.size())
            throw std::logic_error("production J-index debug parallel selection sizes differ");
        for (std::size_t i = 0; i < seed.size(); ++i) {
            const int candidate = seed[i];
            const auto found = std::lower_bound(adjacent.begin(), adjacent.end(), candidate,
                [](const Edge& edge, int vertex) { return edge.to < vertex; });
            if (found == adjacent.end() || found->to != candidate) continue;
            if (found->temporal_edge_id < 0 ||
                static_cast<std::size_t>(found->temporal_edge_id) >= graph.temporal_edges.size())
                throw std::logic_error("production J-index debug invalid driver history ID");
            if (historyOverlapsPrior(graph.temporal_edges[found->temporal_edge_id].activeIntervals(), prior)
                && !std::binary_search(selected.candidates.begin(), selected.candidates.end(), candidate))
                throw std::logic_error("production J-index superset violation at query=" +
                    std::to_string(q) + " candidate=" + std::to_string(candidate));
        }
    }
    const Index* getIndex(int anchor, bool outgoing) const {
        if (unavailable.load(std::memory_order_relaxed))
            throw std::runtime_error("out of memory: production J-index index budget previously exceeded");
        const auto key = static_cast<std::size_t>(anchor) * 2 + (outgoing ? 1 : 0);
        if (auto* value = cache[key].load(std::memory_order_acquire)) return value;
        std::lock_guard<std::mutex> lock(build_mutex);
        if (unavailable.load(std::memory_order_relaxed))
            throw std::runtime_error("out of memory: production J-index index budget previously exceeded");
        if (auto* value = cache[key].load(std::memory_order_acquire)) return value;
        const auto begin = std::chrono::steady_clock::now();
        const auto cpu_begin = threadTime();
        atomicAdd(build_attempts, 1, telemetry_overflow);
        auto finish = [&]() {
            atomicAdd(build_wall_ns, elapsed(begin), telemetry_overflow);
            atomicAdd(build_thread_ns, threadTime() - cpu_begin, telemetry_overflow);
        };
        auto reject = [&](const char* reason) -> const Index* {
            unavailable_reason = reason;
            unavailable.store(true, std::memory_order_relaxed);
            finish();
            throw std::runtime_error(std::string("out of memory: production J-index index budget exceeded: ") + reason);
        };
        const auto& adjacent = (outgoing ? graph.adj : graph.in_adj)[static_cast<std::size_t>(anchor)];
        if (adjacent.size() > std::numeric_limits<std::uint32_t>::max())
            return reject("neighbor_position_exceeds_uint32");
        const U64 offsets_bytes = multiplySize(addSize(block_count, 1), sizeof(U64));
        const U64 scratch_bytes = multiplySize(block_count, sizeof(U64));
        const U64 base = addSize(sizeof(Index), offsets_bytes);
        if (addSize(addSize(persistent_bytes.load(), base), scratch_bytes) > cap)
            return reject("csr_plus_live_build_scratch_exceeds_memory_cap");
        U64 entries = 0;
        for (const auto& adjacency : adjacent) {
            if (adjacency.temporal_edge_id < 0 ||
                static_cast<std::size_t>(adjacency.temporal_edge_id) >= graph.temporal_edges.size())
                throw std::logic_error("production J-index invalid edge ID");
            edgeBlocks(adjacency.temporal_edge_id, [&](U64 first, U64 last) {
                entries = addSize(entries, last - first + 1);
            });
        }
        const U64 owned = addSize(base, multiplySize(entries, sizeof(std::uint32_t)));
        const U64 persistent_after = addSize(persistent_bytes.load(), owned);
        const U64 peak = addSize(persistent_after, scratch_bytes);
        if (peak > cap || block_count > std::numeric_limits<std::size_t>::max() - 1 ||
            entries > std::numeric_limits<std::size_t>::max())
            return reject("csr_plus_live_build_scratch_exceeds_memory_cap");
        auto built = std::make_unique<Index>();
        built->blocks = block_count;
        built->entries = entries;
        built->offsets.reset(new U64[static_cast<std::size_t>(block_count + 1)]{});
        built->positions.reset(new std::uint32_t[static_cast<std::size_t>(entries)]);
        std::unique_ptr<U64[]> cursor(new U64[static_cast<std::size_t>(block_count)]{});
        for (const auto& adjacency : adjacent) {
            edgeBlocks(adjacency.temporal_edge_id, [&](U64 first, U64 last) {
                for (U64 b = first; b <= last; ++b) ++built->offsets[b + 1];
            });
        }
        for (U64 b = 0; b < block_count; ++b) {
            built->offsets[b + 1] += built->offsets[b];
            cursor[b] = built->offsets[b];
        }
        if (built->offsets[block_count] != entries)
            throw std::logic_error("production J-index CSR counting mismatch");
        for (std::size_t position = 0; position < adjacent.size(); ++position) {
            edgeBlocks(adjacent[position].temporal_edge_id, [&](U64 first, U64 last) {
                for (U64 b = first; b <= last; ++b)
                    built->positions[cursor[b]++] = static_cast<std::uint32_t>(position);
            });
        }
        for (U64 b = 0; b < block_count; ++b)
            if (cursor[b] != built->offsets[b + 1])
                throw std::logic_error("production J-index CSR fill mismatch");
        const auto* result = built.get();
        cache[key].store(built.release(), std::memory_order_release);
        persistent_bytes.store(persistent_after);
        maximum(peak_bytes, peak);
        atomicAdd(built_indexes, 1, telemetry_overflow);
        finish();
        return result;
    }
    Local& local() const {
        thread_local std::unordered_map<U64, Local*> registered;
        const auto found = registered.find(generation);
        if (found != registered.end()) return *found->second;
        auto fresh = std::make_unique<Local>(recipes.size());
        auto* pointer = fresh.get();
        {
            std::lock_guard<std::mutex> lock(registry_mutex);
            registry.push_back(std::move(fresh));
        }
        registered.emplace(generation, pointer);
        return *pointer;
    }
    static void count(Local* local, std::size_t q, U64 Counters::* field, U64 amount = 1) {
        if (!local) return;
        accumulate(local->total.*field, amount, local->overflow);
        accumulate(local->nodes[q].*field, amount, local->overflow);
    }
    Aggregate aggregate() const {
        Aggregate result(recipes.size());
        std::lock_guard<std::mutex> lock(registry_mutex);
        result.workers = registry.size();
        for (const auto& local : registry) {
            result.overflow = result.overflow || local->overflow;
            merge(result.total, local->total, result.overflow);
            for (std::size_t q = 0; q < recipes.size(); ++q)
                merge(result.nodes[q], local->nodes[q], result.overflow);
        }
        Counters sum;
        for (const auto& node : result.nodes) merge(sum, node, result.overflow);
        if (!equal(result.total, sum)) throw std::logic_error("production J-index node/worker totals differ");
        if (!result.overflow && result.total.states != result.total.index_selected + result.total.scan_selected)
            throw std::logic_error("production J-index selection accounting differs");
        return result;
    }
};

JIndexExecution::JIndexExecution(const Graph& graph, const window_algebra::Domain& domain,
    int k, std::size_t B, Mode mode, double alpha, std::size_t vertices, bool counters, bool verify,
    const WindowStore* source) {
    const auto begin = std::chrono::steady_clock::now();
    impl_ = std::make_unique<Impl>(graph, domain, k, B, mode, alpha, vertices, counters, verify, source);
    impl_->setup_ns = elapsed(begin);
}
JIndexExecution::~JIndexExecution() = default;

void JIndexExecution::registerDriver(int q, std::uint32_t node, int anchor,
                                    bool outgoing, DriverOrigin origin) {
    const auto begin = std::chrono::steady_clock::now();
    if (impl_->frozen || q < 0 || static_cast<std::size_t>(q) >= impl_->recipes.size()
        || anchor < 0 || static_cast<std::size_t>(anchor) >= impl_->recipes.size() || q == anchor)
        throw std::invalid_argument("production J-index invalid/late driver registration");
    auto& recipe = impl_->recipes[static_cast<std::size_t>(q)];
    if (recipe.registered && (recipe.node != node || recipe.anchor != anchor
        || recipe.outgoing != outgoing || recipe.origin != origin))
        throw std::logic_error("production J-index conflicting fixed driver");
    recipe = {true, node, anchor, outgoing, origin};
    impl_->setup_ns = addSize(impl_->setup_ns, elapsed(begin));
}
void JIndexExecution::freezeDrivers() {
    const auto begin = std::chrono::steady_clock::now();
    impl_->frozen = true;
    impl_->setup_ns = addSize(impl_->setup_ns, elapsed(begin));
}

JIndexExecution::Selection JIndexExecution::select(int q, std::uint32_t node, int anchor,
    bool outgoing, CandidateReadView seed, const std::vector<CandidateReadView>& neighbors,
    window_algebra::WindowView prior) const {
    if (!impl_->frozen || q < 0 || static_cast<std::size_t>(q) >= impl_->recipes.size())
        throw std::logic_error("production J-index select before freeze/invalid query");
    if (!impl_->domain.sameAs(prior.domain()))
        throw std::invalid_argument("production J-index prior time domain differs");
    const auto query = static_cast<std::size_t>(q);
    auto* local = impl_->counters_enabled ? &impl_->local() : nullptr;
    Impl::count(local, query, &Counters::states);
    Selection selected;
    auto fallback = [&](U64 Counters::* reason) {
        Impl::count(local, query, &Counters::scan_selected);
        Impl::count(local, query, reason);
        return std::move(selected);
    };
    const auto& recipe = impl_->recipes[query];
    if (!recipe.registered) return fallback(&Counters::fallback_no_driver);
    if (recipe.node != node || recipe.outgoing != outgoing)
        throw std::logic_error("production J-index request differs from registered driver");
    if (prior.isUniversal()) return fallback(&Counters::fallback_universal);
    if (anchor < 0 || anchor >= impl_->graph.num_vertices)
        throw std::out_of_range("production J-index invalid mapped anchor");
    const auto& adjacent = (outgoing ? impl_->graph.adj : impl_->graph.in_adj)[static_cast<std::size_t>(anchor)];
    if (adjacent.size() < kMinimumNeighbors) return fallback(&Counters::fallback_small_degree);
    Impl::count(local, query, &Counters::eligible_states);
    if (seed.empty() || prior.isEmpty()) {
        selected.used = true;
        Impl::count(local, query, &Counters::index_selected);
        Impl::count(local, query, seed.empty() ? &Counters::empty_seed_states : &Counters::empty_prior_states);
        impl_->verifySelection(q, seed, adjacent, prior, selected);
        return selected;
    }
    const auto* index = impl_->getIndex(anchor, outgoing);
    const auto blocks = impl_->priorBlocks(prior);
    for (U64 b : blocks)
        selected.c_idx = addSize(selected.c_idx, index->offsets[b + 1] - index->offsets[b]);
    if (impl_->mode == Mode::Adaptive || local) {
        const auto costs = PhysicalKernels::stateCosts(seed.size(), neighbors);
        selected.c_scan = std::min(costs.probe, costs.merge);
        selected.costs_evaluated = true;
        Impl::count(local, query, &Counters::cost_evaluated_states);
        Impl::count(local, query, &Counters::c_idx_sum, selected.c_idx);
        Impl::count(local, query, &Counters::c_scan_sum, selected.c_scan);
    }
    if (impl_->mode == Mode::Adaptive &&
        !PhysicalKernels::chooseProbe({selected.c_idx, selected.c_scan}, impl_->alpha))
        return fallback(&Counters::cost_rejected_states);
    selected.used = true;
    struct Cursor { U64 next, end; std::uint32_t position; };
    auto later = [](const Cursor& a, const Cursor& b) { return a.position > b.position; };
    std::vector<Cursor> heap;
    heap.reserve(blocks.size());
    for (U64 b : blocks)
        if (index->offsets[b] != index->offsets[b + 1])
            heap.push_back({index->offsets[b], index->offsets[b + 1], index->positions[index->offsets[b]]});
    std::make_heap(heap.begin(), heap.end(), later);
    const std::size_t reserve = std::min(seed.size(), adjacent.size());
    selected.candidates.reserve(reserve);
    selected.driver_entries.reserve(reserve);
    bool have_previous = false;
    std::uint32_t previous = 0;
    U64 unique = 0;
    std::size_t seed_cursor = 0;
    while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), later);
        auto cursor = heap.back();
        heap.pop_back();
        const auto position = cursor.position;
        if (!have_previous || position != previous) {
            const auto& entry = adjacent[position];
            seed_cursor = PhysicalKernels::lowerBound(seed, entry.to, seed_cursor);
            if (seed_cursor < seed.size() && seed[seed_cursor] == entry.to) {
                selected.candidates.push_back(entry.to);
                selected.driver_entries.push_back(&entry);
            }
            if (local) ++unique;
            previous = position;
            have_previous = true;
        }
        if (++cursor.next < cursor.end) {
            cursor.position = index->positions[cursor.next];
            heap.push_back(cursor);
            std::push_heap(heap.begin(), heap.end(), later);
        }
    }
    if (local) {
        Impl::count(local, query, &Counters::index_selected);
        Impl::count(local, query, &Counters::selected_buckets, blocks.size());
        Impl::count(local, query, &Counters::bucket_entries_consumed, selected.c_idx);
        Impl::count(local, query, &Counters::unique_driver_positions, unique);
        Impl::count(local, query, &Counters::td_membership_probes, unique);
        Impl::count(local, query, &Counters::selected_td_candidates, selected.candidates.size());
    }
    impl_->verifySelection(q, seed, adjacent, prior, selected);
    return selected;
}

std::uint64_t JIndexExecution::setupNanoseconds() const { return impl_->setup_ns; }
std::uint64_t JIndexExecution::lazyBuildThreadNanoseconds() const { return impl_->build_thread_ns.load(); }
std::uint64_t JIndexExecution::persistentIndexBytes() const { return impl_->persistent_bytes.load(); }
std::uint64_t JIndexExecution::peakIndexBytes() const { return impl_->peak_bytes.load(); }
bool JIndexExecution::memoryUnavailable() const { return impl_->unavailable.load(); }

void JIndexExecution::writeJson(std::ostream& out) const {
    const auto aggregate = impl_->aggregate();
    const bool overflow = aggregate.overflow || impl_->telemetry_overflow.load();
    out << "{\"schema\":\"v5_jindex_execution_v1\",\"production\":true,\"observer_only\":false"
        << ",\"mode\":\"" << (impl_->mode == Mode::Adaptive ? "adaptive" : "always") << "\""
        << ",\"B\":" << impl_->B << ",\"alpha\":" << impl_->alpha
        << ",\"n_min\":" << kMinimumNeighbors << ",\"k\":" << impl_->k
        << ",\"counters_enabled\":" << (impl_->counters_enabled ? "true" : "false")
        << ",\"verify_superset\":" << (impl_->verify_superset ? "true" : "false")
        << ",\"window_source\":\"" << (impl_->window_source ? "window_store" : "edge_history") << "\""
        << ",\"plans_frozen\":" << (impl_->frozen ? "true" : "false")
        << ",\"status\":\"" << (overflow ? "UNAVAILABLE_COUNTER_OVERFLOW" :
            impl_->unavailable.load() ? "UNAVAILABLE_MEMORY_OR_REPRESENTATION" : "OK") << "\""
        << ",\"memory_error_reason\":\"" << impl_->unavailable_reason << "\""
        << ",\"counter_overflow\":" << (overflow ? "true" : "false")
        << ",\"time_min\":" << impl_->domain.minimum() << ",\"time_max\":" << impl_->domain.maximum()
        << ",\"window_start_count\":" << impl_->domain.startCount()
        << ",\"baseline_adjacency_temporal_capacity_bytes\":" << impl_->baseline_bytes
        << ",\"cap_bytes\":" << impl_->cap << ",\"persistent_index_bytes\":" << impl_->persistent_bytes.load()
        << ",\"peak_index_plus_build_scratch_bytes\":" << impl_->peak_bytes.load()
        << ",\"setup_ns\":" << impl_->setup_ns << ",\"build_attempts\":" << impl_->build_attempts.load()
        << ",\"built_indexes\":" << impl_->built_indexes.load()
        << ",\"build_wall_ns_sum\":" << impl_->build_wall_ns.load()
        << ",\"build_thread_cpu_ns_sum\":" << impl_->build_thread_ns.load()
        << ",\"registered_counter_workers\":" << aggregate.workers
        << ",\"thread_totals_equal_node_sums\":true"
        << ",\"accounting\":\"index cache pointer array, Index headers, CSR arrays and live build cursor; "
           "excludes immutable graph/W, static recipes, optional counter registry and per-state selection output/heap\""
        << ",\"time_note\":\"setup outside enumeration; lazy build inside enumeration; build wall/CPU sums are not union wall\""
        << ",\"cost_note\":\"original TD seed/all closed-neighbor lengths; always computes c_scan only with counters ON\""
        << ",\"budget_failure_policy\":\"throw out of memory; never silently select scan\""
        << ",\"totals\":";
    if (impl_->counters_enabled) writeCounters(out, aggregate.total); else out << "null";
    out << ",\"nodes\":[";
    for (std::size_t q = 0; q < impl_->recipes.size(); ++q) {
        if (q) out << ',';
        const auto& recipe = impl_->recipes[q];
        out << "{\"q\":" << q << ",\"registered\":" << (recipe.registered ? "true" : "false")
            << ",\"node_id\":";
        if (recipe.registered) out << recipe.node; else out << "null";
        out << ",\"anchor_query\":";
        if (recipe.registered) out << recipe.anchor; else out << "null";
        out << ",\"direction\":";
        if (recipe.registered) out << (recipe.outgoing ? "\"out\"" : "\"in\""); else out << "null";
        out << ",\"origin\":";
        if (recipe.registered) out << (recipe.origin == DriverOrigin::CompiledDag ? "\"compiled_dag\"" : "\"direct_recipe\"");
        else out << "null";
        out << ",\"counts\":";
        if (impl_->counters_enabled) writeCounters(out, aggregate.nodes[q]); else out << "null";
        out << '}';
    }
    out << "]}";
}
