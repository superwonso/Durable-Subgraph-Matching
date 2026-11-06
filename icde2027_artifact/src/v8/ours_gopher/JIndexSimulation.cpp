#include "JIndexSimulation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <limits>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include "PhysicalKernels.h"
#include "WindowAlgebra.h"

namespace {

using U64 = std::uint64_t;
U64 plus(U64 a, U64 b) {
    if (b > std::numeric_limits<U64>::max() - a)
        throw std::overflow_error("J-index diagnostic size overflow");
    return a + b;
}
U64 times(U64 a, U64 b) {
    if (b && a > std::numeric_limits<U64>::max() / b)
        throw std::overflow_error("J-index diagnostic size overflow");
    return a * b;
}
U64 threadCpuNs() {
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0)
        throw std::runtime_error("J-index thread CPU clock unavailable");
    return plus(times(static_cast<U64>(value.tv_sec), 1000000000ULL),
                static_cast<U64>(value.tv_nsec));
}
U64 elapsedNs(std::chrono::steady_clock::time_point start) {
    return static_cast<U64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count());
}
int binding(int q, const SearchState& state) {
    return q < 0 || static_cast<std::size_t>(q) >= state.mapping.size()
        ? -1 : state.mapping[static_cast<std::size_t>(q)];
}
bool outgoing(DagOp op) {
    return op == DagOp::OutNeighborLookup || op == DagOp::OutNeighborView;
}
bool incoming(DagOp op) {
    return op == DagOp::InNeighborLookup || op == DagOp::InNeighborView;
}
window_algebra::Domain fullDomain(const Graph& graph, int k) {
    bool found = false;
    std::int64_t minimum = 0, maximum = -1;
    for (const auto& edge : graph.temporal_edges) for (const auto& interval : edge.activeIntervals()) {
        if (!found) { minimum = interval.start; maximum = interval.end; found = true; }
        else { minimum = std::min(minimum, static_cast<std::int64_t>(interval.start));
               maximum = std::max(maximum, static_cast<std::int64_t>(interval.end)); }
    }
    return window_algebra::Domain::make(minimum, maximum, k);
}
U64 adjacencyTemporalBytes(const Graph& graph) {
    U64 bytes = times(plus(graph.adj.capacity(), graph.in_adj.capacity()), sizeof(std::vector<Edge>));
    for (const auto& edges : graph.adj) bytes = plus(bytes, times(edges.capacity(), sizeof(Edge)));
    for (const auto& edges : graph.in_adj) bytes = plus(bytes, times(edges.capacity(), sizeof(Edge)));
    bytes = plus(bytes, times(graph.temporal_edges.capacity(), sizeof(TemporalEdge)));
    for (const auto& edge : graph.temporal_edges)
        bytes = plus(bytes, times(edge.activeIntervals().capacity(), sizeof(TimeInterval)));
    return bytes;
}

#define JINDEX_FIELDS(X) \
    X(states) X(scan_touched) X(idx_touched) X(passed) \
    X(fallback_no_driver) X(fallback_no_j) X(fallback_small_degree) \
    X(unavailable_states) X(indexed_states) X(cost_evaluated_states) \
    X(index_cost_le_scan_states) X(c_idx_sum) X(c_scan_sum)

