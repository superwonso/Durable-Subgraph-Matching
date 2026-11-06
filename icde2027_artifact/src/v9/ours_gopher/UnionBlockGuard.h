#ifndef DSM_V9_UNION_BLOCK_GUARD_H
#define DSM_V9_UNION_BLOCK_GUARD_H

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include "CandidateDag.h"
#include "TDTree.h"
#include "WindowStore.h"

// E6: necessary condition only. U_R is the OR of the parent's directed
// eroded edge rows for every member of the immutable candidate block R.
// If omega AND U_R is empty, no member's parent edge can extend omega.
// Other directed edges, injectivity and exact supports retain the P4/W tests.
class UnionBlockGuard {
public:
    struct Result { bool rejected = false; std::size_t candidates = 0; bool tested = true; };
    UnionBlockGuard(const Graph&, const Graph&, const QueryPlan&, const TDTree&, const WindowStore&);
    Result check(int query_vertex, int parent_data_vertex, window_algebra::WindowView) const;
    std::uint64_t builtBlocks() const { return built_.load(std::memory_order_relaxed); }
    std::uint64_t storageBytes() const;
    std::uint64_t setupNanoseconds() const { return setup_ns_; }
private:
    struct Payload { std::array<std::uint64_t, window_algebra::kDenseWords> words{}; };
    struct Cell { std::once_flag once; std::unique_ptr<Payload> value; };
    struct Recipe { int parent=-1; bool outgoing=false; std::size_t offset=0; };
    const Graph& graph_;
    const TDTree& tree_;
    const WindowStore& store_;
    std::vector<Recipe> recipes_;
    std::size_t cells_n_=0;
    std::unique_ptr<Cell[]> cells_;
    mutable std::atomic<std::uint64_t> built_{0};
    std::uint64_t setup_ns_=0;
};
#endif
