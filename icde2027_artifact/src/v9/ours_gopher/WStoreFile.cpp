#include "WStoreFile.h"
#include "../ours/v3_canonical_json.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace wstore_file {
namespace {
using Clock = std::chrono::steady_clock;
using Sha = dsm::v3::Sha256Accumulator;
constexpr std::array<unsigned char, 8> kMagic{{'D','S','M','W','S','V','6',0}};
constexpr std::size_t kPrefixBytes = 120;
constexpr std::size_t kSectionBytes = 64;
constexpr std::size_t kDigestBytes = 32;
constexpr std::size_t kBufferBytes = 1U << 20;

std::uint64_t elapsed(Clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}
[[noreturn]] void invalid(const std::string& reason) {
    throw std::runtime_error("W-store: " + reason);
}
std::uint64_t checkedAdd(std::uint64_t a, std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) invalid("size addition overflow");
    return a + b;
}
std::uint64_t checkedMultiply(std::uint64_t a, std::uint64_t b) {
    if (a && b > std::numeric_limits<std::uint64_t>::max() / a) invalid("size multiplication overflow");
    return a * b;
}
void put(std::vector<unsigned char>& out, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) { out.push_back(static_cast<unsigned char>(value)); value >>= 8; }
}
std::uint64_t get(const std::vector<unsigned char>& in, std::size_t& at, unsigned bytes) {
    if (bytes > in.size() || at > in.size() - bytes) invalid("truncated header");
    std::uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= static_cast<std::uint64_t>(in[at++]) << (8 * i);
    return value;
}
std::int64_t signedValue(std::uint64_t value) {
    if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return static_cast<std::int64_t>(value);
    return -static_cast<std::int64_t>(~value) - 1;
}
unsigned hexDigit(char c) {
    if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
    invalid("digest must contain 64 lowercase hexadecimal characters");
}
void putDigest(std::vector<unsigned char>& out, const std::string& digest) {
    if (digest.size() != 64) invalid("digest must contain 64 lowercase hexadecimal characters");
    for (std::size_t i = 0; i < 64; i += 2)
        out.push_back(static_cast<unsigned char>((hexDigit(digest[i]) << 4) | hexDigit(digest[i + 1])));
}
std::string getDigest(const std::vector<unsigned char>& in, std::size_t& at) {
    static constexpr char hex[] = "0123456789abcdef";
    if (at > in.size() || in.size() - at < kDigestBytes) invalid("truncated digest");
    std::string result;
    result.reserve(64);
    for (std::size_t i = 0; i < kDigestBytes; ++i) {
        const auto byte = in[at++]; result.push_back(hex[byte >> 4]); result.push_back(hex[byte & 15]);
    }
    return result;
}
void hashBytes(Sha& hash, const void* bytes, std::size_t size) {
    hash.update(std::string_view(static_cast<const char*>(bytes), size));
}
void hashInteger(Sha& hash, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (unsigned i = 0; i < 8; ++i) { bytes[i] = static_cast<char>(value & 255); value >>= 8; }
    hash.update(std::string_view(bytes.data(), bytes.size()));
}
std::string quoted(const std::string& value) {
    auto result = dsm::v3::canonicalJsonBytes(dsm::v3::Json(value));
    if (!result.empty() && result.back() == '\n') result.pop_back();
    return result;
}
void exactRead(std::istream& in, void* data, std::size_t size) {
    if (!size) return;
    in.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    if (!in || in.gcount() != static_cast<std::streamsize>(size)) invalid("truncated/read-failed file");
}
class ExclusiveFile {
public:
    explicit ExclusiveFile(const std::string& path) {
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd_ < 0) invalid("exclusive create failed for " + path + ": " + std::strerror(errno));
    }
    ~ExclusiveFile() { if (fd_ >= 0) ::close(fd_); }
    void write(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        while (size) {
            const auto n = ::write(fd_, bytes, size);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) invalid("write failed: " + std::string(std::strerror(errno)));
            bytes += n; size -= static_cast<std::size_t>(n);
        }
    }
    void finish(const std::vector<unsigned char>& header) {
        if (::lseek(fd_, 0, SEEK_SET) < 0) invalid("header seek failed");
        write(header.data(), header.size());
        if (::fsync(fd_) != 0) invalid("fsync failed");
        const auto fd = fd_; fd_ = -1;
        if (::close(fd) != 0) invalid("close failed");
    }
