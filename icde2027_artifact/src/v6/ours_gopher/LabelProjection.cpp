#include "LabelProjection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class ArtifactError : public std::runtime_error {
public:
    explicit ArtifactError(const std::string& message)
        : std::runtime_error(message) {}
};

std::uint32_t rotateRight(std::uint32_t value, unsigned shift) {
    return (value >> shift) | (value << (32U - shift));
}

class Sha256 {
public:
    void update(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            block_[block_size_++] = bytes[i];
            total_bytes_ += 1U;
            if (block_size_ == block_.size()) {
                transform(block_.data());
                block_size_ = 0;
            }
        }
    }

    void update(const std::string& text) {
        update(text.data(), text.size());
    }

    std::string finishHex() {
        const std::uint64_t total_bits = total_bytes_ * 8U;
        block_[block_size_++] = 0x80U;
        if (block_size_ > 56U) {
            while (block_size_ < block_.size()) block_[block_size_++] = 0U;
            transform(block_.data());
            block_size_ = 0;
        }
        while (block_size_ < 56U) block_[block_size_++] = 0U;
        for (unsigned shift = 56U; shift <= 56U; shift -= 8U) {
            block_[block_size_++] = static_cast<std::uint8_t>(
                (total_bits >> shift) & 0xffU);
            if (shift == 0U) break;
        }
        transform(block_.data());
        block_size_ = 0;

        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (std::uint32_t value : state_) {
            output << std::setw(8) << value;
        }
        return output.str();
    }

private:
    void transform(const std::uint8_t* block) {
        static constexpr std::array<std::uint32_t, 64> constants = {{
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
            0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
            0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
            0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
            0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U}};
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16U; ++i) {
            const std::size_t offset = i * 4U;
            words[i] =
                (static_cast<std::uint32_t>(block[offset]) << 24U) |
                (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                static_cast<std::uint32_t>(block[offset + 3U]);
        }
        for (std::size_t i = 16U; i < words.size(); ++i) {
            const std::uint32_t lhs = words[i - 15U];
            const std::uint32_t rhs = words[i - 2U];
            const std::uint32_t sigma0 =
                rotateRight(lhs, 7U) ^ rotateRight(lhs, 18U) ^ (lhs >> 3U);
            const std::uint32_t sigma1 =
                rotateRight(rhs, 17U) ^ rotateRight(rhs, 19U) ^ (rhs >> 10U);
            words[i] = words[i - 16U] + sigma0 + words[i - 7U] + sigma1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t i = 0; i < words.size(); ++i) {
            const std::uint32_t sum1 =
                rotateRight(e, 6U) ^ rotateRight(e, 11U) ^ rotateRight(e, 25U);
            const std::uint32_t choose = (e & f) ^ ((~e) & g);
            const std::uint32_t temporary1 =
                h + sum1 + choose + constants[i] + words[i];
            const std::uint32_t sum0 =
                rotateRight(a, 2U) ^ rotateRight(a, 13U) ^ rotateRight(a, 22U);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U}};
    std::array<std::uint8_t, 64> block_{};
    std::size_t block_size_ = 0;
    std::uint64_t total_bytes_ = 0;
};

struct FileContents {
    std::string bytes;
    std::string sha256;
};

FileContents readAndHash(const std::string& filename) {
    std::ifstream input(filename, std::ios::binary);
    if (!input.is_open()) {
        throw ArtifactError("cannot open " + filename);
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        throw ArtifactError("failed while reading " + filename);
    }
    FileContents result;
    result.bytes = buffer.str();
    Sha256 digest;
    digest.update(result.bytes);
    result.sha256 = digest.finishHex();
    return result;
}

class JsonReader {
public:
    JsonReader(const std::string& text, std::string source)
        : text_(text), source_(std::move(source)) {}

