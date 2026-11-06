#include "v3_receipt.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#if defined(__linux__)
#include <linux/openat2.h>
#include <sys/syscall.h>
#endif
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace dsm::v3 {
namespace {

constexpr std::string_view kIntegrityScope =
    "canonical_json_all_fields_except_integrity";
std::atomic<std::uint64_t> g_temp_nonce{1};

struct ScopedFd {
    int value = -1;
    explicit ScopedFd(int descriptor = -1) : value(descriptor) {}
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ~ScopedFd() {
        if (value >= 0) {
            while (::close(value) != 0 && errno == EINTR) {}
        }
    }
};

bool writeAll(int descriptor, std::string_view bytes) noexcept {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(
            descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (count == 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool fsyncNoIntr(int descriptor) noexcept {
    while (::fsync(descriptor) != 0) {
        if (errno != EINTR) return false;
    }
    return true;
}

int openDirectoryBeneathRepository(
    const std::filesystem::path& path) noexcept {
#if defined(__linux__) && defined(SYS_openat2)
    ScopedFd root(::open(".", O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (root.value < 0) return -1;
    const std::string lexical = path.generic_string();
    struct open_how how {};
    how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS |
        RESOLVE_NO_SYMLINKS;
    return static_cast<int>(::syscall(
        SYS_openat2, root.value, lexical.c_str(), &how, sizeof(how)));
#else
    (void)path;
    errno = ENOSYS;
    return -1;
#endif
}

} // namespace

const char* v3PublishStatusName(V3PublishStatus status) noexcept {
    switch (status) {
        case V3PublishStatus::Complete: return "complete";
        case V3PublishStatus::InvalidPayload: return "invalid-payload";
        case V3PublishStatus::ArtifactTooLarge: return "artifact-too-large";
        case V3PublishStatus::DirectoryOpenFailure: return "directory-open-failure";
        case V3PublishStatus::TargetExists: return "target-exists";
        case V3PublishStatus::TemporaryCreateFailure: return "temporary-create-failure";
        case V3PublishStatus::WriteFailure: return "write-failure";
        case V3PublishStatus::TemporaryFsyncFailure: return "temporary-fsync-failure";
        case V3PublishStatus::LinkFailure: return "link-failure";
        case V3PublishStatus::DirectoryFsyncFailure: return "directory-fsync-failure";
        case V3PublishStatus::ReloadFailure: return "reload-failure";
        case V3PublishStatus::ReloadMismatch: return "reload-mismatch";
    }
    return "unknown";
}

V3SealedArtifact sealV3CanonicalArtifact(
    Json payload,
    std::size_t maximum_bytes) {
    if (!payload.isObject() || payload.find("integrity") != nullptr) {
        throw JsonError(
            "receipt payload must be an object without integrity");
    }
    V3SealedArtifact result;
    result.payload_sha256 = sha256Hex(canonicalJsonBytes(payload));
    Json::Object integrity;
    integrity.emplace("algorithm", Json(std::string("sha256")));
    integrity.emplace(
        "canonical_payload_sha256", Json(result.payload_sha256));
    integrity.emplace("scope", Json(std::string(kIntegrityScope)));
    payload.objectValue().emplace(
        "integrity", Json::object(std::move(integrity)));
    result.value = std::move(payload);
    result.bytes = canonicalJsonBytes(result.value);
    if (result.bytes.size() > maximum_bytes) {
        throw JsonError("sealed receipt exceeds maximum artifact bytes");
    }
    if (result.bytes.empty() || result.bytes.back() != '\n' ||
        (result.bytes.size() > 1U &&
         result.bytes[result.bytes.size() - 2U] == '\n')) {
        throw JsonError("sealed receipt does not have exactly one final LF");
    }
    result.file_sha256 = sha256Hex(result.bytes);
    return result;
}

bool V3PublishResult::complete() const noexcept {
    return status == V3PublishStatus::Complete;
}

V3PublishResult publishV3CanonicalNoReplace(
    const std::filesystem::path& destination,
    const V3SealedArtifact& artifact) noexcept {
    V3PublishResult result;
    try {
        if (!artifact.value.isObject() || artifact.bytes.empty() ||
            artifact.file_sha256 != sha256Hex(artifact.bytes)) {
            result.status = V3PublishStatus::InvalidPayload;
            result.detail = "artifact bytes/value/hash are incomplete";
            return result;
        }
        const std::filesystem::path parent = destination.parent_path().empty()
            ? std::filesystem::path(".")
            : destination.parent_path();
        const std::string final_name = destination.filename().string();
        if (final_name.empty() || final_name == "." || final_name == ".." ||
            final_name.find('/') != std::string::npos) {
            result.status = V3PublishStatus::InvalidPayload;
            result.detail = "destination filename is invalid";
            return result;
        }
        if (parent.is_absolute()) {
            result.status = V3PublishStatus::InvalidPayload;
            result.detail = "receipt directory must be repository-relative";
            return result;
        }
        ScopedFd directory(openDirectoryBeneathRepository(parent));
        if (directory.value < 0) {
            result.status = V3PublishStatus::DirectoryOpenFailure;
            result.detail = "cannot open existing receipt directory";
            return result;
        }
        struct stat existing {};
        if (::fstatat(
                directory.value, final_name.c_str(), &existing,
                AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
            result.status = V3PublishStatus::TargetExists;
            result.detail = "matcher receipt target already exists";
            return result;
        }
        const std::uint64_t nonce = g_temp_nonce.fetch_add(1);
        const std::string temporary_name =
            ".matcher_receipt.tmp." + std::to_string(::getpid()) + "." +
            std::to_string(nonce);
        ScopedFd temporary(::openat(
            directory.value,
            temporary_name.c_str(),
            O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW,
            S_IRUSR | S_IWUSR));
        if (temporary.value < 0) {
            result.status = V3PublishStatus::TemporaryCreateFailure;
            result.detail = "cannot create unique same-directory temporary receipt";
            return result;
        }
        bool temp_exists = true;
        auto cleanup = [&]() noexcept {
            if (temp_exists) {
                while (::unlinkat(
                           directory.value, temporary_name.c_str(), 0) != 0 &&
                       errno == EINTR) {}
                temp_exists = false;
            }
        };
        if (!writeAll(temporary.value, artifact.bytes)) {
            cleanup();
            result.status = V3PublishStatus::WriteFailure;
            result.detail = "receipt temporary write failed";
            return result;
        }
        if (!fsyncNoIntr(temporary.value)) {
            cleanup();
            result.status = V3PublishStatus::TemporaryFsyncFailure;
            result.detail = "receipt temporary fsync failed";
            return result;
        }
        if (::linkat(
                directory.value, temporary_name.c_str(),
                directory.value, final_name.c_str(), 0) != 0) {
            const int link_error = errno;
            cleanup();
            if (link_error == EEXIST) {
                result.status = V3PublishStatus::TargetExists;
                result.detail = "receipt link lost create-if-absent race";
            } else {
                result.status = V3PublishStatus::LinkFailure;
                result.detail = "atomic receipt link failed";
            }
            return result;
        }
        result.final_link_created = true;
        if (!fsyncNoIntr(directory.value)) {
            cleanup();
            result.status = V3PublishStatus::DirectoryFsyncFailure;
            result.detail = "directory fsync after final link failed";
            return result;
        }
        cleanup();
        if (!fsyncNoIntr(directory.value)) {
            result.status = V3PublishStatus::DirectoryFsyncFailure;
            result.detail = "directory fsync after temporary cleanup failed";
            return result;
        }
        CanonicalArtifact reloaded;
        try {
            reloaded = loadCanonicalArtifact(destination);
        } catch (const std::exception& error) {
            result.status = V3PublishStatus::ReloadFailure;
            result.detail = error.what();
            return result;
        }
        if (reloaded.value != artifact.value ||
            reloaded.bytes != artifact.bytes ||
            reloaded.file_sha256 != artifact.file_sha256 ||
            reloaded.payload_sha256 != artifact.payload_sha256) {
            result.status = V3PublishStatus::ReloadMismatch;
            result.detail = "reloaded receipt differs from sealed bytes";
            return result;
        }
        result.status = V3PublishStatus::Complete;
        result.file_sha256 = reloaded.file_sha256;
        return result;
    } catch (const std::exception& error) {
        result.status = V3PublishStatus::InvalidPayload;
        result.detail = error.what();
        return result;
    } catch (...) {
        result.status = V3PublishStatus::InvalidPayload;
        result.detail = "unknown receipt publication failure";
        return result;
    }
}

} // namespace dsm::v3
