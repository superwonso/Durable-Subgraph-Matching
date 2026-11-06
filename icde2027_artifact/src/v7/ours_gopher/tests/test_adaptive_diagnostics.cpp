// Standalone Stage-A1 observer unit tests: no real graph, search, or pass.
// Root owns CMake integration and execution. No JSON dependency is required:
// named scalar tokens are checked in an explicitly selected JSON subsection.
#include "AdaptiveDiagnostics.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
using A = AdaptiveDiagnostics;
using U64 = std::uint64_t;
U64 checks = 0;

void require(bool condition, const std::string& description) {
    ++checks;
    if (!condition) throw std::runtime_error(description);
}

std::string snapshot(const A& observer) {
    std::ostringstream output;
    observer.writeJson(output);
    return output.str();
}

std::string totals(const std::string& json) {
    const std::string marker = "\"totals\":";
    const auto begin = json.find(marker);
    const auto end = json.find(",\"funnel_complete\":", begin);
    require(begin != std::string::npos && end != std::string::npos,
            "state totals section is present");
    return json.substr(begin + marker.size(), end - begin - marker.size());
}

std::string timer(const std::string& json) {
    const std::string marker = "\"neighbor_copy_timer\":";
    const auto begin = json.find(marker);
    require(begin != std::string::npos, "timer section is present");
    return json.substr(begin + marker.size());
}

void token(const std::string& section, const std::string& name,
           const std::string& expected) {
    const auto begin = section.find("\"" + name + "\":");
    require(begin != std::string::npos, "missing JSON field: " + name);
    const auto value = begin + name.size() + 3;
    const auto end = section.find_first_of(",}", value);
    require(end != std::string::npos && section.substr(value, end - value) == expected,
            "unexpected JSON value: " + name + " expected " + expected);
}

void number(const std::string& section, const std::string& name, U64 expected) {
    token(section, name, std::to_string(expected));
}

void testCeilLog2() {
    require(A::ceilLog2(0) == 0, "ceilLog2(0) frozen convention");
    require(A::ceilLog2(1) == 0, "ceilLog2(1) frozen convention");
    for (unsigned power = 1; power <= 63; ++power) {
        const U64 value = U64{1} << power;
        require(A::ceilLog2(value) == power, "ceilLog2 exact power of two");
        require(A::ceilLog2(value + 1) == power + 1, "ceilLog2 above power of two");
        if (power >= 2) {
            require(A::ceilLog2(value - 1) == power, "ceilLog2 below power of two");
        }
    }
    require(A::ceilLog2(std::numeric_limits<U64>::max()) == 64,
            "ceilLog2 maximum uint64 without overflow");
}

void testStateProxy() {
    A observer(true, false);
    require(observer.stateCountersEnabled(), "state switch enabled");
    require(!observer.neighborCopyTimerEnabled(), "timer switch disabled");
    // State 1: 5*(0+0+1+2+3)=30; merge proxy 5+0+1+2+3+8=19.
    // State 2: 0*(0+2)=0; merge proxy 0+1+4=5.
    // State 3: no closed neighbors; needed=0, merge proxy=2.
    observer.recordState(3, 7, 5, {0, 1, 2, 3, 8});
    observer.recordState(4, 8, 0, {1, 4});
    observer.recordState(3, 7, 2, {});
    observer.recordStructureOutput(3, 7, 2);
    observer.recordStructureOutput(4, 8, 0);
    observer.recordStructureOutput(3, 7, 1);
    const auto json = snapshot(observer);
    const auto values = totals(json);
    number(json, "schema_version", 1);
    token(json, "overflow", "false");
    token(json, "costs_valid", "true");
    number(values, "states", 3);
    number(values, "raw_candidate_elements", 7);
    number(values, "neighbor_lists", 7);
    number(values, "needed_elements", 30);
    number(values, "state_merge_proxy_elements", 26);
    number(values, "empty_R_states", 1);
    number(values, "empty_neighbor_lists", 1);
    number(values, "singleton_neighbor_lists", 2);
    number(values, "structure_output_candidates", 3);
    number(values, "neighbor_copy_elements", 0);
    number(values, "actual_structural_probes", 0);
    // A structure result was recorded but its candidates were not consumed.
    token(json, "funnel_complete", "false");
    require(values.find("\"raw_sizes\":{\"count\":3,\"sum\":7,\"min\":0,\"max\":5,") != std::string::npos,
            "R histogram summary");
    require(values.find("\"neighbor_sizes\":{\"count\":7,\"sum\":19,\"min\":0,\"max\":8,") != std::string::npos,
            "neighbor size histogram summary");
    require(values.find("\"output_sizes\":{\"count\":3,\"sum\":3,\"min\":0,\"max\":2,") != std::string::npos,
            "output size histogram summary");
    // Empty/zero-cost profiles are represented as zero, not a fabricated ratio.
    A zero(true, false);
    zero.recordState(1, 2, 7, {0, 1});
    number(totals(snapshot(zero)), "needed_elements", 0);
    require(snapshot(zero).find("\"gate_ratio\"") == std::string::npos,
            "observer never decides zero-denominator gate");
}