struct Counters {
#define DECLARE_COUNTER(name) U64 name = 0;
    JINDEX_FIELDS(DECLARE_COUNTER)
#undef DECLARE_COUNTER
};
void add(std::atomic<U64>& value, U64 amount, std::atomic<bool>& invalid) {
    U64 old = value.load(std::memory_order_relaxed);
    for (;;) {
        const bool overflow = amount > std::numeric_limits<U64>::max() - old;
        const U64 next = overflow ? std::numeric_limits<U64>::max() : old + amount;
        if (value.compare_exchange_weak(old, next, std::memory_order_relaxed)) {
            if (overflow) invalid.store(true, std::memory_order_relaxed);
            return;
        }
    }
}
void maximum(std::atomic<U64>& value, U64 next) {
    U64 old = value.load(std::memory_order_relaxed);
    while (next > old && !value.compare_exchange_weak(old, next, std::memory_order_relaxed)) {}
}
void addPlain(U64& value, U64 amount, bool& invalid) {
    if (amount > std::numeric_limits<U64>::max() - value) {
        value = std::numeric_limits<U64>::max();
        invalid = true;
    } else value += amount;
}
void mergeCounters(Counters& result, const Counters& input, bool& invalid) {
#define MERGE_COUNTER(name) addPlain(result.name, input.name, invalid);
    JINDEX_FIELDS(MERGE_COUNTER)
#undef MERGE_COUNTER
}
bool sameCounters(const Counters& lhs, const Counters& rhs) {
#define SAME_COUNTER(name) if (lhs.name != rhs.name) return false;
    JINDEX_FIELDS(SAME_COUNTER)
#undef SAME_COUNTER
    return true;
}
void countersJson(std::ostream& out, const Counters& counters) {
    out << '{';
    bool first = true;
#define WRITE_COUNTER(name) \
    if (!first) { out << ','; } first = false; \
    out << "\"" #name "\":" << counters.name;
    JINDEX_FIELDS(WRITE_COUNTER)
#undef WRITE_COUNTER
    out << '}';
}

}  // namespace

struct JIndexSimulation::Impl {
    struct Neighbor { int anchor = -1; bool out = false; };
    struct Recipe {
        DagNodeId node = kInvalidDagNode;
        int parent = -1;
        std::vector<Neighbor> neighbors;
        bool has_driver = false;
        Neighbor driver;
        DagStructuralPolicy original_policy = DagStructuralPolicy::LegacyMerge;
    };
    struct Index {
        U64 block_count = 0, entry_count = 0;
        std::unique_ptr<U64[]> offsets;
        std::unique_ptr<std::uint32_t[]> positions;
    };
    struct Model {
        std::size_t block_size = 0;
        U64 block_count = 0, cap = 0, cache_slots = 0;
        mutable std::mutex mutex;
        std::unique_ptr<std::atomic<Index*>[]> cache;
        std::atomic<bool> unavailable{false}, overflow{false};
        std::string unavailable_reason;
        std::atomic<U64> persistent_bytes{0}, peak_bytes{0}, built_indexes{0};
        std::atomic<U64> build_attempts{0}, build_wall_ns_sum{0}, build_thread_cpu_ns_sum{0};
        ~Model() {
            // All observer workers have joined before this simulation dies.
            if (cache) for (U64 i = 0; i < cache_slots; ++i)
                delete cache[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
        }
    };
    struct Local {
        std::thread::id owner_thread = std::this_thread::get_id();
        std::array<Counters, 4> totals;
        std::array<std::vector<Counters>, 4> nodes;
        std::array<bool, 4> overflow{{false, false, false, false}};
        U64 largest_token_mask_bytes = 0;
        explicit Local(std::size_t count) { for (auto& node : nodes) node.resize(count); }
    };
    struct Aggregate {
        std::array<Counters, 4> totals;
        std::array<std::vector<Counters>, 4> nodes;
        std::array<bool, 4> overflow{{false, false, false, false}};
        U64 largest_token_mask_bytes = 0;
        std::size_t registered_workers = 0;
        explicit Aggregate(std::size_t count) { for (auto& node : nodes) node.resize(count); }
    };

    const Graph& graph;
    const QueryPlan& plan;
    const TDTree& tree;
    int k;
    const window_algebra::Domain domain;
    U64 baseline_bytes;
    std::vector<Recipe> recipes;
    mutable std::array<Model, 4> models;
    const U64 generation;
    mutable std::mutex local_registry_mutex;
    mutable std::vector<std::unique_ptr<Local>> local_registry;

