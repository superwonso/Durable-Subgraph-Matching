#include "UnionBlockGuard.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>

UnionBlockGuard::UnionBlockGuard(const Graph& graph, const Graph& query,
    const QueryPlan& plan, const TDTree& tree, const WindowStore& store)
    : graph_(graph), tree_(tree), store_(store), recipes_(query.num_vertices) {
    const auto start=std::chrono::steady_clock::now();
    if (store.domain().representation() != window_algebra::Representation::Dense)
        throw std::invalid_argument("E6 union control requires a dense W-store");
    for (int q : plan.order) {
        auto& r=recipes_.at(static_cast<std::size_t>(q));
        r.offset=cells_n_;
        if (q==plan.root) continue;
        r.parent=plan.parent.at(static_cast<std::size_t>(q));
        r.outgoing=GraphUtils::hasEdge(query.adj,r.parent,q);
        if (!r.outgoing && !GraphUtils::hasEdge(query.adj,q,r.parent))
            throw std::logic_error("union parent has no directed query arc");
        cells_n_+=tree.candidateBlocks(q).size();
    }
    cells_=std::make_unique<Cell[]>(cells_n_);
    setup_ns_=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-start).count());
}

UnionBlockGuard::Result UnionBlockGuard::check(int q, int parent, window_algebra::WindowView prior) const {
    const auto& recipe=recipes_.at(static_cast<std::size_t>(q));
    if (recipe.parent<0) throw std::logic_error("union invoked on root");
    const auto& blocks=tree_.candidateBlocks(q);
    const auto found=std::lower_bound(blocks.begin(),blocks.end(),parent,
        [](const TDTreeBlock& b,int v){ return b.v_par<v; });
    if (found==blocks.end() || found->v_par!=parent) return {true,0,false};
    auto& cell=cells_[recipe.offset+static_cast<std::size_t>(found-blocks.begin())];
    std::call_once(cell.once,[&] {
        auto value=std::make_unique<Payload>();
        const auto& adjacency=(recipe.outgoing?graph_.adj:graph_.in_adj).at(static_cast<std::size_t>(parent));
        for (int candidate : found->V_cand) {
            const auto edge=std::lower_bound(adjacency.begin(),adjacency.end(),candidate,
                [](const Edge& e,int v){ return e.to<v; });
            if (edge==adjacency.end() || edge->to!=candidate)
                throw std::logic_error("union candidate lacks its parent edge");
            const auto view=store_.get(edge->temporal_edge_id);
            if (!view.isEmpty()) for (std::size_t i=0;i<store_.domain().wordCount();++i)
                value->words[i]|=view.words()[i];
        }
        cell.value=std::move(value); // published by call_once; then shared read-only
        built_.fetch_add(1,std::memory_order_relaxed);
    });
    std::uint64_t nonempty=0;
    for (std::size_t i=0;i<store_.domain().wordCount();++i)
        nonempty|=cell.value->words[i] & (prior.isUniversal()?~std::uint64_t{0}:
                                        prior.isEmpty()?0:prior.words()[i]);
    return {nonempty==0,found->V_cand.size()};
}

std::uint64_t UnionBlockGuard::storageBytes() const {
    return cells_n_*sizeof(Cell)+recipes_.capacity()*sizeof(Recipe)+builtBlocks()*sizeof(Payload);
}
