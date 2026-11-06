#include "WindowAlgebra.h"
#include "Utils.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace window_algebra {
namespace {

std::uint64_t intervalLength(std::int64_t first, std::int64_t last) {
    if (last < first) throw std::invalid_argument("negative interval length");
    const auto distance = static_cast<std::uint64_t>(last) - static_cast<std::uint64_t>(first);
    if (distance == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("full int64 axis cardinality exceeds uint64");
    return distance + 1;
}

std::size_t checkedBytes(std::size_t count, std::size_t width) {
    if (count > std::numeric_limits<std::size_t>::max() / width)
        throw std::overflow_error("window byte extent overflow");
    return count * width;
}

bool overlaps(const void* a, std::size_t na, const void* b, std::size_t nb) {
    if (!na || !nb) return false;
    if (!a || !b) throw std::invalid_argument("null nonempty window storage");
    const auto x = reinterpret_cast<std::uintptr_t>(a);
    const auto y = reinterpret_cast<std::uintptr_t>(b);
    return x <= y ? y - x < na : x - y < nb;
}

void sameDomain(const Domain& domain, WindowView value) {
    if (!domain.sameAs(value.domain())) throw std::invalid_argument("window domains differ");
}

bool touches(std::int64_t end, std::int64_t next) {
    return next <= end || (end != std::numeric_limits<std::int64_t>::max() && next == end + 1);
}

template<class Getter, class Emit>
void normalizedRuns(const Domain& domain, std::size_t count, Getter get, Emit emit) {
    bool present = false;
    Interval current;
    std::int64_t previous_start = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const Interval next = get(i);
        if (next.end < next.start || next.start < domain.minimum() || next.end > domain.maximum() ||
            domain.span() == 0 || (i && next.start < previous_start))
            throw std::invalid_argument("invalid, unsorted or out-of-domain time interval");
        previous_start = next.start;
        if (!present) { current = next; present = true; }
        else if (touches(current.end, next.start)) current.end = std::max(current.end, next.end);
        else { emit(current); current = next; }
    }
    if (present) emit(current);
}

template<class Emit>
void eachStartRun(const Domain& domain, WindowView input, Emit emit) {
    if (input.kind() == Kind::Empty) return;
    if (input.kind() == Kind::Universal)
        throw std::invalid_argument("universal branch sentinel has no restored interval result");
    if (input.kind() == Kind::Intervals) {
        const auto intervals = input.starts();
        for (std::size_t i = 0; i < intervals.size; ++i) emit(intervals.data[i]);
        return;
    }
    bool open = false;
    std::uint64_t begin = 0;
    for (std::uint64_t i = 0; i < domain.startCount(); ++i) {
        const bool set = (input.words()[i / 64] >> (i % 64)) & 1U;
        if (set && !open) { begin = i; open = true; }
        if (!set && open) { emit(Interval{domain.timestampAt(begin), domain.timestampAt(i - 1)}); open = false; }
    }
    if (open) emit(Interval{domain.timestampAt(begin), domain.lastStart()});
}

} // namespace

