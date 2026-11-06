#include "v3_input.h"

#include "v3_canonical_json.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <numeric>
#include <sstream>
#include <string_view>
#include <tuple>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_map>

#if defined(__linux__)
#include <linux/openat2.h>
#endif

namespace dsm::v3 {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kReadChunkBytes = 64U * 1024U;
constexpr std::size_t kMaximumLineBytes = 1024U * 1024U;

struct ScopedFd {
    int value = -1;
    ScopedFd() = default;
    explicit ScopedFd(int descriptor) : value(descriptor) {}
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&& other) noexcept : value(other.value) {
        other.value = -1;
    }
    ~ScopedFd() {
        if (value >= 0) {
            while (::close(value) < 0 && errno == EINTR) {}
        }
    }
};

bool isCanonicalRepositoryRelativePath(std::string_view path) noexcept {
    if (path.empty() || path.front() == '/' || path.back() == '/' ||
        path.find('\\') != std::string_view::npos ||
        path.find("//") != std::string_view::npos) {
        return false;
    }
    std::size_t begin = 0;
    while (begin < path.size()) {
        const std::size_t end = path.find('/', begin);
        const std::string_view component = path.substr(
            begin,
            end == std::string_view::npos
                ? path.size() - begin
                : end - begin);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }
    return true;
}

