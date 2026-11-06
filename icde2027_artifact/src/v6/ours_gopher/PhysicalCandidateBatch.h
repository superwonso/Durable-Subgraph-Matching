#ifndef DSM_PHYSICAL_CANDIDATE_BATCH_H
#define DSM_PHYSICAL_CANDIDATE_BATCH_H

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>
#include "CandidateDag.h"
#include "CandidateReadView.h"

// Explicit per-candidate temporal input. No executor-global "last lookup"
// state exists. This view expires with its DFS frame's owning batch.
struct ProbeHistoryRow {
    DagNodeId producer = kInvalidDagNode;
    int query_vertex = -1;
    int candidate = -1;
    const int* temporal_edge_ids = nullptr;
    std::size_t size = 0;
};

struct PhysicalCandidateBatch {
    bool owns_candidates = false;
    std::vector<int> owned_candidates;
    CandidateReadView borrowed_candidates;
    std::vector<int> temporal_edge_ids;
    DagNodeId producer = kInvalidDagNode;
    int query_vertex = -1;
    std::size_t history_stride = 0;
    // Optional census attribution, not a pruning decision or timing outcome.
    std::size_t lmax_rejected_candidates = 0;

    PhysicalCandidateBatch() = default;
    PhysicalCandidateBatch(const PhysicalCandidateBatch&) = delete;
    PhysicalCandidateBatch& operator=(const PhysicalCandidateBatch&) = delete;
    PhysicalCandidateBatch(PhysicalCandidateBatch&&) = default;
    PhysicalCandidateBatch& operator=(PhysicalCandidateBatch&&) = default;

    CandidateReadView view() const noexcept {
        return owns_candidates ? CandidateReadView(owned_candidates) : borrowed_candidates;
    }
    std::size_t size() const noexcept { return view().size(); }
    bool empty() const noexcept { return size() == 0; }
    int candidate(std::size_t index) const { return view()[index]; }
    bool hasHistoryRows() const noexcept {
        return producer != kInvalidDagNode && history_stride != 0;
    }
    ProbeHistoryRow historyRow(std::size_t index) const {
        if (!hasHistoryRows() || index >= size() ||
            size() > std::numeric_limits<std::size_t>::max() / history_stride ||
            temporal_edge_ids.size() != size() * history_stride) {
            throw std::logic_error("invalid frame-owned history row");
        }
        return {producer, query_vertex, candidate(index),
                temporal_edge_ids.data() + index * history_stride, history_stride};
    }
    static PhysicalCandidateBatch borrowed(CandidateReadView value) {
        PhysicalCandidateBatch result;
        result.borrowed_candidates = value;
        return result;
    }
    static PhysicalCandidateBatch owned(std::vector<int> value) {
        PhysicalCandidateBatch result;
        result.owns_candidates = true;
        result.owned_candidates = std::move(value);
        return result;
    }
};

#endif
