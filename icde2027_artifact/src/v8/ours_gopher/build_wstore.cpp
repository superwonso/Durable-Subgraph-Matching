// Offline-only utility. No query/TD tree/plan is loaded or constructed.
#include "WStoreFile.h"
#include "RawBitset.h"
#include "../ours/v3_canonical_json.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
std::uint64_t elapsed(Clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}
std::string quoted(const std::string& value) {
    auto result = dsm::v3::canonicalJsonBytes(dsm::v3::Json(value));
    if (!result.empty() && result.back() == '\n') result.pop_back();
    return result;
}
std::vector<int> parseKs(const std::string& value) {
    std::vector<int> result;
    std::stringstream input(value);
    std::string token;
    while (std::getline(input, token, ',')) {
        if (token.empty() || token.find_first_not_of("0123456789") != std::string::npos)
            throw std::invalid_argument("--ks must contain positive decimal integers");
        std::size_t used = 0;
        const auto k = std::stoi(token, &used);
        if (used != token.size() || k <= 0) throw std::invalid_argument("invalid --ks value");
        result.push_back(k);
    }
    if (result.empty() || value.back() == ',') throw std::invalid_argument("empty --ks element");
    std::sort(result.begin(), result.end());
    if (std::adjacent_find(result.begin(), result.end()) != result.end()) throw std::invalid_argument("duplicate --ks value");
    return result;
}
void writeExclusive(const std::string& path, const std::string& content) {
    const auto fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) throw std::runtime_error("exclusive metrics create failed: " + std::string(std::strerror(errno)));
    std::size_t offset = 0;
    while (offset < content.size()) {
        const auto n = ::write(fd, content.data() + offset, content.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ::close(fd); throw std::runtime_error("metrics write failed"); }
        offset += static_cast<std::size_t>(n);
    }
    const auto sync_status = ::fsync(fd);
    const auto close_status = ::close(fd);
    if (sync_status || close_status) throw std::runtime_error("metrics sync/close failed");
}
void usage() {
    std::cout << "build_raw_bitset_store_v8 build --graph FILE --dataset ID --ks 1 --store NEW_FILE [--metrics NEW_JSON]\n"
                 "build_raw_bitset_store_v8 verify --graph FILE --ks 1 --store FILE [--dataset ID] [--metrics NEW_JSON]\n"
                 "Prepared input only: stable-id labels, seed 42, prefilter off.\n"
                 "Build always verifies every requested section against every graph edge.\n"
                 "Outputs are exclusive; partial/old stores are never overwritten.\n";
}
} // namespace