    bool consume(char expected) {
        skipWhitespace();
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect(char expected) {
        if (!consume(expected)) {
            fail(std::string("expected '") + expected + "'");
        }
    }

    char peek() {
        skipWhitespace();
        if (position_ >= text_.size()) fail("unexpected end of JSON");
        return text_[position_];
    }

    std::string string() {
        skipWhitespace();
        if (position_ >= text_.size() || text_[position_] != '"') {
            fail("expected a JSON string");
        }
        ++position_;
        std::string result;
        while (position_ < text_.size()) {
            const unsigned char current =
                static_cast<unsigned char>(text_[position_++]);
            if (current == static_cast<unsigned char>('"')) return result;
            if (current < 0x20U) fail("unescaped control character in string");
            if (current != static_cast<unsigned char>('\\')) {
                result.push_back(static_cast<char>(current));
                continue;
            }
            if (position_ >= text_.size()) fail("truncated string escape");
            const char escaped = text_[position_++];
            switch (escaped) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': appendUnicodeEscape(result); break;
                default: fail("invalid string escape");
            }
        }
        fail("unterminated JSON string");
    }

    std::int64_t integer() {
        const std::string token = numberToken();
        if (token.find_first_of(".eE") != std::string::npos) {
            fail("expected an integer");
        }
        try {
            std::size_t parsed = 0;
            const long long value = std::stoll(token, &parsed);
            if (parsed != token.size()) fail("invalid integer");
            return static_cast<std::int64_t>(value);
        } catch (const std::exception&) {
            fail("integer is out of range");
        }
    }

    double number() {
        const std::string token = numberToken();
        try {
            std::size_t parsed = 0;
            const double value = std::stod(token, &parsed);
            if (parsed != token.size() || !std::isfinite(value)) {
                fail("invalid finite JSON number");
            }
            return value;
        } catch (const std::exception&) {
            fail("invalid JSON number");
        }
    }

    void literal(const std::string& expected) {
        skipWhitespace();
        if (text_.compare(position_, expected.size(), expected) != 0) {
            fail("expected " + expected);
        }
        position_ += expected.size();
    }

    void skipValue(unsigned depth = 0U) {
        if (depth > 128U) fail("JSON nesting is too deep");
        const char next = peek();
        if (next == '"') {
            static_cast<void>(string());
        } else if (next == '{') {
            expect('{');
            if (consume('}')) return;
            for (;;) {
                static_cast<void>(string());
                expect(':');
                skipValue(depth + 1U);
                if (consume('}')) break;
                expect(',');
            }
        } else if (next == '[') {
            expect('[');
            if (consume(']')) return;
            for (;;) {
                skipValue(depth + 1U);
                if (consume(']')) break;
                expect(',');
            }
        } else if (next == 't') {
            literal("true");
        } else if (next == 'f') {
            literal("false");
        } else if (next == 'n') {
            literal("null");
        } else if (next == '-' || (next >= '0' && next <= '9')) {
            static_cast<void>(numberToken());
        } else {
            fail("invalid JSON value");
        }
    }

    void finish() {
        skipWhitespace();
        if (position_ != text_.size()) fail("trailing content after JSON value");
    }

    [[noreturn]] void fail(const std::string& message) const {
        throw ArtifactError(
            source_ + ": JSON byte " + std::to_string(position_) + ": " + message);
    }

private:
    void skipWhitespace() {
        while (position_ < text_.size()) {
            const char value = text_[position_];
            if (value != ' ' && value != '\n' && value != '\r' && value != '\t') {
                break;
            }
            ++position_;
        }
    }

