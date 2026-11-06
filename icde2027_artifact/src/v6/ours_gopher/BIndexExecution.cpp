#include "BIndexExecution.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "CandidateDag.h" // QueryPlan value type only; no DAG construction.
#include "TDTree.h"
#include "WindowStore.h"

namespace {
using U64 = std::uint64_t;
constexpr U64 kMaximumIndexBytes = 2ULL * 1024 * 1024 * 1024;
U64 add(U64 a, U64 b) {
    if (b > std::numeric_limits<U64>::max() - a)
        throw std::overflow_error("B-index size overflow");
    return a + b;
}
U64 mul(U64 a, U64 b) {
    if (b && a > std::numeric_limits<U64>::max() / b)
        throw std::overflow_error("B-index size overflow");
    return a * b;
}
U64 elapsed(std::chrono::steady_clock::time_point start) {
    return static_cast<U64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count());
}
U64 cpuTime() {
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value))
        throw std::runtime_error("B-index thread clock unavailable");
    return add(mul(static_cast<U64>(value.tv_sec), 1000000000ULL),
               static_cast<U64>(value.tv_nsec));
}
U64 baselineBytes(const Graph& graph) {
    // Identical capacity-based denominator to v5 JIndexExecution.
    U64 total = mul(add(graph.adj.capacity(), graph.in_adj.capacity()), sizeof(std::vector<Edge>));
    for (const auto& a : graph.adj) total = add(total, mul(a.capacity(), sizeof(Edge)));
    for (const auto& a : graph.in_adj) total = add(total, mul(a.capacity(), sizeof(Edge)));
    total = add(total, mul(graph.temporal_edges.capacity(), sizeof(TemporalEdge)));
    for (const auto& e : graph.temporal_edges)
        total = add(total, mul(e.activeIntervals().capacity(), sizeof(TimeInterval)));
    return total;
}
void maximum(std::atomic<U64>& value, U64 next) {
    U64 old = value.load(std::memory_order_relaxed);
    while (old < next && !value.compare_exchange_weak(old, next, std::memory_order_relaxed)) {}
}
void count(U64& value, U64 amount, bool& overflow) {
    if (amount > std::numeric_limits<U64>::max() - value) {
        overflow = true;
        value = std::numeric_limits<U64>::max();
    } else value += amount;
}
void atomicCount(std::atomic<U64>& value, U64 amount, std::atomic<bool>& overflow) {
    U64 old = value.load(std::memory_order_relaxed);
    for (;;) {
        const bool invalid = amount > std::numeric_limits<U64>::max() - old;
        const U64 next = invalid ? std::numeric_limits<U64>::max() : old + amount;
        if (value.compare_exchange_weak(old, next, std::memory_order_relaxed)) {
            if (invalid) overflow.store(true, std::memory_order_relaxed);
            return;
        }
    }
}
#define BINDEX_FIELDS(X) \
    X(states) X(index_selected) X(scan_selected) X(eligible_states) \
    X(no_parent_scans) X(empty_block_scans) X(small_block_scans) X(prebuild_scans) \
    X(cap_denial_scans) X(cost_rejected_scans) X(usage_tickets) \
    X(universal_states) X(universal_index_selected) X(universal_scan_selected) \
    X(original_R_sum) X(scan_candidate_sum) X(index_original_R_sum) \
    X(cost_evaluated_states) X(cost_R_sum) X(n_idx_sum) X(selected_buckets) \
    X(bucket_entries_consumed) X(unique_extracted_candidates) \
    X(time_checks) X(time_passes) X(missing_parent_edges) X(empty_parent_windows)
struct Counts {
#define DECLARE(name) U64 name = 0;
    BINDEX_FIELDS(DECLARE)
#undef DECLARE
};
void merge(Counts& a, const Counts& b, bool& overflow) {
#define MERGE(name) count(a.name, b.name, overflow);
    BINDEX_FIELDS(MERGE)
#undef MERGE
}
bool equal(const Counts& a, const Counts& b) {
#define EQUAL(name) if (a.name != b.name) return false;
    BINDEX_FIELDS(EQUAL)
#undef EQUAL
    return true;
}
void writeCounts(std::ostream& out, const Counts& counts) {
    bool first = true;
    out << '{';
#define WRITE(name) if (!first) out << ','; first = false; out << "\"" #name "\":" << counts.name;
    BINDEX_FIELDS(WRITE)
#undef WRITE
    out << '}';
}
} // namespace

