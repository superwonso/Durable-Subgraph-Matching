#include "SearchStatistics.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>

#if defined(__GNUC__) || defined(__clang__)
#define GOPHER_NOINLINE __attribute__((noinline, used))
#else
#define GOPHER_NOINLINE
#endif

extern "C" GOPHER_NOINLINE void gopherStatisticsOffAtomicProbe(
    SearchStatisticsCollector& statistics) {
    statistics.addSpecialized<false>(statistics.dfs_states, 17);
    statistics.setSpecialized<false>(statistics.search_nanoseconds, 29);
}

extern "C" GOPHER_NOINLINE void gopherStatisticsOnAtomicProbe(
    SearchStatisticsCollector& statistics) {
    statistics.addSpecialized<true>(statistics.dfs_states, 17);
    statistics.setSpecialized<true>(statistics.search_nanoseconds, 29);
}

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        SearchStatisticsCollector statistics(true);
        gopherStatisticsOffAtomicProbe(statistics);
        SearchStatistics snapshot = statistics.snapshot();
        require(
            snapshot.dfs_states == 0 &&
                snapshot.search_nanoseconds == 0,
            "the statistics-OFF specialization mutated an atomic counter");

        gopherStatisticsOnAtomicProbe(statistics);
        snapshot = statistics.snapshot();
        require(
            snapshot.dfs_states == 17 &&
                snapshot.search_nanoseconds == 29,
            "the statistics-ON specialization changed counter semantics");

        SearchStatisticsCollector runtime_disabled(false);
        runtime_disabled.add(runtime_disabled.dfs_states, 41);
        runtime_disabled.set(
            runtime_disabled.search_nanoseconds, 43);
        const SearchStatistics disabled_snapshot =
            runtime_disabled.snapshot();
        require(
            disabled_snapshot.dfs_states == 0 &&
                disabled_snapshot.search_nanoseconds == 0,
            "the compatibility runtime API no longer honors disabled mode");

        std::cout << "Statistics specialization tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}

