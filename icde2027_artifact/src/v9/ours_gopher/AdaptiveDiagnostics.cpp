#include "AdaptiveDiagnostics.h"
#include "Utils.h"

#include <chrono>

// Keep the complete opt-in timed materialization policy out of the default
// statistics-OFF kernel, including in a future interprocedural build. This is
// not a clock-call wrapper: the selected path owns both the ID-copy operation
// and its observation. All-OFF continues to execute the original caller loop.
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void AdaptiveDiagnostics::copyNeighborIdsTimed(
    const std::vector<Edge>& source,
    std::vector<int>& destination,
    int query_vertex,
    std::int64_t node_id) {
    // Preconditions at all call sites: timer enabled, capacity reserved.
    // Allocation/reserve, function dispatch, and accounting are not timed.
    using Clock = std::chrono::steady_clock;
    const auto begin = Clock::now();
    for (const Edge& edge : source) destination.push_back(edge.to);
    const auto end = Clock::now();
    const auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
    recordNeighborCopyNanoseconds(query_vertex, node_id, elapsed);
}