struct BIndexExecution::Impl {
    struct Index {
        U64 entries = 0;
        std::unique_ptr<U64[]> offsets;
        std::unique_ptr<std::uint32_t[]> positions;
        std::unique_ptr<const Edge*[]> parent_entries;
    };
    struct Block {
        int parent_data = -1;
        const std::vector<int>* R = nullptr;
        std::atomic<U64> tickets{0};
        std::atomic<Index*> index{nullptr};
        std::atomic<bool> denied{false};
    };
    struct Recipe {
        int parent = -1;
        bool outgoing = false;
        std::size_t begin = 0, end = 0;
    };
    struct Local {
        Counts total;
        std::vector<Counts> nodes;
        bool overflow = false;
        explicit Local(std::size_t n) : nodes(n) {}
    };
    struct Aggregate {
        Counts total;
        std::vector<Counts> nodes;
        bool overflow = false;
        std::size_t workers = 0;
        explicit Aggregate(std::size_t n) : nodes(n) {}
    };

    const Graph& graph;
    const WindowStore& windows;
    const window_algebra::Domain& domain;
    const int k;
    const std::size_t B, n_min;
    const U64 reuse_threshold;
    const double alpha;
    const Mode mode;
    const bool counters_enabled, verify_superset;
    const U64 baseline_bytes, cap, buckets, generation;
    std::vector<Recipe> recipes;
    std::unique_ptr<Block[]> blocks;
    std::size_t block_size = 0, maximum_R = 0;
    bool metadata_denied = false;
    U64 setup_ns = 0;
    mutable std::mutex build_mutex, registry_mutex;
    mutable std::vector<std::unique_ptr<Local>> registry;
    mutable std::atomic<U64> persistent_bytes{0}, peak_bytes{0};
    mutable std::atomic<U64> build_attempts{0}, built_indexes{0}, denied_indexes{0};
    mutable std::atomic<U64> build_wall_ns{0}, build_thread_ns{0};
    mutable std::atomic<bool> telemetry_overflow{false};
    mutable std::atomic<bool> new_builds_disabled{false};

