#ifndef DSM_V6_WSTORE_FILE_H
#define DSM_V6_WSTORE_FILE_H

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Utils.h"
#include "WindowAlgebra.h"

// Versioned, little-endian, dataset-wide W storage. A section has one fixed
// width row per canonical Graph::temporal_edges ID; labels/query/plan are not
// part of its contents. Every section and the header have SHA-256 checksums.
namespace wstore_file {
constexpr std::uint32_t kFormatVersion = 1;

struct Section {
    int k = 0;
    std::uint32_t words = 0;
    std::uint64_t offset = 0;
    std::uint64_t bytes = 0;
    std::string sha256;
    std::uint64_t build_nanoseconds = 0;
};

struct Header {
    std::string dataset_id;
    std::uint64_t edges = 0;
    std::uint64_t vertices = 0;
    std::int64_t time_min = 0;
    std::int64_t time_max = -1;
    std::string graph_file_sha256;
    std::string graph_semantic_sha256;
    std::string header_sha256;
    std::uint64_t header_bytes = 0;
    std::uint64_t file_bytes = 0;
    std::vector<Section> sections;
};

struct LoadMetrics {
    std::uint64_t nanoseconds = 0;
    std::uint64_t read_bytes = 0; // header + requested k payload, no other sections
    std::uint64_t payload_bytes = 0;
    bool rss_available = false;
    std::uint64_t rss_before_bytes = 0;
    std::uint64_t rss_after_bytes = 0;
    std::int64_t rss_delta_bytes = 0;
};

struct LoadedSection {
    Header header;
    Section section;
    window_algebra::Domain domain;
    std::vector<std::uint64_t> payload;
    LoadMetrics metrics;
};

struct BuildReport {
    Header header;
    std::string output_path;
    std::string output_sha256;
    std::uint64_t nanoseconds = 0;
};

class UnsupportedWidth : public std::runtime_error {
public:
    UnsupportedWidth(int duration, std::uint64_t words);
    const int k;
    const std::uint64_t required_words;
};

// Streaming: does not allocate a buffer containing the complete graph/store.
std::string graphFileSha256(const std::string& path);
std::string graphSemanticSha256(const Graph& graph);

// Graph must be the immutable parse of graph_path, using the original IDs and
// full histories. The CLI checks the graph file before/after parsing. Build is
// exclusive and never replaces a preexisting store, including a partial one.
BuildReport build(const Graph& graph, const std::string& graph_path,
                  const std::string& dataset_id, std::vector<int> ks,
                  const std::string& output_path);

// expected_graph_sha256 MUST be the actual input-file digest computed by the
// graph loader, not a value copied from the store. No graph scan/TD construction
// occurs here. Header/file extents, vertex/edge counts and selected payload SHA
// are checked; only this k's payload is read. Full semantic verification below
// is offline, never hidden in the timed query preparation.
LoadedSection load(const std::string& store_path, const std::string& expected_graph_sha256,
                   std::uint64_t expected_edges, std::uint64_t expected_vertices, int k);

// Offline full graph fingerprint plus every-row bit comparison to v5's W
// kernel. Returns checked edge count, throws with k/edge ID on any discrepancy.
std::uint64_t verify(const Graph& graph, const LoadedSection& loaded);
void writeBuildReportJson(std::ostream& out, const BuildReport& report);
void writeLoadMetricsJson(std::ostream& out, const LoadedSection& loaded);
} // namespace wstore_file
#endif