void testPhysicalAndShadowSeparation() {
    A observer(true, false);
    observer.recordNeighborCopy(2, 11, 3, 4);
    observer.recordNeighborCopy(2, 11, 0, 4);
    observer.recordDirectProbe(3);
    observer.recordDirectProbe(3);
    observer.recordShadowLmaxProbe(false, false);
    observer.recordShadowLmaxProbe(true, false);
    observer.recordShadowLmaxProbe(true, true);
    observer.recordNeighborCopyNanoseconds(2, 11, 999); // Timer is disabled.
    const auto json = snapshot(observer);
    const auto values = totals(json);
    number(values, "neighbor_copy_calls", 2);
    number(values, "neighbor_copy_elements", 3);
    number(values, "neighbor_copy_bytes", 12);
    number(values, "actual_structural_probes", 2);
    number(values, "diagnostic_metadata_probes", 3);
    number(values, "diagnostic_missing_edges", 1);
    number(values, "diagnostic_short_edges", 1);
    number(values, "states", 0);
    number(values, "needed_elements", 0);
    token(json, "neighbor_copy_timer", "null");
}

void testFunnelConservation() {
    A observer(true, false);
    observer.recordState(2, 12, 12, {2});
    observer.recordStructureOutput(2, 12, 7);
    observer.recordCandidateOutcome(2, A::Outcome::InjectivityDrop);
    observer.recordCandidateOutcome(2, A::Outcome::InjectivityDrop);
    observer.recordCandidateOutcome(2, A::Outcome::LmaxDrop);
    observer.recordCandidateOutcome(2, A::Outcome::LocalTemporalDrop);
    observer.recordCandidateOutcome(2, A::Outcome::CumulativeSupportDrop);
    observer.recordCandidateOutcome(2, A::Outcome::Extension);
    observer.recordCandidateOutcome(2, A::Outcome::Extension);
    const auto json = snapshot(observer);
    const auto values = totals(json);
    number(values, "raw_candidate_elements", 12);
    number(values, "structure_output_candidates", 7);
    number(values, "candidate_outcomes", 7);
    number(values, "drop_injectivity", 2);
    number(values, "drop_lmax", 1);
    number(values, "drop_local_temporal", 1);
    number(values, "drop_cumulative_support", 1);
    number(values, "injectivity_pass", 5);
    number(values, "lmax_pass", 4);
    number(values, "local_temporal_pass", 3);
    number(values, "temporal_pass", 2);
    number(values, "extensions", 2);
    token(json, "funnel_complete", "true");
    // Logical drops: (R-S)=5 structural +2 injective +1+1+1 time =10=R-E.
    // A duplicate outcome must invalidate the terminal-candidate reconciliation.
    observer.recordCandidateOutcome(2, A::Outcome::Extension);
    token(snapshot(observer), "funnel_complete", "false");

    A impossible(true, false);
    impossible.recordState(1, 1, 0, {});
    impossible.recordStructureOutput(1, 1, 1);
    impossible.recordCandidateOutcome(1, A::Outcome::Extension);
    token(snapshot(impossible), "funnel_complete", "false");
}

void callAllStateHooks(A& observer) {
    observer.recordState(1, 2, 7, {2, 3});
    observer.recordStructureOutput(1, 2, 3);
    observer.recordNeighborCopy(1, 2, 19, 4);
    observer.recordDirectProbe(1);
    observer.recordShadowLmaxProbe(true, false);
    observer.recordCandidateOutcome(1, A::Outcome::Extension);
}