int openBeneathRepository(const std::string& path) noexcept {
#if defined(__linux__) && defined(SYS_openat2)
    ScopedFd root(::open(".", O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (root.value < 0) return -1;
    struct open_how how {};
    how.flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS |
        RESOLVE_NO_SYMLINKS;
    return static_cast<int>(::syscall(
        SYS_openat2, root.value, path.c_str(), &how, sizeof(how)));
#else
    (void)path;
    errno = ENOSYS;
    return -1;
#endif
}

bool durationNs(
    Clock::time_point begin,
    Clock::time_point end,
    std::uint64_t& output) noexcept {
    if (end < begin) return false;
    const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(
        end - begin).count();
    if (count < 0) return false;
    output = static_cast<std::uint64_t>(count);
    return true;
}

enum class LineStatus : std::uint8_t { Empty, Valid, Invalid };

LineStatus parseTemporalLine(
    std::string_view line,
    int& u,
    int& v,
    int& timestamp) noexcept {
    const char* cursor = line.data();
    const char* const end = cursor + line.size();
    auto skip = [&]() {
        while (cursor != end &&
               std::isspace(static_cast<unsigned char>(*cursor)) != 0) {
            ++cursor;
        }
    };
    auto integer = [&](int& value) {
        skip();
        if (cursor == end || *cursor == '#') return false;
        const auto parsed = std::from_chars(cursor, end, value);
        if (parsed.ec != std::errc{} || parsed.ptr == cursor) return false;
        cursor = parsed.ptr;
        return cursor == end || *cursor == '#' ||
            std::isspace(static_cast<unsigned char>(*cursor)) != 0;
    };
    skip();
    if (cursor == end || *cursor == '#') return LineStatus::Empty;
    if (!integer(u) || !integer(v) || !integer(timestamp)) {
        return LineStatus::Invalid;
    }
    skip();
    return cursor == end || *cursor == '#'
        ? LineStatus::Valid
        : LineStatus::Invalid;
}

struct RawTemporalEdge {
    int u = 0;
    int v = 0;
    int timestamp = 0;
};

struct FilteredExternalEdge {
    int u = 0;
    int v = 0;
    std::vector<TimeInterval> intervals;
    std::int64_t active_snapshot_count = 0;
};

bool rawEdgeLess(
    const RawTemporalEdge& lhs,
    const RawTemporalEdge& rhs) noexcept {
    if (lhs.u != rhs.u) return lhs.u < rhs.u;
    if (lhs.v != rhs.v) return lhs.v < rhs.v;
    return lhs.timestamp < rhs.timestamp;
}

int compactId(const std::vector<int>& external_ids, int external_id) noexcept {
    const auto found = std::lower_bound(
        external_ids.begin(), external_ids.end(), external_id);
    if (found == external_ids.end() || *found != external_id) return -1;
    return static_cast<int>(found - external_ids.begin());
}

int unionDuration(std::vector<TimeInterval>& intervals) {
    if (intervals.empty()) return 0;
    std::sort(
        intervals.begin(), intervals.end(),
        [](const TimeInterval& lhs, const TimeInterval& rhs) {
            return std::tie(lhs.start, lhs.end) <
                std::tie(rhs.start, rhs.end);
        });
    std::int64_t total = 0;
    int start = intervals.front().start;
    int end = intervals.front().end;
    for (std::size_t index = 1; index < intervals.size(); ++index) {
        const TimeInterval& interval = intervals[index];
        if (interval.start <= end ||
            (end != std::numeric_limits<int>::max() &&
             interval.start == end + 1)) {
            end = std::max(end, interval.end);
        } else {
            total += static_cast<std::int64_t>(end) - start + 1;
            start = interval.start;
            end = interval.end;
        }
    }
    total += static_cast<std::int64_t>(end) - start + 1;
    return total > std::numeric_limits<int>::max()
        ? std::numeric_limits<int>::max()
        : static_cast<int>(total);
}

bool buildTemporalGraph(
    std::vector<RawTemporalEdge> raw_edges,
    Graph& graph,
    std::uint32_t label_seed,
    bool apply_filter,
    std::string& detail) {
    graph = Graph{};
    graph.input_occurrence_count = raw_edges.size();
    std::sort(raw_edges.begin(), raw_edges.end(), rawEdgeLess);
    std::vector<FilteredExternalEdge> retained;
    retained.reserve(std::min<std::size_t>(
        raw_edges.size() / 4U + 1U, 4'000'000U));
    std::vector<int> all_vertices;
    all_vertices.reserve(std::min<std::size_t>(
        raw_edges.size(), 8'000'000U));

    std::size_t index = 0;
    while (index < raw_edges.size()) {
        const int u = raw_edges[index].u;
        const int v = raw_edges[index].v;
        ++graph.input_unique_edge_count;
        all_vertices.push_back(u);
        all_vertices.push_back(v);
        std::vector<TimeInterval> intervals;
        std::int64_t active = 0;
        bool has_pair = false;
        bool have = false;
        int run_start = 0;
        int previous = 0;
        while (index < raw_edges.size() &&
               raw_edges[index].u == u && raw_edges[index].v == v) {
            const int current = raw_edges[index++].timestamp;
            if (have && current == previous) continue;
            if (active == std::numeric_limits<std::int64_t>::max()) {
                detail = "temporal active-snapshot counter overflow";
                return false;
            }
            ++active;
            if (!have) {
                run_start = previous = current;
                have = true;
            } else if (previous != std::numeric_limits<int>::max() &&
                       current == previous + 1) {
                has_pair = true;
                previous = current;
            } else {
                intervals.push_back({run_start, previous});
                run_start = previous = current;
            }
        }
        if (have) intervals.push_back({run_start, previous});
        if (!apply_filter || has_pair) {
            retained.push_back({u, v, std::move(intervals), active});
        }
    }
    graph.filtered_edge_count = retained.size();
    std::vector<RawTemporalEdge>().swap(raw_edges);

    std::sort(all_vertices.begin(), all_vertices.end());
    all_vertices.erase(
        std::unique(all_vertices.begin(), all_vertices.end()),
        all_vertices.end());
    std::vector<Label> all_labels(all_vertices.size(), kInvalidLabel);
    for (std::size_t i = 0; i < all_vertices.size(); ++i) {
        all_labels[i] = stableIdLabel(all_vertices[i], label_seed);
    }
    graph.external_ids.reserve(retained.size() * 2U);
    for (const FilteredExternalEdge& edge : retained) {
        graph.external_ids.push_back(edge.u);
        graph.external_ids.push_back(edge.v);
    }
    std::sort(graph.external_ids.begin(), graph.external_ids.end());
    graph.external_ids.erase(
        std::unique(graph.external_ids.begin(), graph.external_ids.end()),
        graph.external_ids.end());
    if (graph.external_ids.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        retained.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        detail = "compact graph cardinality exceeds native int range";
        return false;
    }
    graph.num_vertices = static_cast<int>(graph.external_ids.size());
    graph.adj.resize(graph.external_ids.size());
    graph.in_adj.resize(graph.external_ids.size());
    graph.vertex_labels.resize(graph.external_ids.size(), kInvalidLabel);
    std::size_t label_index = 0;
    for (std::size_t i = 0; i < graph.external_ids.size(); ++i) {
        while (label_index < all_vertices.size() &&
               all_vertices[label_index] < graph.external_ids[i]) {
            ++label_index;
        }
        if (label_index == all_vertices.size() ||
            all_vertices[label_index] != graph.external_ids[i]) {
            detail = "stable label projection failed";
            return false;
        }
        graph.vertex_labels[i] = all_labels[label_index];
    }
    std::vector<std::size_t> out_degree(graph.external_ids.size(), 0);
    std::vector<std::size_t> in_degree(graph.external_ids.size(), 0);
    for (const FilteredExternalEdge& edge : retained) {
        const int u = compactId(graph.external_ids, edge.u);
        const int v = compactId(graph.external_ids, edge.v);
        if (u < 0 || v < 0) {
            detail = "compact edge endpoint lookup failed";
            return false;
        }
        ++out_degree[static_cast<std::size_t>(u)];
        ++in_degree[static_cast<std::size_t>(v)];
    }
    for (std::size_t i = 0; i < graph.adj.size(); ++i) {
        graph.adj[i].reserve(out_degree[i]);
        graph.in_adj[i].reserve(in_degree[i]);
    }
    graph.temporal_edges.reserve(retained.size());
    for (FilteredExternalEdge& edge : retained) {
        const int u = compactId(graph.external_ids, edge.u);
        const int v = compactId(graph.external_ids, edge.v);
        const int edge_id = static_cast<int>(graph.temporal_edges.size());
        graph.temporal_edges.emplace_back(
            u, v, std::move(edge.intervals), edge.active_snapshot_count);
        graph.adj[static_cast<std::size_t>(u)].push_back({v, edge_id});
        graph.in_adj[static_cast<std::size_t>(v)].push_back({u, edge_id});
    }
    for (auto& neighbors : graph.adj) {
        std::sort(neighbors.begin(), neighbors.end(),
            [](const Edge& lhs, const Edge& rhs) { return lhs.to < rhs.to; });
    }
    for (auto& neighbors : graph.in_adj) {
        std::sort(neighbors.begin(), neighbors.end(),
            [](const Edge& lhs, const Edge& rhs) { return lhs.to < rhs.to; });
    }
    graph.vertex_active_durations.assign(graph.external_ids.size(), 0);
    graph.neighbor_label_counts.assign(graph.external_ids.size(), {});
    graph.incoming_neighbor_label_counts.assign(graph.external_ids.size(), {});
    std::vector<TimeInterval> incident;
    for (std::size_t vertex = 0; vertex < graph.adj.size(); ++vertex) {
        incident.clear();
        for (const Edge& ref : graph.adj[vertex]) {
            const TemporalEdge& edge = graph.temporal_edges[
                static_cast<std::size_t>(ref.temporal_edge_id)];
            incident.insert(
                incident.end(), edge.activeIntervals().begin(),
                edge.activeIntervals().end());
            const Label label = graph.vertex_labels[
                static_cast<std::size_t>(ref.to)];
            if (label < kLabelCount) {
                ++graph.neighbor_label_counts[vertex][label];
            }
        }
        for (const Edge& ref : graph.in_adj[vertex]) {
            const TemporalEdge& edge = graph.temporal_edges[
                static_cast<std::size_t>(ref.temporal_edge_id)];
            incident.insert(
                incident.end(), edge.activeIntervals().begin(),
                edge.activeIntervals().end());
            const Label label = graph.vertex_labels[
                static_cast<std::size_t>(ref.to)];
            if (label < kLabelCount) {
                ++graph.incoming_neighbor_label_counts[vertex][label];
            }
        }
        graph.vertex_active_durations[vertex] = unionDuration(incident);
    }
    return true;
}

struct QueryAccumulator {
    Graph graph;
    std::unordered_map<std::string, int> vertex_by_key;
    std::vector<std::pair<int, int>> edges;
};

bool parseQueryLine(
    std::string line,
    QueryAccumulator& accumulator,
    std::string& detail) {
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) line.resize(comment);
    std::istringstream parser(line);
    std::string lhs_token;
    std::string rhs_token;
    std::string extra;
    if (!(parser >> lhs_token)) return true;
    if (!(parser >> rhs_token) || (parser >> extra)) {
        detail = "query line must contain exactly two vertex tokens";
        return false;
    }
    auto parse_vertex = [](const std::string& token,
                           std::string& key,
                           Label& label) {
        const std::size_t separator = token.rfind(':');
        const std::string label_text = separator == std::string::npos
            ? token
            : token.substr(separator + 1U);
        key = separator == std::string::npos
            ? token
            : token.substr(0, separator);
        label = labelFromString(label_text);
        return !key.empty() && label != kInvalidLabel;
    };
    std::string lhs_key;
    std::string rhs_key;
    Label lhs_label = kInvalidLabel;
    Label rhs_label = kInvalidLabel;
    if (!parse_vertex(lhs_token, lhs_key, lhs_label) ||
        !parse_vertex(rhs_token, rhs_key, rhs_label)) {
        detail = "query label is outside A-E";
        return false;
    }
    auto vertex = [&](const std::string& key, Label label) {
        const auto found = accumulator.vertex_by_key.find(key);
        if (found != accumulator.vertex_by_key.end()) {
            return accumulator.graph.vertex_labels[
                static_cast<std::size_t>(found->second)] == label
                ? found->second
                : -1;
        }
        if (accumulator.graph.vertex_labels.size() >=
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return -1;
        }
        const int id = static_cast<int>(
            accumulator.graph.vertex_labels.size());
        accumulator.vertex_by_key.emplace(key, id);
        accumulator.graph.vertex_labels.push_back(label);
        return id;
    };
    const int u = vertex(lhs_key, lhs_label);
    const int v = vertex(rhs_key, rhs_label);
    if (u < 0 || v < 0) {
        detail = "query vertex label conflict or cardinality overflow";
        return false;
    }
    accumulator.edges.emplace_back(u, v);
    return true;
}

bool finishQuery(
    QueryAccumulator& accumulator,
    Graph& graph,
    std::string& detail) {
    std::sort(accumulator.edges.begin(), accumulator.edges.end());
    accumulator.edges.erase(
        std::unique(accumulator.edges.begin(), accumulator.edges.end()),
        accumulator.edges.end());
    if (accumulator.graph.vertex_labels.empty()) {
        detail = "query graph is empty";
        return false;
    }
    graph = std::move(accumulator.graph);
    graph.num_vertices = static_cast<int>(graph.vertex_labels.size());
    graph.adj.resize(graph.vertex_labels.size());
    graph.in_adj.resize(graph.vertex_labels.size());
    graph.external_ids.resize(graph.vertex_labels.size());
    std::iota(graph.external_ids.begin(), graph.external_ids.end(), 0);
    for (const auto& edge : accumulator.edges) {
        graph.adj[static_cast<std::size_t>(edge.first)].push_back({edge.second, -1});
        graph.in_adj[static_cast<std::size_t>(edge.second)].push_back({edge.first, -1});
    }
    for (auto& neighbors : graph.adj) {
        std::sort(neighbors.begin(), neighbors.end(),
            [](const Edge& lhs, const Edge& rhs) { return lhs.to < rhs.to; });
    }
    for (auto& neighbors : graph.in_adj) {
        std::sort(neighbors.begin(), neighbors.end(),
            [](const Edge& lhs, const Edge& rhs) { return lhs.to < rhs.to; });
    }
    return true;
}

template <typename ConsumeLine>
V3InputStatus readHashedLines(
    const V3InputFileAuthority& authority,
    ConsumeLine&& consume,
    std::uint64_t& observed_bytes,
    std::string& observed_sha256,
    std::uint64_t& open_count,
    std::string& detail) {
    if (!isCanonicalRepositoryRelativePath(authority.path)) {
        detail = "input path is not canonical repository-relative POSIX";
        return V3InputStatus::UnsafePath;
    }
    const int descriptor = openBeneathRepository(authority.path);
    if (descriptor < 0) {
        detail = "openat2 beneath/no-magic-link input open failed";
        return V3InputStatus::OpenFailure;
    }
    ScopedFd input(descriptor);
    open_count = 1;
    struct stat status {};
    if (::fstat(input.value, &status) != 0) {
        detail = "input fstat failed";
        return V3InputStatus::OpenFailure;
    }
    if (!S_ISREG(status.st_mode)) {
        detail = "input is not a regular file";
        return V3InputStatus::NotRegularFile;
    }
    if (status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) != authority.expected_bytes) {
        detail = "input stat size differs from authority";
        return V3InputStatus::SizeMismatch;
    }
    Sha256Accumulator hash;
    std::array<char, kReadChunkBytes> buffer{};
    std::string pending;
    std::uint64_t line_number = 0;
    while (true) {
        const ssize_t count = ::read(input.value, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            detail = "input read failed";
            return V3InputStatus::ReadFailure;
        }
        if (count == 0) break;
        const std::size_t size = static_cast<std::size_t>(count);
        if (size > authority.expected_bytes - observed_bytes) {
            detail = "observed input exceeds authority byte count";
            return V3InputStatus::SizeMismatch;
        }
        observed_bytes += static_cast<std::uint64_t>(size);
        hash.update(std::string_view(buffer.data(), size));
        std::size_t begin = 0;
        while (begin < size) {
            const void* raw_newline = std::memchr(
                buffer.data() + begin, '\n', size - begin);
            if (raw_newline == nullptr) {
                pending.append(buffer.data() + begin, size - begin);
                if (pending.size() > kMaximumLineBytes) {
                    detail = "input line exceeds one MiB";
                    return V3InputStatus::SyntaxError;
                }
                break;
            }
            const char* newline = static_cast<const char*>(raw_newline);
            const std::size_t fragment = static_cast<std::size_t>(
                newline - (buffer.data() + begin));
            pending.append(buffer.data() + begin, fragment);
            if (pending.size() > kMaximumLineBytes) {
                detail = "input line exceeds one MiB";
                return V3InputStatus::SyntaxError;
            }
            ++line_number;
            if (!consume(pending, line_number, detail)) {
                return V3InputStatus::SyntaxError;
            }
            pending.clear();
            begin += fragment + 1U;
        }
    }
    if (!pending.empty()) {
        ++line_number;
        if (!consume(pending, line_number, detail)) {
            return V3InputStatus::SyntaxError;
        }
    }
    observed_sha256 = hash.finishHex();
    if (observed_bytes != authority.expected_bytes) {
        detail = "observed input byte count differs from authority";
        return V3InputStatus::SizeMismatch;
    }
    if (observed_sha256 != authority.expected_sha256) {
        detail = "observed input SHA-256 differs from authority";
        return V3InputStatus::HashMismatch;
    }
    return V3InputStatus::Complete;
}