private:
    int fd_ = -1;
};
std::optional<std::uint64_t> currentRss() {
    std::ifstream in("/proc/self/statm");
    std::uint64_t virtual_pages = 0, resident_pages = 0;
    const auto page_bytes = ::sysconf(_SC_PAGESIZE);
    if (!(in >> virtual_pages >> resident_pages) || page_bytes <= 0) return std::nullopt;
    if (resident_pages > std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(page_bytes))
        return std::nullopt;
    return resident_pages * static_cast<std::uint64_t>(page_bytes);
}
std::pair<std::int64_t, std::int64_t> timeDomain(const Graph& graph) {
    std::int64_t minimum = std::numeric_limits<std::int64_t>::max();
    std::int64_t maximum = std::numeric_limits<std::int64_t>::min();
    for (const auto& edge : graph.temporal_edges) {
        const auto& runs = edge.activeIntervals();
        if (!runs.empty()) {
            minimum = std::min(minimum, static_cast<std::int64_t>(runs.front().start));
            maximum = std::max(maximum, static_cast<std::int64_t>(runs.back().end));
        }
    }
    return minimum <= maximum ? std::make_pair(minimum, maximum) : std::make_pair(std::int64_t{0}, std::int64_t{-1});
}
std::vector<unsigned char> encodeHeader(Header& h) {
    std::vector<unsigned char> out(kMagic.begin(), kMagic.end());
    put(out, kFormatVersion, 4); put(out, h.header_bytes, 4);
    put(out, h.sections.size(), 4); put(out, h.dataset_id.size(), 4);
    put(out, h.edges, 8); put(out, h.vertices, 8);
    put(out, static_cast<std::uint64_t>(h.time_min), 8); put(out, static_cast<std::uint64_t>(h.time_max), 8);
    putDigest(out, h.graph_file_sha256); putDigest(out, h.graph_semantic_sha256);
    if (out.size() != kPrefixBytes) invalid("internal header prefix size");
    out.insert(out.end(), h.dataset_id.begin(), h.dataset_id.end());
    for (const auto& section : h.sections) {
        put(out, static_cast<std::uint64_t>(section.k), 4); put(out, section.words, 4);
        put(out, section.offset, 8); put(out, section.bytes, 8);
        putDigest(out, section.sha256); put(out, section.build_nanoseconds, 8);
    }
    h.header_sha256 = dsm::v3::sha256Hex(std::string_view(reinterpret_cast<const char*>(out.data()), out.size()));
    putDigest(out, h.header_sha256);
    if (out.size() != h.header_bytes) invalid("internal complete header size");
    return out;
}
Header readHeader(std::ifstream& in) {
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (end < 0) invalid("cannot determine file size");
    const auto file_bytes = static_cast<std::uint64_t>(end);
    in.seekg(0);
    std::vector<unsigned char> bytes(24);
    exactRead(in, bytes.data(), bytes.size());
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) invalid("wrong magic");
    std::size_t at = 8;
    if (get(bytes, at, 4) != kFormatVersion) invalid("unsupported version");
    const auto header_bytes = get(bytes, at, 4);
    const auto count = get(bytes, at, 4);
    const auto dataset_bytes = get(bytes, at, 4);
    if (!count || count > 64 || !dataset_bytes || dataset_bytes > 4096 ||
        header_bytes != kPrefixBytes + dataset_bytes + count * kSectionBytes + kDigestBytes ||
        header_bytes > 65536 || header_bytes > file_bytes) invalid("invalid header lengths");
    bytes.resize(static_cast<std::size_t>(header_bytes));
    exactRead(in, bytes.data() + 24, bytes.size() - 24);
    Header h;
    h.header_bytes = header_bytes; h.file_bytes = file_bytes;
    h.edges = get(bytes, at, 8); h.vertices = get(bytes, at, 8);
    h.time_min = signedValue(get(bytes, at, 8)); h.time_max = signedValue(get(bytes, at, 8));
    h.graph_file_sha256 = getDigest(bytes, at); h.graph_semantic_sha256 = getDigest(bytes, at);
    h.dataset_id.assign(reinterpret_cast<const char*>(bytes.data() + at), static_cast<std::size_t>(dataset_bytes));
    at += static_cast<std::size_t>(dataset_bytes);
    if (h.dataset_id.find('\0') != std::string::npos) invalid("NUL in dataset identifier");
    std::uint64_t expected_offset = header_bytes;
    int last_k = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        Section section;
        const auto k = get(bytes, at, 4);
        if (!k || k > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) || k <= static_cast<std::uint64_t>(last_k))
            invalid("k list is not positive, sorted and unique");
        section.k = static_cast<int>(k); last_k = section.k;
        section.words = static_cast<std::uint32_t>(get(bytes, at, 4));
        section.offset = get(bytes, at, 8); section.bytes = get(bytes, at, 8);
        section.sha256 = getDigest(bytes, at); section.build_nanoseconds = get(bytes, at, 8);
        const auto domain = window_algebra::Domain::make(h.time_min, h.time_max, section.k);
        if (domain.wordCount() > window_algebra::kDenseWords) throw UnsupportedWidth(section.k, domain.wordCount());
        if (section.words != domain.wordCount() || section.offset != expected_offset ||
            section.bytes != checkedMultiply(checkedMultiply(h.edges, section.words), 8)) invalid("inconsistent section geometry");
        expected_offset = checkedAdd(expected_offset, section.bytes);
        h.sections.push_back(section);
    }
    h.header_sha256 = getDigest(bytes, at);
    if (at != bytes.size() || expected_offset != file_bytes) invalid("noncanonical/truncated/trailing file extent");
    const auto actual = dsm::v3::sha256Hex(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size() - kDigestBytes));
    if (actual != h.header_sha256) invalid("header SHA-256 mismatch");
    return h;
}
void headerJson(std::ostream& out, const Header& h) {
    out << "\"format_version\":" << kFormatVersion << ",\"dataset_id\":" << quoted(h.dataset_id)
        << ",\"edges\":" << h.edges << ",\"vertices\":" << h.vertices
        << ",\"time_min\":" << h.time_min << ",\"time_max\":" << h.time_max
        << ",\"graph_file_sha256\":" << quoted(h.graph_file_sha256)
        << ",\"graph_semantic_sha256\":" << quoted(h.graph_semantic_sha256)
        << ",\"header_sha256\":" << quoted(h.header_sha256)
        << ",\"header_bytes\":" << h.header_bytes << ",\"store_file_bytes\":" << h.file_bytes;
}
} // namespace