int main(int argc, char** argv) {
    std::string metrics_path;
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") { usage(); return 0; }
        if (argc < 2) throw std::invalid_argument("build or verify subcommand is required");
        const std::string mode = argv[1];
        if (mode != "build" && mode != "verify") throw std::invalid_argument("unknown subcommand");
        std::map<std::string, std::string> args;
        for (int i = 2; i < argc; i += 2) {
            const std::string key = argv[i];
            if (i + 1 >= argc || (key != "--graph" && key != "--dataset" && key != "--ks" &&
                                  key != "--store" && key != "--metrics"))
                throw std::invalid_argument("unknown/missing option: " + key);
            if (!args.emplace(key, argv[i + 1]).second) throw std::invalid_argument("duplicate option: " + key);
        }
        for (const auto* required : {"--graph", "--ks", "--store"})
            if (!args.count(required) || args.at(required).empty()) throw std::invalid_argument(std::string("missing ") + required);
        if (mode == "build" && (!args.count("--dataset") || args.at("--dataset").empty()))
            throw std::invalid_argument("build requires --dataset");
        if (args.count("--metrics")) metrics_path = args.at("--metrics");
        if (!metrics_path.empty() && ::access(metrics_path.c_str(), F_OK) == 0)
            throw std::runtime_error("metrics output already exists");
        if (mode == "build" && ::access(args.at("--store").c_str(), F_OK) == 0)
            throw std::runtime_error("store output already exists");
        const auto ks = parseKs(args.at("--ks"));
        if (ks != std::vector<int>{1})
            throw std::invalid_argument("v8 raw-bitset store requires --ks 1 only");
        auto started = Clock::now();
        const auto input_sha = wstore_file::graphFileSha256(args.at("--graph"));
        std::uint64_t sha_ns = elapsed(started);
        Graph graph;
        started = Clock::now();
        if (!readTemporalGraph(args.at("--graph"), graph, 42, nullptr, LabelMode::StableId, nullptr, 0))
            throw std::runtime_error("prepared temporal graph parse failed");
        const auto graph_read_ns = elapsed(started);
        // This is validation of the already-prepared input, not an additional
        // filter. A malformed non-F2 file is rejected, never silently changed.
        for (const auto& edge : graph.temporal_edges)
            if (edge.maximumConsecutiveRun() < 2) throw std::runtime_error("prepared input contains a non-F2 edge");
        started = Clock::now();
        if (input_sha != wstore_file::graphFileSha256(args.at("--graph")))
            throw std::runtime_error("input graph file changed during parsing");
        sha_ns += elapsed(started);
        std::ostringstream build_json;
        if (mode == "build") {
            const auto report = wstore_file::build(graph, args.at("--graph"), args.at("--dataset"), ks, args.at("--store"));
            if (report.header.graph_file_sha256 != input_sha) throw std::runtime_error("input graph file changed before offline build");
            wstore_file::writeBuildReportJson(build_json, report);
        } else build_json << "null";
        std::ostringstream verification;
        verification << '[';
        for (std::size_t i = 0; i < ks.size(); ++i) {
            if (i) verification << ',';
            // Only one selected-k section is resident at a time.
            const auto loaded = wstore_file::load(args.at("--store"), input_sha, graph.temporal_edges.size(), graph.num_vertices, ks[i]);
            if (args.count("--dataset") && loaded.header.dataset_id != args.at("--dataset"))
                throw std::runtime_error("dataset identifier mismatch");
            started = Clock::now();
            const auto checked = wstore_file::verify(graph, loaded);
            const auto independent = raw_bitset::verifyEveryBit(graph, loaded);
            const auto verify_ns = elapsed(started);
            verification << "{\"k\":" << ks[i] << ",\"checked_edges\":" << checked
                         << ",\"independent_checked_edges\":" << independent.edges
                         << ",\"independent_logical_bits\":" << independent.logical_bits
                         << ",\"independent_padding_bits\":" << independent.padding_bits
                         << ",\"independent_oracle\":\"interval_membership_no_window_kernel\""
                         << ",\"verification_ns\":" << verify_ns << ",\"load\":";
            wstore_file::writeLoadMetricsJson(verification, loaded);
            verification << '}';
        }
        verification << ']';
        started = Clock::now();
        if (input_sha != wstore_file::graphFileSha256(args.at("--graph")))
            throw std::runtime_error("input graph file changed during offline build/verification");
        sha_ns += elapsed(started);
        std::ostringstream out;
        out << "{\"status\":\"PASS\",\"mode\":" << quoted(mode)
            << ",\"representation\":\"raw_time_bitset\",\"query_k_independent\":true"
            << ",\"graph_file_sha256\":" << quoted(input_sha)
            << ",\"graph_read_ns\":" << graph_read_ns << ",\"graph_sha_ns\":" << sha_ns
            << ",\"label_mode\":\"stable-id\",\"label_seed\":42,\"prefilter\":\"off\",\"build\":"
            << build_json.str() << ",\"verification\":" << verification.str() << "}\n";
        if (!metrics_path.empty()) writeExclusive(metrics_path, out.str());
        std::cout << out.str();
        return 0;
    } catch (const wstore_file::UnsupportedWidth& error) {
        std::cout << "{\"status\":\"UNSUPPORTED_K\",\"k\":" << error.k
                  << ",\"required_words\":" << error.required_words
                  << ",\"maximum_words\":16,\"reason\":" << quoted(error.what()) << "}\n";
        return 2;
    } catch (const std::exception& error) {
        // Keep any partial store and stdout/stderr evidence for inspection.
        // There is deliberately no retry, substitution or unlink-on-failure.
        std::cout << "{\"status\":\"FAIL\",\"reason\":" << quoted(error.what()) << "}\n";
        std::cerr << "build_wstore: " << error.what() << '\n';
        return 1;
    }
}