    static U64 nextGeneration() {
        static std::atomic<U64> value{1};
        const U64 result = value.fetch_add(1, std::memory_order_relaxed);
        if (result == 0 || result == std::numeric_limits<U64>::max())
            throw std::overflow_error("J-index observer generation exhausted");
        return result;
    }

    Impl(const Graph& g, const Graph& query, const QueryPlan& p,
         const CandidateDag& dag, const TDTree& td, int duration)
        : graph(g), plan(p), tree(td), k(std::max(1, duration)),
          domain(fullDomain(g, k)), baseline_bytes(adjacencyTemporalBytes(g)),
          recipes(static_cast<std::size_t>(query.num_vertices)), generation(nextGeneration()) {
        if (!dag.passes.p4 || dag.passes.temporalRewriteAny())
            throw std::invalid_argument("J-index simulation requires actual P4 S1 with P1--P3 disabled");
        for (std::size_t q = 0; q < recipes.size(); ++q) {
            auto& r = recipes[q];
            if (q >= dag.structural_roots.size()) throw std::logic_error("J-index missing structural root");
            const auto id = dag.structural_roots[q];
            if (id == kInvalidDagNode) continue;
            const auto& node = dag.node(id);
            if (node.op != DagOp::FusedEdgeProbe || node.parents.empty())
                throw std::invalid_argument("J-index driver selection requires an existing FusedEdgeProbe");
            r.node = id;
            r.original_policy = node.structural_policy;
            r.parent = plan.parent[q];
            for (std::size_t i = 1; i < node.parents.size(); ++i) {
                const auto& leaf = dag.node(node.parents[i]);
                if (!outgoing(leaf.op) && !incoming(leaf.op))
                    throw std::logic_error("J-index canonical structural parent is not adjacency");
                r.neighbors.push_back({leaf.parameter.first_query_vertex, outgoing(leaf.op)});
            }
            if (!r.neighbors.empty()) {
                r.has_driver = true;
                r.driver = r.neighbors.front();
            }
        }
        for (std::size_t i = 0; i < models.size(); ++i) {
            auto& model = models[i];
            model.block_size = JIndexSimulation::kBlockSizes[i];
            model.block_count = (domain.startCount() + model.block_size - 1) / model.block_size;
            model.cap = std::min<U64>(2ULL * 1024 * 1024 * 1024, baseline_bytes);
            model.cache_slots = times(static_cast<U64>(graph.num_vertices), 2);
            const U64 base = plus(sizeof(std::unique_ptr<std::atomic<Index*>[]>),
                                  times(model.cache_slots, sizeof(std::atomic<Index*>)));
            if (base > model.cap || model.cache_slots > std::numeric_limits<std::size_t>::max()) {
                model.unavailable.store(true);
                model.unavailable_reason = "cache_pointer_array_exceeds_memory_cap";
                continue;
            }
            // A fixed array avoids relying on vector's implementation-specific
            // spare capacity when checking the allocation cap beforehand.
            model.cache.reset(new std::atomic<Index*>[static_cast<std::size_t>(model.cache_slots)]);
            for (U64 slot = 0; slot < model.cache_slots; ++slot)
                model.cache[static_cast<std::size_t>(slot)].store(nullptr, std::memory_order_relaxed);
            model.persistent_bytes.store(base);
            model.peak_bytes.store(base);
        }
    }

    CandidateReadView neighbors(const Neighbor& descriptor, const SearchState& state) const {
        const int vertex = binding(descriptor.anchor, state);
        if (vertex < 0 || vertex >= graph.num_vertices) return {};
        return CandidateReadView((descriptor.out ? graph.adj : graph.in_adj)[static_cast<std::size_t>(vertex)]);
    }