struct WindowAccess {
    static Interval* data(IntervalBuffer& value) {
        return value.overflow_.empty() ? value.inline_.data() : value.overflow_.data();
    }
    static void clear(IntervalBuffer& value) { value.size_ = 0; }
    static void append(IntervalBuffer& value, Interval interval) {
        if (value.size_ == value.capacity()) throw ScratchCapacityError(value.size_ + 1, value.capacity());
        data(value)[value.size_++] = interval;
    }
    static void appendMerged(IntervalBuffer& value, Interval interval) {
        if (value.size_ && touches(data(value)[value.size_ - 1].end, interval.start))
            data(value)[value.size_ - 1].end = std::max(data(value)[value.size_ - 1].end, interval.end);
        else append(value, interval);
    }
    static void checkCapacity(const IntervalBuffer& value, std::size_t needed) {
        if (needed > value.capacity()) throw ScratchCapacityError(needed, value.capacity());
    }
    static void noAlias(const void* input, std::size_t bytes, const WindowBuffer& out) {
        if (overlaps(input, bytes, out.words_.data(), sizeof(out.words_)) ||
            overlaps(input, bytes, out.intervals_.data(), checkedBytes(out.intervals_.capacity(), sizeof(Interval))))
            throw std::invalid_argument("window input aliases output scratch");
    }
    static void noAlias(WindowView input, const WindowBuffer& out) {
        if (input.kind() == Kind::Dense) noAlias(input.words(), checkedBytes(input.wordCount(), sizeof(std::uint64_t)), out);
        else if (input.kind() == Kind::Intervals) noAlias(input.starts().data, checkedBytes(input.starts().size, sizeof(Interval)), out);
    }
    static void noAlias(WindowView input, const IntervalBuffer& out) {
        const void* pointer = nullptr; std::size_t bytes = 0;
        if (input.kind() == Kind::Dense) { pointer = input.words(); bytes = checkedBytes(input.wordCount(), sizeof(std::uint64_t)); }
        if (input.kind() == Kind::Intervals) { pointer = input.starts().data; bytes = checkedBytes(input.starts().size, sizeof(Interval)); }
        if (overlaps(pointer, bytes, out.data(), checkedBytes(out.capacity(), sizeof(Interval))))
            throw std::invalid_argument("window restore aliases input scratch");
    }
    static void reset(const Domain& domain, WindowBuffer& out, Kind kind = Kind::Empty) {
        out.domain_ = &domain; out.kind_ = kind; out.words_.fill(0); clear(out.intervals_);
    }
    static void setRange(const Domain& domain, WindowBuffer& out, Interval starts) {
        const auto first = static_cast<std::uint64_t>(starts.start) - static_cast<std::uint64_t>(domain.minimum());
        const auto last = static_cast<std::uint64_t>(starts.end) - static_cast<std::uint64_t>(domain.minimum());
        for (auto word = first / 64; word <= last / 64; ++word) {
            const unsigned lo = word == first / 64 ? static_cast<unsigned>(first % 64) : 0;
            const unsigned hi = word == last / 64 ? static_cast<unsigned>(last % 64) : 63;
            const auto lower = std::numeric_limits<std::uint64_t>::max() << lo;
            const auto upper = hi == 63 ? std::numeric_limits<std::uint64_t>::max() : ((std::uint64_t{1} << (hi + 1)) - 1);
            out.words_[static_cast<std::size_t>(word)] |= lower & upper;
        }
        out.kind_ = Kind::Dense;
    }
    template<class Getter>
    static void build(const Domain& domain, std::size_t count, Getter get, WindowBuffer& out) {
        // Proof: a k-window lies in A intersection B iff it lies in both, so W
        // distributes over intersection. Every k-window lies inside a maximal
        // run of length at least k, so kappa removes no window. Conversely every
        // point of each qualifying maximal run lies in one of its k-windows,
        // making the union of those windows exactly kappa(X). Thus repeated
        // W AND preserves the old kappa(J intersection kappa(S)) support and
        // emptiness, provided universal initialization/self-loops are unchanged.
        std::size_t needed = 0;
        normalizedRuns(domain, count, get, [&](Interval run) {
            if (intervalLength(run.start, run.end) >= static_cast<std::uint64_t>(domain.k())) ++needed;
        });
        if (domain.representation() == Representation::Intervals) checkCapacity(out.intervals_, needed);
        reset(domain, out);
        normalizedRuns(domain, count, get, [&](Interval run) {
            if (intervalLength(run.start, run.end) < static_cast<std::uint64_t>(domain.k())) return;
            const Interval starts{run.start, run.end - (domain.k() - 1)};
            if (domain.representation() == Representation::Dense) setRange(domain, out, starts);
            else { append(out.intervals_, starts); out.kind_ = Kind::Intervals; }
        });
    }
    static void copy(const Domain& domain, WindowView input, WindowBuffer& out) {
        if (input.kind() == Kind::Intervals) checkCapacity(out.intervals_, input.starts().size);
        reset(domain, out, input.kind());
        if (input.kind() == Kind::Dense) std::copy_n(input.words(), input.wordCount(), out.words_.begin());
        if (input.kind() == Kind::Intervals)
            for (std::size_t i = 0; i < input.starts().size; ++i) append(out.intervals_, input.starts().data[i]);
    }
    static void intersect(const Domain& domain, WindowView lhs, WindowView rhs, WindowBuffer& out) {
        if (lhs.isEmpty() || rhs.isEmpty()) { reset(domain, out); return; }
        if (lhs.isUniversal()) { copy(domain, rhs, out); return; }
        if (rhs.isUniversal()) { copy(domain, lhs, out); return; }
        if (domain.representation() == Representation::Dense) {
            reset(domain, out);
            for (std::size_t i = 0; i < static_cast<std::size_t>(domain.wordCount()); ++i) {
                out.words_[i] = lhs.words()[i] & rhs.words()[i];
                if (out.words_[i]) out.kind_ = Kind::Dense;
            }
            return;
        }
        const auto a = lhs.starts(), b = rhs.starts();
        auto scan = [&](auto emit) {
            std::size_t i = 0, j = 0;
            while (i < a.size && j < b.size) {
                const auto first = std::max(a.data[i].start, b.data[j].start);
                const auto last = std::min(a.data[i].end, b.data[j].end);
                if (first <= last) emit(Interval{first, last});
                if (a.data[i].end < b.data[j].end) ++i; else ++j;
            }
        };
        std::size_t needed = 0;
        scan([&](Interval) { ++needed; });
        checkCapacity(out.intervals_, needed);
        reset(domain, out);
        scan([&](Interval value) { append(out.intervals_, value); out.kind_ = Kind::Intervals; });
    }
};