void testTimerOnlyAndAllOff() {
    A observer(false, true);
    require(!observer.stateCountersEnabled(), "timer-only state switch disabled");
    require(observer.neighborCopyTimerEnabled(), "timer-only timer switch enabled");
    callAllStateHooks(observer); // Even accidental calls are no-ops.
    observer.recordNeighborCopyNanoseconds(1, 2, 3);
    observer.recordNeighborCopyNanoseconds(1, 2, 9);
    observer.recordNeighborCopyNanoseconds(4, 5, 5);
    const auto json = snapshot(observer);
    token(json, "state_profile", "null");
    token(json, "costs_valid", "false");
    token(json, "overflow", "false");
    const auto timing = timer(json);
    token(timing, "overflow", "false");
    number(timing, "samples", 3);
    number(timing, "nanoseconds", 17);
    require(timing.find("\"query_vertex\":1,\"node_id\":2,\"samples\":2,\"nanoseconds\":12") != std::string::npos,
            "timer context aggregation remains independent");
    require(json.find("\"neighbor_copy_elements\":") == std::string::npos,
            "no state counter payload in timer-only output");

    A disabled;
    require(!disabled.stateCountersEnabled() && !disabled.neighborCopyTimerEnabled(),
            "all observer switches default off");
    callAllStateHooks(disabled);
    disabled.recordNeighborCopyNanoseconds(1, 2, 100);
    const auto off = snapshot(disabled);
    token(off, "state_profile", "null");
    token(off, "neighbor_copy_timer", "null");
    token(off, "overflow", "false");
    std::ostringstream member_off;
    disabled.writeJsonMembers(member_off, false);
    require(member_off.str() == "  \"adaptive_diagnostics\": null,\n",
            "metrics member disabled form");
    std::ostringstream member_on;
    observer.writeJsonMembers(member_on, true);
    require(member_on.str() == "  \"adaptive_diagnostics\": " + json + ",\n",
            "metrics member enabled form");
}

void testOverflowFailsClosed() {
    const auto largest = std::numeric_limits<std::size_t>::max();
    A observer(true, false);
    // On 64-bit, multiplication itself overflows. On 32-bit, two products
    // overflow the byte sum. No large allocation or graph is involved.
    static_assert(sizeof(std::size_t) >= 4, "test requires a >=32-bit target");
    observer.recordNeighborCopy(1, 2, largest, largest);
    observer.recordNeighborCopy(1, 2, largest, largest);
    const auto json = snapshot(observer);
    token(json, "overflow", "true");
    token(json, "costs_valid", "false");
    token(json, "funnel_complete", "false");
    number(totals(json), "neighbor_copy_bytes", std::numeric_limits<U64>::max());

    if constexpr (sizeof(std::size_t) >= sizeof(U64)) {
        A state_overflow(true, false);
        state_overflow.recordState(1, 2, largest, {3});
        const auto state_json = snapshot(state_overflow);
        token(state_json, "overflow", "true");
        token(state_json, "costs_valid", "false");
        number(totals(state_json), "needed_elements", std::numeric_limits<U64>::max());
    }

    A timer_overflow(false, true);
    timer_overflow.recordNeighborCopyNanoseconds(1, 2, std::numeric_limits<U64>::max());
    timer_overflow.recordNeighborCopyNanoseconds(1, 2, 1);
    const auto timer_json = snapshot(timer_overflow);
    token(timer_json, "overflow", "false"); // State overflow remains independent.
    token(timer(timer_json), "overflow", "true");
    number(timer(timer_json), "samples", 2);
    number(timer(timer_json), "nanoseconds", std::numeric_limits<U64>::max());
}
} // namespace

int main() {
    try {
        testCeilLog2();
        testStateProxy();
        testPhysicalAndShadowSeparation();
        testFunnelConservation();
        testTimerOnlyAndAllOff();
        testOverflowFailsClosed();
        std::cout << "{\"status\":\"PASS\",\"suite\":\"adaptive_diagnostics\","
                     "\"fixtures\":6,\"checks\":" << checks
                  << ",\"native_graph_runs\":0}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Adaptive-diagnostics unit failed: " << error.what() << '\n';
        return 1;
    }
}
