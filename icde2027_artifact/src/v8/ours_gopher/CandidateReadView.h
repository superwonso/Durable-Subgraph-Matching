#ifndef DSM_CANDIDATE_READ_VIEW_H
#define DSM_CANDIDATE_READ_VIEW_H

#include <cstddef>
#include <vector>
#include "Utils.h"

// A readonly ID projection, never a reinterpret_cast of Edge storage. The
// graph/TD owner is immutable during enumeration; owned results live in a DFS
// frame's PhysicalCandidateBatch, which constructs a fresh view on demand.
class CandidateReadView {
public:
    CandidateReadView() = default;
    explicit CandidateReadView(const std::vector<int>& ids) : ids_(&ids) {}
    explicit CandidateReadView(const std::vector<Edge>& edges) : edges_(&edges) {}
    std::size_t size() const noexcept {
        return ids_ ? ids_->size() : edges_ ? edges_->size() : 0;
    }
    bool empty() const noexcept { return size() == 0; }
    int operator[](std::size_t index) const noexcept {
        return ids_ ? (*ids_)[index] : (*edges_)[index].to;
    }
    const Edge* edge(std::size_t index) const noexcept {
        return edges_ ? &(*edges_)[index] : nullptr;
    }
    bool projectsEdges() const noexcept { return edges_ != nullptr; }
private:
    const std::vector<int>* ids_ = nullptr;
    const std::vector<Edge>* edges_ = nullptr;
};

#endif
