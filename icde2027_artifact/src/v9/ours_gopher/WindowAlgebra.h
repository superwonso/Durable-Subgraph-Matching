#ifndef DSM_WINDOW_ALGEBRA_H
#define DSM_WINDOW_ALGEBRA_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

struct TimeInterval;

namespace window_algebra {

constexpr std::size_t kDenseWords = 16;
constexpr std::size_t kInlineIntervals = 8;

struct Interval {
    std::int64_t start = 0;
    std::int64_t end = -1;
    bool operator==(const Interval& rhs) const noexcept {
        return start == rhs.start && end == rhs.end;
    }
};

struct IntervalView {
    const Interval* data = nullptr;
    std::size_t size = 0;
};

enum class Representation { Dense, Intervals };
enum class RepresentationPolicy { Auto, ForceIntervals };
enum class Kind { Empty, Universal, Dense, Intervals };

// Original integer timestamps: never rank-compress gaps. An empty domain is
// expressed by maximum < minimum. The entire INT64_MIN..INT64_MAX range is
// rejected because its cardinality does not fit uint64_t; int32 axes are safe.
class Domain {
public:
    static Domain make(std::int64_t minimum, std::int64_t maximum,
                       std::int64_t k, RepresentationPolicy policy = RepresentationPolicy::Auto);
    // E1: preserve the eroded logical domain, but visit raw-axis physical words.
    Domain paddedToRawWords() const;
    std::int64_t minimum() const noexcept { return minimum_; }
    std::int64_t maximum() const noexcept { return maximum_; }
    std::int64_t k() const noexcept { return k_; }
    std::uint64_t span() const noexcept { return span_; }
    std::uint64_t startCount() const noexcept { return starts_; }
    std::uint64_t wordCount() const noexcept { return words_; }
    Representation representation() const noexcept { return representation_; }
    std::int64_t lastStart() const; // error when there are no valid starts
    std::int64_t timestampAt(std::uint64_t offset) const;
    bool sameAs(const Domain& rhs) const noexcept;
private:
    Domain() = default;
    std::int64_t minimum_ = 0, maximum_ = -1, k_ = 1;
    std::uint64_t span_ = 0, starts_ = 0, words_ = 0;
    Representation representation_ = Representation::Dense;
};

struct WindowAccess;

// Borrowed immutable edge-store or scratch view. Domain and backing storage
// must outlive the view. Factories validate external data once; neither callers
// nor another worker may mutate that data during the view's lifetime.
class WindowView {
public:
    static WindowView empty(const Domain& domain);
    static WindowView universal(const Domain& domain);
    static WindowView dense(const Domain& domain, const std::uint64_t* words, std::size_t count);
    static WindowView intervals(const Domain& domain, IntervalView starts);
    const Domain& domain() const;
    Kind kind() const noexcept { return kind_; }
    bool isUniversal() const noexcept { return kind_ == Kind::Universal; }
    bool isEmpty() const noexcept { return kind_ == Kind::Empty; }
    const std::uint64_t* words() const noexcept { return words_; }
    std::size_t wordCount() const noexcept { return size_; }
    IntervalView starts() const noexcept { return {intervals_, size_}; }
private:
    friend struct WindowAccess;
    friend class WindowBuffer;
    const Domain* domain_ = nullptr;
    Kind kind_ = Kind::Empty;
    const std::uint64_t* words_ = nullptr;
    const Interval* intervals_ = nullptr;
    std::size_t size_ = 0;
};

class ScratchCapacityError : public std::length_error {
public:
    ScratchCapacityError(std::size_t required, std::size_t available);
    const std::size_t required;
    const std::size_t available;
};

// reserveIntervals() is the ONLY growth operation. Call it before enumeration,
// using the bounds below, for each worker/DFS-depth scratch buffer. No arbitrary
// cap/truncation: an insufficient caller allocation fails explicitly, never
// substitutes an empty answer. Reserve/growth invalidates existing views.
class IntervalBuffer {
public:
    IntervalBuffer() = default;
    IntervalBuffer(const IntervalBuffer&) = delete;
    IntervalBuffer& operator=(const IntervalBuffer&) = delete;
    IntervalBuffer(IntervalBuffer&& other) noexcept;
    IntervalBuffer& operator=(IntervalBuffer&& other) noexcept;
    void reserveIntervals(std::size_t count);
    std::size_t capacity() const noexcept;
    std::size_t size() const noexcept { return size_; }
    IntervalView view() const noexcept { return {data(), size_}; }
    const Interval* data() const noexcept;
private:
    friend struct WindowAccess;
    std::array<Interval, kInlineIntervals> inline_{};
    // Constructed storage slots, not the logical result length. Kernel writes
    // never resize this vector, so no candidate-path allocation occurs.
    std::vector<Interval> overflow_;
    std::size_t size_ = 0;
};

class WindowBuffer {
public:
    WindowBuffer() = default;
    WindowBuffer(const WindowBuffer&) = delete;
    WindowBuffer& operator=(const WindowBuffer&) = delete;
    WindowBuffer(WindowBuffer&& other) noexcept;
    WindowBuffer& operator=(WindowBuffer&& other) noexcept;
    void reserveIntervals(std::size_t count) { intervals_.reserveIntervals(count); }
    std::size_t intervalCapacity() const noexcept { return intervals_.capacity(); }
    WindowView view() const;
private:
    friend struct WindowAccess;
    const Domain* domain_ = nullptr;
    Kind kind_ = Kind::Empty;
    std::array<std::uint64_t, kDenseWords> words_{};
    IntervalBuffer intervals_;
};

// Checked upper bounds, not run-dependent tuning. For an intersection of m/n
// normalized start runs, m+n is safe; min(m,n) is NOT a valid capacity bound.
std::size_t intervalIntersectionUpperBound(std::size_t lhs, std::size_t rhs);
std::size_t buildIntervalCapacity(const Domain& domain, std::size_t input_intervals);
std::size_t intersectionIntervalCapacity(const Domain& domain, WindowView lhs, WindowView rhs);
std::size_t restoreIntervalCapacity(const Domain& domain, WindowView input);
std::uint64_t densePayloadBytes(const Domain& domain, std::uint64_t relevant_edges);

// Sorted input intervals may overlap/touch; build coalesces them BEFORE trimming
// to k. Invalid/out-of-domain/unsorted input fails. Normal successful calls do
// not allocate. No input may overlap ANY output buffer storage. lhs/rhs may
// alias one another. Domain metadata must match, including representation.
void buildInto(const Domain& domain, IntervalView input, WindowBuffer& output);
void buildInto(const Domain& domain, const TimeInterval* input, std::size_t count, WindowBuffer& output);
void intersectInto(const Domain& domain, WindowView lhs, WindowView rhs, WindowBuffer& output);
void restoreInto(const Domain& domain, WindowView input, IntervalBuffer& output);

// Universal is an explicit unconstrained-branch sentinel, not an interval
// result. It is preserved by universal AND universal and is the identity for
// real supports. restoreInto(universal) throws: callers must retain the old
// has_active_intervals=false contract until the first actual edge constraint.
void setUniversal(const Domain& domain, WindowBuffer& output);

} // namespace window_algebra
#endif