UnsupportedWidth::UnsupportedWidth(int duration, std::uint64_t words)
    : std::runtime_error("W-store unsupported width: k=" + std::to_string(duration) +
                         " requires " + std::to_string(words) + " words; maximum is 16 (no fallback)"),
      k(duration), required_words(words) {}

std::string graphFileSha256(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) invalid("cannot open file for SHA-256: " + path);
    std::vector<char> buffer(kBufferBytes);
    Sha hash;
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (in.gcount() > 0) hash.update(std::string_view(buffer.data(), static_cast<std::size_t>(in.gcount())));
    }
    if (!in.eof()) invalid("file SHA-256 read failed: " + path);
    return hash.finishHex();
}

std::string graphSemanticSha256(const Graph& graph) {
    if (graph.num_vertices < 0 || (!graph.external_ids.empty() && graph.external_ids.size() != static_cast<std::size_t>(graph.num_vertices)))
        invalid("invalid graph vertex mapping");
    Sha hash;
    hash.update("DSM-WSTORE-GRAPH-SEMANTIC-v1");
    hashInteger(hash, static_cast<std::uint64_t>(graph.num_vertices));
    hashInteger(hash, graph.temporal_edges.size());
    for (int v = 0; v < graph.num_vertices; ++v)
        hashInteger(hash, static_cast<std::uint64_t>(static_cast<std::int64_t>(graph.externalId(v))));
    for (const auto& edge : graph.temporal_edges) {
        if (edge.u < 0 || edge.v < 0 || edge.u >= graph.num_vertices || edge.v >= graph.num_vertices)
            invalid("invalid graph endpoint");
        hashInteger(hash, static_cast<std::uint64_t>(static_cast<std::int64_t>(graph.externalId(edge.u))));
        hashInteger(hash, static_cast<std::uint64_t>(static_cast<std::int64_t>(graph.externalId(edge.v))));
        hashInteger(hash, edge.activeIntervals().size());
        for (const auto& run : edge.activeIntervals()) {
            hashInteger(hash, static_cast<std::uint64_t>(static_cast<std::int64_t>(run.start)));
            hashInteger(hash, static_cast<std::uint64_t>(static_cast<std::int64_t>(run.end)));
        }
    }
    return hash.finishHex();
}