    // Intervals remain in the original signed timestamp domain. Window
    // starts are [a,b-k+1]; only THEN subtract the graph-wide minimum and
    // divide by B. Merge repeated block IDs across separate history runs.
    template <typename Function>
    void blockRanges(const std::vector<TimeInterval>& intervals, std::size_t B, Function function) const {
        if (domain.startCount() == 0) return;
        bool previous = false;
        U64 last_seen = 0;
        for (const auto& interval : intervals) {
            const std::int64_t last_start = static_cast<std::int64_t>(interval.end) - k + 1;
            if (last_start < interval.start) continue;
            const auto first_time = std::max<std::int64_t>(interval.start, domain.minimum());
            const auto final_time = std::min<std::int64_t>(last_start, domain.lastStart());
            if (first_time > final_time) continue;
            U64 first = static_cast<U64>(first_time - domain.minimum()) / B;
            const U64 last = static_cast<U64>(final_time - domain.minimum()) / B;
            if (previous && first <= last_seen) first = last_seen + 1;
            if (first > last) continue;
            function(first, last);
            previous = true;
            last_seen = last;
        }
    }

    const Index* getIndex(Model& model, int anchor, bool out) const {
        if (anchor < 0 || anchor >= graph.num_vertices)
            throw std::logic_error("J-index invalid mapped driver anchor");
        if (model.unavailable.load(std::memory_order_relaxed)) return nullptr;
        const auto key = static_cast<std::size_t>(anchor) * 2 + (out ? 1 : 0);
        // Published CSR payloads are immutable for the simulation lifetime:
        // successful cache hits need no mutex and do not serialize 16T.
        if (const auto* cached = model.cache[key].load(std::memory_order_acquire)) return cached;
        std::lock_guard<std::mutex> lock(model.mutex);
        if (model.unavailable.load(std::memory_order_relaxed)) return nullptr;
        if (const auto* cached = model.cache[key].load(std::memory_order_acquire)) return cached;
        const auto begin = std::chrono::steady_clock::now();
        const auto cpu_begin = threadCpuNs();
        add(model.build_attempts, 1, model.overflow);
        const auto& adjacent = (out ? graph.adj : graph.in_adj)[static_cast<std::size_t>(anchor)];
        auto finish_timing = [&]() {
            add(model.build_wall_ns_sum, elapsedNs(begin), model.overflow);
            add(model.build_thread_cpu_ns_sum, threadCpuNs() - cpu_begin, model.overflow);
        };
        auto reject = [&](const char* reason) -> const Index* {
            model.unavailable_reason = reason;
            model.unavailable.store(true, std::memory_order_relaxed);
            finish_timing();
            return nullptr;
        };
        if (adjacent.size() > std::numeric_limits<std::uint32_t>::max())
            return reject("neighbor_position_exceeds_uint32");
        const U64 offset_bytes = times(plus(model.block_count, 1), sizeof(U64));
        U64 entries = 0;
        for (const auto& adjacency : adjacent) {
            if (adjacency.temporal_edge_id < 0 ||
                static_cast<std::size_t>(adjacency.temporal_edge_id) >= graph.temporal_edges.size())
                throw std::logic_error("J-index invalid immutable temporal edge ID");
            const auto& edge = graph.temporal_edges[static_cast<std::size_t>(adjacency.temporal_edge_id)];
            blockRanges(edge.activeIntervals(), model.block_size, [&](U64 first, U64 last) {
                entries = plus(entries, last - first + 1);
            });
        }
        const U64 owned = plus(sizeof(Index), plus(offset_bytes, times(entries, sizeof(std::uint32_t))));
        // A separate fill cursor is temporary live build memory. It is
        // included in peak/cap checks BEFORE any CSR or scratch allocation.
        const U64 scratch = times(model.block_count, sizeof(U64));
        const U64 persistent_after = plus(model.persistent_bytes.load(), owned);
        const U64 peak = plus(persistent_after, scratch);
        if (peak > model.cap || model.block_count > std::numeric_limits<std::size_t>::max() - 1 ||
            entries > std::numeric_limits<std::size_t>::max())
            return reject("csr_plus_live_build_scratch_exceeds_memory_cap");
        auto index = std::make_unique<Index>();
        index->block_count = model.block_count;
        index->entry_count = entries;
        index->offsets.reset(new U64[static_cast<std::size_t>(model.block_count + 1)]{});
        index->positions.reset(new std::uint32_t[static_cast<std::size_t>(entries)]);
        std::unique_ptr<U64[]> cursor(new U64[static_cast<std::size_t>(model.block_count)]{});
        for (const auto& adjacency : adjacent) {
            const auto& edge = graph.temporal_edges[static_cast<std::size_t>(adjacency.temporal_edge_id)];
            blockRanges(edge.activeIntervals(), model.block_size, [&](U64 first, U64 last) {
                for (U64 block = first; block <= last; ++block) ++index->offsets[block + 1];
            });
        }
        for (U64 block = 0; block < model.block_count; ++block) {
            index->offsets[block + 1] += index->offsets[block];
            cursor[block] = index->offsets[block];
        }
        if (index->offsets[model.block_count] != entries)
            throw std::logic_error("J-index first/second pass entry counts disagree");
        for (std::size_t position = 0; position < adjacent.size(); ++position) {
            const auto& edge = graph.temporal_edges[static_cast<std::size_t>(adjacent[position].temporal_edge_id)];
            blockRanges(edge.activeIntervals(), model.block_size, [&](U64 first, U64 last) {
                for (U64 block = first; block <= last; ++block)
                    index->positions[cursor[block]++] = static_cast<std::uint32_t>(position);
            });
        }
        for (U64 block = 0; block < model.block_count; ++block)
            if (cursor[block] != index->offsets[block + 1])
                throw std::logic_error("J-index CSR fill disagrees with offsets");
        const auto* result = index.get();
        model.cache[key].store(index.release(), std::memory_order_release);
        model.persistent_bytes.store(persistent_after);
        maximum(model.peak_bytes, peak);
        add(model.built_indexes, 1, model.overflow);
        finish_timing();
        return result;
    }