bool queryAuthorityMatches(
    const Graph& graph,
    const V3QueryParsedAuthority& authority) noexcept {
    if (graph.num_vertices < 0 ||
        static_cast<std::uint64_t>(graph.num_vertices) != authority.vertices ||
        graph.vertex_labels.size() != authority.labels.size() ||
        graph.adj.size() != authority.vertices) {
        return false;
    }
    std::uint64_t edge_count = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> edges;
    for (std::size_t u = 0; u < graph.adj.size(); ++u) {
        if (graph.adj[u].size() >
            std::numeric_limits<std::uint64_t>::max() - edge_count) {
            return false;
        }
        edge_count += static_cast<std::uint64_t>(graph.adj[u].size());
        for (const Edge& edge : graph.adj[u]) {
            if (edge.to < 0) return false;
            edges.emplace_back(u, static_cast<std::uint64_t>(edge.to));
        }
    }
    if (edge_count != authority.edges || edges != authority.directed_edges) {
        return false;
    }
    for (std::size_t vertex = 0; vertex < graph.vertex_labels.size(); ++vertex) {
        if (labelToString(graph.vertex_labels[vertex]) != authority.labels[vertex]) {
            return false;
        }
    }
    return true;
}

} // namespace

const char* v3InputStatusName(V3InputStatus status) noexcept {
    switch (status) {
        case V3InputStatus::Complete: return "complete";
        case V3InputStatus::UnsafePath: return "unsafe-path";
        case V3InputStatus::OpenFailure: return "open-failure";
        case V3InputStatus::NotRegularFile: return "not-regular-file";
        case V3InputStatus::SizeMismatch: return "size-mismatch";
        case V3InputStatus::ReadFailure: return "read-failure";
        case V3InputStatus::ByteCountOverflow: return "byte-count-overflow";
        case V3InputStatus::HashMismatch: return "hash-mismatch";
        case V3InputStatus::SyntaxError: return "syntax-error";
        case V3InputStatus::ParsedAuthorityMismatch: return "parsed-authority-mismatch";
        case V3InputStatus::AllocationFailure: return "allocation-failure";
        case V3InputStatus::ArithmeticOverflow: return "arithmetic-overflow";
    }
    return "unknown";
}