    std::string numberToken() {
        skipWhitespace();
        const std::size_t start = position_;
        if (position_ < text_.size() && text_[position_] == '-') ++position_;
        if (position_ >= text_.size()) fail("truncated JSON number");
        if (text_[position_] == '0') {
            ++position_;
            if (position_ < text_.size() && text_[position_] >= '0' &&
                text_[position_] <= '9') {
                fail("leading zero in JSON number");
            }
        } else {
            if (text_[position_] < '1' || text_[position_] > '9') {
                fail("invalid JSON number");
            }
            while (position_ < text_.size() && text_[position_] >= '0' &&
                   text_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            const std::size_t fractional_start = position_;
            while (position_ < text_.size() && text_[position_] >= '0' &&
                   text_[position_] <= '9') {
                ++position_;
            }
            if (fractional_start == position_) fail("empty fractional part");
        }
        if (position_ < text_.size() &&
            (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() &&
                (text_[position_] == '+' || text_[position_] == '-')) {
                ++position_;
            }
            const std::size_t exponent_start = position_;
            while (position_ < text_.size() && text_[position_] >= '0' &&
                   text_[position_] <= '9') {
                ++position_;
            }
            if (exponent_start == position_) fail("empty exponent");
        }
        return text_.substr(start, position_ - start);
    }

    unsigned hex4() {
        if (position_ + 4U > text_.size()) fail("truncated Unicode escape");
        unsigned value = 0U;
        for (unsigned i = 0U; i < 4U; ++i) {
            const char digit = text_[position_++];
            value <<= 4U;
            if (digit >= '0' && digit <= '9') {
                value += static_cast<unsigned>(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
                value += static_cast<unsigned>(digit - 'a') + 10U;
            } else if (digit >= 'A' && digit <= 'F') {
                value += static_cast<unsigned>(digit - 'A') + 10U;
            } else {
                fail("non-hexadecimal Unicode escape");
            }
        }
        return value;
    }

    void appendUnicodeEscape(std::string& output) {
        unsigned codepoint = hex4();
        if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
            if (position_ + 2U > text_.size() || text_[position_] != '\\' ||
                text_[position_ + 1U] != 'u') {
                fail("unpaired high Unicode surrogate");
            }
            position_ += 2U;
            const unsigned low = hex4();
            if (low < 0xdc00U || low > 0xdfffU) {
                fail("invalid low Unicode surrogate");
            }
            codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) +
                (low - 0xdc00U);
        } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
            fail("unpaired low Unicode surrogate");
        }
        if (codepoint <= 0x7fU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else if (codepoint <= 0xffffU) {
            output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else {
            output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        }
    }

    const std::string& text_;
    std::string source_;
    std::size_t position_ = 0;
};

void rememberKey(
    JsonReader& reader,
    std::set<std::string>& seen,
    const std::string& key) {
    if (!seen.insert(key).second) reader.fail("duplicate object key " + key);
}

void requireKeys(
    JsonReader& reader,
    const std::set<std::string>& seen,
    std::initializer_list<const char*> required) {
    for (const char* key : required) {
        if (seen.count(key) == 0U) reader.fail("missing object key " + std::string(key));
    }
}

std::size_t nonnegativeSize(JsonReader& reader, const std::string& field) {
    const std::int64_t value = reader.integer();
    if (value < 0) reader.fail(field + " must be nonnegative");
    const auto unsigned_value = static_cast<std::uint64_t>(value);
    if (unsigned_value > std::numeric_limits<std::size_t>::max()) {
        reader.fail(field + " is out of range");
    }
    return static_cast<std::size_t>(unsigned_value);
}

std::uint32_t globalClass(JsonReader& reader, const std::string& field) {
    const std::int64_t value = reader.integer();
    if (value < 0 ||
        static_cast<std::uint64_t>(value) >
            std::numeric_limits<std::uint32_t>::max()) {
        reader.fail(field + " is out of range");
    }
    return static_cast<std::uint32_t>(value);
}

bool validSha256(const std::string& value) {
    return value.size() == 64U &&
        std::all_of(value.begin(), value.end(), [](char digit) {
            return (digit >= '0' && digit <= '9') ||
                (digit >= 'a' && digit <= 'f');
        });
}

struct DigestIdentity {
    std::string algorithm;
    std::string value;
};

DigestIdentity parseDigest(JsonReader& reader) {
    DigestIdentity result;
    std::set<std::string> seen;
    reader.expect('{');
    if (reader.consume('}')) reader.fail("digest object cannot be empty");
    for (;;) {
        const std::string key = reader.string();
        rememberKey(reader, seen, key);
        reader.expect(':');
        if (key == "algorithm") {
            result.algorithm = reader.string();
        } else if (key == "value") {
            result.value = reader.string();
        } else {
            reader.fail("unknown digest key " + key);
        }
        if (reader.consume('}')) break;
        reader.expect(',');
    }
    requireKeys(reader, seen, {"algorithm", "value"});
    if (result.algorithm != "sha256" || !validSha256(result.value)) {
        reader.fail("digest must be a lowercase sha256 identity");
    }
    return result;
}

struct Scheme {
    std::string id;
    std::size_t label_count = 0;
    std::set<std::string> parameter_keys;
};

std::set<std::string> parseParameterObject(JsonReader& reader) {
    std::set<std::string> keys;
    reader.expect('{');
    if (reader.consume('}')) return keys;
    for (;;) {
        const std::string key = reader.string();
        rememberKey(reader, keys, key);
        reader.expect(':');
        reader.skipValue();
        if (reader.consume('}')) break;
        reader.expect(',');
    }
    return keys;
}

Scheme parseScheme(JsonReader& reader) {
    Scheme result;
    std::set<std::string> seen;
    reader.expect('{');
    if (reader.consume('}')) reader.fail("scheme object cannot be empty");
    for (;;) {
        const std::string key = reader.string();
        rememberKey(reader, seen, key);
        reader.expect(':');
        if (key == "id") {
            result.id = reader.string();
        } else if (key == "label_count") {
            result.label_count = nonnegativeSize(reader, key);
        } else if (key == "parameters") {
            result.parameter_keys = parseParameterObject(reader);
        } else if (key == "seed") {
            const std::size_t seed = nonnegativeSize(reader, key);
            if (seed > std::numeric_limits<std::uint32_t>::max()) {
                reader.fail("scheme seed exceeds uint32");
            }
        } else {
            reader.fail("unknown scheme key " + key);
        }
        if (reader.consume('}')) break;
        reader.expect(',');
    }
    requireKeys(reader, seen, {"id", "label_count", "parameters", "seed"});
    const std::set<std::string> stable_parameters = {
        "assignment", "control_role", "vertex_set_invariant"};
    const std::set<std::string> degree_parameters = {
        "apportionment", "class_order", "degree_basis", "rank_order",
        "target_class_weights", "weight_family", "zipf_exponent"};
    if (result.id == "stable-id-splitmix64-v1") {
        if (result.parameter_keys != stable_parameters) {
            reader.fail("unexpected stable-id scheme parameters");
        }
    } else if (result.id == "degree-correlated-zipf12-rank-block-v1") {
        if (result.parameter_keys != degree_parameters) {
            reader.fail("unexpected degree-correlated scheme parameters");
        }
    } else {
        reader.fail("unsupported label-map scheme " + result.id);
    }
    return result;
}

struct GlobalArtifact {
    std::vector<std::pair<int, std::uint32_t>> mapping;
    std::vector<std::pair<std::uint32_t, std::size_t>> declared_classes;
    std::string embedded_mapping_sha256;
    std::size_t vertex_count = 0;
    std::size_t label_count = 0;
    double normalized_entropy = 0.0;
};

std::vector<std::pair<std::uint32_t, std::size_t>> parseClasses(
    JsonReader& reader) {
    std::vector<std::pair<std::uint32_t, std::size_t>> result;
    reader.expect('[');
    if (reader.consume(']')) return result;
    for (;;) {
        std::uint32_t class_id = 0;
        std::size_t count = 0;
        std::set<std::string> seen;
        reader.expect('{');
        if (reader.consume('}')) reader.fail("class row cannot be empty");
        for (;;) {
            const std::string key = reader.string();
            rememberKey(reader, seen, key);
            reader.expect(':');
            if (key == "global_class_id") {
                class_id = globalClass(reader, key);
            } else if (key == "vertex_count") {
                count = nonnegativeSize(reader, key);
            } else {
                reader.fail("unknown class-row key " + key);
            }
            if (reader.consume('}')) break;
            reader.expect(',');
        }
        requireKeys(reader, seen, {"global_class_id", "vertex_count"});
        result.emplace_back(class_id, count);
        if (reader.consume(']')) break;
        reader.expect(',');
    }
    return result;
}

std::vector<std::pair<int, std::uint32_t>> parseGlobalMapping(
    JsonReader& reader) {
    std::vector<std::pair<int, std::uint32_t>> result;
    reader.expect('[');
    if (reader.consume(']')) return result;
    int previous_vertex = -1;
    for (;;) {
        reader.expect('[');
        const std::int64_t vertex = reader.integer();
        reader.expect(',');
        const std::uint32_t class_id = globalClass(reader, "mapping class ID");
        reader.expect(']');
        if (vertex < 0 || vertex > std::numeric_limits<int>::max()) {
            reader.fail("mapping vertex ID is outside signed nonnegative int32");
        }
        const int external_id = static_cast<int>(vertex);
        if (external_id <= previous_vertex) {
            reader.fail("mapping vertex IDs must be strictly increasing");
        }
        previous_vertex = external_id;
        result.emplace_back(external_id, class_id);
        if (reader.consume(']')) break;
        reader.expect(',');
    }
    return result;
}

GlobalArtifact parseGlobalArtifact(const FileContents& file, const std::string& source) {
    JsonReader reader(file.bytes, source);
    GlobalArtifact result;
    std::string artifact_kind;
    int schema_version = 0;
    Scheme scheme;
    DigestIdentity mapping_digest;
    std::set<std::string> seen;
    reader.expect('{');
    if (reader.consume('}')) reader.fail("global label map cannot be empty");
    for (;;) {
        const std::string key = reader.string();
        rememberKey(reader, seen, key);
        reader.expect(':');
        if (key == "artifact_kind") {
            artifact_kind = reader.string();
        } else if (key == "classes") {
            result.declared_classes = parseClasses(reader);
        } else if (key == "mapping") {
            result.mapping = parseGlobalMapping(reader);
        } else if (key == "mapping_digest") {
            mapping_digest = parseDigest(reader);
        } else if (key == "normalized_entropy") {
            result.normalized_entropy = reader.number();
        } else if (key == "schema_version") {
            schema_version = static_cast<int>(reader.integer());
        } else if (key == "scheme") {
            scheme = parseScheme(reader);
        } else if (key == "vertex_count") {
            result.vertex_count = nonnegativeSize(reader, key);
        } else {
            reader.fail("unknown global-label-map key " + key);
        }
        if (reader.consume('}')) break;
        reader.expect(',');
    }
    reader.finish();
    requireKeys(
        reader,
        seen,
        {"artifact_kind", "classes", "mapping", "mapping_digest",
         "normalized_entropy", "schema_version", "scheme", "vertex_count"});
    if (artifact_kind != "dsm-explicit-vertex-label-map" || schema_version != 1) {
        reader.fail("unsupported global label-map kind or schema version");
    }
    if (scheme.label_count != 5U && scheme.label_count != 20U &&
        scheme.label_count != 100U) {
        reader.fail("global label cardinality must be 5, 20, or 100");
    }
    result.label_count = scheme.label_count;
    result.embedded_mapping_sha256 = mapping_digest.value;
    if (result.vertex_count == 0U || result.mapping.size() != result.vertex_count) {
        reader.fail("vertex_count does not match the nonempty mapping");
    }
    if (result.declared_classes.size() != result.label_count) {
        reader.fail("classes must contain exactly one row per global class");
    }

    std::vector<std::size_t> actual_counts(result.label_count, 0U);
    for (const auto& row : result.mapping) {
        if (row.second >= result.label_count) {
            reader.fail("mapping contains an out-of-range global class");
        }
        ++actual_counts[row.second];
    }
    std::vector<std::size_t> declared_counts(result.label_count, 0U);
    std::vector<bool> class_seen(result.label_count, false);
    for (const auto& row : result.declared_classes) {
        if (row.first >= result.label_count || class_seen[row.first]) {
            reader.fail("classes contains a duplicate or out-of-range class");
        }
        class_seen[row.first] = true;
        declared_counts[row.first] = row.second;
    }
    if (actual_counts != declared_counts) {
        reader.fail("declared class counts do not match mapping rows");
    }
    if (result.normalized_entropy < 0.0 || result.normalized_entropy > 1.0) {
        reader.fail("normalized_entropy must lie in [0,1]");
    }
    double entropy = 0.0;
    for (std::size_t count : actual_counts) {
        if (count != 0U) {
            const double probability =
                static_cast<double>(count) / static_cast<double>(result.vertex_count);
            entropy -= probability * std::log(probability);
        }
    }
    entropy /= std::log(static_cast<double>(result.label_count));
    if (std::abs(entropy - result.normalized_entropy) > 1e-12) {
        reader.fail("normalized_entropy does not match mapping rows");
    }

    Sha256 digest;
    digest.update(
        "dsm-explicit-label-map-v1\t" + std::to_string(result.label_count) + "\n");
    for (const auto& row : result.mapping) {
        digest.update(
            std::to_string(row.first) + "\t" + std::to_string(row.second) + "\n");
    }
    const std::string computed = digest.finishHex();
    if (computed != result.embedded_mapping_sha256) {
        reader.fail("mapping_digest does not match the canonical mapping");
    }
    return result;
}

struct LocalSlot {
    std::uint32_t global_class = 0;
    std::size_t runtime_class = 0;
    std::string runtime_label;
};

std::vector<std::uint32_t> parseSelectedClasses(JsonReader& reader) {
    std::vector<std::uint32_t> result;
    reader.expect('[');
    if (reader.consume(']')) return result;
    for (;;) {
        result.push_back(globalClass(reader, "selected global class"));
        if (reader.consume(']')) break;
        reader.expect(',');
    }
    return result;
}

std::vector<LocalSlot> parseLocalSlots(JsonReader& reader) {
    std::vector<LocalSlot> result;
    reader.expect('[');
    if (reader.consume(']')) return result;
    for (;;) {
        LocalSlot slot;
        std::set<std::string> seen;
        reader.expect('{');
        if (reader.consume('}')) reader.fail("local slot cannot be empty");
        for (;;) {
            const std::string key = reader.string();
            rememberKey(reader, seen, key);
            reader.expect(':');
            if (key == "global_class_id") {
                slot.global_class = globalClass(reader, key);
            } else if (key == "runtime_class_id") {
                slot.runtime_class = nonnegativeSize(reader, key);
            } else if (key == "runtime_label") {
                slot.runtime_label = reader.string();
            } else {
                reader.fail("unknown local-slot key " + key);
            }
            if (reader.consume('}')) break;
            reader.expect(',');
        }
        requireKeys(reader, seen, {"global_class_id", "runtime_class_id", "runtime_label"});
        result.push_back(std::move(slot));
        if (reader.consume(']')) break;
        reader.expect(',');
    }
    return result;
}

struct ProjectionArtifact {
    std::string global_file_sha256;
    std::string global_mapping_sha256;
    std::string query_sha256;
    std::size_t global_label_cardinality = 0;
    std::vector<std::uint32_t> selected_classes;
};

ProjectionArtifact parseProjectionArtifact(
    const FileContents& file,
    const std::string& source) {
    JsonReader reader(file.bytes, source);
    ProjectionArtifact result;
    std::string artifact_kind;
    int schema_version = 0;
    std::size_t runtime_slot_count = 0;
    std::string nonselected_policy;
    int invalid_runtime_class_id = 0;
    DigestIdentity global_mapping_digest;
    std::vector<LocalSlot> local_slots;
    std::set<std::string> seen;
    reader.expect('{');
    if (reader.consume('}')) reader.fail("query label projection cannot be empty");
    for (;;) {
        const std::string key = reader.string();
        rememberKey(reader, seen, key);
        reader.expect(':');
        if (key == "artifact_kind") {
            artifact_kind = reader.string();
        } else if (key == "global_label_cardinality") {
            result.global_label_cardinality = nonnegativeSize(reader, key);
        } else if (key == "global_label_map_file_sha256") {
            result.global_file_sha256 = reader.string();
        } else if (key == "global_label_map_mapping_digest") {
            global_mapping_digest = parseDigest(reader);
        } else if (key == "invalid_runtime_class_id") {
            invalid_runtime_class_id = static_cast<int>(reader.integer());
        } else if (key == "local_slots") {
            local_slots = parseLocalSlots(reader);
        } else if (key == "nonselected_global_class_policy") {
            nonselected_policy = reader.string();
        } else if (key == "query_sha256") {
            result.query_sha256 = reader.string();
        } else if (key == "runtime_slot_count") {
            runtime_slot_count = nonnegativeSize(reader, key);
        } else if (key == "schema_version") {
            schema_version = static_cast<int>(reader.integer());
        } else if (key == "selected_global_classes_in_A_to_E_order") {
            result.selected_classes = parseSelectedClasses(reader);
        } else {
            reader.fail("unknown query-label-projection key " + key);
        }
        if (reader.consume('}')) break;
        reader.expect(',');
    }
    reader.finish();
    requireKeys(
        reader,
        seen,
        {"artifact_kind", "global_label_cardinality",
         "global_label_map_file_sha256", "global_label_map_mapping_digest",
         "invalid_runtime_class_id", "local_slots",
         "nonselected_global_class_policy", "query_sha256", "runtime_slot_count",
         "schema_version", "selected_global_classes_in_A_to_E_order"});
    if (artifact_kind != "dsm-query-label-projection" || schema_version != 1) {
        reader.fail("unsupported query label-projection kind or schema version");
    }
    if (!validSha256(result.global_file_sha256) ||
        !validSha256(result.query_sha256)) {
        reader.fail("file identities must be lowercase sha256 values");
    }
    result.global_mapping_sha256 = global_mapping_digest.value;
    if (result.global_label_cardinality != 5U &&
        result.global_label_cardinality != 20U &&
        result.global_label_cardinality != 100U) {
        reader.fail("global_label_cardinality must be 5, 20, or 100");
    }
    if (runtime_slot_count != kLabelCount || invalid_runtime_class_id != -1 ||
        nonselected_policy != "invalid") {
        reader.fail("projection must use five runtime slots and invalid nonselected classes");
    }
    if (result.selected_classes.empty() ||
        result.selected_classes.size() > kLabelCount) {
        reader.fail("projection must select between one and five global classes");
    }
    std::set<std::uint32_t> unique_classes;
    for (std::uint32_t selected : result.selected_classes) {
        if (selected >= result.global_label_cardinality ||
            !unique_classes.insert(selected).second) {
            reader.fail("selected global classes must be unique and in range");
        }
    }
    if (local_slots.size() != result.selected_classes.size()) {
        reader.fail("local_slots must have one row per selected global class");
    }
    for (std::size_t index = 0; index < local_slots.size(); ++index) {
        const LocalSlot& slot = local_slots[index];
        const std::string expected_label(
            1, static_cast<char>('A' + static_cast<int>(index)));
        if (slot.runtime_class != index || slot.runtime_label != expected_label ||
            slot.global_class != result.selected_classes[index]) {
            reader.fail("local_slots is not the declared A-to-E bijection");
        }
    }
    return result;
}

} // namespace