    Local& localCounters() const {
        // Generation prevents a test fixture reusing an address from seeing
        // a stale pointer. Only first use per worker takes the registry lock.
        thread_local std::unordered_map<U64, Local*> registered;
        const auto found = registered.find(generation);
        if (found != registered.end()) return *found->second;
        auto local = std::make_unique<Local>(recipes.size());
        auto* pointer = local.get();
        {
            std::lock_guard<std::mutex> lock(local_registry_mutex);
            local_registry.push_back(std::move(local));
        }
        registered.emplace(generation, pointer);
        return *pointer;
    }

    void addPair(Local& local, std::size_t b, int q, U64 Counters::* field, U64 amount) const {
        addPlain(local.totals[b].*field, amount, local.overflow[b]);
        addPlain(local.nodes[b][static_cast<std::size_t>(q)].*field, amount, local.overflow[b]);
    }

    Aggregate aggregate() const {
        Aggregate result(recipes.size());
        std::lock_guard<std::mutex> lock(local_registry_mutex);
        result.registered_workers = local_registry.size();
        for (const auto& local : local_registry) {
            result.largest_token_mask_bytes =
                std::max(result.largest_token_mask_bytes, local->largest_token_mask_bytes);
            for (std::size_t b = 0; b < models.size(); ++b) {
                result.overflow[b] = result.overflow[b] || local->overflow[b];
                mergeCounters(result.totals[b], local->totals[b], result.overflow[b]);
                for (std::size_t q = 0; q < recipes.size(); ++q)
                    mergeCounters(result.nodes[b][q], local->nodes[b][q], result.overflow[b]);
            }
        }
        for (std::size_t b = 0; b < models.size(); ++b) {
            Counters node_sum;
            for (const auto& node : result.nodes[b]) mergeCounters(node_sum, node, result.overflow[b]);
            if (!sameCounters(node_sum, result.totals[b]))
                throw std::logic_error("J-index thread totals and independent node sums disagree");
        }
        return result;
    }
};

JIndexSimulation::JIndexSimulation(const Graph& graph, const Graph& query,
    const QueryPlan& plan, const CandidateDag& dag, const TDTree& tree, int k)
    : impl_(std::make_unique<Impl>(graph, query, plan, dag, tree, k)) {}
JIndexSimulation::~JIndexSimulation() = default;

JIndexSimulation::StateToken JIndexSimulation::observe(
    int q, const SearchState& state, const PhysicalCandidateBatch& batch) const {
    if (q < 0 || static_cast<std::size_t>(q) >= impl_->recipes.size())
        throw std::out_of_range("J-index observer query vertex");
    const auto& recipe = impl_->recipes[static_cast<std::size_t>(q)];
    StateToken token;
    token.owner = this;
    auto& local = impl_->localCounters();
    token.local_counter_slot = &local;
    token.query_vertex = q;
    token.node = recipe.node;
    token.scan_mask.resize(batch.size(), 0);
    U64 scan_count = 0;
    for (std::size_t i = 0; i < batch.size(); ++i) {
        const int candidate = batch.candidate(i);
        if (std::find(state.mapping.begin(), state.mapping.end(), candidate) == state.mapping.end()) {
            token.scan_mask[i] = 1;
            ++scan_count;
        }
    }
    std::vector<CandidateReadView> all_neighbors;
    all_neighbors.reserve(recipe.neighbors.size());
    for (const auto& neighbor : recipe.neighbors) all_neighbors.push_back(impl_->neighbors(neighbor, state));
    const auto* seed = recipe.node == kInvalidDagNode ? nullptr :
        impl_->tree.candidatesForParent(q, binding(recipe.parent, state));
    const auto costs = PhysicalKernels::stateCosts(seed ? seed->size() : 0, all_neighbors);
    const U64 c_scan = std::min(costs.probe, costs.merge);
    const auto driver = recipe.has_driver ? impl_->neighbors(recipe.driver, state) : CandidateReadView{};
    for (std::size_t b = 0; b < impl_->models.size(); ++b) {
        auto& model = impl_->models[b];
        impl_->addPair(local, b, q, &Counters::states, 1);
        if (model.unavailable.load() || model.overflow.load() || local.overflow[b]) {
            impl_->addPair(local, b, q, &Counters::unavailable_states, 1);
            continue;
        }
        token.available[b] = true;
        token.index_masks[b] = token.scan_mask;
        impl_->addPair(local, b, q, &Counters::scan_touched, scan_count);
        if (!recipe.has_driver) {
            impl_->addPair(local, b, q, &Counters::fallback_no_driver, 1);
            impl_->addPair(local, b, q, &Counters::idx_touched, scan_count);
            continue;
        }
        if (!state.has_active_intervals) {
            impl_->addPair(local, b, q, &Counters::fallback_no_j, 1);
            impl_->addPair(local, b, q, &Counters::idx_touched, scan_count);
            continue;
        }
        if (driver.size() < kMinimumNeighbors) {
            impl_->addPair(local, b, q, &Counters::fallback_small_degree, 1);
            impl_->addPair(local, b, q, &Counters::idx_touched, scan_count);
            continue;
        }
        const auto* index = impl_->getIndex(model, binding(recipe.driver.anchor, state), recipe.driver.out);
        if (!index) {
            token.available[b] = false;
            token.index_masks[b].clear();
            impl_->addPair(local, b, q, &Counters::unavailable_states, 1);
            continue;
        }
        std::vector<std::pair<U64, U64>> selected_ranges;
        impl_->blockRanges(state.active_intervals, model.block_size,
                           [&](U64 first, U64 last) { selected_ranges.emplace_back(first, last); });
        U64 c_idx = 0;
        for (const auto& range : selected_ranges)
            for (U64 block = range.first; block <= range.second; ++block)
                c_idx = plus(c_idx, index->offsets[block + 1] - index->offsets[block]);
        U64 indexed_count = 0;
        for (std::size_t i = 0; i < batch.size(); ++i) {
            if (!token.scan_mask[i]) continue;
            const auto position = PhysicalKernels::lowerBound(driver, batch.candidate(i));
            if (position == driver.size() || driver[position] != batch.candidate(i))
                throw std::logic_error("J-index driver does not contain actual S1 structural candidate");
            bool included = false;
            for (const auto& range : selected_ranges) {
                for (U64 block = range.first; block <= range.second; ++block) {
                    const auto begin = index->positions.get() + index->offsets[block];
                    const auto end = index->positions.get() + index->offsets[block + 1];
                    if (std::binary_search(begin, end, static_cast<std::uint32_t>(position))) {
                        included = true;
                        break;
                    }
                }
                if (included) break;
            }
            token.index_masks[b][i] = included ? 1 : 0;
            if (included) ++indexed_count;
        }
        impl_->addPair(local, b, q, &Counters::idx_touched, indexed_count);
        impl_->addPair(local, b, q, &Counters::indexed_states, 1);
        impl_->addPair(local, b, q, &Counters::cost_evaluated_states, 1);
        impl_->addPair(local, b, q, &Counters::c_idx_sum, c_idx);
        impl_->addPair(local, b, q, &Counters::c_scan_sum, c_scan);
        if (c_idx <= c_scan) impl_->addPair(local, b, q, &Counters::index_cost_le_scan_states, 1);
    }
    U64 mask_bytes = token.scan_mask.capacity();
    for (const auto& mask : token.index_masks) mask_bytes = plus(mask_bytes, mask.capacity());
    local.largest_token_mask_bytes = std::max(local.largest_token_mask_bytes, mask_bytes);
    return token;
}

void JIndexSimulation::recordAccepted(const StateToken& token, std::size_t candidate_index) const {
    if (token.owner != this || token.query_vertex < 0 ||
        static_cast<std::size_t>(token.query_vertex) >= impl_->recipes.size() ||
        token.node != impl_->recipes[static_cast<std::size_t>(token.query_vertex)].node ||
        candidate_index >= token.scan_mask.size() || !token.scan_mask[candidate_index])
        throw std::logic_error("J-index accepted-candidate callback has foreign token or failed injectivity");
    auto* local = static_cast<Impl::Local*>(token.local_counter_slot);
    if (!local || local->owner_thread != std::this_thread::get_id())
        throw std::logic_error("J-index token/counter slot crossed worker threads");
    for (std::size_t b = 0; b < impl_->models.size(); ++b) {
        if (!token.available[b]) continue;
        if (candidate_index >= token.index_masks[b].size() || !token.index_masks[b][candidate_index])
            throw std::logic_error("J-index simulation counterexample: exact accepted candidate absent from index mask; B=" +
                                   std::to_string(kBlockSizes[b]) + ", q=" + std::to_string(token.query_vertex) +
                                   ", index=" + std::to_string(candidate_index));
        impl_->addPair(*local, b, token.query_vertex, &Counters::passed, 1);
    }
}

void JIndexSimulation::writeJson(std::ostream& out) const {
    const auto aggregate = impl_->aggregate();
    out << "{\"schema\":\"v5_jindex_simulation_v1\",\"observer_only\":true,"
        << "\"k\":" << impl_->k << ",\"time_min\":" << impl_->domain.minimum()
        << ",\"time_max\":" << impl_->domain.maximum()
        << ",\"window_start_count\":" << impl_->domain.startCount()
        << ",\"n_min\":" << kMinimumNeighbors
        << ",\"baseline_adjacency_temporal_capacity_bytes\":" << impl_->baseline_bytes
        << ",\"largest_single_token_mask_bytes\":" << aggregate.largest_token_mask_bytes
        << ",\"registered_counter_workers\":" << aggregate.registered_workers
        << ",\"thread_totals_equal_node_sums\":true"
        << ",\"counter_scope\":\"post-structure/post-injectivity candidates; accepted callback after exact outer J\","
        << "\"driver_rule\":\"first canonical adjacency parent of each compiled FusedEdgeProbe; counterfactual index driver, not observed current S1 driver\","
        << "\"bucket_scope\":\"all mapped-anchor neighbors, including outside query-domain union; positions ascending and unique per bucket\","
        << "\"cost_scope\":\"c_idx/c_scan sums and decision denominator only indexed states; fallback contributes idx_touched=scan_touched\","
        << "\"memory_scope\":\"capacity-based adjacency+temporal payload baseline; index cache pointers, CSR objects/arrays, and concurrent live build scratch; allocator headers excluded; diagnostic masks/counters excluded\","
        << "\"build_time_scope\":\"sum of per-index steady-clock build durations excluding mutex wait, and sum of thread CPU durations; not query wall time, not union-of-intervals, not performance evidence\","
        << "\"recipes\":[";
    bool first = true;
    for (std::size_t q = 0; q < impl_->recipes.size(); ++q) {
        const auto& r = impl_->recipes[q];
        if (!first) out << ',';
        first = false;
        out << "{\"q\":" << q << ",\"node_id\":";
        if (r.node == kInvalidDagNode) out << "null"; else out << r.node;
        out << ",\"has_driver\":" << (r.has_driver ? "true" : "false")
            << ",\"anchor_query_vertex\":" << r.driver.anchor
            << ",\"direction\":\"" << (r.driver.out ? "out" : "in") << "\""
            << ",\"current_s1_policy\":\""
            << (r.original_policy == DagStructuralPolicy::StateCost ? "state_cost" :
                r.original_policy == DagStructuralPolicy::Probe ? "fixed_td_probe" : "legacy_merge")
            << "\",\"mapped_neighbor_lists\":" << r.neighbors.size() << '}';
    }
    out << "],\"blocks\":[";
    for (std::size_t b = 0; b < impl_->models.size(); ++b) {
        const auto& m = impl_->models[b];
        const bool overflow = m.overflow.load() || aggregate.overflow[b];
        std::lock_guard<std::mutex> lock(m.mutex);
        if (b) out << ',';
        out << "{\"B\":" << m.block_size << ",\"block_count\":" << m.block_count
            << ",\"status\":\"" << (overflow ? "UNAVAILABLE_COUNTER_OVERFLOW" :
                m.unavailable.load() ? "UNAVAILABLE_MEMORY_OR_REPRESENTATION" : "OK") << "\""
            << ",\"unavailable_reason\":\"" << m.unavailable_reason << "\""
            << ",\"counter_overflow\":" << (overflow ? "true" : "false")
            << ",\"cap_bytes\":" << m.cap
            << ",\"persistent_index_bytes\":" << m.persistent_bytes.load()
            << ",\"peak_index_plus_build_scratch_bytes\":" << m.peak_bytes.load()
            << ",\"build_attempts\":" << m.build_attempts.load()
            << ",\"built_indexes\":" << m.built_indexes.load()
            << ",\"build_wall_ns_sum\":" << m.build_wall_ns_sum.load()
            << ",\"build_thread_cpu_ns_sum\":" << m.build_thread_cpu_ns_sum.load()
            << ",\"totals\":";
        countersJson(out, aggregate.totals[b]);
        out << ",\"nodes\":[";
        for (std::size_t q = 0; q < impl_->recipes.size(); ++q) {
            if (q) out << ',';
            out << "{\"q\":" << q << ",\"counts\":";
            countersJson(out, aggregate.nodes[b][q]);
            out << '}';
        }
        out << "]}";
    }
    out << "]}";
}

#undef JINDEX_FIELDS