BuildReport build(const Graph& graph, const std::string& graph_path, const std::string& dataset_id,
                  std::vector<int> ks, const std::string& output_path) {
    const auto start = Clock::now();
    if (dataset_id.empty() || dataset_id.size() > 4096 || dataset_id.find('\0') != std::string::npos)
        invalid("invalid dataset identifier");
    if (ks.empty() || ks.size() > 64) invalid("empty/oversized k list");
    std::sort(ks.begin(), ks.end());
    if (ks.front() <= 0 || std::adjacent_find(ks.begin(), ks.end()) != ks.end()) invalid("k list must be positive and unique");
    BuildReport report;
    auto& h = report.header;
    report.output_path = output_path;
    h.dataset_id = dataset_id; h.edges = graph.temporal_edges.size(); h.vertices = graph.num_vertices;
    const auto axis = timeDomain(graph); h.time_min = axis.first; h.time_max = axis.second;
    h.header_bytes = kPrefixBytes + dataset_id.size() + ks.size() * kSectionBytes + kDigestBytes;
    h.file_bytes = h.header_bytes;
    for (const auto k : ks) {
        const auto domain = window_algebra::Domain::make(h.time_min, h.time_max, k);
        if (domain.wordCount() > window_algebra::kDenseWords) throw UnsupportedWidth(k, domain.wordCount());
        Section section;
        section.k = k; section.words = static_cast<std::uint32_t>(domain.wordCount());
        section.offset = h.file_bytes; section.bytes = checkedMultiply(checkedMultiply(h.edges, section.words), 8);
        h.file_bytes = checkedAdd(h.file_bytes, section.bytes);
        h.sections.push_back(section);
    }
    if (h.file_bytes > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) invalid("file exceeds platform offsets");
    h.graph_file_sha256 = graphFileSha256(graph_path);
    h.graph_semantic_sha256 = graphSemanticSha256(graph);
    ExclusiveFile output(output_path); // no output exists until all width checks pass
    std::vector<unsigned char> placeholder(static_cast<std::size_t>(h.header_bytes), 0);
    output.write(placeholder.data(), placeholder.size());
    std::vector<unsigned char> buffer;
    buffer.reserve(kBufferBytes + window_algebra::kDenseWords * 8);
    for (auto& section : h.sections) {
        const auto section_start = Clock::now();
        const auto domain = window_algebra::Domain::make(h.time_min, h.time_max, section.k);
        window_algebra::WindowBuffer window;
        Sha hash;
        auto flush = [&] {
            if (buffer.empty()) return;
            hashBytes(hash, buffer.data(), buffer.size()); output.write(buffer.data(), buffer.size()); buffer.clear();
        };
        for (const auto& edge : graph.temporal_edges) {
            const auto& intervals = edge.activeIntervals();
            window_algebra::buildInto(domain, intervals.data(), intervals.size(), window);
            const auto view = window.view();
            for (std::uint32_t w = 0; w < section.words; ++w)
                put(buffer, view.isEmpty() ? 0 : view.words()[w], 8);
            if (buffer.size() >= kBufferBytes) flush();
        }
        flush();
        section.sha256 = hash.finishHex(); section.build_nanoseconds = elapsed(section_start);
    }
    const auto header = encodeHeader(h);
    output.finish(header);
    report.output_sha256 = graphFileSha256(output_path);
    report.nanoseconds = elapsed(start);
    return report;
}

LoadedSection load(const std::string& store_path, const std::string& expected_graph_sha256,
                   std::uint64_t expected_edges, std::uint64_t expected_vertices, int k) {
    const auto start = Clock::now();
    const auto rss_before = currentRss();
    std::vector<unsigned char> checked_digest;
    putDigest(checked_digest, expected_graph_sha256);
    std::ifstream in(store_path, std::ios::binary);
    if (!in) invalid("cannot open store: " + store_path);
    auto h = readHeader(in);
    if (h.graph_file_sha256 != expected_graph_sha256) invalid("actual graph-file SHA-256 mismatch");
    if (h.edges != expected_edges || h.vertices != expected_vertices) invalid("loaded graph edge/vertex count mismatch");
    const auto found = std::find_if(h.sections.begin(), h.sections.end(), [=](const Section& s) { return s.k == k; });
    if (found == h.sections.end()) invalid("requested k=" + std::to_string(k) + " section is absent");
    const auto section = *found;
    const auto domain = window_algebra::Domain::make(h.time_min, h.time_max, k);
    LoadedSection result{std::move(h), section, domain, {}, {}};
    if (section.bytes / 8 > result.payload.max_size()) invalid("selected payload exceeds addressable memory");
    result.payload.resize(static_cast<std::size_t>(section.bytes / 8));
    in.seekg(static_cast<std::streamoff>(section.offset));
    if (!in) invalid("selected section seek failed");
    {
        std::vector<unsigned char> buffer(kBufferBytes);
        Sha hash;
        std::size_t destination = 0;
        std::uint64_t remaining = section.bytes;
        while (remaining) {
            const auto bytes = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            exactRead(in, buffer.data(), bytes); hashBytes(hash, buffer.data(), bytes);
            for (std::size_t at = 0; at < bytes;) result.payload[destination++] = get(buffer, at, 8);
            remaining -= bytes;
        }
        if (hash.finishHex() != section.sha256) invalid("selected section SHA-256 mismatch for k=" + std::to_string(k));
    }
    const auto partial = static_cast<unsigned>(domain.startCount() % 64);
    if (partial && section.words) {
        const auto invalid_mask = ~((std::uint64_t{1} << partial) - 1);
        for (std::uint64_t id = 0; id < result.header.edges; ++id)
            if (result.payload[static_cast<std::size_t>((id + 1) * section.words - 1)] & invalid_mask)
                invalid("nonzero high padding bits");
    }
    result.metrics.read_bytes = checkedAdd(result.header.header_bytes, section.bytes);
    result.metrics.payload_bytes = section.bytes;
    const auto rss_after = currentRss();
    if (rss_before && rss_after && *rss_before <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
        *rss_after <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        result.metrics.rss_available = true;
        result.metrics.rss_before_bytes = *rss_before; result.metrics.rss_after_bytes = *rss_after;
        result.metrics.rss_delta_bytes = static_cast<std::int64_t>(*rss_after) - static_cast<std::int64_t>(*rss_before);
    }
    result.metrics.nanoseconds = elapsed(start);
    return result;
}