Domain Domain::make(std::int64_t minimum, std::int64_t maximum, std::int64_t k, RepresentationPolicy policy) {
    if (k <= 0) throw std::invalid_argument("window k must be positive");
    Domain d; d.minimum_ = minimum; d.maximum_ = maximum; d.k_ = k;
    d.span_ = maximum < minimum ? 0 : intervalLength(minimum, maximum);
    d.starts_ = d.span_ >= static_cast<std::uint64_t>(k) ? d.span_ - static_cast<std::uint64_t>(k) + 1 : 0;
    d.words_ = d.starts_ / 64 + (d.starts_ % 64 != 0);
    d.representation_ = policy == RepresentationPolicy::Auto && d.words_ <= kDenseWords
        ? Representation::Dense : Representation::Intervals;
    return d;
}

std::int64_t Domain::lastStart() const {
    if (!starts_) throw std::logic_error("empty window-start domain");
    return maximum_ - (k_ - 1);
}

Domain Domain::paddedToRawWords() const {
    Domain result = *this;
    const auto raw_words = span_ / 64 + (span_ % 64 != 0);
    if (representation_ != Representation::Dense || raw_words > kDenseWords)
        throw std::invalid_argument("raw-width padding requires a <=16-word dense axis");
    result.words_ = raw_words;
    return result;
}

std::int64_t Domain::timestampAt(std::uint64_t offset) const {
    if (offset >= span_) throw std::out_of_range("timestamp offset outside original axis");
    constexpr auto limit = std::numeric_limits<std::int64_t>::max();
    if (offset <= static_cast<std::uint64_t>(limit)) {
        const auto signed_offset = static_cast<std::int64_t>(offset);
        if (minimum_ > limit - signed_offset) throw std::overflow_error("timestamp offset overflow");
        return minimum_ + signed_offset;
    }
    if (minimum_ >= 0) throw std::overflow_error("timestamp offset overflow");
    const auto magnitude = static_cast<std::uint64_t>(-(minimum_ + 1)) + 1;
    const auto positive = offset - magnitude;
    if (positive > static_cast<std::uint64_t>(limit)) throw std::overflow_error("timestamp offset overflow");
    return static_cast<std::int64_t>(positive);
}

bool Domain::sameAs(const Domain& rhs) const noexcept {
    return minimum_ == rhs.minimum_ && maximum_ == rhs.maximum_ && k_ == rhs.k_ && representation_ == rhs.representation_ && words_ == rhs.words_;
}

WindowView WindowView::empty(const Domain& domain) { WindowView v; v.domain_ = &domain; return v; }
WindowView WindowView::universal(const Domain& domain) { auto v = empty(domain); v.kind_ = Kind::Universal; return v; }
const Domain& WindowView::domain() const {
    if (!domain_) throw std::logic_error("uninitialized window view");
    return *domain_;
}

