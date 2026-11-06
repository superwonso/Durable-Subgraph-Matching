#ifndef V3_RECEIPT_H
#define V3_RECEIPT_H

#include <cstdint>
#include <filesystem>
#include <string>

#include "v3_canonical_json.h"

namespace dsm::v3 {

enum class V3PublishStatus : std::uint8_t {
    Complete,
    InvalidPayload,
    ArtifactTooLarge,
    DirectoryOpenFailure,
    TargetExists,
    TemporaryCreateFailure,
    WriteFailure,
    TemporaryFsyncFailure,
    LinkFailure,
    DirectoryFsyncFailure,
    ReloadFailure,
    ReloadMismatch
};

const char* v3PublishStatusName(V3PublishStatus status) noexcept;

struct V3SealedArtifact {
    Json value;
    std::string bytes;
    std::string payload_sha256;
    std::string file_sha256;
};

V3SealedArtifact sealV3CanonicalArtifact(
    Json payload,
    std::size_t maximum_bytes = 16U * 1024U * 1024U);

struct V3PublishResult {
    V3PublishStatus status = V3PublishStatus::InvalidPayload;
    std::string detail;
    bool final_link_created = false;
    std::string file_sha256;

    bool complete() const noexcept;
};

// Same-directory, create-if-absent publication.  linkat(), not rename(), is
// the authority operation, so an existing matcher receipt is never replaced.
// A final link that was already made is deliberately preserved if a later
// fsync/reload check fails.
V3PublishResult publishV3CanonicalNoReplace(
    const std::filesystem::path& destination,
    const V3SealedArtifact& artifact) noexcept;

} // namespace dsm::v3

#endif // V3_RECEIPT_H
