#ifndef DSM_OPERATOR_KERNELS_H
#define DSM_OPERATOR_KERNELS_H

#include <algorithm>
#include <iterator>
#include <vector>

#include "OperatorCounters.h"
#include "Utils.h"

// Fixed BEFORE evaluation. This is not selected or tuned from run timings.
constexpr std::size_t kOperatorGallopingRatio = 32;

namespace OperatorKernels {

inline std::vector<int> intersectCandidates(
    const std::vector<int>& lhs, const std::vector<int>& rhs,
    bool adaptive, OperatorCounters* ops,
    StructuralProfile::Context profile_context = {}) {
    OPS_ADD(ops, structural_intersections_executed, 1);
    std::vector<int> result;
    result.reserve(std::min(lhs.size(), rhs.size()));
    const auto* small = &lhs;
    const auto* large = &rhs;
    if (small->size() > large->size()) std::swap(small, large);
    if (adaptive && !small->empty() &&
        large->size() / small->size() >= kOperatorGallopingRatio) {
        OPS_ADD(ops, galloping_intersections, 1);
        auto cursor = large->begin();
        for (int value : *small) {
            cursor = std::lower_bound(cursor, large->end(), value);
            if (cursor == large->end()) break;
            if (*cursor == value) result.push_back(value);
        }
        if (ops != nullptr) ops->physical_profile.recordBinary(
            profile_context, lhs.size(), rhs.size(), result.size(), true);
    } else {
        std::set_intersection(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                              std::back_inserter(result));
        if (ops != nullptr) ops->physical_profile.recordBinary(
            profile_context, lhs.size(), rhs.size(), result.size(), false);
    }
    OPS_ADD(ops, intersection_output_elements, result.size());
    return result;
}

inline std::vector<TimeInterval> intersectIntervals(
    const std::vector<TimeInterval>& lhs,
    const std::vector<TimeInterval>& rhs,
    int minimum_length, bool adaptive, OperatorCounters* ops) {
    OPS_ADD(ops, time_intersections_executed, 1);
    OPS_ADD(ops, time_input_elements, lhs.size() + rhs.size());
    // minimum_length=1 denotes the RAW intersection IR operation introduced
    // by P1; its κ is represented once by a separate KappaNormalize node.
    if (minimum_length > 1) OPS_ADD(ops, kappa_executed, 1);
    const auto* small = &lhs;
    const auto* large = &rhs;
    if (small->size() > large->size()) std::swap(small, large);
    std::vector<TimeInterval> result;
    if (adaptive && !small->empty() &&
        large->size() / small->size() >= kOperatorGallopingRatio) {
        OPS_ADD(ops, galloping_intersections, 1);
        result.reserve(std::min(lhs.size(), rhs.size()));
        auto cursor = large->begin();
        for (const auto& interval : *small) {
            OPS_ADD(ops, time_elements_read, 1);
            cursor = std::lower_bound(
                cursor, large->end(), interval.start,
                [ops](const TimeInterval& item, int start) {
                    OPS_ADD(ops, time_elements_read, 1);
                    return item.end < start;
                });
            for (auto it = cursor; it != large->end(); ++it) {
                OPS_ADD(ops, time_elements_read, 1);
                if (it->start > interval.end) break;
                const int start = std::max(interval.start, it->start);
                const int end = std::min(interval.end, it->end);
                const auto length = static_cast<long long>(end) - start + 1;
                if (end >= start && length >= minimum_length) result.push_back({start, end});
            }
        }
    } else if (ops == nullptr) {
        result = intersectTimeIntervals(lhs, rhs, minimum_length);
    } else {
        // Instrumentation-only mirror of the original two-pointer kernel.
        // Count logical interval-record visits, not scalar field loads or
        // cache misses: two input records inspected per merge iteration.
        result.reserve(std::min(lhs.size(), rhs.size()));
        std::size_t i = 0, j = 0;
        const int required = std::max(1, minimum_length);
        while (i < lhs.size() && j < rhs.size()) {
            OPS_ADD(ops, time_elements_read, 2);
            const int start = std::max(lhs[i].start, rhs[j].start);
            const int end = std::min(lhs[i].end, rhs[j].end);
            const auto length = static_cast<long long>(end) - start + 1;
            if (end >= start && length >= required) result.push_back({start, end});
            if (lhs[i].end < rhs[j].end) ++i;
            else ++j;
        }
    }
    OPS_ADD(ops, intersection_output_elements, result.size());
    return result;
}

inline std::vector<TimeInterval> kappa(
    const std::vector<TimeInterval>& input, int k, OperatorCounters* ops) {
    OPS_ADD(ops, kappa_executed, 1);
    OPS_ADD(ops, time_input_elements, input.size());
    std::vector<TimeInterval> result;
    result.reserve(input.size());
    for (const auto& interval : input) {
        OPS_ADD(ops, time_elements_read, 1);
        if (interval.length() >= k) result.push_back(interval);
    }
    return result;
}

} // namespace OperatorKernels
#endif