std::uint64_t verify(const Graph& graph, const LoadedSection& loaded) {
    const auto axis = timeDomain(graph);
    if (loaded.header.edges != graph.temporal_edges.size() || loaded.header.vertices != static_cast<std::uint64_t>(graph.num_vertices) ||
        loaded.header.time_min != axis.first || loaded.header.time_max != axis.second ||
        loaded.header.graph_semantic_sha256 != graphSemanticSha256(graph)) invalid("offline graph semantic fingerprint/domain mismatch");
    window_algebra::WindowBuffer expected;
    for (std::size_t id = 0; id < graph.temporal_edges.size(); ++id) {
        const auto& intervals = graph.temporal_edges[id].activeIntervals();
        window_algebra::buildInto(loaded.domain, intervals.data(), intervals.size(), expected);
        const auto view = expected.view();
        for (std::size_t w = 0; w < loaded.section.words; ++w)
            if (loaded.payload.at(id * loaded.section.words + w) != (view.isEmpty() ? 0 : view.words()[w]))
                invalid("bit verification failed k=" + std::to_string(loaded.section.k) + " edge_id=" + std::to_string(id));
    }
    return graph.temporal_edges.size();
}

void writeBuildReportJson(std::ostream& out, const BuildReport& report) {
    out << '{'; headerJson(out, report.header);
    out << ",\"output_path\":" << quoted(report.output_path) << ",\"output_sha256\":" << quoted(report.output_sha256)
        << ",\"build_total_ns\":" << report.nanoseconds << ",\"sections\":[";
    for (std::size_t i = 0; i < report.header.sections.size(); ++i) {
        if (i) out << ',';
        const auto& section = report.header.sections[i];
        out << "{\"k\":" << section.k << ",\"words\":" << section.words << ",\"offset\":" << section.offset
            << ",\"bytes\":" << section.bytes << ",\"sha256\":" << quoted(section.sha256)
            << ",\"build_ns\":" << section.build_nanoseconds << '}';
    }
    out << "]}";
}
void writeLoadMetricsJson(std::ostream& out, const LoadedSection& loaded) {
    out << '{'; headerJson(out, loaded.header);
    const auto& m = loaded.metrics;
    out << ",\"k\":" << loaded.section.k << ",\"words\":" << loaded.section.words
        << ",\"section_sha256\":" << quoted(loaded.section.sha256) << ",\"section_bytes\":" << loaded.section.bytes
        << ",\"read_bytes\":" << m.read_bytes << ",\"load_ns\":" << m.nanoseconds
        << ",\"rss_available\":" << (m.rss_available ? "true" : "false") << ",\"rss_before_bytes\":";
    if (m.rss_available) out << m.rss_before_bytes; else out << "null";
    out << ",\"rss_after_bytes\":";
    if (m.rss_available) out << m.rss_after_bytes; else out << "null";
    out << ",\"rss_delta_bytes\":";
    if (m.rss_available) out << m.rss_delta_bytes; else out << "null";
    out << ",\"runtime_semantic_scan\":false,\"query_build_ns\":0}";
}
} // namespace wstore_file
