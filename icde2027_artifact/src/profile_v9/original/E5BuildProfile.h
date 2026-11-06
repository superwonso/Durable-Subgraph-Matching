#pragma once
#include <chrono>
#include <cstdint>

// Diagnostic-only timers. The build path is serial; these categories are
// disjoint. Scope::finish also runs on early return/continue via destruction.
namespace e5_build_profile {
using Clock = std::chrono::steady_clock;
struct Totals {
    std::uint64_t vertex_ns=0, temporal_ns=0, leaf_ns=0;
    std::uint64_t vertex_calls=0, temporal_calls=0, leaf_calls=0;
};
inline thread_local Totals totals;
class Scope {
    std::uint64_t& destination_;
    Clock::time_point start_;
    bool finished_=false;
public:
    Scope(std::uint64_t& ns, std::uint64_t& calls):destination_(ns),start_(Clock::now()) { ++calls; }
    void finish() {
        if (!finished_) {
            destination_ += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start_).count();
            finished_=true;
        }
    }
    ~Scope() { finish(); }
};
}