WindowView WindowView::dense(const Domain& domain, const std::uint64_t* words, std::size_t count) {
    if (domain.representation() != Representation::Dense || count != domain.wordCount() || (count && !words))
        throw std::invalid_argument("invalid external dense window view");
    auto v = empty(domain);
    if (!count) return v;
    const auto logical_words = domain.startCount() / 64 + (domain.startCount() % 64 != 0);
    if (domain.startCount() % 64 && (words[logical_words - 1] >> (domain.startCount() % 64)))
        throw std::invalid_argument("nonzero window padding bits");
    for (std::size_t i = static_cast<std::size_t>(logical_words); i < count; ++i)
        if (words[i]) throw std::invalid_argument("nonzero physical padding word");
    bool nonempty = false;
    for (std::size_t i = 0; i < count; ++i) nonempty |= words[i] != 0;
    if (nonempty) { v.kind_ = Kind::Dense; v.words_ = words; v.size_ = count; }
    return v;
}

WindowView WindowView::intervals(const Domain& domain, IntervalView starts) {
    if (domain.representation() != Representation::Intervals || (starts.size && !starts.data))
        throw std::invalid_argument("invalid external interval window view");
    auto v = empty(domain);
    for (std::size_t i = 0; i < starts.size; ++i) {
        const auto x = starts.data[i];
        if (!domain.startCount() || x.start < domain.minimum() || x.end > domain.lastStart() || x.end < x.start ||
            (i && touches(starts.data[i - 1].end, x.start)))
            throw std::invalid_argument("non-normalized or out-of-domain window starts");
    }
    if (starts.size) { v.kind_ = Kind::Intervals; v.intervals_ = starts.data; v.size_ = starts.size; }
    return v;
}

ScratchCapacityError::ScratchCapacityError(std::size_t needed, std::size_t supplied)
    : std::length_error("insufficient preallocated window scratch"), required(needed), available(supplied) {}

IntervalBuffer::IntervalBuffer(IntervalBuffer&& other) noexcept
    : inline_(other.inline_), overflow_(std::move(other.overflow_)), size_(other.size_) {
    other.size_ = 0;
}
IntervalBuffer& IntervalBuffer::operator=(IntervalBuffer&& other) noexcept {
    if (this != &other) {
        inline_ = other.inline_; overflow_ = std::move(other.overflow_); size_ = other.size_;
        other.size_ = 0;
    }
    return *this;
}
void IntervalBuffer::reserveIntervals(std::size_t count) {
    if (count <= capacity()) return;
    checkedBytes(count, sizeof(Interval));
    const bool was_inline = overflow_.empty();
    overflow_.resize(count);
    if (was_inline) std::copy_n(inline_.begin(), size_, overflow_.begin());
}
std::size_t IntervalBuffer::capacity() const noexcept { return overflow_.empty() ? kInlineIntervals : overflow_.size(); }
const Interval* IntervalBuffer::data() const noexcept { return overflow_.empty() ? inline_.data() : overflow_.data(); }
WindowBuffer::WindowBuffer(WindowBuffer&& other) noexcept
    : domain_(other.domain_), kind_(other.kind_), words_(other.words_), intervals_(std::move(other.intervals_)) {
    other.domain_ = nullptr; other.kind_ = Kind::Empty;
}
WindowBuffer& WindowBuffer::operator=(WindowBuffer&& other) noexcept {
    if (this != &other) {
        domain_ = other.domain_; kind_ = other.kind_; words_ = other.words_;
        intervals_ = std::move(other.intervals_);
        other.domain_ = nullptr; other.kind_ = Kind::Empty;
    }
    return *this;
}
WindowView WindowBuffer::view() const {
    if (!domain_) throw std::logic_error("uninitialized window scratch");
    // Internal values are already validated; constructing their borrowed view
    // does not re-scan history or allocate a control block.
    WindowView v = WindowView::empty(*domain_); v.kind_ = kind_;
    if (kind_ == Kind::Dense) { v.words_ = words_.data(); v.size_ = static_cast<std::size_t>(domain_->wordCount()); }
    if (kind_ == Kind::Intervals) { v.intervals_ = intervals_.data(); v.size_ = intervals_.size(); }
    return v;
}

