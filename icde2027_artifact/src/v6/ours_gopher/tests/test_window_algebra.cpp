// Unit/property executable only. This file does not launch graph experiments.
// Frozen workload: seed 20261020; 1,000,000 pairs round-robin over 30 strata;
// 1,000 chains per stratum with lengths 1..10 equally represented.
#include "../WindowAlgebra.h"
#include "../Utils.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace allocation_probe {
thread_local bool enabled = false;
thread_local std::uint64_t calls = 0;
}
// Count allocations only inside successful kernel calls after explicit reserve.
// Oracle construction, logging, exception construction and reserve are excluded.
void* operator new(std::size_t bytes) {
    if (allocation_probe::enabled) ++allocation_probe::calls;
    if (void* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
namespace wa = window_algebra;
using Points = std::vector<std::uint8_t>;
constexpr std::uint64_t kSeed = 20261020;
constexpr std::uint64_t kPairCount = 1000000;
constexpr std::uint64_t kChainCount = 30000;
constexpr std::array<int, 6> kHorizons{{8, 63, 64, 65, 200, 1000}};
constexpr std::array<int, 5> kDurabilities{{2, 3, 4, 5, 6}};

struct Context {
    std::filesystem::path output;
    std::string phase = "units";
    std::string representation;
    std::uint64_t index = 0;
    int horizon = 0, k = 0, step = 0;
    std::int64_t origin = 0;
    std::vector<wa::Interval> a, b;
    bool failure_written = false;
} context;
std::uint64_t unit_checks = 0, kernel_calls = 0, checked_chain_steps = 0;

std::string escaped(const std::string& value) {
    std::string result;
    for (const auto ch : value) {
        if (ch == '\\' || ch == '"') { result += '\\'; result += ch; }
        else if (ch == '\n') result += "\\n";
        else if (ch == '\r') result += "\\r";
        else if (ch == '\t') result += "\\t";
        else result += ch;
    }
    return result;
}
void writeIntervals(std::ostream& out, const std::vector<wa::Interval>& values) {
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        out << '[' << values[i].start << ',' << values[i].end << ']';
    }
    out << ']';
}
[[noreturn]] void fail(const std::string& message) {
    allocation_probe::enabled = false;
    if (!context.failure_written) {
        context.failure_written = true;
        std::ofstream out(context.output / "COUNTEREXAMPLE.json", std::ios::out | std::ios::trunc);
        out << "{\n\"status\":\"FAIL\",\"seed\":" << kSeed
            << ",\"phase\":\"" << escaped(context.phase) << "\",\"index\":" << context.index
            << ",\"T\":" << context.horizon << ",\"k\":" << context.k
            << ",\"origin\":" << context.origin << ",\"step\":" << context.step
            << ",\"representation\":\"" << escaped(context.representation)
            << "\",\"reason\":\"" << escaped(message) << "\",\"A\":";
        writeIntervals(out, context.a); out << ",\"B\":"; writeIntervals(out, context.b);
        out << "\n}\n";
    }
    throw std::runtime_error(message);
}
void require(bool condition, const std::string& message) {
    if (!condition) fail(message);
}
template<class Fn>
void kernel(Fn&& fn) {
    const auto previous = allocation_probe::calls;
    allocation_probe::enabled = true;
    try { fn(); }
    catch (...) { allocation_probe::enabled = false; throw; }
    allocation_probe::enabled = false;
    ++kernel_calls;
    require(allocation_probe::calls == previous, "successful kernel path allocated heap storage");
}
template<class Exception, class Fn>
void mustThrow(Fn&& fn, const char* message) {
    bool caught = false;
    try { fn(); } catch (const Exception&) { caught = true; }
    require(caught, message); ++unit_checks;
}

// Independent oracle: explicit integer snapshots; no production W/interval
// algebra is used to calculate expected starts, kappa or chain support.
Points pointIntersection(const Points& a, const Points& b) {
    require(a.size() == b.size(), "oracle input sizes differ");
    Points result(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) result[i] = a[i] && b[i];
    return result;
}
Points oracleKappa(const Points& input, int k) {
    Points result(input.size(), 0);
    std::size_t i = 0;
    while (i < input.size()) {
        if (!input[i]) { ++i; continue; }
        const auto first = i;
        while (i < input.size() && input[i]) ++i;
        if (i - first >= static_cast<std::size_t>(k))
            std::fill(result.begin() + static_cast<std::ptrdiff_t>(first),
                      result.begin() + static_cast<std::ptrdiff_t>(i), 1);
    }
    return result;
}
Points oracleWindows(const Points& input, int k) {
    if (input.size() < static_cast<std::size_t>(k)) return {};
    Points result(input.size() - static_cast<std::size_t>(k) + 1, 0);
    for (std::size_t start = 0; start < result.size(); ++start) {
        bool all = true;
        for (int offset = 0; offset < k; ++offset)
            all = all && input[start + static_cast<std::size_t>(offset)];
        result[start] = all;
    }
    return result;
}
std::vector<wa::Interval> pointRuns(const Points& input, std::int64_t origin) {
    std::vector<wa::Interval> result;
    for (std::size_t i = 0; i < input.size();) {
        if (!input[i]) { ++i; continue; }
        const auto first = i;
        while (i < input.size() && input[i]) ++i;
        result.push_back({origin + static_cast<std::int64_t>(first),
                          origin + static_cast<std::int64_t>(i - 1)});
    }
    return result;
}
wa::IntervalView iv(const std::vector<wa::Interval>& values) {
    return {values.data(), values.size()};
}
void checkIntervals(wa::IntervalView actual, const std::vector<wa::Interval>& expected, const char* name) {
    if (actual.size != expected.size()) {
        std::ostringstream detail;
        detail << name << ": interval count " << actual.size << " != " << expected.size()
               << "; expected="; writeIntervals(detail, expected);
        detail << "; actual=";
        std::vector<wa::Interval> observed;
        if (actual.size) observed.assign(actual.data, actual.data + actual.size);
        writeIntervals(detail, observed);
        fail(detail.str());
    }
    for (std::size_t i = 0; i < actual.size; ++i) {
        if (!(actual.data[i] == expected[i])) {
            std::ostringstream detail;
            detail << name << ": interval " << i << " actual=[" << actual.data[i].start
                   << ',' << actual.data[i].end << "] expected=[" << expected[i].start
                   << ',' << expected[i].end << ']';
            fail(detail.str());
        }
    }
}
void checkWindow(wa::WindowView actual, const Points& expected, const char* name) {
    require(!actual.isUniversal(), std::string(name) + ": unresolved universal support");
    const bool empty = std::none_of(expected.begin(), expected.end(), [](auto x) { return x != 0; });
    require(actual.isEmpty() == empty, std::string(name) + ": empty-tag mismatch");
    if (empty) return;
    if (actual.kind() == wa::Kind::Dense) {
        require(actual.domain().startCount() == expected.size(), std::string(name) + ": domain length mismatch");
        for (std::size_t i = 0; i < expected.size(); ++i)
            if (((actual.words()[i / 64] >> (i % 64)) & 1U) != expected[i])
                fail(std::string(name) + ": dense start differs at offset " + std::to_string(i));
        if (expected.size() % 64)
            require((actual.words()[actual.wordCount() - 1] >> (expected.size() % 64)) == 0,
                    std::string(name) + ": nonzero padding");
    } else {
        require(actual.kind() == wa::Kind::Intervals, std::string(name) + ": wrong real-support kind");
        checkIntervals(actual.starts(), pointRuns(expected, actual.domain().minimum()), name);
    }
}

// A reproducible mixture includes full/empty, independent bits, long bursts,
// sparse points and dense-with-gaps; randomness never selects an experimental arm.
Points randomPoints(std::mt19937_64& rng, int horizon, std::uint64_t ordinal) {
    Points result(static_cast<std::size_t>(horizon), 0);
    switch (ordinal % 8) {
        case 0: break;
        case 1: std::fill(result.begin(), result.end(), 1); break;
        case 2: case 3: case 4: {
            const unsigned threshold = ordinal % 8 == 2 ? 25 : (ordinal % 8 == 3 ? 60 : 90);
            for (auto& value : result) value = (rng() % 100) < threshold;
            break;
        }
        default: {
            bool active = (rng() & 1U) != 0;
            for (std::size_t i = 0; i < result.size();) {
                const std::size_t count = 1 + static_cast<std::size_t>(rng() % (ordinal % 8 == 7 ? 4 : 31));
                const auto last = std::min(result.size(), i + count);
                std::fill(result.begin() + static_cast<std::ptrdiff_t>(i),
                          result.begin() + static_cast<std::ptrdiff_t>(last), active);
                i = last; active = !active;
            }
            break;
        }
    }
    return result;
}

struct Workspace {
    wa::WindowBuffer a, b, conjunction, normalized_a, normalized_b, exact_intersection;
    wa::WindowBuffer chain, next_chain;
    wa::IntervalBuffer restored;
    Workspace() {
        // This reserve is outside every tested kernel and covers all <=1000
        // point-set fixtures. Longer boundary units reserve independently.
        for (auto* buffer : {&a, &b, &conjunction, &normalized_a, &normalized_b,
                             &exact_intersection, &chain, &next_chain})
            buffer->reserveIntervals(1001);
        restored.reserveIntervals(1001);
    }
};

void testPair(std::mt19937_64& rng, std::uint64_t index, Workspace& work) {
    const auto stratum = static_cast<std::size_t>(index % 30);
    const int horizon = kHorizons[stratum / 5], k = kDurabilities[stratum % 5];
    const std::int64_t origin = index % 3 == 0 ? -137 : (index % 3 == 1 ? 0 : 173);
    const auto a = randomPoints(rng, horizon, index / 30);
    const auto b = randomPoints(rng, horizon, index / 30 + 3);
    const auto both = pointIntersection(a, b);
    const auto ka = oracleKappa(a, k), kb = oracleKappa(b, k);
    const auto wa_expected = oracleWindows(a, k), wb_expected = oracleWindows(b, k);
    const auto wi_expected = oracleWindows(both, k);
    const auto a_runs = pointRuns(a, origin), b_runs = pointRuns(b, origin);
    const auto ka_runs = pointRuns(ka, origin), kb_runs = pointRuns(kb, origin);
    const auto both_runs = pointRuns(both, origin), ki_runs = pointRuns(oracleKappa(both, k), origin);
    context.phase = "pairs"; context.index = index; context.horizon = horizon;
    context.k = k; context.origin = origin; context.step = 0; context.a = a_runs; context.b = b_runs;
    for (const auto policy : {wa::RepresentationPolicy::Auto, wa::RepresentationPolicy::ForceIntervals}) {
        context.representation = policy == wa::RepresentationPolicy::Auto ? "auto_dense" : "forced_intervals";
        const auto domain = wa::Domain::make(origin, origin + horizon - 1, k, policy);
        kernel([&] { wa::buildInto(domain, iv(a_runs), work.a); });
        kernel([&] { wa::buildInto(domain, iv(b_runs), work.b); });
        kernel([&] { wa::buildInto(domain, iv(ka_runs), work.normalized_a); });
        kernel([&] { wa::buildInto(domain, iv(kb_runs), work.normalized_b); });
        kernel([&] { wa::buildInto(domain, iv(both_runs), work.exact_intersection); });
        kernel([&] { wa::intersectInto(domain, work.a.view(), work.b.view(), work.conjunction); });
        checkWindow(work.a.view(), wa_expected, "W(A)");
        checkWindow(work.b.view(), wb_expected, "W(B)");
        checkWindow(work.normalized_a.view(), wa_expected, "W(kappa(A)) = W(A)");
        checkWindow(work.normalized_b.view(), wb_expected, "W(kappa(B)) = W(B)");
        checkWindow(work.exact_intersection.view(), wi_expected, "W(A intersection B)");
        checkWindow(work.conjunction.view(), wi_expected, "W(A) AND W(B)");
        kernel([&] { wa::restoreInto(domain, work.a.view(), work.restored); });
        checkIntervals(work.restored.view(), ka_runs, "restore(W(A)) = kappa(A)");
        kernel([&] { wa::restoreInto(domain, work.b.view(), work.restored); });
        checkIntervals(work.restored.view(), kb_runs, "restore(W(B)) = kappa(B)");
        kernel([&] { wa::restoreInto(domain, work.conjunction.view(), work.restored); });
        checkIntervals(work.restored.view(), ki_runs, "restore(W(A) AND W(B))");
    }
}

void testChain(std::mt19937_64& rng, std::uint64_t index, Workspace& work) {
    const auto stratum = static_cast<std::size_t>(index % 30);
    const int horizon = kHorizons[stratum / 5], k = kDurabilities[stratum % 5];
    const int length = 1 + static_cast<int>((index / 30) % 10);
    const std::int64_t origin = index % 2 ? -501 : 29;
    // Both representations see the exact same pre-generated chain.
    std::vector<Points> histories;
    std::vector<std::vector<wa::Interval>> intervals;
    for (int step = 0; step < length; ++step) {
        histories.push_back(randomPoints(rng, horizon, index / 30 + static_cast<std::uint64_t>(step) + 1));
        intervals.push_back(pointRuns(histories.back(), origin));
    }
    context.phase = "chains"; context.index = index; context.horizon = horizon;
    context.k = k; context.origin = origin;
    for (const auto policy : {wa::RepresentationPolicy::Auto, wa::RepresentationPolicy::ForceIntervals}) {
        context.representation = policy == wa::RepresentationPolicy::Auto ? "auto_dense" : "forced_intervals";
        const auto domain = wa::Domain::make(origin, origin + horizon - 1, k, policy);
        kernel([&] { wa::setUniversal(domain, work.chain); });
        Points support(static_cast<std::size_t>(horizon), 1);
        for (int step = 0; step < length; ++step) {
            context.step = step + 1; context.a = pointRuns(support, origin);
            context.b = intervals[static_cast<std::size_t>(step)];
            // Mirrors the original recurrence, including both kappa operations.
            support = oracleKappa(pointIntersection(support, oracleKappa(histories[static_cast<std::size_t>(step)], k)), k);
            kernel([&] { wa::buildInto(domain, iv(intervals[static_cast<std::size_t>(step)]), work.b); });
            kernel([&] { wa::intersectInto(domain, work.chain.view(), work.b.view(), work.next_chain); });
            std::swap(work.chain, work.next_chain);
            checkWindow(work.chain.view(), oracleWindows(support, k), "chain W support");
            kernel([&] { wa::restoreInto(domain, work.chain.view(), work.restored); });
            checkIntervals(work.restored.view(), pointRuns(support, origin), "chain exact active intervals");
            ++checked_chain_steps;
        }
    }
}

void checkBuildRestore(const wa::Domain& domain, const std::vector<wa::Interval>& input,
                       const std::vector<wa::Interval>& expected) {
    wa::WindowBuffer built; wa::IntervalBuffer restored;
    built.reserveIntervals(input.size());
    kernel([&] { wa::buildInto(domain, iv(input), built); });
    restored.reserveIntervals(wa::restoreIntervalCapacity(domain, built.view()));
    kernel([&] { wa::restoreInto(domain, built.view(), restored); });
    checkIntervals(restored.view(), expected, "boundary build/restore"); ++unit_checks;
}
void units() {
    context.phase = "units";
    const auto low = std::numeric_limits<std::int64_t>::min();
    const auto high = std::numeric_limits<std::int64_t>::max();
    for (const auto policy : {wa::RepresentationPolicy::Auto, wa::RepresentationPolicy::ForceIntervals}) {
        context.representation = policy == wa::RepresentationPolicy::Auto ? "auto" : "intervals";
        for (const auto origin : {std::int64_t{-15}, std::int64_t{0},
                                  static_cast<std::int64_t>(std::numeric_limits<int>::min()),
                                  static_cast<std::int64_t>(std::numeric_limits<int>::max()) - 80,
                                  low, high - 80}) {
            const auto domain = wa::Domain::make(origin, origin + 80, 3, policy);
            checkBuildRestore(domain, {{origin, origin + 1}, {origin + 2, origin + 3},
                                      {origin + 7, origin + 7}, {origin + 10, origin + 19}},
                              {{origin, origin + 3}, {origin + 10, origin + 19}});
            checkBuildRestore(domain, {{origin, origin + 8}, {origin + 2, origin + 3},
                                      {origin + 8, origin + 12}}, {{origin, origin + 12}});
            require(domain.timestampAt(80) == origin + 80, "boundary timestampAt endpoint"); ++unit_checks;
        }
        for (int horizon : {0, 1, 2, 3, 63, 64, 65, 66, 1025, 1026, 2049}) {
            const auto domain = wa::Domain::make(-10, -10 + horizon - 1, 3, policy);
            const std::vector<wa::Interval> input = horizon ? std::vector<wa::Interval>{{-10, -10 + horizon - 1}}
                                                           : std::vector<wa::Interval>{};
            checkBuildRestore(domain, input, horizon >= 3 ? input : std::vector<wa::Interval>{});
            wa::WindowBuffer universal, empty, result;
            kernel([&] { wa::setUniversal(domain, universal); });
            kernel([&] { wa::buildInto(domain, {}, empty); });
            kernel([&] { wa::intersectInto(domain, universal.view(), universal.view(), result); });
            require(result.view().isUniversal() && !result.view().isEmpty(), "universal identity lost");
            kernel([&] { wa::intersectInto(domain, universal.view(), empty.view(), result); });
            require(result.view().isEmpty(), "empty must absorb universal"); ++unit_checks;
            wa::IntervalBuffer restored;
            mustThrow<std::invalid_argument>([&] { wa::restoreInto(domain, universal.view(), restored); },
                                            "universal restore must explicitly fail");
            mustThrow<std::invalid_argument>([&] { wa::restoreIntervalCapacity(domain, universal.view()); },
                                            "universal restore bound must explicitly fail");
        }
    }
    require(wa::Domain::make(0, 1024, 2).representation() == wa::Representation::Dense, "16-word boundary");
    require(wa::Domain::make(0, 1025, 2).representation() == wa::Representation::Intervals, ">16-word fallback");
    unit_checks += 2;
    mustThrow<std::overflow_error>([&] { wa::Domain::make(low, high, 2); }, "full int64 span must reject overflow");
    const auto huge = wa::Domain::make(low, high - 1, 2);
    require(huge.span() == std::numeric_limits<std::uint64_t>::max() &&
            huge.timestampAt(huge.span() - 1) == high - 1 &&
            huge.timestampAt(std::uint64_t{1} << 63) == 0,
            "near-full int64 original axis"); ++unit_checks;
    checkBuildRestore(huge, {{low, low + 4}, {high - 5, high - 1}},
                      {{low, low + 4}, {high - 5, high - 1}});
    checkBuildRestore(wa::Domain::make(low, high - 1, high),
                      {{low, high - 1}}, {{low, high - 1}});
    mustThrow<std::invalid_argument>([] { wa::Domain::make(0, 10, 0); }, "nonpositive k");
    mustThrow<std::out_of_range>([] { wa::Domain::make(0, 10, 2).timestampAt(11); }, "outside timestamp offset");
    mustThrow<std::logic_error>([] { wa::Domain::make(0, 1, 3).lastStart(); }, "no-window lastStart");
    mustThrow<std::overflow_error>([] {
        wa::intervalIntersectionUpperBound(std::numeric_limits<std::size_t>::max(), 1);
    }, "capacity addition overflow");
    const auto dense = wa::Domain::make(0, 65, 2);
    require(wa::densePayloadBytes(dense, 3) == 48, "dense payload estimate"); ++unit_checks;
    require(wa::densePayloadBytes(wa::Domain::make(0, 0, 2), std::numeric_limits<std::uint64_t>::max()) == 0,
            "zero-word payload estimate"); ++unit_checks;
    mustThrow<std::overflow_error>([&] {
        wa::densePayloadBytes(dense, std::numeric_limits<std::uint64_t>::max());
    }, "dense payload multiplication overflow");
    std::array<std::uint64_t, 2> words{{0, 2}};
    mustThrow<std::invalid_argument>([&] { wa::WindowView::dense(dense, words.data(), words.size()); }, "padding validation");
    words = {{std::uint64_t{1} << 63, 1}};
    const auto external = wa::WindowView::dense(dense, words.data(), words.size());
    wa::IntervalBuffer external_restored;
    kernel([&] { wa::restoreInto(dense, external, external_restored); });
    checkIntervals(external_restored.view(), {{63, 65}}, "cross-word external starts"); ++unit_checks;

    for (const auto policy : {wa::RepresentationPolicy::Auto, wa::RepresentationPolicy::ForceIntervals}) {
        const auto domain = wa::Domain::make(0, 100, 2, policy);
        wa::WindowBuffer a, b, result;
        const std::vector<wa::Interval> full{{0, 100}};
        kernel([&] { wa::buildInto(domain, iv(full), a); });
        kernel([&] { wa::buildInto(domain, iv(full), b); });
        mustThrow<std::invalid_argument>([&] { wa::intersectInto(domain, a.view(), b.view(), a); }, "lhs alias rejection");
        mustThrow<std::invalid_argument>([&] { wa::intersectInto(domain, a.view(), b.view(), b); }, "rhs alias rejection");
        const auto bad_domain = wa::Domain::make(0, 100, 3, policy);
        mustThrow<std::invalid_argument>([&] { wa::intersectInto(bad_domain, a.view(), b.view(), result); }, "domain mismatch");
        const std::vector<wa::Interval> unsorted{{5, 9}, {0, 3}}, outside{{-1, 4}}, negative{{4, 3}};
        mustThrow<std::invalid_argument>([&] { wa::buildInto(domain, iv(unsorted), result); }, "unsorted source");
        mustThrow<std::invalid_argument>([&] { wa::buildInto(domain, iv(outside), result); }, "outside source");
        mustThrow<std::invalid_argument>([&] { wa::buildInto(domain, iv(negative), result); }, "negative source");
        kernel([&] { wa::intersectInto(domain, a.view(), a.view(), result); });
        wa::IntervalBuffer restored;
        kernel([&] { wa::restoreInto(domain, result.view(), restored); });
        checkIntervals(restored.view(), full, "shared lhs/rhs allowed"); ++unit_checks;
        const std::array<TimeInterval, 2> native{{{0, 1}, {2, 4}}};
        kernel([&] { wa::buildInto(domain, native.data(), native.size(), result); });
        kernel([&] { wa::restoreInto(domain, result.view(), restored); });
        checkIntervals(restored.view(), {{0, 4}}, "native TimeInterval adapter"); ++unit_checks;
        wa::WindowBuffer moved(std::move(result));
        kernel([&] { wa::restoreInto(domain, moved.view(), restored); });
        checkIntervals(restored.view(), {{0, 4}}, "moved window buffer");
        kernel([&] { wa::buildInto(domain, iv(full), result); });
        require(!result.view().isEmpty(), "reuse moved-from window buffer"); ++unit_checks;
    }
    const auto interval_domain = wa::Domain::make(0, 100, 2, wa::RepresentationPolicy::ForceIntervals);
    const std::vector<wa::Interval> adjacent{{1, 2}, {3, 4}}, invalid_start{{100, 100}};
    mustThrow<std::invalid_argument>([&] { wa::WindowView::intervals(interval_domain, iv(adjacent)); }, "external adjacent starts");
    mustThrow<std::invalid_argument>([&] { wa::WindowView::intervals(interval_domain, iv(invalid_start)); }, "external start beyond lastStart");
    std::vector<wa::Interval> many;
    for (int i = 0; i < 10; ++i) many.push_back({i * 4, i * 4 + 1});
    wa::WindowBuffer small;
    kernel([&] { wa::setUniversal(interval_domain, small); });
    bool capacity_seen = false;
    try { wa::buildInto(interval_domain, iv(many), small); }
    catch (const wa::ScratchCapacityError& error) {
        capacity_seen = error.required == 10 && error.available == wa::kInlineIntervals;
    }
    require(capacity_seen && small.view().isUniversal(), "insufficient scratch must fail, not truncate or overwrite"); ++unit_checks;
    small.reserveIntervals(wa::buildIntervalCapacity(interval_domain, many.size()));
    kernel([&] { wa::buildInto(interval_domain, iv(many), small); });
    mustThrow<std::invalid_argument>([&] { wa::buildInto(interval_domain, small.view().starts(), small); }, "source alias rejection");
    wa::IntervalBuffer restored;
    restored.reserveIntervals(wa::restoreIntervalCapacity(interval_domain, small.view()));
    kernel([&] { wa::restoreInto(interval_domain, small.view(), restored); });
    checkIntervals(restored.view(), many, "overflow scratch exact result");
    const auto aliased = wa::WindowView::intervals(interval_domain, restored.view());
    mustThrow<std::invalid_argument>([&] { wa::restoreInto(interval_domain, aliased, restored); }, "restore alias rejection");
    wa::IntervalBuffer moved(std::move(restored));
    checkIntervals(moved.view(), many, "overflow moved buffer exact result");
    restored.reserveIntervals(20);
    kernel([&] { wa::restoreInto(interval_domain, small.view(), restored); });
    checkIntervals(restored.view(), many, "moved-from interval buffer reusable"); unit_checks += 3;
    require(wa::intervalIntersectionUpperBound(1, 10) == 11, "m+n bound, not min(m,n)");
    require(wa::intersectionIntervalCapacity(interval_domain, small.view(), wa::WindowView::universal(interval_domain)) == 10,
            "universal interval copy bound"); unit_checks += 2;
}

void writeReceipt(const std::array<std::uint64_t, 30>& pairs,
                  const std::array<std::array<std::uint64_t, 10>, 30>& chains) {
    std::ofstream out(context.output / "window_algebra_receipt.json");
    if (!out) fail("could not create property receipt");
    out << "{\n\"status\":\"PASS\",\"seed\":" << kSeed
        << ",\"random_pairs\":" << kPairCount << ",\"chain_cases\":" << kChainCount
        << ",\"chain_steps\":165000,\"chain_step_representation_checks\":" << checked_chain_steps
        << ",\"representations\":[\"auto_dense\",\"forced_intervals\"],"
        << "\"independent_oracle\":\"explicit integer snapshot sets\","
        << "\"original_integer_axis\":true,\"active_intervals_compared\":true,"
        << "\"identities\":[\"W(A intersection B)=W(A) intersection W(B)\","
        << "\"W(kappa(X))=W(X)\",\"restore(W(X))=kappa(X)\"],"
        << "\"successful_kernel_calls\":" << kernel_calls
        << ",\"successful_kernel_heap_allocations\":" << allocation_probe::calls
        << ",\"unit_checks\":" << unit_checks << ",\"failures\":0,\n\"strata\":[\n";
    for (std::size_t s = 0; s < 30; ++s) {
        if (s) out << ",\n";
        out << "{\"T\":" << kHorizons[s / 5] << ",\"k\":" << kDurabilities[s % 5]
            << ",\"pairs\":" << pairs[s] << ",\"chains\":1000,\"chain_lengths\":{";
        for (std::size_t length = 0; length < 10; ++length) {
            if (length) out << ',';
            out << '"' << length + 1 << "\":" << chains[s][length];
        }
        out << "}}";
    }
    out << "\n]}\n";
    if (!out) fail("could not finish property receipt");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: test_window_algebra OUTPUT_DIRECTORY\n"; return 2;
    }
    context.output = argv[1];
    try {
        std::filesystem::create_directories(context.output);
        if (std::filesystem::exists(context.output / "window_algebra_receipt.json") ||
            std::filesystem::exists(context.output / "COUNTEREXAMPLE.json"))
            throw std::runtime_error("refusing to overwrite previous property evidence");
        units();
        Workspace work;
        std::mt19937_64 rng(kSeed);
        std::array<std::uint64_t, 30> pairs{};
        std::array<std::array<std::uint64_t, 10>, 30> chains{};
        for (std::uint64_t i = 0; i < kPairCount; ++i) {
            testPair(rng, i, work); ++pairs[static_cast<std::size_t>(i % 30)];
        }
        for (std::uint64_t i = 0; i < kChainCount; ++i) {
            testChain(rng, i, work);
            ++chains[static_cast<std::size_t>(i % 30)][static_cast<std::size_t>((i / 30) % 10)];
        }
        for (std::size_t s = 0; s < 30; ++s) {
            require(pairs[s] == kPairCount / 30 + (s < kPairCount % 30), "pair stratum count");
            for (auto count : chains[s]) require(count == 100, "chain length stratum count");
        }
        require(checked_chain_steps == 330000, "all chain steps checked in both representations");
        require(allocation_probe::calls == 0, "normal kernels must not allocate");
        writeReceipt(pairs, chains);
        std::cout << "PASS pairs=" << kPairCount << " chains=" << kChainCount
                  << " representation_step_checks=" << checked_chain_steps << '\n';
        return 0;
    } catch (const std::exception& error) {
        allocation_probe::enabled = false;
        if (!context.failure_written &&
            !std::filesystem::exists(context.output / "window_algebra_receipt.json") &&
            !std::filesystem::exists(context.output / "COUNTEREXAMPLE.json")) {
            try { fail(std::string("unexpected exception: ") + error.what()); } catch (...) {}
        }
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
