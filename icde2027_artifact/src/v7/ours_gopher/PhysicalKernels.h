#ifndef DSM_PHYSICAL_KERNELS_H
#define DSM_PHYSICAL_KERNELS_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>
#include "CandidateReadView.h"
#include "CandidateDag.h"
#include "OperatorCounters.h"

namespace PhysicalKernels {

inline std::uint64_t checkedAdd(std::uint64_t a, std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a)
        throw std::overflow_error("physical state cost overflow");
    return a + b;
}
inline std::uint64_t checkedMultiply(std::uint64_t a, std::uint64_t b) {
    if (b && a > std::numeric_limits<std::uint64_t>::max() / b)
        throw std::overflow_error("physical state cost overflow");
    return a * b;
}
struct StateCosts { std::uint64_t probe = 0, merge = 0; };
inline StateCosts stateCosts(std::size_t seed, const std::vector<CandidateReadView>& neighbors) {
    StateCosts result{0, static_cast<std::uint64_t>(seed)};
    std::uint64_t logarithms = 0;
    for (auto view : neighbors) {
        result.merge = checkedAdd(result.merge, view.size());
        logarithms = checkedAdd(logarithms, StructuralProfile::ceilLog2(view.size()));
    }
    result.probe = checkedMultiply(seed, logarithms);
    return result;
}
inline bool chooseProbe(StateCosts cost, double alpha) {
    // Exact comparisons for the three preregistered alpha values. In
    // particular .5 uses floor division, not rounded floating point costs.
    if (alpha == 0.5) return cost.probe <= cost.merge / 2;
    if (alpha == 1.0) return cost.probe <= cost.merge;
    if (alpha == 2.0) return cost.merge > std::numeric_limits<std::uint64_t>::max() / 2 ||
        cost.probe <= cost.merge * 2;
    throw std::invalid_argument("physical alpha must be 0.5, 1, or 2");
}
inline std::size_t lowerBound(CandidateReadView view, int candidate, std::size_t first = 0) {
    std::size_t last = view.size();
    while (first < last) {
        const std::size_t mid = first + (last - first) / 2;
        if (view[mid] < candidate) first = mid + 1;
        else last = mid;
    }
    return first;
}
inline std::vector<int> intersectViews(CandidateReadView lhs, CandidateReadView rhs,
                                     bool adaptive, OperatorCounters* ops,
                                     DagNodeId node) {
    OPS_ADD(ops, structural_intersections_executed, 1);
    std::vector<int> result;
    result.reserve(std::min(lhs.size(), rhs.size()));
    CandidateReadView small = lhs, large = rhs;
    if (small.size() > large.size()) std::swap(small, large);
    const bool binary = adaptive && !small.empty() &&
        large.size() / small.size() >= kDagAdaptiveIntersectionRatio;
    if (binary) {
        OPS_ADD(ops, galloping_intersections, 1);
        std::size_t cursor = 0;
        for (std::size_t i = 0; i < small.size(); ++i) {
            cursor = lowerBound(large, small[i], cursor);
            if (cursor == large.size()) break;
            if (large[cursor] == small[i]) result.push_back(small[i]);
        }
    } else {
        std::size_t i = 0, j = 0;
        while (i < lhs.size() && j < rhs.size()) {
            if (lhs[i] < rhs[j]) ++i;
            else if (rhs[j] < lhs[i]) ++j;
            else { result.push_back(lhs[i]); ++i; ++j; }
        }
    }
    if (ops) ops->physical_profile.recordBinary(
        {StructuralProfile::Backend::RewrittenDag, node, -1}, lhs.size(), rhs.size(), result.size(), binary);
    OPS_ADD(ops, intersection_output_elements, result.size());
    OPS_ADD(ops, physical_output_elements_materialized, result.size());
    return result;
}

} // namespace PhysicalKernels
#endif