std::size_t intervalIntersectionUpperBound(std::size_t lhs, std::size_t rhs) {
    if (lhs > std::numeric_limits<std::size_t>::max() - rhs) throw std::overflow_error("interval bound overflow");
    return lhs + rhs;
}
std::size_t buildIntervalCapacity(const Domain& domain, std::size_t input_intervals) {
    return domain.representation() == Representation::Intervals ? input_intervals : 0;
}
std::size_t intersectionIntervalCapacity(const Domain& domain, WindowView lhs, WindowView rhs) {
    sameDomain(domain, lhs); sameDomain(domain, rhs);
    if (domain.representation() == Representation::Dense || lhs.isEmpty() || rhs.isEmpty()) return 0;
    if (lhs.isUniversal()) return rhs.kind() == Kind::Intervals ? rhs.starts().size : 0;
    if (rhs.isUniversal()) return lhs.starts().size;
    return intervalIntersectionUpperBound(lhs.starts().size, rhs.starts().size);
}
std::size_t restoreIntervalCapacity(const Domain& domain, WindowView input) {
    sameDomain(domain, input);
    if (input.isUniversal()) throw std::invalid_argument("cannot restore unconstrained universal branch");
    if (input.isEmpty()) return 0;
    if (input.kind() == Kind::Intervals) return input.starts().size;
    return static_cast<std::size_t>(domain.startCount() / 2 + domain.startCount() % 2);
}
std::uint64_t densePayloadBytes(const Domain& domain, std::uint64_t relevant_edges) {
    constexpr auto limit = std::numeric_limits<std::uint64_t>::max();
    if (!relevant_edges) return 0;
    if (domain.wordCount() > limit / 8 || relevant_edges > limit / (domain.wordCount() ? domain.wordCount() * 8 : 1))
        throw std::overflow_error("dense window payload estimate overflow");
    return relevant_edges * domain.wordCount() * 8;
}
void buildInto(const Domain& domain, IntervalView input, WindowBuffer& output) {
    if (input.size && !input.data) throw std::invalid_argument("null interval input");
    WindowAccess::noAlias(input.data, checkedBytes(input.size, sizeof(Interval)), output);
    WindowAccess::build(domain, input.size, [&](std::size_t i) { return input.data[i]; }, output);
}
void buildInto(const Domain& domain, const TimeInterval* input, std::size_t count, WindowBuffer& output) {
    if (count && !input) throw std::invalid_argument("null native interval input");
    WindowAccess::noAlias(input, checkedBytes(count, sizeof(TimeInterval)), output);
    WindowAccess::build(domain, count, [&](std::size_t i) { return Interval{input[i].start, input[i].end}; }, output);
}
void intersectInto(const Domain& domain, WindowView lhs, WindowView rhs, WindowBuffer& output) {
    sameDomain(domain, lhs); sameDomain(domain, rhs);
    WindowAccess::noAlias(lhs, output); WindowAccess::noAlias(rhs, output);
    WindowAccess::intersect(domain, lhs, rhs, output);
}
void restoreInto(const Domain& domain, WindowView input, IntervalBuffer& output) {
    sameDomain(domain, input); WindowAccess::noAlias(input, output);
    std::size_t needed = 0; Interval last; bool present = false;
    eachStartRun(domain, input, [&](Interval starts) {
        const Interval restored{starts.start, starts.end + (domain.k() - 1)};
        if (present && touches(last.end, restored.start)) last.end = std::max(last.end, restored.end);
        else { ++needed; last = restored; present = true; }
    });
    WindowAccess::checkCapacity(output, needed); WindowAccess::clear(output);
    eachStartRun(domain, input, [&](Interval starts) {
        WindowAccess::appendMerged(output, {starts.start, starts.end + (domain.k() - 1)});
    });
}
void setUniversal(const Domain& domain, WindowBuffer& output) { WindowAccess::reset(domain, output, Kind::Universal); }

} // namespace window_algebra