bool loadExplicitRuntimeLabelMap(
    const std::string& global_label_map_file,
    const std::string& query_label_projection_file,
    const std::string& query_file,
    ExplicitRuntimeLabelMap& result,
    std::string& error) {
    try {
        const FileContents global_file = readAndHash(global_label_map_file);
        const FileContents projection_file =
            readAndHash(query_label_projection_file);
        const FileContents query_contents = readAndHash(query_file);
        const GlobalArtifact global =
            parseGlobalArtifact(global_file, global_label_map_file);
        const ProjectionArtifact projection =
            parseProjectionArtifact(projection_file, query_label_projection_file);

        if (projection.global_file_sha256 != global_file.sha256) {
            throw ArtifactError(
                "query label projection names a different global label-map file sha256");
        }
        if (projection.global_mapping_sha256 != global.embedded_mapping_sha256) {
            throw ArtifactError(
                "query label projection names a different global mapping sha256");
        }
        if (projection.global_label_cardinality != global.label_count) {
            throw ArtifactError(
                "query label projection and global map disagree on label cardinality");
        }
        if (projection.query_sha256 != query_contents.sha256) {
            throw ArtifactError(
                "query label projection names a different query file sha256");
        }

        ExplicitRuntimeLabelMap loaded;
        loaded.selected_global_classes = projection.selected_classes;
        loaded.global_label_cardinality = global.label_count;
        loaded.global_label_map_file_sha256 = global_file.sha256;
        loaded.global_label_map_mapping_sha256 = global.embedded_mapping_sha256;
        loaded.query_label_projection_file_sha256 = projection_file.sha256;
        loaded.query_file_sha256 = query_contents.sha256;
        loaded.labels_by_external_id.reserve(global.mapping.size());
        for (const auto& row : global.mapping) {
            Label runtime_label = kInvalidLabel;
            for (std::size_t slot = 0;
                 slot < projection.selected_classes.size();
                 ++slot) {
                if (row.second == projection.selected_classes[slot]) {
                    runtime_label = static_cast<Label>(slot);
                    ++loaded.runtime_class_counts[slot];
                    break;
                }
            }
            if (runtime_label == kInvalidLabel) ++loaded.invalid_vertex_count;
            loaded.labels_by_external_id.emplace_back(row.first, runtime_label);
        }
        result = std::move(loaded);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        result = ExplicitRuntimeLabelMap{};
        error = exception.what();
        return false;
    }
}
