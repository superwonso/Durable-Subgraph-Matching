#ifndef V3_INPUT_H
#define V3_INPUT_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "Utils.h"

namespace dsm::v3 {

enum class V3InputStatus : std::uint8_t {
    Complete,
    UnsafePath,
    OpenFailure,
    NotRegularFile,
    SizeMismatch,
    ReadFailure,
    ByteCountOverflow,
    HashMismatch,
    SyntaxError,
    ParsedAuthorityMismatch,
    AllocationFailure,
    ArithmeticOverflow
};

const char* v3InputStatusName(V3InputStatus status) noexcept;

struct V3InputFileAuthority {
    std::string path;
    std::uint64_t expected_bytes = 0;
    std::string expected_sha256;
};

struct V3QueryParsedAuthority {
    std::uint64_t vertices = 0;
    std::uint64_t edges = 0;
    std::vector<std::string> labels;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> directed_edges;
};

struct V3DataInputResult {
    V3InputStatus status = V3InputStatus::OpenFailure;
    Graph graph;
    std::uint64_t observed_bytes = 0;
    std::string observed_sha256;
    std::uint64_t raw_read_ns = 0;
    std::uint64_t internal_filter_ns = 0;
    std::uint64_t open_count = 0;
    std::uint64_t occurrences = 0;
    std::uint64_t unique_directed_edges = 0;
    std::uint64_t retained_unique_directed_edges = 0;
    std::uint64_t active_vertices = 0;
    std::string detail;

    bool complete() const noexcept;
};

struct V3QueryInputResult {
    V3InputStatus status = V3InputStatus::OpenFailure;
    Graph graph;
    std::uint64_t observed_bytes = 0;
    std::string observed_sha256;
    std::uint64_t query_read_ns = 0;
    std::uint64_t open_count = 0;
    std::string detail;

    bool complete() const noexcept;
};

// Both functions open their authority path exactly once.  The same read()
// chunks feed SHA-256, byte accounting, line framing, and parsing.  Paths are
// repository-relative and resolved beneath the current repository directory;
// there is no ordinary-open fallback if the kernel cannot enforce beneath/no-
// magic-link resolution.
V3DataInputResult readV3TemporalData(
    const V3InputFileAuthority& authority,
    std::uint32_t label_seed,
    bool apply_consecutive_pair_filter = true) noexcept;

V3QueryInputResult readV3Query(
    const V3InputFileAuthority& authority,
    const V3QueryParsedAuthority& parsed_authority) noexcept;

} // namespace dsm::v3

#endif // V3_INPUT_H
