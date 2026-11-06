// Offline W-store native units. Run only in a fresh artifact directory.
// This test never touches an existing dataset, store, benchmark or paper.
#include "../WStoreFile.h"
#include "../WindowStore.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
std::uint64_t checks = 0, row_bits = 0;
void require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
    ++checks;
}
template<class Fn> void rejected(Fn&& fn, const std::string& reason) {
    bool caught = false;
    try { fn(); } catch (const std::exception&) { caught = true; }
    require(caught, reason);
}
void freshText(const fs::path& path, const std::string& content) {
    require(!fs::exists(path), "fixture would overwrite file");
    std::ofstream output(path, std::ios::binary);
    output << content;
    require(static_cast<bool>(output), "fixture write failed");
}
Graph parse(const fs::path& path) {
    Graph graph;
    require(readTemporalGraph(path.string(), graph, 42, nullptr, LabelMode::StableId, nullptr, 0), "fixture parse failed");
    return graph;
}
void flip(const fs::path& path, std::uint64_t offset) {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(static_cast<std::streamoff>(offset));
    char byte = 0; file.get(byte);
    require(static_cast<bool>(file), "mutation read failed");
    byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 1U);
    file.seekp(static_cast<std::streamoff>(offset)); file.put(byte);
    require(static_cast<bool>(file), "mutation write failed");
}
bool active(const TemporalEdge& edge, std::int64_t t) {
    for (const auto& run : edge.activeIntervals()) if (run.start <= t && t <= run.end) return true;
    return false;
}
void independentBits(const Graph& graph, const wstore_file::LoadedSection& loaded) {
    for (std::size_t id = 0; id < graph.temporal_edges.size(); ++id) {
        for (std::uint64_t offset = 0; offset < loaded.domain.startCount(); ++offset) {
            const auto start = loaded.domain.timestampAt(offset);
            bool expected = true;
            for (int delta = 0; delta < loaded.section.k; ++delta)
                expected = expected && active(graph.temporal_edges[id], start + delta);
            const bool observed = (loaded.payload.at(id * loaded.section.words + offset / 64) >> (offset % 64)) & 1U;
            require(observed == expected, "independent explicit-snapshot W oracle mismatch");
            ++row_bits;
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("test_wstore_file NEW_ARTIFACT_DIRECTORY");
        const fs::path output = argv[1];
        if (!fs::create_directory(output)) throw std::runtime_error("artifact directory must not exist");
        const auto graph_path = output / "signed_gaps.txt";
        // Includes negative origin, distant singleton, reciprocal arcs, self
        // loop, duplicate tuple and deliberately unsorted input. The resulting
        // temporal IDs are canonical directed endpoint order, not input order.
        freshText(graph_path,
            "90 40 66\n10 40 -41\n10 40 -40\n10 40 -39\n10 40 -38\n10 40 -37\n"
            "40 10 -40\n40 10 -39\n40 10 -38\n40 10 -37\n40 10 -36\n"
            "40 40 20\n40 40 21\n40 40 22\n40 40 23\n"
            "10 90 3\n10 90 4\n10 90 7\n10 90 8\n"
            "90 40 64\n90 40 65\n90 40 66\n90 40 67\n90 40 68\n");
        auto graph = parse(graph_path);
        const auto sha = wstore_file::graphFileSha256(graph_path.string());
        const auto store_path = output / "all_k.wstore";
        const auto built = wstore_file::build(graph, graph_path.string(), "synthetic_signed", {2,3,4,5}, store_path.string());
        require(built.header.time_min == -41 && built.header.time_max == 68, "original signed time axis changed");
        require(built.header.sections.size() == 4, "section count");
        require(built.header.file_bytes == fs::file_size(store_path), "file size receipt");
        require(built.output_sha256 == wstore_file::graphFileSha256(store_path.string()), "whole store SHA");
        for (const auto k : {2,3,4,5}) {
            const auto loaded = wstore_file::load(store_path.string(), sha, graph.temporal_edges.size(), graph.num_vertices, k);
            require(wstore_file::verify(graph, loaded) == graph.temporal_edges.size(), "full-edge kernel verification count");
            independentBits(graph, loaded);
            require(loaded.metrics.read_bytes == loaded.header.header_bytes + loaded.section.bytes, "selected-section-only byte count");
            require(loaded.metrics.read_bytes < loaded.header.file_bytes, "other k payloads must not be read");
            WindowStore store(graph, store_path.string(), sha, k);
            require(store.loaded() && !store.lazy() && !store.forcedIntervals(), "loaded store mode");
            require(store.preparationNanoseconds() == 0 && store.builtEdges() == 0 && store.lazyBuildThreadNanoseconds() == 0,
                    "loaded store must not report query W construction");
            require(store.relevantEdges() == graph.temporal_edges.size(), "all graph IDs eligible");
            require(store.loadedBytes() == loaded.section.bytes && store.scratchIntervalBound(100) == 0, "dense metadata");
            for (std::size_t id = 0; id < graph.temporal_edges.size(); ++id) {
                require(store.eligible(static_cast<int>(id)), "graph-wide eligibility");
                const auto view = store.get(static_cast<int>(id));
                for (std::size_t w = 0; w < loaded.section.words; ++w)
                    require((view.isEmpty() ? 0 : view.words()[w]) == loaded.payload[id * loaded.section.words + w], "stable direct-ID row");
            }
            require(!store.eligible(-1) && !store.eligible(static_cast<int>(graph.temporal_edges.size())), "invalid edge eligibility");
            rejected([&] { store.get(-1); }, "negative edge ID accepted");
            std::ostringstream json; store.writeLoadJson(json);
            require(json.str().find("\"query_build_ns\":0") != std::string::npos &&
                    json.str().find("\"runtime_semantic_scan\":false") != std::string::npos, "load JSON contract");
        }
        const auto load_k = [&](const fs::path& path, int k) {
            return wstore_file::load(path.string(), sha, graph.temporal_edges.size(), graph.num_vertices, k);
        };
        rejected([&] { wstore_file::build(graph, graph_path.string(), "x", {2}, store_path.string()); }, "existing store replaced");
        rejected([&] { wstore_file::load(store_path.string(), std::string(64, '0'), graph.temporal_edges.size(), graph.num_vertices, 2); }, "wrong input SHA accepted");
        rejected([&] { wstore_file::load(store_path.string(), sha, graph.temporal_edges.size() + 1, graph.num_vertices, 2); }, "wrong edge count accepted");
        rejected([&] { wstore_file::load(store_path.string(), sha, graph.temporal_edges.size(), graph.num_vertices + 1, 2); }, "wrong vertex count accepted");
        rejected([&] { load_k(store_path, 6); }, "missing k accepted");
        const auto bad_header = output / "bad_header.wstore";
        fs::copy_file(store_path, bad_header); flip(bad_header, 120);
        rejected([&] { load_k(bad_header, 2); }, "header corruption accepted");
        const auto bad_payload = output / "bad_payload.wstore";
        fs::copy_file(store_path, bad_payload); flip(bad_payload, built.header.sections[0].offset);
        rejected([&] { load_k(bad_payload, 2); }, "selected payload corruption accepted");
        // Explicit selected-section contract: corrupt unselected k2 is not read
        // or checksummed when loading k3; full offline tool checks all sections.
        require(wstore_file::verify(graph, load_k(bad_payload, 3)) == graph.temporal_edges.size(), "unselected section was needlessly loaded");
        const auto truncated = output / "truncated.wstore";
        fs::copy_file(store_path, truncated); fs::resize_file(truncated, fs::file_size(truncated) - 1);
        rejected([&] { load_k(truncated, 2); }, "truncated extent accepted");
        const auto trailing = output / "trailing.wstore";
        fs::copy_file(store_path, trailing); fs::resize_file(trailing, fs::file_size(trailing) + 1);
        rejected([&] { load_k(trailing, 2); }, "trailing extent accepted");
        auto changed = graph;
        std::swap(changed.temporal_edges[0], changed.temporal_edges[1]);
        const auto reference = load_k(store_path, 2);
        rejected([&] { wstore_file::verify(changed, reference); }, "offline edge-order fingerprint did not detect mismatch");
        changed = graph;
        std::reverse(changed.vertex_labels.begin(), changed.vertex_labels.end());
        require(wstore_file::graphSemanticSha256(changed) == wstore_file::graphSemanticSha256(graph), "labels incorrectly affect graph W identity");

        const auto short_path = output / "short.txt";
        freshText(short_path, "1 2 0\n1 2 1\n");
        const auto short_graph = parse(short_path);
        const auto short_store = output / "short.wstore";
        wstore_file::build(short_graph, short_path.string(), "short", {2,3,4,5}, short_store.string());
        const auto zero = wstore_file::load(short_store.string(), wstore_file::graphFileSha256(short_path.string()), 1, 2, 5);
        require(zero.section.words == 0 && zero.section.bytes == 0 && zero.payload.empty(), "zero-width k section");
        require(wstore_file::verify(short_graph, zero) == 1, "zero-width verification");
        WindowStore short_loaded(short_graph, short_store.string(), wstore_file::graphFileSha256(short_path.string()), 5);
        require(short_loaded.get(0).isEmpty(), "zero-width direct lookup");

        const auto wide_path = output / "wide.txt";
        freshText(wide_path, "1 2 0\n1 2 1\n2 1 1025\n2 1 1026\n");
        const auto wide_graph = parse(wide_path);
        const auto unsupported = output / "unsupported.wstore";
        bool width_rejected = false;
        try { wstore_file::build(wide_graph, wide_path.string(), "wide", {2,4}, unsupported.string()); }
        catch (const wstore_file::UnsupportedWidth& error) { width_rejected = error.k == 2 && error.required_words == 17; }
        require(width_rejected && !fs::exists(unsupported), "unsupported k must fail specifically before file creation");
        const auto supported = output / "sixteen_words.wstore";
        wstore_file::build(wide_graph, wide_path.string(), "wide", {4,5}, supported.string());
        const auto sixteen = wstore_file::load(supported.string(), wstore_file::graphFileSha256(wide_path.string()), 2, 2, 4);
        require(sixteen.section.words == 16, "exact sixteen-word boundary");
        independentBits(wide_graph, sixteen);

        const auto empty_path = output / "empty.txt";
        freshText(empty_path, "");
        Graph empty;
        const auto empty_store = output / "empty.wstore";
        wstore_file::build(empty, empty_path.string(), "empty", {2,3,4,5}, empty_store.string());
        const auto empty_loaded = wstore_file::load(empty_store.string(), wstore_file::graphFileSha256(empty_path.string()), 0, 0, 2);
        require(empty_loaded.header.time_min == 0 && empty_loaded.header.time_max == -1 && wstore_file::verify(empty, empty_loaded) == 0, "empty graph section");
        std::cout << "{\"status\":\"PASS\",\"checks\":" << checks << ",\"independent_row_bits\":" << row_bits
                  << ",\"ks\":[2,3,4,5],\"signed_axis\":true,\"selected_section_only\":true,\"corruption_rejection\":true}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test_wstore_file FAIL: " << error.what() << '\n';
        return 1;
    }
}