bool V3DataInputResult::complete() const noexcept {
    return status == V3InputStatus::Complete;
}

bool V3QueryInputResult::complete() const noexcept {
    return status == V3InputStatus::Complete;
}

V3DataInputResult readV3TemporalData(
    const V3InputFileAuthority& authority,
    std::uint32_t label_seed,
    bool apply_consecutive_pair_filter) noexcept {
    V3DataInputResult result;
    try {
        std::vector<RawTemporalEdge> raw;
        const auto read_begin = Clock::now();
        result.status = readHashedLines(
            authority,
            [&](const std::string& line, std::uint64_t line_number,
                std::string& detail) {
                int u = 0;
                int v = 0;
                int timestamp = 0;
                const LineStatus status = parseTemporalLine(line, u, v, timestamp);
                if (status == LineStatus::Empty) return true;
                if (status != LineStatus::Valid || u < 0 || v < 0) {
                    detail = "invalid temporal line " + std::to_string(line_number);
                    return false;
                }
                raw.push_back({u, v, timestamp});
                return true;
            },
            result.observed_bytes,
            result.observed_sha256,
            result.open_count,
            result.detail);
        const auto read_end = Clock::now();
        if (!durationNs(read_begin, read_end, result.raw_read_ns)) {
            result.status = V3InputStatus::ArithmeticOverflow;
            result.detail = "raw-read timer is invalid";
            return result;
        }
        if (result.status != V3InputStatus::Complete) return result;
        const auto filter_begin = read_end;
        if (!buildTemporalGraph(
                std::move(raw), result.graph, label_seed,
                apply_consecutive_pair_filter, result.detail)) {
            result.status = V3InputStatus::ArithmeticOverflow;
            return result;
        }
        const auto filter_end = Clock::now();
        if (!durationNs(filter_begin, filter_end, result.internal_filter_ns)) {
            result.status = V3InputStatus::ArithmeticOverflow;
            result.detail = "filter timer is invalid";
            return result;
        }
        result.occurrences = static_cast<std::uint64_t>(
            result.graph.input_occurrence_count);
        result.unique_directed_edges = static_cast<std::uint64_t>(
            result.graph.input_unique_edge_count);
        result.retained_unique_directed_edges = static_cast<std::uint64_t>(
            result.graph.filtered_edge_count);
        result.active_vertices = static_cast<std::uint64_t>(
            result.graph.external_ids.size());
        result.status = V3InputStatus::Complete;
        return result;
    } catch (const std::bad_alloc&) {
        result.status = V3InputStatus::AllocationFailure;
        result.detail = "data input allocation failure";
        return result;
    } catch (const std::exception& error) {
        result.status = V3InputStatus::SyntaxError;
        result.detail = error.what();
        return result;
    } catch (...) {
        result.status = V3InputStatus::SyntaxError;
        result.detail = "unknown data input failure";
        return result;
    }
}

