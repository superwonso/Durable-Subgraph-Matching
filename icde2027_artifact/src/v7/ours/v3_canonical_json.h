#ifndef V3_CANONICAL_JSON_H
#define V3_CANONICAL_JSON_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <array>

namespace dsm::v3 {

class JsonError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ArtifactIntegrityError : public JsonError {
public:
    using JsonError::JsonError;
};

class ArtifactIoError : public JsonError {
public:
    using JsonError::JsonError;
};

// A deliberately small evidence-JSON value.  JSON numbers are unsigned
// 64-bit integers only; floating point values are not representable.
class Json {
public:
    enum class Type : std::uint8_t {
        Null,
        Boolean,
        UnsignedInteger,
        String,
        Array,
        Object
    };

    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    Json() noexcept;
    explicit Json(bool value) noexcept;
    explicit Json(std::uint64_t value) noexcept;
    explicit Json(std::string value);

    static Json null() noexcept;
    static Json array(Array value = {});
    static Json object(Object value = {});
    static Json parse(std::string_view bytes);

    Type type() const noexcept;
    bool isNull() const noexcept;
    bool isBoolean() const noexcept;
    bool isUnsignedInteger() const noexcept;
    bool isString() const noexcept;
    bool isArray() const noexcept;
    bool isObject() const noexcept;

    bool boolean() const;
    std::uint64_t unsignedInteger() const;
    const std::string& string() const;
    const Array& arrayValue() const;
    Array& arrayValue();
    const Object& objectValue() const;
    Object& objectValue();

    const Json* find(std::string_view key) const;
    Json* find(std::string_view key);
    const Json& at(std::string_view key) const;
    Json& at(std::string_view key);

    bool operator==(const Json& other) const noexcept;
    bool operator!=(const Json& other) const noexcept;

private:
    Type type_ = Type::Null;
    bool boolean_ = false;
    std::uint64_t unsigned_integer_ = 0;
    std::string string_;
    Array array_;
    Object object_;
};

// Matches benchmark.icde2027_v3.canonical_artifact.canonical_json_bytes:
// UTF-8, sorted object keys, compact separators, and exactly one final LF.
std::string canonicalJsonBytes(const Json& value);
std::string sha256Hex(std::string_view bytes);

// Incremental SHA-256 used by the closed V3 input path.  Unlike sha256Hex(),
// this object never requires the complete input to be resident in memory.
// finishHex() is one-shot: updating or finishing an already-finalized object
// is a typed error rather than silently hashing a different byte stream.
class Sha256Accumulator {
public:
    void update(std::string_view bytes);
    std::string finishHex();
    std::uint64_t observedBytes() const noexcept;

private:
    static std::uint32_t rotateRight(
        std::uint32_t value, unsigned count) noexcept;
    void transform() noexcept;
    std::array<std::uint32_t, 8> state_{{
        0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,
        0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U}};
    std::array<std::uint8_t, 64> block_{};
    std::size_t block_size_ = 0;
    std::uint64_t total_bytes_ = 0;
    bool finalized_ = false;
};

struct CanonicalArtifact {
    Json value;
    std::string bytes;
    std::string file_sha256;
    std::string payload_sha256;
};

// Reads only the named artifact, rejects oversized/noncanonical input, and
// verifies the exact three-field SHA-256 integrity envelope.
CanonicalArtifact loadCanonicalArtifact(
    const std::filesystem::path& path,
    std::size_t maximum_bytes = 16U * 1024U * 1024U);

Json withoutIntegrity(const Json& value);
std::string canonicalPayloadSha256(const Json& value);
bool isLowercaseSha256(const Json& value) noexcept;

} // namespace dsm::v3

#endif // V3_CANONICAL_JSON_H
