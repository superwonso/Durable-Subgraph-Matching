#include "v3_canonical_json.h"
#include "v3_input.h"
#include "v3_receipt.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void writeBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.is_open(), "cannot open I/O fixture output");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(output.good(), "cannot finish I/O fixture output");
}

dsm::v3::V3InputFileAuthority authority(
    const std::filesystem::path& path,
    const std::string& bytes) {
    return {
        path.generic_string(),
        static_cast<std::uint64_t>(bytes.size()),
        dsm::v3::sha256Hex(bytes)};
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: test_v3_io_receipt RELATIVE_FIXTURE_ROOT");
        const std::filesystem::path root(argv[1]);
        require(!root.is_absolute(), "fixture root must be repository-relative");
        std::filesystem::create_directories(root);

        const std::vector<std::string> variants{
            "1 2 1\n1 2 2\n",
            "1 2 1\r\n1 2 2\r\n",
            "1 2 1\n1 2 2"};
        std::uint64_t expected_fingerprint = 0;
        for (std::size_t index = 0; index < variants.size(); ++index) {
            const auto path = root / ("data_" + std::to_string(index) + ".txt");
            writeBytes(path, variants[index]);
            const auto result = dsm::v3::readV3TemporalData(
                authority(path, variants[index]), 42, true);
            require(result.complete(), "LF/CRLF/no-final-LF data read failed");
            require(result.open_count == 1, "data input was opened more than once");
            require(result.observed_bytes == variants[index].size(),
                "data observed bytes drift");
            require(result.graph.input_occurrence_count == 2 &&
                    result.graph.input_unique_edge_count == 1 &&
                    result.graph.filtered_edge_count == 1,
                "data parser/filter semantic drift");
            const std::uint64_t fingerprint =
                temporalGraphContentFingerprint(result.graph);
            if (index == 0) expected_fingerprint = fingerprint;
            require(fingerprint == expected_fingerprint,
                "line-ending variant changed logical graph");
        }

        auto wrong_hash = authority(root / "data_0.txt", variants[0]);
        wrong_hash.expected_sha256 = std::string(64, '0');
        auto wrong_hash_result = dsm::v3::readV3TemporalData(
            wrong_hash, 42, true);
        require(wrong_hash_result.status == dsm::v3::V3InputStatus::HashMismatch &&
                wrong_hash_result.open_count == 1,
            "data hash mismatch did not fail after one observed stream");

        auto wrong_size = authority(root / "data_0.txt", variants[0]);
        ++wrong_size.expected_bytes;
        auto wrong_size_result = dsm::v3::readV3TemporalData(
            wrong_size, 42, true);
        require(wrong_size_result.status == dsm::v3::V3InputStatus::SizeMismatch,
            "data expected-size mismatch was not distinguished");

        const std::string malformed = "1 2 not_an_integer\n";
        const auto malformed_path = root / "malformed.txt";
        writeBytes(malformed_path, malformed);
        const auto malformed_result = dsm::v3::readV3TemporalData(
            authority(malformed_path, malformed), 42, true);
        require(malformed_result.status == dsm::v3::V3InputStatus::SyntaxError,
            "malformed temporal syntax was not distinguished");

        const std::string query_bytes = "q0:A q1:B\nq1:B q2:C";
        const auto query_path = root / "query.qry";
        writeBytes(query_path, query_bytes);
        dsm::v3::V3QueryParsedAuthority query_authority;
        query_authority.vertices = 3;
        query_authority.edges = 2;
        query_authority.labels = {"A", "B", "C"};
        query_authority.directed_edges = {{0, 1}, {1, 2}};
        const auto query = dsm::v3::readV3Query(
            authority(query_path, query_bytes), query_authority);
        require(query.complete() && query.open_count == 1,
            "query one-stream parse/hash failed");
        query_authority.labels[2] = "D";
        const auto query_mismatch = dsm::v3::readV3Query(
            authority(query_path, query_bytes), query_authority);
        require(query_mismatch.status ==
                    dsm::v3::V3InputStatus::ParsedAuthorityMismatch,
            "parsed query authority mismatch was not distinguished");

        dsm::v3::Json::Object payload;
        payload.emplace("schema", dsm::v3::Json(std::string("publish-race")));
        payload.emplace("schema_version", dsm::v3::Json(std::uint64_t{1}));
        const dsm::v3::V3SealedArtifact artifact =
            dsm::v3::sealV3CanonicalArtifact(
                dsm::v3::Json::object(std::move(payload)));
        const std::filesystem::path destination = root / "race" / "receipt.json";
        std::filesystem::create_directories(destination.parent_path());
        constexpr int process_count = 64;
        std::vector<pid_t> children;
        for (int index = 0; index < process_count; ++index) {
            const pid_t child = ::fork();
            require(child >= 0, "fork failed during no-overwrite race");
            if (child == 0) {
                const auto published = dsm::v3::publishV3CanonicalNoReplace(
                    destination, artifact);
                if (published.status == dsm::v3::V3PublishStatus::Complete) {
                    ::_exit(0);
                }
                if (published.status == dsm::v3::V3PublishStatus::TargetExists) {
                    ::_exit(1);
                }
                ::_exit(2);
            }
            children.push_back(child);
        }
        int winners = 0;
        int existing = 0;
        for (pid_t child : children) {
            int status = 0;
            require(::waitpid(child, &status, 0) == child && WIFEXITED(status),
                "publish race child did not exit normally");
            const int code = WEXITSTATUS(status);
            if (code == 0) ++winners;
            else if (code == 1) ++existing;
            else throw std::runtime_error("publish race child hit non-EEXIST failure");
        }
        require(winners == 1 && existing == process_count - 1,
            "atomic no-overwrite race did not yield exactly one winner");
        const dsm::v3::CanonicalArtifact reloaded =
            dsm::v3::loadCanonicalArtifact(destination);
        require(reloaded.bytes == artifact.bytes &&
                reloaded.file_sha256 == artifact.file_sha256,
            "publish race winner content drifted");
        require(dsm::v3::publishV3CanonicalNoReplace(destination, artifact).status ==
                    dsm::v3::V3PublishStatus::TargetExists,
            "pre-existing final receipt was not protected");

        std::cout << "All V3 one-stream input and atomic receipt tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "V3 I/O/receipt test failure: " << error.what() << '\n';
        return 1;
    }
}