V3QueryInputResult readV3Query(
    const V3InputFileAuthority& authority,
    const V3QueryParsedAuthority& parsed_authority) noexcept {
    V3QueryInputResult result;
    try {
        QueryAccumulator accumulator;
        const auto begin = Clock::now();
        result.status = readHashedLines(
            authority,
            [&](const std::string& line, std::uint64_t,
                std::string& detail) {
                return parseQueryLine(line, accumulator, detail);
            },
            result.observed_bytes,
            result.observed_sha256,
            result.open_count,
            result.detail);
        if (result.status == V3InputStatus::Complete &&
            !finishQuery(accumulator, result.graph, result.detail)) {
            result.status = V3InputStatus::SyntaxError;
        }
        if (result.status == V3InputStatus::Complete &&
            !queryAuthorityMatches(result.graph, parsed_authority)) {
            result.status = V3InputStatus::ParsedAuthorityMismatch;
            result.detail = "parsed query differs from signed authority";
        }
        const auto end = Clock::now();
        if (!durationNs(begin, end, result.query_read_ns)) {
            result.status = V3InputStatus::ArithmeticOverflow;
            result.detail = "query-read timer is invalid";
        }
        return result;
    } catch (const std::bad_alloc&) {
        result.status = V3InputStatus::AllocationFailure;
        result.detail = "query input allocation failure";
        return result;
    } catch (const std::exception& error) {
        result.status = V3InputStatus::SyntaxError;
        result.detail = error.what();
        return result;
    } catch (...) {
        result.status = V3InputStatus::SyntaxError;
        result.detail = "unknown query input failure";
        return result;
    }
}

} // namespace dsm::v3