    static U64 nextGeneration() {
        static std::atomic<U64> next{1};
        const auto value = next.fetch_add(1, std::memory_order_relaxed);
        if (!value || value == std::numeric_limits<U64>::max())
            throw std::overflow_error("B-index generation exhausted");
        return value;
    }
    Impl(const Graph& g, const Graph& query, const QueryPlan& plan, const TDTree& tree,
         const WindowStore& w, int duration, std::size_t width, std::size_t minimum,
         U64 threshold, double factor, Mode policy, bool counts, bool verify)
        : graph(g), windows(w), domain(w.domain()), k(duration), B(width), n_min(minimum),
          reuse_threshold(threshold), alpha(factor), mode(policy), counters_enabled(counts),
          verify_superset(verify), baseline_bytes(baselineBytes(g)),
          cap(std::min(kMaximumIndexBytes, baseline_bytes)),
          buckets(width ? domain.startCount() / width + (domain.startCount() % width != 0) : 0),
          generation(nextGeneration()), recipes(static_cast<std::size_t>(query.num_vertices)) {
        if (k < 1 || domain.k() != k || domain.wordCount() > window_algebra::kDenseWords ||
            (B != 8 && B != 16 && B != 32) || !n_min ||
            (reuse_threshold != 1 && reuse_threshold != 2) ||
            (alpha != .25 && alpha != .5 && alpha != 1.))
            throw std::invalid_argument("B-index requires <=16 words, B=8/16/32, c=1/2, alpha=.25/.5/1");
        if (plan.parent.size() != recipes.size() || plan.order.size() != recipes.size())
            throw std::invalid_argument("B-index requires complete fixed QueryPlan");
        for (std::size_t q = 0; q < recipes.size(); ++q) {
            auto& recipe = recipes[q];
            recipe.begin = block_size;
            if (static_cast<int>(q) != plan.root) {
                recipe.parent = plan.parent[q];
                if (recipe.parent < 0 || static_cast<std::size_t>(recipe.parent) >= recipes.size())
                    throw std::invalid_argument("B-index requires connected fixed parent tree");
                recipe.outgoing = GraphUtils::hasEdge(query.adj, recipe.parent, static_cast<int>(q));
                if (!recipe.outgoing && !GraphUtils::hasEdge(query.adj, static_cast<int>(q), recipe.parent))
                    throw std::logic_error("B-index parent is not a query arc");
                // Reciprocal TD relations expand parent -> child. The reverse
                // arc remains a separate exact P4/W constraint after selection.
                const auto& existing = tree.candidateBlocks(static_cast<int>(q));
                block_size = static_cast<std::size_t>(add(block_size, existing.size()));
                for (const auto& block : existing) maximum_R = std::max(maximum_R, block.V_cand.size());
            }
            recipe.end = block_size;
        }
        const U64 metadata = mul(block_size, sizeof(Block));
        if (metadata > cap) {
            // Sticky global denial, not OOM. No partial block registry is used.
            metadata_denied = true;
            return;
        }
        if (block_size) blocks.reset(new Block[block_size]);
        persistent_bytes.store(metadata);
        peak_bytes.store(metadata);
        for (std::size_t q = 0; q < recipes.size(); ++q) {
            const auto& recipe = recipes[q];
            if (recipe.parent < 0) continue;
            const auto& existing = tree.candidateBlocks(static_cast<int>(q));
            for (std::size_t i = 0; i < existing.size(); ++i) {
                auto& block = blocks[recipe.begin + i];
                block.parent_data = existing[i].v_par;
                block.R = &existing[i].V_cand;
                if ((i && existing[i - 1].v_par >= existing[i].v_par) ||
                    (verify_superset && (!std::is_sorted(block.R->begin(), block.R->end()) ||
                    std::adjacent_find(block.R->begin(), block.R->end()) != block.R->end())))
                    throw std::logic_error("B-index requires immutable sorted unique TD blocks");
            }
        }
    }
    ~Impl() {
        if (blocks) for (std::size_t i = 0; i < block_size; ++i)
            delete blocks[i].index.load(std::memory_order_relaxed);
    }
    Local& local() const {
        // One active query index per worker. Unlike a forever-growing TLS map,
        // this does not retain one cache entry per tiny correctness fixture.
        thread_local U64 cached_generation = 0;
        thread_local Local* cached = nullptr;
        if (cached_generation == generation) return *cached;
        auto fresh = std::make_unique<Local>(recipes.size());
        auto* pointer = fresh.get();
        {
            std::lock_guard<std::mutex> guard(registry_mutex);
            registry.push_back(std::move(fresh));
        }
        cached_generation = generation;
        cached = pointer;
        return *pointer;
    }
    static void record(Local* local, std::size_t q, U64 Counts::* field, U64 amount = 1) {
        if (!local) return;
        count(local->total.*field, amount, local->overflow);
        count(local->nodes[q].*field, amount, local->overflow);
    }
    U64 offset(std::int64_t time) const {
        return static_cast<U64>(time) - static_cast<U64>(domain.minimum());
    }
    template<class Function>
    void eachBucket(window_algebra::WindowView value, Function function) const {
        if (!domain.sameAs(value.domain())) throw std::logic_error("B-index W domain mismatch");
        if (value.isEmpty()) return;
        if (value.isUniversal()) {
            for (U64 b = 0; b < buckets; ++b) function(b);
        } else if (value.kind() == window_algebra::Kind::Dense) {
            // B divides 64. Each block is visited once even with multiple bits.
            const U64 mask = (U64{1} << B) - 1;
            for (std::size_t word = 0; word < value.wordCount(); ++word) {
                const U64 bits = value.words()[word];
                for (std::size_t shift = 0; shift < 64; shift += B) {
                    const U64 b = (word * 64 + shift) / B;
                    if (((bits >> shift) & mask) != 0) {
                        if (b >= buckets) throw std::logic_error("B-index nonzero W padding");
                        function(b);
                    }
                }
            }
        } else {
            bool seen = false;
            U64 last = 0;
            const auto intervals = value.starts();
            for (std::size_t i = 0; i < intervals.size; ++i) {
                U64 first = offset(intervals.data[i].start) / B;
                const U64 end = offset(intervals.data[i].end) / B;
                if (end >= buckets) throw std::logic_error("B-index W interval outside domain");
                if (seen && first <= last) first = last + 1;
                for (U64 b = first; b <= end; ++b) function(b);
                seen = true;
                last = end;
            }
        }
    }
    U64 word(window_algebra::WindowView value, U64 w) const {
        if (value.isEmpty()) return 0;
        U64 mask = std::numeric_limits<U64>::max();
        if (w + 1 == domain.wordCount() && domain.startCount() % 64)
            mask = (U64{1} << (domain.startCount() % 64)) - 1;
        if (value.isUniversal()) return mask;
        if (value.kind() == window_algebra::Kind::Dense) return value.words()[w];
        U64 result = 0;
        const auto intervals = value.starts();
        for (std::size_t i = 0; i < intervals.size; ++i) {
            const U64 lo = offset(intervals.data[i].start), hi = offset(intervals.data[i].end);
            if (lo >= (w + 1) * 64) break;
            if (hi < w * 64) continue;
            const U64 first = std::max(lo, w * 64) % 64;
            const U64 last = std::min(hi, w * 64 + 63) % 64;
            result |= (std::numeric_limits<U64>::max() << first) &
                (last == 63 ? std::numeric_limits<U64>::max() : (U64{1} << (last + 1)) - 1);
        }
        return result & mask;
    }
    Block* findBlock(std::size_t q, int parent) const {
        const auto& recipe = recipes[q];
        std::size_t lo = recipe.begin, hi = recipe.end;
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (blocks[mid].parent_data < parent) lo = mid + 1; else hi = mid;
        }
        return lo < recipe.end && blocks[lo].parent_data == parent ? &blocks[lo] : nullptr;
    }
    const Edge* parentEntry(const Recipe& recipe, int parent, int candidate) const {
        const auto& adjacent = (recipe.outgoing ? graph.adj : graph.in_adj)[static_cast<std::size_t>(parent)];
        const auto found = std::lower_bound(adjacent.begin(), adjacent.end(), candidate,
            [](const Edge& edge, int value) { return edge.to < value; });
        if (found == adjacent.end() || found->to != candidate) return nullptr;
        if (found->temporal_edge_id < 0 ||
            static_cast<std::size_t>(found->temporal_edge_id) >= graph.temporal_edges.size())
            throw std::logic_error("B-index invalid parent temporal edge ID");
        const auto& edge = graph.temporal_edges[static_cast<std::size_t>(found->temporal_edge_id)];
        if (edge.u != (recipe.outgoing ? parent : candidate) ||
            edge.v != (recipe.outgoing ? candidate : parent))
            throw std::logic_error("B-index parent direction differs from graph handle");
        return &*found;
    }
    const Index* getIndex(std::size_t q, Block& block, Local* local) const {
        if (auto* ready = block.index.load(std::memory_order_acquire)) return ready;
        if (new_builds_disabled.load(std::memory_order_acquire) ||
            block.denied.load(std::memory_order_acquire)) return nullptr;
        // Globally serialize construction so the cap includes every live build
        // scratch array. The fast published path has no mutex. A ticket > c
        // may build if the c-th caller was descheduled: no missed-build race.
        std::lock_guard<std::mutex> guard(build_mutex);
        if (auto* ready = block.index.load(std::memory_order_acquire)) return ready;
        if (new_builds_disabled.load(std::memory_order_acquire) ||
            block.denied.load(std::memory_order_acquire)) return nullptr;
        const auto begin = std::chrono::steady_clock::now();
        const auto cpu = cpuTime();
        atomicCount(build_attempts, 1, telemetry_overflow);
        auto finish = [&]() {
            atomicCount(build_wall_ns, elapsed(begin), telemetry_overflow);
            atomicCount(build_thread_ns, cpuTime() - cpu, telemetry_overflow);
        };
        auto denied = [&]() -> const Index* {
            block.denied.store(true, std::memory_order_release);
            // The final policy stops all subsequent NEW construction. Already
            // published indices remain valid, and no eviction/OOM is induced.
            new_builds_disabled.store(true, std::memory_order_release);
            atomicCount(denied_indexes, 1, telemetry_overflow);
            finish();
            return nullptr;
        };
        const auto& R = *block.R;
        if (R.size() > std::numeric_limits<std::uint32_t>::max()) return denied();
        const U64 offsets_bytes = mul(add(buckets, 1), sizeof(U64));
        const U64 handles_bytes = mul(R.size(), sizeof(const Edge*));
        const U64 scratch_bytes = mul(buckets, sizeof(U64));
        const U64 base = add(sizeof(Index), add(offsets_bytes, handles_bytes));
        const U64 live = persistent_bytes.load(std::memory_order_relaxed);
        if (add(add(live, base), scratch_bytes) > cap) return denied();
        auto index = std::make_unique<Index>();
        index->offsets.reset(new U64[static_cast<std::size_t>(buckets + 1)]{});
        index->parent_entries.reset(new const Edge*[R.size()]{});
        maximum(peak_bytes, add(live, base));
        for (std::size_t i = 0; i < R.size(); ++i) {
            const auto* entry = parentEntry(recipes[q], block.parent_data, R[i]);
            index->parent_entries[i] = entry;
            if (!entry) { record(local, q, &Counts::missing_parent_edges); continue; }
            bool present = false;
            eachBucket(windows.get(entry->temporal_edge_id), [&](U64 b) {
                index->offsets[b + 1] = add(index->offsets[b + 1], 1);
                index->entries = add(index->entries, 1);
                present = true;
            });
            if (!present) record(local, q, &Counts::empty_parent_windows);
        }
        const U64 owned = add(base, mul(index->entries, sizeof(std::uint32_t)));
        const U64 after = add(live, owned);
        const U64 peak = add(after, scratch_bytes);
        if (peak > cap || index->entries > std::numeric_limits<std::size_t>::max()) return denied();
        index->positions.reset(new std::uint32_t[static_cast<std::size_t>(index->entries)]);
        std::unique_ptr<U64[]> cursor(new U64[static_cast<std::size_t>(buckets)]{});
        maximum(peak_bytes, peak);
        for (U64 b = 0; b < buckets; ++b) {
            index->offsets[b + 1] = add(index->offsets[b + 1], index->offsets[b]);
            cursor[b] = index->offsets[b];
        }
        if (index->offsets[buckets] != index->entries)
            throw std::logic_error("B-index CSR prefix count mismatch");
        for (std::size_t i = 0; i < R.size(); ++i) {
            const auto* entry = index->parent_entries[i];
            if (!entry) continue;
            eachBucket(windows.get(entry->temporal_edge_id), [&](U64 b) {
                index->positions[cursor[b]++] = static_cast<std::uint32_t>(i);
            });
        }
        for (U64 b = 0; b < buckets; ++b)
            if (cursor[b] != index->offsets[b + 1]) throw std::logic_error("B-index CSR fill mismatch");
        const auto* ready = index.get();
        persistent_bytes.store(after, std::memory_order_relaxed);
        block.index.store(index.release(), std::memory_order_release);
        atomicCount(built_indexes, 1, telemetry_overflow);
        finish();
        return ready;
    }
    Aggregate aggregate() const {
        Aggregate result(recipes.size());
        std::lock_guard<std::mutex> guard(registry_mutex);
        result.workers = registry.size();
        for (const auto& worker : registry) {
            result.overflow = result.overflow || worker->overflow;
            merge(result.total, worker->total, result.overflow);
            for (std::size_t q = 0; q < recipes.size(); ++q) merge(result.nodes[q], worker->nodes[q], result.overflow);
        }
        Counts sum;
        for (const auto& node : result.nodes) merge(sum, node, result.overflow);
        if (!equal(sum, result.total)) throw std::logic_error("B-index node/worker totals differ");
        if (!result.overflow && (result.total.states != result.total.index_selected + result.total.scan_selected ||
            result.total.scan_selected != result.total.no_parent_scans + result.total.empty_block_scans +
                result.total.small_block_scans + result.total.prebuild_scans + result.total.cap_denial_scans +
                result.total.cost_rejected_scans ||
            result.total.time_passes > result.total.time_checks ||
            result.total.universal_states != result.total.universal_index_selected + result.total.universal_scan_selected))
            throw std::logic_error("B-index decision accounting mismatch");
        return result;
    }
};

