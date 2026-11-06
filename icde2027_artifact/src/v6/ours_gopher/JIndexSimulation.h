#ifndef DSM_V5_J_INDEX_SIMULATION_H
#define DSM_V5_J_INDEX_SIMULATION_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <vector>

#include "DagExecutor.h"

// Stage A'2 observer only. It never supplies candidates/support to DFS and
// never changes the actual S1 strategy, ordering, pruning, or extensions.
class JIndexSimulation {
public:
    static constexpr std::array<std::size_t, 4> kBlockSizes{{16, 32, 64, 128}};
    static constexpr std::size_t kMinimumNeighbors = 64;

    struct StateToken {
        StateToken() = default;
        StateToken(const StateToken&) = delete;
        StateToken& operator=(const StateToken&) = delete;
        StateToken(StateToken&&) = default;
        StateToken& operator=(StateToken&&) = default;
        const JIndexSimulation* owner = nullptr;
        void* local_counter_slot = nullptr;  // observer-private, owned by this simulation
        int query_vertex = -1;
        DagNodeId node = kInvalidDagNode;
        std::vector<std::uint8_t> scan_mask;
        std::array<std::vector<std::uint8_t>, 4> index_masks;
        std::array<bool, 4> available{{false, false, false, false}};
    };

    JIndexSimulation(const Graph& temporal_graph, const Graph& query_graph,
                     const QueryPlan& plan, const CandidateDag& dag,
                     const TDTree& td_tree, int minimum_duration);
    ~JIndexSimulation();
    JIndexSimulation(const JIndexSimulation&) = delete;
    JIndexSimulation& operator=(const JIndexSimulation&) = delete;

    // Call once after the real S1 structural batch has been created, before
    // its candidate loop. This shadow-checks injectivity without pruning.
    StateToken observe(int query_vertex, const SearchState& state,
                       const PhysicalCandidateBatch& batch) const;

    // Call only after the original interval/J path accepts this candidate.
    // Any candidate absent from a valid simulated mask is a counterexample
    // and throws immediately. No observer ever changes the accepted result.
    void recordAccepted(const StateToken& token, std::size_t candidate_index) const;
    // Call after all observer workers have joined; merges their plain local
    // counters and independently verifies totals against the per-node sums.
    void writeJson(std::ostream& output) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