void BIndexExecution::WorkerScratch::reserve(std::size_t maximum_block_size) {
    if (!touched_words.empty()) throw std::logic_error("B-index cannot resize active scratch");
    const auto words = maximum_block_size / 64 + (maximum_block_size % 64 != 0);
    bitmap.resize(words, 0);
    touched_words.reserve(words);
}
std::size_t BIndexExecution::WorkerScratch::storageBytes() const {
    return static_cast<std::size_t>(add(mul(bitmap.capacity(), sizeof(U64)),
        mul(touched_words.capacity(), sizeof(std::size_t))));
}
BIndexExecution::BIndexExecution(const Graph& graph, const Graph& query, const QueryPlan& plan,
    const TDTree& tree, const WindowStore& windows, int k, std::size_t B, std::size_t n_min,
    U64 c, double alpha, Mode mode, bool counters, bool verify) {
    const auto begin = std::chrono::steady_clock::now();
    impl_ = std::make_unique<Impl>(graph, query, plan, tree, windows, k, B, n_min, c, alpha, mode, counters, verify);
    impl_->setup_ns = elapsed(begin);
}
BIndexExecution::~BIndexExecution() = default;

BIndexExecution::Selection BIndexExecution::select(int q, int parent,
    const std::vector<int>& R, window_algebra::WindowView prior, WorkerScratch& scratch) const {
    if (q < 0 || static_cast<std::size_t>(q) >= impl_->recipes.size()) throw std::out_of_range("B-index query vertex");
    if (!impl_->domain.sameAs(prior.domain())) throw std::logic_error("B-index prior domain differs");
    auto* local = impl_->counters_enabled ? &impl_->local() : nullptr;
    const auto query = static_cast<std::size_t>(q);
    const auto& recipe = impl_->recipes[query];
    Selection result;
    result.parent_query = recipe.parent;
    result.parent_outgoing = recipe.outgoing;
    Impl::record(local, query, &Counts::states);
    Impl::record(local, query, &Counts::original_R_sum, R.size());
    if (prior.isUniversal()) Impl::record(local, query, &Counts::universal_states);
    auto scan = [&](U64 Counts::* reason) {
        Impl::record(local, query, &Counts::scan_selected);
        Impl::record(local, query, reason);
        Impl::record(local, query, &Counts::scan_candidate_sum, R.size());
        if (prior.isUniversal()) Impl::record(local, query, &Counts::universal_scan_selected);
        return std::move(result);
    };
    if (recipe.parent < 0) return scan(&Counts::no_parent_scans);
    if (R.empty()) return scan(&Counts::empty_block_scans);
    if (R.size() < impl_->n_min) return scan(&Counts::small_block_scans);
    Impl::record(local, query, &Counts::eligible_states);
    if (parent < 0 || parent >= impl_->graph.num_vertices) throw std::out_of_range("B-index mapped parent");
    if (impl_->metadata_denied) return scan(&Counts::cap_denial_scans);
    auto* block = impl_->findBlock(query, parent);
    if (!block || block->R != &R) throw std::logic_error("B-index requires the original fixed TD block");
    const auto* index = block->index.load(std::memory_order_acquire);
    if (!index) {
        if (impl_->new_builds_disabled.load(std::memory_order_acquire))
            return scan(&Counts::cap_denial_scans);
        const U64 old_ticket = block->tickets.fetch_add(1, std::memory_order_relaxed);
        if (old_ticket == std::numeric_limits<U64>::max()) throw std::overflow_error("B-index usage counter exhausted");
        Impl::record(local, query, &Counts::usage_tickets);
        if (old_ticket + 1 < impl_->reuse_threshold) return scan(&Counts::prebuild_scans);
        index = impl_->getIndex(query, *block, local);
    }
    if (!index) return scan(&Counts::cap_denial_scans);
    std::array<U64, 128> selected{}; // max 1024 starts / B=8
    std::size_t selected_count = 0;
    impl_->eachBucket(prior, [&](U64 b) {
        if (selected_count == selected.size()) throw std::logic_error("B-index bucket scratch bound exceeded");
        selected[selected_count++] = b;
        result.n_idx = add(result.n_idx, index->offsets[b + 1] - index->offsets[b]);
    });
    Impl::record(local, query, &Counts::cost_evaluated_states);
    Impl::record(local, query, &Counts::cost_R_sum, R.size());
    Impl::record(local, query, &Counts::n_idx_sum, result.n_idx);
    const unsigned denominator = impl_->alpha == .25 ? 4 : impl_->alpha == .5 ? 2 : 1;
    // Integer division implements n_idx <= alpha*|R| exactly for alpha in
    // {1/4,1/2,1}, without multiplication overflow or a compiler extension.
    if (impl_->mode == Mode::Adaptive &&
        result.n_idx > static_cast<U64>(R.size()) / denominator)
        return scan(&Counts::cost_rejected_scans);

    const auto words = R.size() / 64 + (R.size() % 64 != 0);
    if (scratch.bitmap.size() < words || scratch.touched_words.capacity() < words || !scratch.touched_words.empty())
        throw std::logic_error("B-index worker scratch was not prepared or is still active");
    struct Reset {
        WorkerScratch& scratch;
        ~Reset() {
            for (auto word : scratch.touched_words) scratch.bitmap[word] = 0;
            scratch.touched_words.clear();
        }
    } reset{scratch};
    for (std::size_t s = 0; s < selected_count; ++s) {
        const auto b = selected[s];
        for (U64 i = index->offsets[b]; i < index->offsets[b + 1]; ++i) {
            const auto position = index->positions[i];
            if (position >= R.size()) throw std::logic_error("B-index CSR position outside R");
            const auto word = position / 64;
            if (!scratch.bitmap[word]) scratch.touched_words.push_back(word);
            scratch.bitmap[word] |= U64{1} << (position % 64);
        }
    }
    // Touched-word iteration avoids a full |R| scan. Sorting word IDs restores
    // R's fixed candidate order; no TD membership probe is performed.
    std::sort(scratch.touched_words.begin(), scratch.touched_words.end());
    const auto reserve = static_cast<std::size_t>(std::min<U64>(R.size(), result.n_idx));
    result.candidates.reserve(reserve);
    result.parent_entries.reserve(reserve);
    for (const auto word : scratch.touched_words) {
        U64 bits = scratch.bitmap[word];
        while (bits) {
            const auto position = word * 64 + static_cast<unsigned>(__builtin_ctzll(bits));
            const auto* entry = index->parent_entries[position];
            if (!entry) throw std::logic_error("B-index indexed a missing parent edge");
            result.candidates.push_back(R[position]);
            result.parent_entries.push_back(entry);
            bits &= bits - 1;
        }
    }
    result.used = true;
    if (result.n_idx < result.candidates.size())
        throw std::logic_error("B-index duplicate-inclusive n_idx below unique candidates");
    if (impl_->verify_superset) {
        // Independent exact W overlap, not another bucket calculation. This
        // establishes only a necessary-condition superset, never final answers.
        for (std::size_t i = 0; i < R.size(); ++i) {
            const auto* entry = index->parent_entries[i];
            if (!entry) continue;
            const auto edge = impl_->windows.get(entry->temporal_edge_id);
            bool overlap = false;
            for (U64 w = 0; w < impl_->domain.wordCount(); ++w)
                if (impl_->word(edge, w) & impl_->word(prior, w)) { overlap = true; break; }
            if (overlap && !std::binary_search(result.candidates.begin(), result.candidates.end(), R[i]))
                throw std::logic_error("B-index debug superset violation");
        }
    }
    Impl::record(local, query, &Counts::index_selected);
    Impl::record(local, query, &Counts::index_original_R_sum, R.size());
    Impl::record(local, query, &Counts::selected_buckets, selected_count);
    Impl::record(local, query, &Counts::bucket_entries_consumed, result.n_idx);
    Impl::record(local, query, &Counts::unique_extracted_candidates, result.candidates.size());
    if (prior.isUniversal()) Impl::record(local, query, &Counts::universal_index_selected);
    return result;
}
void BIndexExecution::recordTimeCheck(int q, U64 n) const {
    if (!impl_->counters_enabled) return;
    if (q < 0 || static_cast<std::size_t>(q) >= impl_->recipes.size()) throw std::out_of_range("B-index time-check q");
    Impl::record(&impl_->local(), static_cast<std::size_t>(q), &Counts::time_checks, n);
}
void BIndexExecution::recordTimePass(int q, U64 n) const {
    if (!impl_->counters_enabled) return;
    if (q < 0 || static_cast<std::size_t>(q) >= impl_->recipes.size()) throw std::out_of_range("B-index time-pass q");
    Impl::record(&impl_->local(), static_cast<std::size_t>(q), &Counts::time_passes, n);
}
std::size_t BIndexExecution::maximumBlockSize() const { return impl_->maximum_R; }
U64 BIndexExecution::setupNanoseconds() const { return impl_->setup_ns; }
U64 BIndexExecution::buildThreadNanoseconds() const { return impl_->build_thread_ns.load(); }
U64 BIndexExecution::buildWallNanoseconds() const { return impl_->build_wall_ns.load(); }
U64 BIndexExecution::storageBytes() const { return impl_->persistent_bytes.load(); }
U64 BIndexExecution::peakIndexBytes() const { return impl_->peak_bytes.load(); }
void BIndexExecution::writeJson(std::ostream& out) const {
    const auto counts = impl_->aggregate();
    const bool overflow = counts.overflow || impl_->telemetry_overflow.load();
    out << "{\"schema\":\"v6_bindex_execution_v1\",\"production\":true,\"mode\":\""
        << (impl_->mode == Mode::Always ? "always" : "adaptive") << "\",\"B\":" << impl_->B
        << ",\"n_min\":" << impl_->n_min << ",\"c\":" << impl_->reuse_threshold << ",\"alpha\":" << impl_->alpha
        << ",\"k\":" << impl_->k << ",\"counters_enabled\":" << (impl_->counters_enabled ? "true" : "false")
        << ",\"verify_superset\":" << (impl_->verify_superset ? "true" : "false")
        << ",\"status\":\"" << (overflow ? "UNAVAILABLE_COUNTER_OVERFLOW" : "OK") << "\""
        << ",\"counter_overflow\":" << (overflow ? "true" : "false")
        << ",\"time_min\":" << impl_->domain.minimum() << ",\"time_max\":" << impl_->domain.maximum()
        << ",\"window_start_count\":" << impl_->domain.startCount()
        << ",\"baseline_adjacency_temporal_capacity_bytes\":" << impl_->baseline_bytes
        << ",\"cap_bytes\":" << impl_->cap << ",\"persistent_index_bytes\":" << impl_->persistent_bytes.load()
        << ",\"peak_index_plus_build_scratch_bytes\":" << impl_->peak_bytes.load()
        << ",\"metadata_cap_denied\":" << (impl_->metadata_denied ? "true" : "false")
        << ",\"new_builds_disabled\":" << (impl_->new_builds_disabled.load() ? "true" : "false")
        << ",\"registered_blocks\":" << impl_->block_size << ",\"maximum_R\":" << impl_->maximum_R
        << ",\"setup_ns\":" << impl_->setup_ns << ",\"build_attempts\":" << impl_->build_attempts.load()
        << ",\"built_indexes\":" << impl_->built_indexes.load() << ",\"cap_denied_indexes\":" << impl_->denied_indexes.load()
        << ",\"build_wall_ns_sum\":" << impl_->build_wall_ns.load()
        << ",\"build_thread_cpu_ns_sum\":" << impl_->build_thread_ns.load()
        << ",\"registered_counter_workers\":" << counts.workers << ",\"thread_totals_equal_node_sums\":true"
        << ",\"budget_failure_policy\":\"sticky scan fallback; no eviction; not OOM\""
        << ",\"accounting\":\"block atomic registry, Index headers, CSR, parent handles, live build cursor; "
           "excludes immutable graph/W/TD, static recipes, counter registry, worker bitmap/touched scratch, returned batch\""
        << ",\"position_unit\":\"original TD R position; no TD membership probe\""
        << ",\"universal_policy\":\"all buckets; same alpha*n_R decision, no unconditional special-case scan\""
        << ",\"time_note\":\"setup outside enumeration; lazy build included in enumeration; build sums are not union wall\""
        << ",\"totals\":";
    if (impl_->counters_enabled) writeCounts(out, counts.total); else out << "null";
    out << ",\"nodes\":[";
    for (std::size_t q = 0; q < impl_->recipes.size(); ++q) {
        if (q) out << ',';
        const auto& r = impl_->recipes[q];
        out << "{\"q\":" << q << ",\"parent_query\":" << r.parent << ",\"direction\":";
        if (r.parent < 0) out << "null"; else out << (r.outgoing ? "\"out\"" : "\"in\"");
        out << ",\"registered_blocks\":" << r.end - r.begin << ",\"counts\":";
        if (impl_->counters_enabled) writeCounts(out, counts.nodes[q]); else out << "null";
        out << '}';
    }
    out << "]}";
}
