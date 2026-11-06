#include "E3E4AdapterCommonV1.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <variant>

namespace dsm::e3e4::adapter_v1 {
namespace {

[[noreturn]] void fail(ErrorCode code, const std::string& message) {
    throw AdapterError(code, message);
}

std::string errnoMessage(const std::string& context) {
    return context + ": " + std::strerror(errno);
}

bool isLowerSha256(std::string_view value) {
    if (value.size() != 64U) return false;
    for (const char ch : value) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::uint32_t rotateRight(std::uint32_t value, unsigned shift) {
    return (value >> shift) | (value << (32U - shift));
}

class Sha256 final {
public:
    void update(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t index = 0; index < size; ++index) {
            block_[block_size_++] = bytes[index];
            ++total_bytes_;
            if (block_size_ == block_.size()) {
                transform(block_.data());
                block_size_ = 0;
            }
        }
    }

    void update(std::string_view value) { update(value.data(), value.size()); }

    std::string finishHex() {
        if (finished_) fail(ErrorCode::kIntegerOverflow, "sha256 finalized twice");
        finished_ = true;
        if (total_bytes_ > std::numeric_limits<std::uint64_t>::max() / 8U) {
            fail(ErrorCode::kIntegerOverflow, "sha256 byte count overflow");
        }
        const std::uint64_t total_bits = total_bytes_ * 8U;
        block_[block_size_++] = 0x80U;
        if (block_size_ > 56U) {
            while (block_size_ < block_.size()) block_[block_size_++] = 0U;
            transform(block_.data());
            block_size_ = 0;
        }
        while (block_size_ < 56U) block_[block_size_++] = 0U;
        for (int shift = 56; shift >= 0; shift -= 8) {
            block_[block_size_++] = static_cast<std::uint8_t>(
                (total_bits >> static_cast<unsigned>(shift)) & 0xffU);
        }
        transform(block_.data());
        block_size_ = 0;

        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const std::uint32_t value : state_) {
            output << std::setw(8) << value;
        }
        return output.str();
    }

private:
    void transform(const std::uint8_t* block) {
        static constexpr std::array<std::uint32_t, 64> constants{{
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
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
        }};
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16U; ++index) {
            const std::size_t offset = index * 4U;
            words[index] =
                (static_cast<std::uint32_t>(block[offset]) << 24U) |
                (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                static_cast<std::uint32_t>(block[offset + 3U]);
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            const std::uint32_t lhs = words[index - 15U];
            const std::uint32_t rhs = words[index - 2U];
            const std::uint32_t sigma0 =
                rotateRight(lhs, 7U) ^ rotateRight(lhs, 18U) ^ (lhs >> 3U);
            const std::uint32_t sigma1 =
                rotateRight(rhs, 17U) ^ rotateRight(rhs, 19U) ^ (rhs >> 10U);
            words[index] =
                words[index - 16U] + sigma0 + words[index - 7U] + sigma1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const std::uint32_t sum1 =
                rotateRight(e, 6U) ^ rotateRight(e, 11U) ^ rotateRight(e, 25U);
            const std::uint32_t choose = (e & f) ^ ((~e) & g);
            const std::uint32_t temporary1 =
                h + sum1 + choose + constants[index] + words[index];
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
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    }};
    std::array<std::uint8_t, 64> block_{};
    std::size_t block_size_ = 0;
    std::uint64_t total_bytes_ = 0;
    bool finished_ = false;
};

struct JsonValue final {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;
    using Storage = std::variant<
        std::nullptr_t,
        bool,
        std::int64_t,
        std::uint64_t,
        double,
        std::string,
        Array,
        Object>;

    Storage value = nullptr;

    JsonValue() = default;
    JsonValue(std::nullptr_t) : value(nullptr) {}
    JsonValue(bool input) : value(input) {}
    JsonValue(std::int64_t input) : value(input) {}
    JsonValue(std::uint64_t input) : value(input) {}
    JsonValue(double input) : value(input) {}
    JsonValue(std::string input) : value(std::move(input)) {}
    JsonValue(const char* input) : value(std::string(input)) {}
    JsonValue(Array input) : value(std::move(input)) {}
    JsonValue(Object input) : value(std::move(input)) {}
};

void appendUtf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7fU) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0x10ffffU) {
        output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        fail(ErrorCode::kInvalidUtf8, "Unicode codepoint out of range");
    }
}

bool validUtf8(std::string_view input) {
    std::size_t index = 0;
    while (index < input.size()) {
        const auto first = static_cast<unsigned char>(input[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        std::size_t length = 0;
        std::uint32_t codepoint = 0;
        if (first >= 0xc2U && first <= 0xdfU) {
            length = 2;
            codepoint = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            length = 3;
            codepoint = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            length = 4;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (index + length > input.size()) return false;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(input[index + offset]);
            if ((next & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        if ((length == 3U && codepoint < 0x800U) ||
            (length == 4U && codepoint < 0x10000U) ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
            codepoint > 0x10ffffU) {
            return false;
        }
        index += length;
    }
    return true;
}

class JsonParser final {
public:
    explicit JsonParser(std::string_view text) : text_(text) {
        if (!validUtf8(text_)) fail(ErrorCode::kInvalidUtf8, "invalid UTF-8 JSON");
    }

    JsonValue parse() {
        JsonValue result = value();
        whitespace();
        if (position_ != text_.size()) error("trailing JSON data");
        return result;
    }

private:
    [[noreturn]] void error(const std::string& message) const {
        fail(
            ErrorCode::kInvalidJson,
            message + " at JSON byte " + std::to_string(position_));
    }

    void whitespace() {
        while (position_ < text_.size()) {
            const char ch = text_[position_];
            if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
                ++position_;
            } else {
                break;
            }
        }
    }

    bool consume(char expected) {
        whitespace();
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect(char expected) {
        if (!consume(expected)) {
            error(std::string("expected '") + expected + "'");
        }
    }

    JsonValue value() {
        whitespace();
        if (position_ >= text_.size()) error("unexpected end of JSON");
        switch (text_[position_]) {
            case '{': return object();
            case '[': return array();
            case '"': return JsonValue(string());
            case 't': literal("true"); return JsonValue(true);
            case 'f': literal("false"); return JsonValue(false);
            case 'n': literal("null"); return JsonValue(nullptr);
            default: return number();
        }
    }

    void literal(std::string_view expected) {
        if (text_.substr(position_, expected.size()) != expected) {
            error("invalid JSON literal");
        }
        position_ += expected.size();
    }

    std::uint32_t hex4() {
        if (position_ + 4U > text_.size()) error("truncated Unicode escape");
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4U; ++index) {
            const char ch = text_[position_++];
            value <<= 4U;
            if (ch >= '0' && ch <= '9') value |= static_cast<unsigned>(ch - '0');
            else if (ch >= 'a' && ch <= 'f') value |= 10U + static_cast<unsigned>(ch - 'a');
            else if (ch >= 'A' && ch <= 'F') value |= 10U + static_cast<unsigned>(ch - 'A');
            else error("invalid Unicode escape");
        }
        return value;
    }

    std::string string() {
        whitespace();
        if (position_ >= text_.size() || text_[position_] != '"') {
            error("expected JSON string");
        }
        ++position_;
        std::string output;
        while (position_ < text_.size()) {
            const auto ch = static_cast<unsigned char>(text_[position_++]);
            if (ch == '"') return output;
            if (ch < 0x20U) error("unescaped control byte in string");
            if (ch != '\\') {
                output.push_back(static_cast<char>(ch));
                continue;
            }
            if (position_ >= text_.size()) error("truncated escape");
            const char escaped = text_[position_++];
            switch (escaped) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    std::uint32_t codepoint = hex4();
                    if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                        if (position_ + 2U > text_.size() ||
                            text_[position_] != '\\' || text_[position_ + 1U] != 'u') {
                            error("missing low surrogate");
                        }
                        position_ += 2U;
                        const std::uint32_t low = hex4();
                        if (low < 0xdc00U || low > 0xdfffU) error("invalid low surrogate");
                        codepoint = 0x10000U +
                            ((codepoint - 0xd800U) << 10U) + (low - 0xdc00U);
                    } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
                        error("unpaired low surrogate");
                    }
                    appendUtf8(output, codepoint);
                    break;
                }
                default: error("invalid string escape");
            }
        }
        error("unterminated JSON string");
    }

    JsonValue number() {
        whitespace();
        const std::size_t begin = position_;
        if (position_ < text_.size() && text_[position_] == '-') ++position_;
        if (position_ >= text_.size()) error("truncated number");
        if (text_[position_] == '0') {
            ++position_;
            if (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                error("leading zero in number");
            }
        } else if (text_[position_] >= '1' && text_[position_] <= '9') {
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
            }
        } else {
            error("invalid number");
        }
        bool floating = false;
        if (position_ < text_.size() && text_[position_] == '.') {
            floating = true;
            ++position_;
            const std::size_t fraction = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
            }
            if (position_ == fraction) error("empty fractional part");
        }
        if (position_ < text_.size() &&
            (text_[position_] == 'e' || text_[position_] == 'E')) {
            floating = true;
            ++position_;
            if (position_ < text_.size() &&
                (text_[position_] == '+' || text_[position_] == '-')) {
                ++position_;
            }
            const std::size_t exponent = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
            }
            if (position_ == exponent) error("empty exponent");
        }
        const std::string_view token = text_.substr(begin, position_ - begin);
        if (!floating) {
            if (!token.empty() && token.front() == '-') {
                std::int64_t parsed = 0;
                const auto result = std::from_chars(
                    token.data(), token.data() + token.size(), parsed);
                if (result.ec != std::errc{} ||
                    result.ptr != token.data() + token.size()) {
                    error("negative integer outside int64");
                }
                return JsonValue(parsed);
            }
            std::uint64_t parsed = 0;
            const auto result = std::from_chars(
                token.data(), token.data() + token.size(), parsed);
            if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
                error("nonnegative integer outside uint64");
            }
            return JsonValue(parsed);
        }
        double parsed = 0.0;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size() ||
            !std::isfinite(parsed)) {
            error("invalid finite binary64 number");
        }
        return JsonValue(parsed);
    }

    JsonValue array() {
        expect('[');
        JsonValue::Array output;
        if (consume(']')) return JsonValue(std::move(output));
        for (;;) {
            output.push_back(value());
            if (consume(']')) break;
            expect(',');
        }
        return JsonValue(std::move(output));
    }

    JsonValue object() {
        expect('{');
        JsonValue::Object output;
        if (consume('}')) return JsonValue(std::move(output));
        for (;;) {
            const std::string key = string();
            expect(':');
            JsonValue parsed = value();
            if (!output.emplace(key, std::move(parsed)).second) {
                error("duplicate object key " + key);
            }
            if (consume('}')) break;
            expect(',');
        }
        return JsonValue(std::move(output));
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

void appendJsonString(std::string& output, std::string_view input) {
    output.push_back('"');
    static constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char ch : input) {
        switch (ch) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (ch < 0x20U) {
                    output += "\\u00";
                    output.push_back(hex[(ch >> 4U) & 0x0fU]);
                    output.push_back(hex[ch & 0x0fU]);
                } else {
                    output.push_back(static_cast<char>(ch));
                }
        }
    }
    output.push_back('"');
}

void appendCanonicalJson(std::string& output, const JsonValue& value) {
    if (std::holds_alternative<std::nullptr_t>(value.value)) {
        output += "null";
    } else if (const auto* boolean = std::get_if<bool>(&value.value)) {
        output += *boolean ? "true" : "false";
    } else if (const auto* integer = std::get_if<std::int64_t>(&value.value)) {
        output += std::to_string(*integer);
    } else if (const auto* unsigned_integer =
                   std::get_if<std::uint64_t>(&value.value)) {
        output += std::to_string(*unsigned_integer);
    } else if (const auto* number = std::get_if<double>(&value.value)) {
        if (!std::isfinite(*number)) fail(ErrorCode::kInvalidJson, "nonfinite JSON");
        std::array<char, 64> buffer{};
        const auto result = std::to_chars(
            buffer.data(), buffer.data() + buffer.size(), *number,
            std::chars_format::general,
            std::numeric_limits<double>::max_digits10);
        if (result.ec != std::errc{}) fail(ErrorCode::kInvalidJson, "binary64 serialization failed");
        output.append(buffer.data(), result.ptr);
    } else if (const auto* string = std::get_if<std::string>(&value.value)) {
        appendJsonString(output, *string);
    } else if (const auto* array = std::get_if<JsonValue::Array>(&value.value)) {
        output.push_back('[');
        for (std::size_t index = 0; index < array->size(); ++index) {
            if (index != 0U) output.push_back(',');
            appendCanonicalJson(output, (*array)[index]);
        }
        output.push_back(']');
    } else {
        const auto& object = std::get<JsonValue::Object>(value.value);
        output.push_back('{');
        bool first = true;
        for (const auto& [key, child] : object) {
            if (!first) output.push_back(',');
            first = false;
            appendJsonString(output, key);
            output.push_back(':');
            appendCanonicalJson(output, child);
        }
        output.push_back('}');
    }
}

std::string canonicalJson(const JsonValue& value) {
    std::string output;
    appendCanonicalJson(output, value);
    return output;
}

const JsonValue::Object& asObject(const JsonValue& value, std::string_view context) {
    const auto* result = std::get_if<JsonValue::Object>(&value.value);
    if (result == nullptr) {
        fail(ErrorCode::kSchemaMismatch, std::string(context) + " must be object");
    }
    return *result;
}

const JsonValue::Array& asArray(const JsonValue& value, std::string_view context) {
    const auto* result = std::get_if<JsonValue::Array>(&value.value);
    if (result == nullptr) {
        fail(ErrorCode::kSchemaMismatch, std::string(context) + " must be array");
    }
    return *result;
}

const std::string& asString(const JsonValue& value, std::string_view context) {
    const auto* result = std::get_if<std::string>(&value.value);
    if (result == nullptr) {
        fail(ErrorCode::kSchemaMismatch, std::string(context) + " must be string");
    }
    return *result;
}

std::int64_t asInteger(const JsonValue& value, const std::string& context) {
    if (const auto* signed_value = std::get_if<std::int64_t>(&value.value)) {
        return *signed_value;
    }
    if (const auto* unsigned_value = std::get_if<std::uint64_t>(&value.value)) {
        if (*unsigned_value <= static_cast<std::uint64_t>(
                                   std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(*unsigned_value);
        }
    }
    fail(ErrorCode::kSchemaMismatch, context + " must fit signed integer-not-bool");
}

bool asBoolean(const JsonValue& value, const std::string& context) {
    const auto* result = std::get_if<bool>(&value.value);
    if (result == nullptr) fail(ErrorCode::kSchemaMismatch, context + " must be bool");
    return *result;
}

double asNumber(const JsonValue& value, const std::string& context) {
    if (const auto* floating = std::get_if<double>(&value.value)) return *floating;
    if (const auto* integer = std::get_if<std::int64_t>(&value.value)) {
        return static_cast<double>(*integer);
    }
    if (const auto* integer = std::get_if<std::uint64_t>(&value.value)) {
        return static_cast<double>(*integer);
    }
    fail(ErrorCode::kSchemaMismatch, context + " must be number");
}

const JsonValue& member(
    const JsonValue::Object& object,
    std::string_view key,
    std::string_view context) {
    const auto iterator = object.find(std::string(key));
    if (iterator == object.end()) {
        fail(
            ErrorCode::kSchemaMismatch,
            std::string(context) + " missing " + std::string(key));
    }
    return iterator->second;
}

void exactKeys(
    const JsonValue::Object& object,
    std::initializer_list<std::string_view> expected,
    const std::string& context) {
    std::set<std::string> required;
    for (const std::string_view key : expected) required.emplace(key);
    if (object.size() != required.size()) {
        fail(ErrorCode::kSchemaMismatch, context + " key count mismatch");
    }
    for (const auto& [key, ignored] : object) {
        static_cast<void>(ignored);
        if (required.erase(key) != 1U) {
            fail(ErrorCode::kSchemaMismatch, context + " unknown key " + key);
        }
    }
    if (!required.empty()) fail(ErrorCode::kSchemaMismatch, context + " missing keys");
}

std::uint64_t nonnegativeUint64(const JsonValue& value, const std::string& context) {
    if (const auto* unsigned_value = std::get_if<std::uint64_t>(&value.value)) {
        return *unsigned_value;
    }
    if (const auto* signed_value = std::get_if<std::int64_t>(&value.value)) {
        if (*signed_value < 0) {
            fail(ErrorCode::kSchemaMismatch, context + " must be nonnegative");
        }
        return static_cast<std::uint64_t>(*signed_value);
    }
    fail(ErrorCode::kSchemaMismatch, context + " must be uint64-not-bool");
}

std::uint32_t boundedUint32(
    const JsonValue& value,
    std::uint32_t maximum,
    const std::string& context) {
    const std::uint64_t input = nonnegativeUint64(value, context);
    if (input > maximum) fail(ErrorCode::kSchemaMismatch, context + " out of range");
    return static_cast<std::uint32_t>(input);
}

std::uint64_t checkedAdd(std::uint64_t lhs, std::uint64_t rhs, const char* context) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        fail(ErrorCode::kIntegerOverflow, std::string(context) + " uint64 overflow");
    }
    return lhs + rhs;
}

}  // namespace

AdapterError::AdapterError(ErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

ErrorCode AdapterError::code() const noexcept { return code_; }

std::string sha256Hex(std::string_view bytes) {
    Sha256 digest;
    digest.update(bytes);
    return digest.finishHex();
}

}  // namespace dsm::e3e4::adapter_v1

namespace dsm::e3e4::adapter_v1 {
namespace {

std::size_t phaseIndex(CommonPhase phase) {
    switch (phase) {
        case CommonPhase::kPlanning: return 0U;
        case CommonPhase::kTdConstruction: return 1U;
        case CommonPhase::kDagCompilation: return 2U;
        case CommonPhase::kEnumeration: return 3U;
        case CommonPhase::kOutputFinalization: return 4U;
    }
    fail(ErrorCode::kInvalidPhaseLedger, "invalid common phase");
}

void validateNativeName(std::string_view name) {
    if (name.empty() || name.find('\0') != std::string_view::npos ||
        !validUtf8(name)) {
        fail(ErrorCode::kInvalidPhaseLedger, "native interval name is invalid");
    }
}

void validateApplicability(ArmId arm, CommonPhase phase, bool applicable) {
    static_cast<void>(phaseIndex(phase));
    const bool expected =
        phase == CommonPhase::kDagCompilation
            ? arm == ArmId::kFinalDurableDag
            : true;
    if (applicable != expected) {
        fail(ErrorCode::kInvalidPhaseLedger, "common-phase applicability drift");
    }
}

int advancePhaseRegion(
    ArmId arm,
    int current_region,
    CommonPhase next_phase,
    ErrorCode error_code) {
    if (arm == ArmId::kCeciDurReimpl) {
        switch (current_region) {
            case -1:
                if (next_phase == CommonPhase::kPlanning) return 0;
                break;
            case 0:
                if (next_phase == CommonPhase::kPlanning) return 0;
                if (next_phase == CommonPhase::kTdConstruction) return 1;
                break;
            case 1:
                if (next_phase == CommonPhase::kTdConstruction) return 1;
                if (next_phase == CommonPhase::kDagCompilation) return 2;
                if (next_phase == CommonPhase::kEnumeration) return 3;
                break;
            case 2:
                if (next_phase == CommonPhase::kDagCompilation) return 2;
                if (next_phase == CommonPhase::kEnumeration) return 3;
                break;
            case 3:
                if (next_phase == CommonPhase::kEnumeration) return 3;
                if (next_phase == CommonPhase::kPlanning) return 0;
                if (next_phase == CommonPhase::kOutputFinalization) return 4;
                break;
            case 4:
                if (next_phase == CommonPhase::kOutputFinalization) return 4;
                break;
        }
        fail(error_code, "CECI snapshot phase order drift");
    }
    switch (current_region) {
        case -1:
            if (next_phase == CommonPhase::kPlanning) return 0;
            break;
        case 0:
            if (next_phase == CommonPhase::kPlanning ||
                next_phase == CommonPhase::kTdConstruction) {
                return 0;
            }
            if (next_phase == CommonPhase::kDagCompilation) return 1;
            break;
        case 1:
            if (next_phase == CommonPhase::kDagCompilation) return 1;
            if (next_phase == CommonPhase::kEnumeration) return 2;
            break;
        case 2:
            if (next_phase == CommonPhase::kEnumeration) return 2;
            if (next_phase == CommonPhase::kOutputFinalization) return 3;
            break;
        case 3:
            if (next_phase == CommonPhase::kOutputFinalization) return 3;
            break;
    }
    fail(error_code, "native interval common-phase order drift");
}

void addPhaseDuration(
    PhaseTotals& totals,
    CommonPhase phase,
    std::uint64_t duration) {
    switch (phase) {
        case CommonPhase::kPlanning:
            totals.planning_ns =
                checkedAdd(totals.planning_ns, duration, "planning total");
            return;
        case CommonPhase::kTdConstruction:
            totals.td_construction_ns = checkedAdd(
                totals.td_construction_ns, duration, "TD-construction total");
            return;
        case CommonPhase::kDagCompilation:
            totals.dag_compilation_ns = checkedAdd(
                totals.dag_compilation_ns, duration, "DAG-compilation total");
            return;
        case CommonPhase::kEnumeration:
            totals.enumeration_ns = checkedAdd(
                totals.enumeration_ns, duration, "enumeration total");
            return;
        case CommonPhase::kOutputFinalization:
            totals.output_finalization_ns = checkedAdd(
                totals.output_finalization_ns, duration,
                "output-finalization total");
            return;
    }
    fail(ErrorCode::kInvalidPhaseLedger, "invalid phase total target");
}

std::uint64_t sumPhaseTotals(const PhaseTotals& totals) {
    std::uint64_t sum = 0;
    sum = checkedAdd(sum, totals.planning_ns, "phase sum");
    sum = checkedAdd(sum, totals.td_construction_ns, "phase sum");
    sum = checkedAdd(sum, totals.dag_compilation_ns, "phase sum");
    sum = checkedAdd(sum, totals.enumeration_ns, "phase sum");
    sum = checkedAdd(sum, totals.output_finalization_ns, "phase sum");
    return sum;
}

PhaseTotals validateLedgerSnapshot(
    ArmId arm,
    const PhaseLedgerSnapshot& snapshot) {
    if (snapshot.native_intervals.empty()) {
        fail(ErrorCode::kInvalidChildReceipt, "native interval ledger is empty");
    }
    PhaseTotals recomputed;
    std::array<bool, 5> seen{};
    std::uint64_t expected_start = 0;
    int phase_region = -1;
    for (std::size_t index = 0; index < snapshot.native_intervals.size(); ++index) {
        const NativeInterval& interval = snapshot.native_intervals[index];
        if (interval.interval_index != index) {
            fail(ErrorCode::kInvalidChildReceipt, "native interval index drift");
        }
        validateNativeName(interval.native_name);
        validateApplicability(arm, interval.common_phase, interval.applicable);
        phase_region = advancePhaseRegion(
            arm,
            phase_region,
            interval.common_phase,
            ErrorCode::kInvalidChildReceipt);
        if (interval.start_offset_ns != expected_start ||
            interval.end_offset_ns < interval.start_offset_ns ||
            interval.duration_ns !=
                interval.end_offset_ns - interval.start_offset_ns) {
            fail(ErrorCode::kInvalidChildReceipt, "native interval coverage drift");
        }
        if (!interval.applicable && interval.duration_ns != 0U) {
            fail(
                ErrorCode::kInvalidChildReceipt,
                "non-applicable native interval must have zero duration");
        }
        expected_start = interval.end_offset_ns;
        seen[phaseIndex(interval.common_phase)] = true;
        addPhaseDuration(recomputed, interval.common_phase, interval.duration_ns);
    }
    const int required_final_region =
        arm == ArmId::kCeciDurReimpl ? 4 : 3;
    if (phase_region != required_final_region ||
        !std::all_of(seen.begin(), seen.end(), [](bool value) { return value; })) {
        fail(ErrorCode::kInvalidChildReceipt, "five common phases are not all represented");
    }
    recomputed.prepared_query_total_ns = sumPhaseTotals(recomputed);
    if (expected_start != recomputed.prepared_query_total_ns ||
        snapshot.totals.planning_ns != recomputed.planning_ns ||
        snapshot.totals.td_construction_ns != recomputed.td_construction_ns ||
        snapshot.totals.dag_compilation_ns != recomputed.dag_compilation_ns ||
        snapshot.totals.enumeration_ns != recomputed.enumeration_ns ||
        snapshot.totals.output_finalization_ns !=
            recomputed.output_finalization_ns ||
        snapshot.totals.prepared_query_total_ns !=
            recomputed.prepared_query_total_ns) {
        fail(ErrorCode::kInvalidChildReceipt, "native interval aggregate drift");
    }
    return recomputed;
}

void validateIdentity(
    const FileIdentity& identity,
    std::string_view expected_role,
    bool object_h0_required) {
    if (identity.role != expected_role || identity.path.empty() ||
        identity.bytes == 0U || !isLowerSha256(identity.sha256) ||
        identity.nlink != 1U || identity.file_type != "regular" ||
        identity.device == 0U || identity.inode == 0U ||
        (identity.mode != 0600U && identity.mode != 0644U)) {
        fail(ErrorCode::kInvalidChildReceipt, "input file identity drift");
    }
    if (identity.path.front() == '/' || identity.path.back() == '/' ||
        identity.path.find('\0') != std::string::npos) {
        fail(ErrorCode::kInvalidChildReceipt, "input identity path is unsafe");
    }
    std::size_t component_begin = 0;
    while (component_begin < identity.path.size()) {
        const std::size_t slash = identity.path.find('/', component_begin);
        const std::size_t component_end =
            slash == std::string::npos ? identity.path.size() : slash;
        const std::string_view component(
            identity.path.data() + component_begin,
            component_end - component_begin);
        if (component.empty() || component == "." || component == "..") {
            fail(ErrorCode::kInvalidChildReceipt, "input identity path is unsafe");
        }
        if (slash == std::string::npos) break;
        component_begin = slash + 1U;
    }
    if (object_h0_required && !identity.object_sha256.has_value()) {
        fail(ErrorCode::kInvalidChildReceipt, "required input object H0 is absent");
    }
    if (identity.object_sha256.has_value() &&
        !isLowerSha256(*identity.object_sha256)) {
        fail(ErrorCode::kInvalidChildReceipt, "input object H0 is invalid");
    }
}

std::string validateReceiptInputBindingsAndExpectedPlan(
    const InputIdentitySet& identities);

JsonValue identityJson(const FileIdentity& identity) {
    JsonValue::Object object;
    object.emplace("bytes", JsonValue(identity.bytes));
    object.emplace("device", JsonValue(identity.device));
    object.emplace("file_type", JsonValue(identity.file_type));
    object.emplace("inode", JsonValue(identity.inode));
    object.emplace("mode", JsonValue(static_cast<std::uint64_t>(identity.mode)));
    object.emplace("nlink", JsonValue(identity.nlink));
    if (identity.object_sha256.has_value()) {
        object.emplace("object_sha256_or_null", JsonValue(*identity.object_sha256));
    } else {
        object.emplace("object_sha256_or_null", JsonValue(nullptr));
    }
    object.emplace("path", JsonValue(identity.path));
    object.emplace("role", JsonValue(identity.role));
    object.emplace("sha256", JsonValue(identity.sha256));
    return JsonValue(std::move(object));
}

JsonValue planConsumptionJson(
    const InternalPlanConsumptionObservation& observation) {
    JsonValue::Object object;
    object.emplace(
        "backend_arm",
        JsonValue(std::string(armToken(observation.backend_arm))));
    object.emplace(
        "exact_executable_decomposition_passed_to_td_tree",
        JsonValue(
            observation.exact_executable_decomposition_passed_to_td_tree));
    object.emplace(
        "loader_record_plan_consumed",
        JsonValue(observation.loader_record_plan_consumed));
    object.emplace(
        "payload_object_sha256",
        JsonValue(observation.payload_object_sha256));
    object.emplace("plan_consumed", JsonValue(observation.plan_consumed));
    object.emplace(
        "runtime_object_sha256",
        JsonValue(observation.runtime_object_sha256));
    object.emplace(
        "same_executable_plan_object",
        JsonValue(observation.same_executable_plan_object));
    return JsonValue(std::move(object));
}

JsonValue loadsJson(const ExcludedLoadDurations& loads) {
    JsonValue::Object object;
    object.emplace(
        "all_completed_before_boundary",
        JsonValue(loads.all_completed_before_boundary));
    object.emplace(
        "explicit_label_map_load_ns",
        JsonValue(loads.explicit_label_map_load_ns));
    object.emplace("prepared_graph_load_ns", JsonValue(loads.prepared_graph_load_ns));
    object.emplace("query_load_ns", JsonValue(loads.query_load_ns));
    object.emplace(
        "query_projection_load_ns", JsonValue(loads.query_projection_load_ns));
    return JsonValue(std::move(object));
}

JsonValue intervalsJson(const std::vector<NativeInterval>& intervals) {
    JsonValue::Array output;
    output.reserve(intervals.size());
    for (const NativeInterval& interval : intervals) {
        JsonValue::Object object;
        object.emplace("applicable", JsonValue(interval.applicable));
        object.emplace(
            "common_phase", JsonValue(std::string(phaseToken(interval.common_phase))));
        object.emplace("duration_ns", JsonValue(interval.duration_ns));
        object.emplace("end_offset_ns", JsonValue(interval.end_offset_ns));
        object.emplace("interval_index", JsonValue(interval.interval_index));
        object.emplace("native_name", JsonValue(interval.native_name));
        object.emplace("start_offset_ns", JsonValue(interval.start_offset_ns));
        output.emplace_back(std::move(object));
    }
    return JsonValue(std::move(output));
}

JsonValue stringVectorJson(const std::vector<std::uint32_t>& values) {
    JsonValue::Array output;
    output.reserve(values.size());
    for (const std::uint32_t value : values) {
        output.emplace_back(static_cast<std::uint64_t>(value));
    }
    return JsonValue(std::move(output));
}

}  // namespace

struct PhaseLedger::Impl final {
    ArmId arm;
    std::uint64_t boundary_ns;
    bool started = false;
    bool finished = false;
    CommonPhase active_phase = CommonPhase::kPlanning;
    std::string active_native_name;
    bool active_applicable = true;
    std::uint64_t active_start_ns = 0;
    PhaseLedgerSnapshot snapshot;
    std::array<bool, 5> seen{};
    int phase_region = -1;

    Impl(ArmId input_arm, std::uint64_t input_boundary)
        : arm(input_arm), boundary_ns(input_boundary) {
        if (armToken(arm) == "invalid-arm") {
            fail(ErrorCode::kInvalidPhaseLedger, "invalid phase-ledger arm");
        }
    }

    void validateOpen(
        CommonPhase phase,
        std::string_view native_name,
        bool applicable,
        std::uint64_t now_ns) const {
        if (finished) {
            fail(ErrorCode::kInvalidPhaseLedger, "phase ledger is already finished");
        }
        validateNativeName(native_name);
        validateApplicability(arm, phase, applicable);
        static_cast<void>(advancePhaseRegion(
            arm, phase_region, phase, ErrorCode::kInvalidPhaseLedger));
        if (now_ns < boundary_ns ||
            (started && now_ns < active_start_ns)) {
            fail(ErrorCode::kClockRegression, "phase ledger clock regression");
        }
    }

    void close(std::uint64_t now_ns) {
        if (!started) {
            fail(ErrorCode::kInvalidPhaseLedger, "phase ledger was not started");
        }
        if (now_ns < active_start_ns) {
            fail(ErrorCode::kClockRegression, "phase interval clock regression");
        }
        const std::uint64_t duration = now_ns - active_start_ns;
        if (!active_applicable && duration != 0U) {
            fail(
                ErrorCode::kInvalidPhaseLedger,
                "non-applicable interval must be zero-duration");
        }
        const std::uint64_t start_offset = active_start_ns - boundary_ns;
        const std::uint64_t end_offset = now_ns - boundary_ns;
        snapshot.native_intervals.push_back({
            static_cast<std::uint64_t>(snapshot.native_intervals.size()),
            active_native_name,
            active_phase,
            start_offset,
            end_offset,
            duration,
            active_applicable,
        });
        seen[phaseIndex(active_phase)] = true;
        addPhaseDuration(snapshot.totals, active_phase, duration);
    }
};

PhaseLedger::PhaseLedger(ArmId arm, std::uint64_t boundary_ns)
    : impl_(std::make_unique<Impl>(arm, boundary_ns)) {}

PhaseLedger::~PhaseLedger() = default;
PhaseLedger::PhaseLedger(PhaseLedger&&) noexcept = default;
PhaseLedger& PhaseLedger::operator=(PhaseLedger&&) noexcept = default;

void PhaseLedger::start(
    CommonPhase phase,
    std::string native_name,
    bool applicable,
    std::uint64_t now_ns) {
    if (!impl_) fail(ErrorCode::kInvalidPhaseLedger, "moved-from phase ledger");
    if (impl_->started) {
        fail(ErrorCode::kInvalidPhaseLedger, "phase ledger started twice");
    }
    impl_->validateOpen(phase, native_name, applicable, now_ns);
    if (now_ns != impl_->boundary_ns) {
        fail(ErrorCode::kInvalidPhaseLedger, "first interval must start at boundary");
    }
    impl_->started = true;
    impl_->phase_region = advancePhaseRegion(
        impl_->arm, -1, phase, ErrorCode::kInvalidPhaseLedger);
    impl_->active_phase = phase;
    impl_->active_native_name = std::move(native_name);
    impl_->active_applicable = applicable;
    impl_->active_start_ns = now_ns;
}

void PhaseLedger::transition(
    CommonPhase next_phase,
    std::string next_native_name,
    bool next_applicable,
    std::uint64_t now_ns) {
    if (!impl_) fail(ErrorCode::kInvalidPhaseLedger, "moved-from phase ledger");
    if (!impl_->started) {
        fail(ErrorCode::kInvalidPhaseLedger, "transition before ledger start");
    }
    impl_->validateOpen(next_phase, next_native_name, next_applicable, now_ns);
    const int next_region = advancePhaseRegion(
        impl_->arm,
        impl_->phase_region,
        next_phase,
        ErrorCode::kInvalidPhaseLedger);
    impl_->close(now_ns);
    impl_->active_phase = next_phase;
    impl_->active_native_name = std::move(next_native_name);
    impl_->active_applicable = next_applicable;
    impl_->active_start_ns = now_ns;
    impl_->phase_region = next_region;
}

PhaseLedgerSnapshot PhaseLedger::finish(std::uint64_t now_ns) {
    if (!impl_) fail(ErrorCode::kInvalidPhaseLedger, "moved-from phase ledger");
    if (!impl_->started || impl_->finished) {
        fail(ErrorCode::kInvalidPhaseLedger, "invalid phase-ledger finish state");
    }
    impl_->close(now_ns);
    const int required_final_region =
        impl_->arm == ArmId::kCeciDurReimpl ? 4 : 3;
    if (impl_->phase_region != required_final_region || !std::all_of(
            impl_->seen.begin(), impl_->seen.end(), [](bool value) { return value; })) {
        fail(ErrorCode::kInvalidPhaseLedger, "five common phases are required");
    }
    impl_->snapshot.totals.prepared_query_total_ns =
        sumPhaseTotals(impl_->snapshot.totals);
    if (now_ns - impl_->boundary_ns !=
        impl_->snapshot.totals.prepared_query_total_ns) {
        fail(ErrorCode::kInvalidPhaseLedger, "phase ledger is not contiguous/additive");
    }
    impl_->finished = true;
    return impl_->snapshot;
}

CanonicalChildReceipt serializeCanonicalChildReceipt(
    const ChildReceiptValue& value) {
    if (value.governance_id != kGovernanceId) {
        fail(ErrorCode::kInvalidChildReceipt, "child governance ID drift");
    }
    if (armToken(value.arm) == "invalid-arm") {
        fail(ErrorCode::kInvalidChildReceipt, "child arm ID drift");
    }
    validateIdentity(
        value.input_identities.prepared_graph, "prepared_graph", false);
    validateIdentity(
        value.input_identities.explicit_label_map, "explicit_label_map", false);
    validateIdentity(
        value.input_identities.query_projection, "query_projection", true);
    validateIdentity(value.input_identities.query, "query", false);
    if (value.input_identities.minimum_duration_k < 3U ||
        value.input_identities.minimum_duration_k > 5U) {
        fail(ErrorCode::kInvalidChildReceipt, "minimum duration is outside 3..5");
    }
    const std::string expected_typed_plan =
        validateReceiptInputBindingsAndExpectedPlan(value.input_identities);
    if (isInternalArm(value.arm)) {
        if (!value.input_identities.typed_plan.has_value()) {
            fail(ErrorCode::kInvalidChildReceipt, "internal child lacks typed plan identity");
        }
        validateIdentity(*value.input_identities.typed_plan, "typed_plan", true);
        if (value.input_identities.typed_plan->path != expected_typed_plan) {
            fail(
                ErrorCode::kInvalidChildReceipt,
                "typed plan identity does not match the selected case");
        }
        if (!value.plan_consumption.has_value()) {
            fail(
                ErrorCode::kInvalidChildReceipt,
                "internal child lacks typed-plan consumption evidence");
        }
        const InternalPlanConsumptionObservation& consumption =
            *value.plan_consumption;
        if (consumption.backend_arm != value.arm ||
            !consumption.plan_consumed ||
            consumption.loader_record_plan_consumed ||
            !consumption.same_executable_plan_object ||
            !consumption.exact_executable_decomposition_passed_to_td_tree ||
            !isLowerSha256(consumption.payload_object_sha256) ||
            !isLowerSha256(consumption.runtime_object_sha256) ||
            !value.input_identities.typed_plan->object_sha256.has_value() ||
            *value.input_identities.typed_plan->object_sha256 !=
                consumption.runtime_object_sha256) {
            fail(
                ErrorCode::kInvalidChildReceipt,
                "internal typed-plan consumption evidence drift");
        }
    } else if (value.input_identities.typed_plan.has_value()) {
        fail(ErrorCode::kInvalidChildReceipt, "external child supplied typed plan identity");
    } else if (value.plan_consumption.has_value()) {
        fail(
            ErrorCode::kInvalidChildReceipt,
            "external child supplied typed-plan consumption evidence");
    }
    if (!value.loads.all_completed_before_boundary) {
        fail(ErrorCode::kInvalidChildReceipt, "excluded loads did not complete before boundary");
    }
    const PhaseTotals totals = validateLedgerSnapshot(value.arm, value.ledger);
    if (value.threads.requested != kRequiredThreads ||
        value.threads.effective != kRequiredThreads ||
        value.threads.parallel_mode != "roots" ||
        value.threads.affinity_cpu_set.size() != kRequiredThreads) {
        fail(ErrorCode::kInvalidChildReceipt, "thread observation drift");
    }
    for (std::size_t index = 0;
         index < value.threads.affinity_cpu_set.size(); ++index) {
        if (value.threads.affinity_cpu_set[index] != index) {
            fail(ErrorCode::kInvalidChildReceipt, "affinity must be exact CPUs 0..15");
        }
    }
    if (value.child_wall_segments.clock != "steady_clock" ||
        value.child_wall_segments.unit != "ns" ||
        value.child_wall_segments.primary_total_ns !=
            totals.prepared_query_total_ns) {
        fail(ErrorCode::kInvalidChildReceipt, "child wall-segment contract drift");
    }

    JsonValue::Object inputs;
    inputs.emplace(
        "explicit_label_map",
        identityJson(value.input_identities.explicit_label_map));
    inputs.emplace(
        "minimum_duration_k",
        JsonValue(static_cast<std::uint64_t>(
            value.input_identities.minimum_duration_k)));
    inputs.emplace(
        "prepared_graph", identityJson(value.input_identities.prepared_graph));
    inputs.emplace("query", identityJson(value.input_identities.query));
    inputs.emplace(
        "query_projection",
        identityJson(value.input_identities.query_projection));
    if (value.input_identities.typed_plan.has_value()) {
        inputs.emplace(
            "typed_plan", identityJson(*value.input_identities.typed_plan));
    } else {
        inputs.emplace("typed_plan", JsonValue(nullptr));
    }

    JsonValue::Object boundary;
    boundary.emplace("clock", JsonValue("steady_clock"));
    boundary.emplace("end_offset_ns", JsonValue(totals.prepared_query_total_ns));
    boundary.emplace("start_offset_ns", JsonValue(std::uint64_t{0}));
    boundary.emplace("unit", JsonValue("ns"));

    JsonValue::Object threads;
    threads.emplace(
        "affinity_cpu_set", stringVectorJson(value.threads.affinity_cpu_set));
    threads.emplace(
        "effective", JsonValue(static_cast<std::uint64_t>(value.threads.effective)));
    threads.emplace("parallel_mode", JsonValue(value.threads.parallel_mode));
    threads.emplace(
        "requested", JsonValue(static_cast<std::uint64_t>(value.threads.requested)));

    JsonValue::Object wall;
    wall.emplace("clock", JsonValue(value.child_wall_segments.clock));
    wall.emplace(
        "postboundary_ns", JsonValue(value.child_wall_segments.postboundary_ns));
    wall.emplace(
        "preboundary_ns", JsonValue(value.child_wall_segments.preboundary_ns));
    wall.emplace(
        "primary_total_ns",
        JsonValue(value.child_wall_segments.primary_total_ns));
    wall.emplace("unit", JsonValue(value.child_wall_segments.unit));

    JsonValue::Object root;
    root.emplace("arm_id", JsonValue(std::string(armToken(value.arm))));
    root.emplace("artifact_kind", JsonValue(std::string(kChildLedgerArtifactKind)));
    root.emplace("boundary", JsonValue(std::move(boundary)));
    root.emplace("child_wall_segments", JsonValue(std::move(wall)));
    root.emplace("count_overflow", JsonValue(value.count_overflow));
    root.emplace("dag_compilation_ns", JsonValue(totals.dag_compilation_ns));
    root.emplace("enumeration_ns", JsonValue(totals.enumeration_ns));
    root.emplace("governance_id", JsonValue(value.governance_id));
    root.emplace("input_identities", JsonValue(std::move(inputs)));
    root.emplace("loads", loadsJson(value.loads));
    root.emplace("match_count", JsonValue(value.match_count));
    root.emplace("native_intervals", intervalsJson(value.ledger.native_intervals));
    root.emplace("output_finalization_ns", JsonValue(totals.output_finalization_ns));
    if (value.plan_consumption.has_value()) {
        root.emplace(
            "plan_consumption",
            planConsumptionJson(*value.plan_consumption));
    } else {
        root.emplace("plan_consumption", JsonValue(nullptr));
    }
    root.emplace("planning_ns", JsonValue(totals.planning_ns));
    root.emplace(
        "prepared_query_total_ns", JsonValue(totals.prepared_query_total_ns));
    root.emplace("schema_version", JsonValue(std::uint64_t{1}));
    root.emplace("td_construction_ns", JsonValue(totals.td_construction_ns));
    root.emplace("threads", JsonValue(std::move(threads)));

    const std::string preimage_canonical = canonicalJson(JsonValue(root));
    const std::string preimage_h0 = sha256Hex(preimage_canonical);
    root.emplace("receipt_preimage_h0", JsonValue(preimage_h0));
    const std::string canonical = canonicalJson(JsonValue(std::move(root)));
    CanonicalChildReceipt output;
    output.canonical_lf_bytes = canonical + "\n";
    output.receipt_preimage_object_sha256 = preimage_h0;
    output.object_sha256 = sha256Hex(canonical);
    output.canonical_lf_sha256 = sha256Hex(output.canonical_lf_bytes);
    return output;
}

}  // namespace dsm::e3e4::adapter_v1

namespace dsm::e3e4::adapter_v1 {
namespace {

ExpectedRegularFile expectedFile(
    std::string role,
    std::string path,
    std::string sha256,
    std::uint64_t bytes,
    std::uint32_t mode,
    std::optional<std::string> object_sha256 = std::nullopt) {
    return {
        std::move(role), std::move(path), std::move(sha256), bytes, mode, 1,
        std::move(object_sha256)};
}

struct CorpusRow final {
    std::uint32_t projection_index = 0;
    std::string dataset_id;
    std::string query_shape;
    ExpectedRegularFile graph;
    ExpectedRegularFile map;
    std::string mapping_sha256;
    ExpectedRegularFile projection;
    ExpectedRegularFile query;
    std::vector<std::uint32_t> minimum_durations;
};

const std::vector<CorpusRow>& corpusRegistry() {
    static const std::vector<CorpusRow> rows{
        {
            0, "sx_superuser", "path3",
            expectedFile("prepared_graph", "Dataset/sx-superuser-temporal-1w-unique_filtered.txt",
                         "2c44e53e1569c97d14b33ffe4979d9a4c2c54c997da76e51bd63f24b9bf55e61",
                         2109304, 0644),
            expectedFile("explicit_label_map",
                         "benchmark/icde2027_m4_candidate_pool_v2/label_maps/sx_superuser/uniform_c005.json",
                         "8cfa3badb35b47a00c9b36d96cfaeec87e2925ba79689d714a62faa4605032fc",
                         195720, 0600),
            "b68e633ed81cfea8d445757d21e7c52360ddee19f70498f0568a8a468efb0f5a",
            expectedFile("query_projection",
                         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/sx_superuser/path3_projection.json",
                         "71709f2fd15dd3eace21c812bcf44090a7cb6751a5ff8414dc5de634600573c9",
                         1150, 0644,
                         "a32a842c74ae6a45a548752405de0e2fc7329d4fb2f58d966f5cae9dcf3ded4d"),
            expectedFile("query", "benchmark/queries/paper_parameters_v2/sx_superuser/path3_k3_rank001.qry",
                         "c18f766169645492d0f5c09c9adb5ea4f2254b78e2d0c35133fd7122f509dd64",
                         30, 0644),
            {3, 4, 5},
        },
        {
            1, "wiki_talk", "path3",
            expectedFile("prepared_graph", "Dataset/wiki-talk-temporal-timeinstance-unique-1w_filtered.txt",
                         "9cac4c20eafe23f936e458664a64c011192e1a294fef48b964e749d3f80ac106",
                         15425450, 0644),
            expectedFile("explicit_label_map",
                         "benchmark/icde2027_m4_candidate_pool_v2/label_maps/wiki_talk/uniform_c005.json",
                         "3146679b344625ab70f06a4cb35225ff5a7a40b9f9710f0b4a4ca3f06b1229e3",
                         769064, 0600),
            "6d3cfa30a0d7a97e399c14fa026864ef0b9b7ae2a577f47a7bce2073e65a5335",
            expectedFile("query_projection",
                         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/wiki_talk/path3_projection.json",
                         "86cead68014532f1019285547d4b4bdc56efe0a00718e4fca7c6df6d67a7df1f",
                         1150, 0644,
                         "9f35c90b95cff76551517f46394343933e754a0b3796c3bef0b2eeb19f8af546"),
            expectedFile("query", "benchmark/queries/paper_parameters_v2/wiki_talk/path3_k3_rank001.qry",
                         "113c7d189cf249a2ef3983f9e0b2888f5bdc9e1613275f390abbe52d220a634e",
                         30, 0644),
            {3},
        },
        {
            2, "sx_stackoverflow", "path3",
            expectedFile("prepared_graph", "Dataset/sx-stackoverflow-timeinstance-unique-1w_filtered.txt",
                         "cf189ecf3b9b89aa172421616b3f42af6c7e3beebcd77484f7757eb1e85fa78f",
                         119581001, 0644),
            expectedFile("explicit_label_map",
                         "benchmark/icde2027_m4_candidate_pool_v2/label_maps/sx_stackoverflow/uniform_c005.json",
                         "477e14e1f1507b38769324632599a444dcbb6ed031fdcd8eed1bde8d4ffd01a3",
                         6223268, 0600),
            "0308cd5ceae65586c7966bd643b77c47ad606e959b34f4e90d571ead4b464206",
            expectedFile("query_projection",
                         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/sx_stackoverflow/path3_projection.json",
                         "df1852d39d92d1533df9ec7fd1e53a21ab51c4e8bcb3dc9b5d017ec8365754fc",
                         1150, 0644,
                         "561773f99dc3e8009f2bcd80329ddf7947ceaa2687a56db40562a5d572fd4548"),
            expectedFile("query", "benchmark/queries/paper_parameters_v2/sx_stackoverflow/path3_k3_rank001.qry",
                         "113c7d189cf249a2ef3983f9e0b2888f5bdc9e1613275f390abbe52d220a634e",
                         30, 0644),
            {3},
        },
        {
            3, "bitcoin", "path3",
            expectedFile("prepared_graph", "Dataset/bitcoin-temporal-timeinstance-unique-1w_filtered.txt",
                         "8cda283c93d62f5ba24b9a6623ada9ec967e19f50fc0cec7cb17b7893a4cf93c",
                         129812770, 0644),
            expectedFile("explicit_label_map",
                         "benchmark/icde2027_m4_candidate_pool_v2/label_maps/bitcoin/uniform_c005.json",
                         "240bd53eeb397b3bfa751b678d39d20db0f20f29fc4cd48b01796507b59cab50",
                         10546053, 0600),
            "3daf90a80c34064608a815ef850379b36da6e6c84d2241b9b345e488c8648b59",
            expectedFile("query_projection",
                         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/bitcoin/path3_projection.json",
                         "607eef53fe08db0ac8e659344423af3b8a0d7ccb7ff559f5edc66a077f6cafd4",
                         1150, 0644,
                         "b7bab049eaf162a79c077de04dcdb1e59aa07e21a5d5e3973fc7dbb2129994f3"),
            expectedFile("query", "benchmark/queries/paper_parameters_v2/bitcoin/path3_k3_rank001.qry",
                         "113c7d189cf249a2ef3983f9e0b2888f5bdc9e1613275f390abbe52d220a634e",
                         30, 0644),
            {3, 4, 5},
        },
        {
            4, "bitcoin", "cycle3",
            expectedFile("prepared_graph", "Dataset/bitcoin-temporal-timeinstance-unique-1w_filtered.txt",
                         "8cda283c93d62f5ba24b9a6623ada9ec967e19f50fc0cec7cb17b7893a4cf93c",
                         129812770, 0644),
            expectedFile("explicit_label_map",
                         "benchmark/icde2027_m4_candidate_pool_v2/label_maps/bitcoin/uniform_c005.json",
                         "240bd53eeb397b3bfa751b678d39d20db0f20f29fc4cd48b01796507b59cab50",
                         10546053, 0600),
            "3daf90a80c34064608a815ef850379b36da6e6c84d2241b9b345e488c8648b59",
            expectedFile("query_projection",
                         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/bitcoin/cycle3_projection.json",
                         "4c069507e0655df1321ec7e57b96340ca98d750f3bf9139e7f17484ad5fe7d8c",
                         1150, 0644,
                         "f899ccaecf635b3775406ad07b981318f89b4f0e82aaa810cc3f3c6cb9f97cef"),
            expectedFile("query", "benchmark/queries/paper_parameters_v2/bitcoin/cycle3_k3_rank001.qry",
                         "5a896bc44ae1c21107dad48129d2a8f9b31f0a2de145681097427ef068fda6f1",
                         30, 0644),
            {3},
        },
        {
            5, "bitcoin", "tailed4",
            expectedFile("prepared_graph", "Dataset/bitcoin-temporal-timeinstance-unique-1w_filtered.txt",
                         "8cda283c93d62f5ba24b9a6623ada9ec967e19f50fc0cec7cb17b7893a4cf93c",
                         129812770, 0644),
            expectedFile("explicit_label_map",
                         "benchmark/icde2027_m4_candidate_pool_v2/label_maps/bitcoin/uniform_c005.json",
                         "240bd53eeb397b3bfa751b678d39d20db0f20f29fc4cd48b01796507b59cab50",
                         10546053, 0600),
            "3daf90a80c34064608a815ef850379b36da6e6c84d2241b9b345e488c8648b59",
            expectedFile("query_projection",
                         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/bitcoin/tailed4_projection.json",
                         "fcee690f06f403668ad78397241edee02af7d32c19d1f30e0303e0e9a66d6423",
                         1150, 0644,
                         "0f05cef67aad8a85d14e0ebec349b42a67cfec8cc69d13e4eac07cb660b0a341"),
            expectedFile("query", "benchmark/queries/paper_parameters_v2/bitcoin/tailed4_k3_rank001.qry",
                         "9126d810ec452f8dd23458e231878c2e03ac035c1de424bfb129b4a58eef04f0",
                         40, 0644),
            {3},
        },
        {
            6, "bitcoin", "path4",
            expectedFile("prepared_graph", "Dataset/bitcoin-temporal-timeinstance-unique-1w_filtered.txt",
                         "8cda283c93d62f5ba24b9a6623ada9ec967e19f50fc0cec7cb17b7893a4cf93c",
                         129812770, 0644),
            expectedFile("explicit_label_map",
                         "benchmark/icde2027_m4_candidate_pool_v2/label_maps/bitcoin/uniform_c005.json",
                         "240bd53eeb397b3bfa751b678d39d20db0f20f29fc4cd48b01796507b59cab50",
                         10546053, 0600),
            "3daf90a80c34064608a815ef850379b36da6e6c84d2241b9b345e488c8648b59",
            expectedFile("query_projection",
                         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/bitcoin/path4_projection.json",
                         "788d0f66c472cd35da2d679292f7fb8b6de1bfa7b65a57bc417f856b7c7b1236",
                         1150, 0644,
                         "c851f1c1b0233236e1c65579550bc14aa01c66d879157b0c3077022a4cd3b59a"),
            expectedFile("query", "benchmark/queries/paper_parameters_v2/bitcoin/path4_k3_rank001.qry",
                         "f15807eaea434bc8fd097566905077bd41f0fdefe253dbb28e2489b3078ce8fe",
                         40, 0644),
            {3},
        },
        {
            7, "bitcoin", "path5",
            expectedFile("prepared_graph", "Dataset/bitcoin-temporal-timeinstance-unique-1w_filtered.txt",
                         "8cda283c93d62f5ba24b9a6623ada9ec967e19f50fc0cec7cb17b7893a4cf93c",
                         129812770, 0644),
            expectedFile("explicit_label_map",
                         "benchmark/icde2027_m4_candidate_pool_v2/label_maps/bitcoin/uniform_c005.json",
                         "240bd53eeb397b3bfa751b678d39d20db0f20f29fc4cd48b01796507b59cab50",
                         10546053, 0600),
            "3daf90a80c34064608a815ef850379b36da6e6c84d2241b9b345e488c8648b59",
            expectedFile("query_projection",
                         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/bitcoin/path5_projection.json",
                         "ddc126762e54602ee2bab164b7f7a4f65695c4786625e3074fe77d7fc7cfa702",
                         1150, 0644,
                         "869dad622bd70005286866058453092c08230890b22a27f46f5d77678e51e823"),
            expectedFile("query", "benchmark/queries/paper_parameters_v2/bitcoin/path5_k3_rank001.qry",
                         "6311d6a29cf6c571b1dc02d376c15b02ea189e893caf312830412568912d2a2f",
                         50, 0644),
            {3},
        },
        {
            8, "sx_superuser", "path4",
            expectedFile("prepared_graph", "Dataset/sx-superuser-temporal-1w-unique_filtered.txt",
                         "2c44e53e1569c97d14b33ffe4979d9a4c2c54c997da76e51bd63f24b9bf55e61",
                         2109304, 0644),
            expectedFile("explicit_label_map",
                         "benchmark/icde2027_m4_candidate_pool_v2/label_maps/sx_superuser/uniform_c005.json",
                         "8cfa3badb35b47a00c9b36d96cfaeec87e2925ba79689d714a62faa4605032fc",
                         195720, 0600),
            "b68e633ed81cfea8d445757d21e7c52360ddee19f70498f0568a8a468efb0f5a",
            expectedFile("query_projection",
                         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/sx_superuser/path4_projection.json",
                         "cbba5d850a726ea780c60a9e8827e2e28be2bba7ea62fd9f7d270abc2ea6f3c7",
                         1150, 0644,
                         "a27110e7c3ccfe319433bc22107276f7785dfe9714760c369f0f0b15185f7cf9"),
            expectedFile("query", "benchmark/queries/paper_parameters_v2/sx_superuser/path4_k3_rank001.qry",
                         "33ef9f9fb6e980f70d7bcd6f2282536f10341230126a2c795928b42ad3e157ea",
                         40, 0644),
            {3},
        },
        {
            9, "sx_superuser", "star5",
            expectedFile("prepared_graph", "Dataset/sx-superuser-temporal-1w-unique_filtered.txt",
                         "2c44e53e1569c97d14b33ffe4979d9a4c2c54c997da76e51bd63f24b9bf55e61",
                         2109304, 0644),
            expectedFile("explicit_label_map",
                         "benchmark/icde2027_m4_candidate_pool_v2/label_maps/sx_superuser/uniform_c005.json",
                         "8cfa3badb35b47a00c9b36d96cfaeec87e2925ba79689d714a62faa4605032fc",
                         195720, 0600),
            "b68e633ed81cfea8d445757d21e7c52360ddee19f70498f0568a8a468efb0f5a",
            expectedFile("query_projection",
                         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/sx_superuser/star5_projection.json",
                         "b7effaf2fdc0fbaf41584db7623a7366f87682f4bc0a60e0a771f1068a196304",
                         1150, 0644,
                         "c16aa059cef22853fc21749ede9cba980099a3b91394c2dc27591f84280ca958"),
            expectedFile("query", "benchmark/queries/paper_parameters_v2/sx_superuser/star5_k3_rank001.qry",
                         "6c8fe184b5efd325f94236235a276269ad1c092a1521d9e4b4653b5cb4c7f4f8",
                         50, 0644),
            {3},
        },
    };
    return rows;
}

const CorpusRow& selectCorpusRow(const ClosedInvocation& invocation) {
    const CorpusRow* selected = nullptr;
    for (const CorpusRow& row : corpusRegistry()) {
        if (row.graph.path == invocation.prepared_graph_path &&
            row.map.path == invocation.explicit_label_map_path &&
            row.projection.path == invocation.query_projection_path &&
            row.query.path == invocation.query_path &&
            std::find(
                row.minimum_durations.begin(), row.minimum_durations.end(),
                invocation.minimum_duration_k) != row.minimum_durations.end()) {
            if (selected != nullptr) {
                fail(ErrorCode::kCrossBindingMismatch, "ambiguous corpus registry row");
            }
            selected = &row;
        }
    }
    if (selected == nullptr) {
        fail(ErrorCode::kCrossBindingMismatch, "argv is not one exact projection-authority case binding");
    }
    return *selected;
}

std::string caseIdFor(const CorpusRow& row, std::uint32_t minimum_duration_k) {
    switch (row.projection_index) {
        case 0:
            if (minimum_duration_k == 3U) {
                return "icde27_full_fair_sx_superuser_path3_e3_k3_r001";
            }
            if (minimum_duration_k == 4U) {
                return "icde27_full_fair_sx_superuser_path3_e3_k4_r001";
            }
            if (minimum_duration_k == 5U) {
                return "icde27_full_fair_sx_superuser_path3_e3_k5_r001";
            }
            break;
        case 1: return "icde27_full_fair_wiki_talk_path3_e3_k3_r001";
        case 2: return "icde27_full_fair_sx_stackoverflow_path3_e3_k3_r001";
        case 3:
            if (minimum_duration_k == 3U) {
                return "icde27_full_fair_bitcoin_path3_e3_k3_r001";
            }
            if (minimum_duration_k == 4U) {
                return "icde27_full_fair_bitcoin_path3_e3_k4_r001";
            }
            if (minimum_duration_k == 5U) {
                return "icde27_full_fair_bitcoin_path3_e3_k5_r001";
            }
            break;
        case 4: return "icde27_full_fair_bitcoin_cycle3_e3_k3_r001";
        case 5: return "icde27_full_fair_bitcoin_tailed4_e4_k3_r001";
        case 6: return "icde27_full_fair_bitcoin_path4_e4_k3_r001";
        case 7: return "icde27_full_fair_bitcoin_path5_e5_k3_r001";
        case 8: return "icde27_full_fair_sx_superuser_path4_e4_k3_r001";
        case 9: return "icde27_full_fair_sx_superuser_star5_e5_k3_r001";
    }
    fail(ErrorCode::kCrossBindingMismatch, "case ID cannot be derived from corpus row and K");
}

std::string expectedTypedPlanPath(
    const CorpusRow& row,
    std::uint32_t minimum_duration_k) {
    return
        "benchmark/icde2027_e3_e4_typed_plan_authority_v1/typed_plans/" +
        caseIdFor(row, minimum_duration_k) + ".json";
}

void requireIdentityMatchesPin(
    const FileIdentity& actual,
    const ExpectedRegularFile& expected) {
    if (actual.role != expected.role || actual.path != expected.path ||
        actual.sha256 != expected.sha256 || actual.bytes != expected.bytes ||
        actual.mode != expected.mode || actual.nlink != expected.nlink ||
        actual.file_type != "regular" ||
        actual.object_sha256 != expected.object_sha256) {
        fail(ErrorCode::kInvalidChildReceipt, "child input differs from corpus pin");
    }
}

std::string validateReceiptInputBindingsAndExpectedPlan(
    const InputIdentitySet& identities) {
    ClosedInvocation invocation;
    invocation.prepared_graph_path = identities.prepared_graph.path;
    invocation.explicit_label_map_path = identities.explicit_label_map.path;
    invocation.query_projection_path = identities.query_projection.path;
    invocation.query_path = identities.query.path;
    invocation.minimum_duration_k = identities.minimum_duration_k;
    const CorpusRow& row = selectCorpusRow(invocation);
    requireIdentityMatchesPin(identities.prepared_graph, row.graph);
    requireIdentityMatchesPin(identities.explicit_label_map, row.map);
    requireIdentityMatchesPin(identities.query_projection, row.projection);
    requireIdentityMatchesPin(identities.query, row.query);
    return expectedTypedPlanPath(row, identities.minimum_duration_k);
}

struct StatSnapshot final {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t size = 0;
    std::uint64_t nlink = 0;
    std::uint32_t mode = 0;
    std::uint32_t uid = 0;
    std::uint32_t gid = 0;
    std::int64_t mtime_sec = 0;
    std::int64_t mtime_nsec = 0;
    std::int64_t ctime_sec = 0;
    std::int64_t ctime_nsec = 0;
};

std::vector<std::string> splitRelativePath(
    std::string_view path,
    const std::string& context);
StatSnapshot fstatSnapshot(int fd, const std::string& context);
StatSnapshot statAtSnapshot(
    int parent_fd,
    const std::string& name,
    const std::string& context);
bool sameAnchor(const StatSnapshot& lhs, const StatSnapshot& rhs);
void closeAll(std::vector<int>& fds) noexcept;
std::vector<std::string> directoryNamesBounded(
    int fd,
    const std::string& context);
int openDirectoryAt(
    int parent_fd,
    const std::string& name,
    const std::string& context);

struct HeldDirectory final {
    std::string path;
    std::vector<int> fds;
    std::vector<std::string> component_names;
    std::vector<StatSnapshot> identities;
    std::vector<std::string> expected_names;
    std::uint64_t expected_nlink = 0;

    HeldDirectory() = default;
    HeldDirectory(HeldDirectory&& other) noexcept
        : path(std::move(other.path)),
          fds(std::move(other.fds)),
          component_names(std::move(other.component_names)),
          identities(std::move(other.identities)),
          expected_names(std::move(other.expected_names)),
          expected_nlink(other.expected_nlink) {
        other.fds.clear();
        other.expected_nlink = 0;
    }
    HeldDirectory& operator=(HeldDirectory&& other) noexcept {
        if (this != &other) {
            closeAll(fds);
            path = std::move(other.path);
            fds = std::move(other.fds);
            component_names = std::move(other.component_names);
            identities = std::move(other.identities);
            expected_names = std::move(other.expected_names);
            expected_nlink = other.expected_nlink;
            other.fds.clear();
            other.expected_nlink = 0;
        }
        return *this;
    }
    HeldDirectory(const HeldDirectory&) = delete;
    HeldDirectory& operator=(const HeldDirectory&) = delete;
    ~HeldDirectory() { closeAll(fds); }

    void prove() const {
        if (fds.empty() || fds.size() != identities.size() ||
            fds.size() != component_names.size() + 1U) {
            fail(ErrorCode::kIdentityDrift, "held directory state invalid");
        }
        for (std::size_t index = 0; index < fds.size(); ++index) {
            const StatSnapshot held = fstatSnapshot(fds[index], path + " held fstat");
            if (!sameAnchor(held, identities[index])) {
                fail(ErrorCode::kIdentityDrift, path + " held directory drift");
            }
            if (index > 0U) {
                const StatSnapshot lexical = statAtSnapshot(
                    fds[index - 1U], component_names[index - 1U],
                    path + " lexical proof");
                if (!sameAnchor(lexical, identities[index])) {
                    fail(ErrorCode::kIdentityDrift, path + " directory detached");
                }
            }
        }
        const StatSnapshot leaf = fstatSnapshot(fds.back(), path + " final fstat");
        if (!S_ISDIR(static_cast<mode_t>(leaf.mode)) ||
            (leaf.mode & 07777U) != 0755U || leaf.nlink != expected_nlink) {
            fail(ErrorCode::kIdentityDrift, path + " directory stat drift");
        }
        if (directoryNamesBounded(fds.back(), path) != expected_names) {
            fail(ErrorCode::kIdentityDrift, path + " exact inventory drift");
        }
    }
};

HeldDirectory openHeldDirectory(
    const RepositoryAnchor& repository,
    const std::string& path,
    std::vector<std::string> expected_names,
    std::uint64_t expected_nlink) {
    HeldDirectory result;
    result.path = path;
    result.expected_names = std::move(expected_names);
    std::sort(result.expected_names.begin(), result.expected_names.end());
    result.expected_nlink = expected_nlink;
    const std::vector<std::string> components = splitRelativePath(path, "authority directory");
    const int root_duplicate = ::fcntl(repository.fd(), F_DUPFD_CLOEXEC, 3);
    if (root_duplicate < 0) fail(ErrorCode::kOpenFailed, errnoMessage("dup repository root"));
    result.fds.push_back(root_duplicate);
    result.identities.push_back(fstatSnapshot(root_duplicate, "authority directory root"));
    int current = root_duplicate;
    for (const std::string& component : components) {
        const int child = openDirectoryAt(current, component, path + " component");
        result.component_names.push_back(component);
        result.fds.push_back(child);
        result.identities.push_back(fstatSnapshot(child, path + " component fstat"));
        current = child;
    }
    result.prove();
    return result;
}

}  // namespace

struct ProjectionAuthorityGuard::Impl final {
    struct DirectoryBinding final {
        int authority_fd = -1;
        int file_chain_fd = -1;
        StatSnapshot expected;
    };

    HeldRegularFile receipt;
    std::vector<std::pair<std::string, HeldRegularFile>> projection_leaves;
    std::vector<HeldDirectory> directories;
    std::vector<DirectoryBinding> directory_bindings;
    std::string selected_projection_path;

    void prove() const {
        receipt.proveStable();
        for (const auto& row : projection_leaves) row.second.proveStable();
        for (const HeldDirectory& directory : directories) directory.prove();
        for (const DirectoryBinding& binding : directory_bindings) {
            const StatSnapshot authority =
                fstatSnapshot(binding.authority_fd, "authority tree binding");
            const StatSnapshot file_chain =
                fstatSnapshot(binding.file_chain_fd, "file tree binding");
            if (!sameAnchor(authority, binding.expected) ||
                !sameAnchor(file_chain, binding.expected) ||
                !sameAnchor(authority, file_chain)) {
                fail(
                    ErrorCode::kIdentityDrift,
                    "projection authority files do not share one held tree");
            }
        }
    }

    const FileIdentity* leafIdentity(const std::string& path) const {
        for (const auto& row : projection_leaves) {
            if (row.first == path) return &row.second.identity();
        }
        return nullptr;
    }

    const HeldRegularFile* leafFile(const std::string& path) const {
        for (const auto& row : projection_leaves) {
            if (row.first == path) return &row.second;
        }
        return nullptr;
    }

    void proveSelectedProjection(const HeldRegularFile& selected) const {
        if (selected_projection_path.empty()) {
            fail(ErrorCode::kIdentityDrift, "selected projection was not bound");
        }
        selected.proveStable();
        const FileIdentity* expected = leafIdentity(selected_projection_path);
        const FileIdentity& actual = selected.identity();
        if (expected == nullptr || actual.role != expected->role ||
            actual.path != selected_projection_path ||
            actual.sha256 != expected->sha256 ||
            actual.object_sha256 != expected->object_sha256 ||
            actual.bytes != expected->bytes || actual.mode != expected->mode ||
            actual.nlink != expected->nlink ||
            actual.file_type != expected->file_type ||
            actual.device != expected->device || actual.inode != expected->inode) {
            fail(
                ErrorCode::kIdentityDrift,
                "current selected projection differs from held authority leaf");
        }
    }
};

ProjectionAuthorityGuard::ProjectionAuthorityGuard() = default;
ProjectionAuthorityGuard::ProjectionAuthorityGuard(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
ProjectionAuthorityGuard::~ProjectionAuthorityGuard() = default;
ProjectionAuthorityGuard::ProjectionAuthorityGuard(ProjectionAuthorityGuard&&) noexcept = default;
ProjectionAuthorityGuard& ProjectionAuthorityGuard::operator=(ProjectionAuthorityGuard&&) noexcept = default;

void ProjectionAuthorityGuard::proveStable() const {
    if (!impl_) fail(ErrorCode::kIdentityDrift, "empty projection authority guard");
    impl_->prove();
}

const FileIdentity& ProjectionAuthorityGuard::receiptIdentity() const noexcept {
    static const FileIdentity empty;
    return impl_ ? impl_->receipt.identity() : empty;
}

ProjectionAuthorityGuard ProjectionAuthorityGuard::open(
    const RepositoryAnchor& repository) {
    auto impl = std::make_unique<ProjectionAuthorityGuard::Impl>();
    impl->directories.push_back(openHeldDirectory(
        repository,
        "benchmark/icde2027_e3_e4_shared_inputs_v1",
        {"query_projection_authority_receipt_v1.json", "query_projections"},
        3));
    impl->directories.push_back(openHeldDirectory(
        repository,
        "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections",
        {"bitcoin", "sx_stackoverflow", "sx_superuser", "wiki_talk"},
        6));
    impl->directories.push_back(openHeldDirectory(
        repository,
        "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/bitcoin",
        {"cycle3_projection.json", "path3_projection.json", "path4_projection.json",
         "path5_projection.json", "tailed4_projection.json"},
        2));
    impl->directories.push_back(openHeldDirectory(
        repository,
        "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/sx_stackoverflow",
        {"path3_projection.json"},
        2));
    impl->directories.push_back(openHeldDirectory(
        repository,
        "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/sx_superuser",
        {"path3_projection.json", "path4_projection.json", "star5_projection.json"},
        2));
    impl->directories.push_back(openHeldDirectory(
        repository,
        "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/wiki_talk",
        {"path3_projection.json"},
        2));

    const auto bindDirectoryPrefixes = [&impl](
        const HeldDirectory& canonical,
        const HeldDirectory& other,
        std::size_t prefix_size) {
        if (canonical.fds.size() < prefix_size ||
            other.fds.size() < prefix_size) {
            fail(ErrorCode::kIdentityDrift, "authority directory chain is short");
        }
        for (std::size_t index = 0; index < prefix_size; ++index) {
            if (!sameAnchor(canonical.identities[index], other.identities[index])) {
                fail(
                    ErrorCode::kIdentityDrift,
                    "authority directories came from different tree generations");
            }
            impl->directory_bindings.push_back({
                canonical.fds[index],
                other.fds[index],
                canonical.identities[index],
            });
        }
    };
    const HeldDirectory& root_directory = impl->directories.at(0);
    const HeldDirectory& query_projection_directory = impl->directories.at(1);
    bindDirectoryPrefixes(root_directory, query_projection_directory, 3U);
    for (std::size_t index = 2; index < impl->directories.size(); ++index) {
        bindDirectoryPrefixes(
            query_projection_directory, impl->directories[index], 4U);
    }

    const auto bindFileChain = [&impl](
        const HeldDirectory& canonical,
        const HeldRegularFile& file) {
        file.proveStable();
        if (file.heldDirectoryCount() != canonical.fds.size()) {
            fail(ErrorCode::kIdentityDrift, "authority file parent chain is incomplete");
        }
        for (std::size_t index = 0; index < canonical.fds.size(); ++index) {
            const int file_directory_fd = file.heldDirectoryFd(index);
            const StatSnapshot file_directory = fstatSnapshot(
                file_directory_fd, "authority file parent chain");
            if (!sameAnchor(file_directory, canonical.identities[index])) {
                fail(
                    ErrorCode::kIdentityDrift,
                    "authority file opened from a different tree generation");
            }
            impl->directory_bindings.push_back({
                canonical.fds[index],
                file_directory_fd,
                canonical.identities[index],
            });
        }
    };

    impl->receipt = openExpectedRegularFile(
        repository,
        expectedFile(
            "query_projection_authority_receipt",
            std::string(kProjectionAuthorityReceiptPath),
            std::string(kProjectionAuthorityReceiptSha256),
            kProjectionAuthorityReceiptBytes,
            0644,
            std::string(kProjectionAuthorityReceiptObjectSha256)));
    bindFileChain(root_directory, impl->receipt);

    for (const CorpusRow& row : corpusRegistry()) {
        impl->projection_leaves.emplace_back(
            row.projection.path,
            openExpectedRegularFile(repository, row.projection));
        const HeldDirectory* dataset_directory = nullptr;
        if (row.dataset_id == "bitcoin") {
            dataset_directory = &impl->directories.at(2);
        } else if (row.dataset_id == "sx_stackoverflow") {
            dataset_directory = &impl->directories.at(3);
        } else if (row.dataset_id == "sx_superuser") {
            dataset_directory = &impl->directories.at(4);
        } else if (row.dataset_id == "wiki_talk") {
            dataset_directory = &impl->directories.at(5);
        }
        if (dataset_directory == nullptr) {
            fail(ErrorCode::kCrossBindingMismatch, "unknown authority dataset");
        }
        bindFileChain(*dataset_directory, impl->projection_leaves.back().second);
    }
    impl->prove();
    return ProjectionAuthorityGuard(std::move(impl));
}

namespace {

std::uint64_t elapsed(std::uint64_t begin, std::uint64_t end, const char* context) {
    if (end < begin) fail(ErrorCode::kClockRegression, std::string(context) + " clock regression");
    return end - begin;
}

}  // namespace

std::uint64_t steadyClockNowNanoseconds() {
    using Clock = std::chrono::steady_clock;
    const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
    if (value < 0) fail(ErrorCode::kClockRegression, "steady clock is negative");
    return static_cast<std::uint64_t>(value);
}

ValidatedInputs loadValidatedInputs(
    const RepositoryAnchor& repository,
    const ClosedInvocation& invocation) {
    if (repository.absolutePath() != kCanonicalRepositoryRoot) {
        fail(
            ErrorCode::kCrossBindingMismatch,
            "production input loader requires the canonical repository root");
    }
    std::vector<std::string> replay_argv{"e3-e4-adapter-v1"};
    replay_argv.insert(
        replay_argv.end(),
        invocation.exact_argv_without_program.begin(),
        invocation.exact_argv_without_program.end());
    std::vector<const char*> replay_pointers;
    replay_pointers.reserve(replay_argv.size());
    for (const std::string& token : replay_argv) {
        replay_pointers.push_back(token.c_str());
    }
    const ClosedInvocation validated = parseClosedInvocation(
        static_cast<int>(replay_pointers.size()),
        replay_pointers.data(),
        invocation.arm);
    if (validated.prepared_graph_path != invocation.prepared_graph_path ||
        validated.explicit_label_map_path != invocation.explicit_label_map_path ||
        validated.query_projection_path != invocation.query_projection_path ||
        validated.query_path != invocation.query_path ||
        validated.minimum_duration_k != invocation.minimum_duration_k ||
        validated.threads != invocation.threads ||
        validated.count_only != invocation.count_only ||
        validated.child_phase_receipt_path !=
            invocation.child_phase_receipt_path ||
        validated.typed_plan_path != invocation.typed_plan_path ||
        validated.exact_argv_without_program !=
            invocation.exact_argv_without_program ||
        validated.exact_argv_object_sha256 !=
            invocation.exact_argv_object_sha256) {
        fail(ErrorCode::kInvalidInvocation, "ClosedInvocation was mutated after parsing");
    }
    const CorpusRow& row = selectCorpusRow(validated);
    ValidatedInputs result;

    std::uint64_t begin = steadyClockNowNanoseconds();
    result.prepared_graph = openExpectedRegularFile(repository, row.graph);
    result.raw_temporal_edges =
        parseRawTemporalGraphBytes(result.prepared_graph.bytes());
    std::uint64_t end = steadyClockNowNanoseconds();
    result.loads.prepared_graph_load_ns = elapsed(begin, end, "graph load");

    begin = steadyClockNowNanoseconds();
    result.explicit_label_map = openExpectedRegularFile(repository, row.map);
    const ParsedExplicitLabelMap label_map =
        parseExplicitLabelMapBytes(result.explicit_label_map.bytes());
    if (label_map.mapping_sha256 != row.mapping_sha256) {
        fail(ErrorCode::kHashMismatch, "registry mapping H0 mismatch");
    }
    end = steadyClockNowNanoseconds();
    result.loads.explicit_label_map_load_ns = elapsed(begin, end, "map load");

    begin = steadyClockNowNanoseconds();
    result.projection_authority = ProjectionAuthorityGuard::open(repository);
    result.query_projection = openExpectedRegularFile(repository, row.projection);
    const FileIdentity* authority_leaf =
        result.projection_authority.impl_->leafIdentity(row.projection.path);
    const HeldRegularFile* authority_file =
        result.projection_authority.impl_->leafFile(row.projection.path);
    if (authority_leaf == nullptr ||
        authority_file == nullptr ||
        authority_leaf->device != result.query_projection.identity().device ||
        authority_leaf->inode != result.query_projection.identity().inode ||
        authority_leaf->sha256 != result.query_projection.identity().sha256) {
        fail(ErrorCode::kIdentityDrift, "selected projection differs from held authority leaf");
    }
    authority_file->proveStable();
    result.query_projection.proveStable();
    if (authority_file->heldDirectoryCount() !=
            result.query_projection.heldDirectoryCount()) {
        fail(ErrorCode::kIdentityDrift, "selected projection parent chain is incomplete");
    }
    for (std::size_t index = 0;
         index < authority_file->heldDirectoryCount(); ++index) {
        const int authority_directory_fd = authority_file->heldDirectoryFd(index);
        const int selected_directory_fd =
            result.query_projection.heldDirectoryFd(index);
        const StatSnapshot expected_parent = fstatSnapshot(
            authority_directory_fd, "selected authority parent chain");
        const StatSnapshot selected_parent = fstatSnapshot(
            selected_directory_fd, "selected projection parent chain");
        if (!sameAnchor(expected_parent, selected_parent)) {
            fail(
                ErrorCode::kIdentityDrift,
                "selected projection came from a different authority tree");
        }
    }
    result.projection_authority.impl_->selected_projection_path =
        row.projection.path;
    const ParsedQueryProjection projection =
        parseQueryProjectionBytes(result.query_projection.bytes());
    if (projection.global_label_map_file_sha256 != row.map.sha256 ||
        projection.global_label_map_mapping_sha256 != row.mapping_sha256) {
        fail(ErrorCode::kCrossBindingMismatch, "projection registry map binding mismatch");
    }
    end = steadyClockNowNanoseconds();
    result.loads.query_projection_load_ns = elapsed(begin, end, "projection load");

    begin = steadyClockNowNanoseconds();
    result.query = openExpectedRegularFile(repository, row.query);
    result.parsed_query = parseQueryBytes(result.query.bytes());
    result.runtime_labels_by_external_id = validateCoverageAndProjectLabels(
        result.raw_temporal_edges,
        label_map,
        projection,
        result.explicit_label_map.identity().sha256,
        result.query.identity().sha256);
    result.global_label_map_mapping_sha256 = label_map.mapping_sha256;
    end = steadyClockNowNanoseconds();
    result.loads.query_load_ns = elapsed(begin, end, "query load");
    result.loads.all_completed_before_boundary = true;
    result.proveStable();
    repository.proveStable();
    return result;
}

void ValidatedInputs::proveStable() const {
    projection_authority.proveStable();
    prepared_graph.proveStable();
    explicit_label_map.proveStable();
    if (!projection_authority.impl_) {
        fail(ErrorCode::kIdentityDrift, "empty projection authority guard");
    }
    projection_authority.impl_->proveSelectedProjection(query_projection);
    query.proveStable();
}

}  // namespace dsm::e3e4::adapter_v1

namespace dsm::e3e4::adapter_v1 {
namespace {

StatSnapshot snapshot(const struct stat& info) {
    if (info.st_size < 0) fail(ErrorCode::kSizeDrift, "negative file size");
    StatSnapshot result;
    result.device = static_cast<std::uint64_t>(info.st_dev);
    result.inode = static_cast<std::uint64_t>(info.st_ino);
    result.size = static_cast<std::uint64_t>(info.st_size);
    result.nlink = static_cast<std::uint64_t>(info.st_nlink);
    result.mode = static_cast<std::uint32_t>(info.st_mode);
    result.uid = static_cast<std::uint32_t>(info.st_uid);
    result.gid = static_cast<std::uint32_t>(info.st_gid);
#if defined(__APPLE__)
    result.mtime_sec = info.st_mtimespec.tv_sec;
    result.mtime_nsec = info.st_mtimespec.tv_nsec;
    result.ctime_sec = info.st_ctimespec.tv_sec;
    result.ctime_nsec = info.st_ctimespec.tv_nsec;
#else
    result.mtime_sec = info.st_mtim.tv_sec;
    result.mtime_nsec = info.st_mtim.tv_nsec;
    result.ctime_sec = info.st_ctim.tv_sec;
    result.ctime_nsec = info.st_ctim.tv_nsec;
#endif
    return result;
}

StatSnapshot fstatSnapshot(int fd, const std::string& context) {
    struct stat info {};
    if (::fstat(fd, &info) != 0) fail(ErrorCode::kIdentityDrift, errnoMessage(context));
    return snapshot(info);
}

StatSnapshot statAtSnapshot(int parent_fd, const std::string& name, const std::string& context) {
    struct stat info {};
    if (::fstatat(parent_fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        fail(ErrorCode::kIdentityDrift, errnoMessage(context));
    }
    return snapshot(info);
}

bool sameAnchor(const StatSnapshot& lhs, const StatSnapshot& rhs) {
    return lhs.device == rhs.device && lhs.inode == rhs.inode &&
        (lhs.mode & S_IFMT) == (rhs.mode & S_IFMT) &&
        (lhs.mode & 07777U) == (rhs.mode & 07777U) && lhs.uid == rhs.uid &&
        lhs.gid == rhs.gid;
}

bool sameLeaf(const StatSnapshot& lhs, const StatSnapshot& rhs) {
    return sameAnchor(lhs, rhs) && lhs.size == rhs.size && lhs.nlink == rhs.nlink &&
        lhs.mtime_sec == rhs.mtime_sec && lhs.mtime_nsec == rhs.mtime_nsec &&
        lhs.ctime_sec == rhs.ctime_sec && lhs.ctime_nsec == rhs.ctime_nsec;
}

void closeAll(std::vector<int>& fds) noexcept {
    for (auto iterator = fds.rbegin(); iterator != fds.rend(); ++iterator) {
        if (*iterator >= 0) {
            const int saved = errno;
            static_cast<void>(::close(*iterator));
            errno = saved;
        }
    }
    fds.clear();
}

std::vector<std::string> splitAbsolutePath(
    const std::string& path,
    const std::string& context) {
    if (path.empty() || path.front() != '/' || path.find('\0') != std::string::npos ||
        (path.size() > 1U && path.back() == '/')) {
        fail(ErrorCode::kUnsafePath, context + " must be normalized absolute path");
    }
    if (path == "/") return {};
    return splitRelativePath(std::string_view(path).substr(1U), context);
}

std::vector<std::string> directoryNamesBounded(int fd, const std::string& context) {
    const StatSnapshot original = fstatSnapshot(fd, context + " original fstat");
    const int duplicate = ::openat(
        fd, ".", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (duplicate < 0) {
        fail(ErrorCode::kIdentityDrift, errnoMessage(context + " reopen"));
    }
    const StatSnapshot reopened =
        fstatSnapshot(duplicate, context + " reopened fstat");
    if (!sameAnchor(original, reopened)) {
        static_cast<void>(::close(duplicate));
        fail(ErrorCode::kIdentityDrift, context + " directory self-reopen drift");
    }
    DIR* directory = ::fdopendir(duplicate);
    if (directory == nullptr) {
        const int saved = errno;
        ::close(duplicate);
        errno = saved;
        fail(ErrorCode::kIdentityDrift, errnoMessage(context + " fdopendir"));
    }
    std::vector<std::string> names;
    errno = 0;
    for (;;) {
        dirent* entry = ::readdir(directory);
        if (entry == nullptr) break;
        const std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        if (names.size() >= 64U) {
            ::closedir(directory);
            fail(ErrorCode::kIdentityDrift, context + " exceeds bounded inventory");
        }
        names.push_back(name);
    }
    const int read_error = errno;
    if (::closedir(directory) != 0 && read_error == 0) {
        fail(ErrorCode::kIdentityDrift, errnoMessage(context + " closedir"));
    }
    if (read_error != 0) {
        errno = read_error;
        fail(ErrorCode::kIdentityDrift, errnoMessage(context + " readdir"));
    }
    std::sort(names.begin(), names.end());
    if (std::adjacent_find(names.begin(), names.end()) != names.end()) {
        fail(ErrorCode::kIdentityDrift, context + " duplicate directory entry");
    }
    return names;
}

int openDirectoryAt(int parent_fd, const std::string& name, const std::string& context) {
    const StatSnapshot lexical = statAtSnapshot(parent_fd, name, context + " lstat");
    if (!S_ISDIR(static_cast<mode_t>(lexical.mode))) {
        fail(ErrorCode::kWrongFileType, context + " is not directory");
    }
    const int fd = ::openat(
        parent_fd, name.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) fail(ErrorCode::kOpenFailed, errnoMessage(context + " openat"));
    const StatSnapshot held = fstatSnapshot(fd, context + " fstat");
    if (!sameAnchor(lexical, held)) {
        ::close(fd);
        fail(ErrorCode::kIdentityDrift, context + " changed during directory open");
    }
    return fd;
}

std::string readExactFile(int fd, std::uint64_t expected_bytes, const std::string& context) {
    if (expected_bytes > static_cast<std::uint64_t>(
                             std::numeric_limits<std::size_t>::max())) {
        fail(ErrorCode::kSizeDrift, context + " does not fit address space");
    }
    if (::lseek(fd, 0, SEEK_SET) < 0) {
        fail(ErrorCode::kOpenFailed, errnoMessage(context + " lseek"));
    }
    std::string output(static_cast<std::size_t>(expected_bytes), '\0');
    std::size_t offset = 0;
    while (offset < output.size()) {
        const ssize_t count = ::read(fd, output.data() + offset, output.size() - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            fail(ErrorCode::kOpenFailed, errnoMessage(context + " read"));
        }
        if (count == 0) fail(ErrorCode::kSizeDrift, context + " short read");
        offset += static_cast<std::size_t>(count);
    }
    char extra = 0;
    for (;;) {
        const ssize_t count = ::read(fd, &extra, 1);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) fail(ErrorCode::kOpenFailed, errnoMessage(context + " eof read"));
        if (count != 0) fail(ErrorCode::kSizeDrift, context + " grew during read");
        break;
    }
    return output;
}

}  // namespace

struct RepositoryAnchor::Impl final {
    std::string absolute_path;
    std::vector<int> fds;
    std::vector<std::string> component_names;
    std::vector<StatSnapshot> identities;

    ~Impl() { closeAll(fds); }

    void prove() const {
        if (fds.empty() || fds.size() != identities.size() ||
            fds.size() != component_names.size() + 1U) {
            fail(ErrorCode::kIdentityDrift, "repository anchor state is invalid");
        }
        for (std::size_t index = 0; index < fds.size(); ++index) {
            const StatSnapshot held = fstatSnapshot(fds[index], "repository held fstat");
            if (!sameAnchor(held, identities[index])) {
                fail(ErrorCode::kIdentityDrift, "repository held directory drift");
            }
            if (index > 0U) {
                const StatSnapshot lexical = statAtSnapshot(
                    fds[index - 1U], component_names[index - 1U],
                    "repository lexical reattachment");
                if (!sameAnchor(lexical, identities[index])) {
                    fail(ErrorCode::kIdentityDrift, "repository directory detached");
                }
            }
        }
    }
};

RepositoryAnchor::RepositoryAnchor() = default;
RepositoryAnchor::RepositoryAnchor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
RepositoryAnchor::~RepositoryAnchor() = default;
RepositoryAnchor::RepositoryAnchor(RepositoryAnchor&&) noexcept = default;
RepositoryAnchor& RepositoryAnchor::operator=(RepositoryAnchor&&) noexcept = default;

RepositoryAnchor RepositoryAnchor::openAbsolute(const std::string& absolute_repo_root) {
    auto impl = std::make_unique<Impl>();
    impl->absolute_path = absolute_repo_root;
    const std::vector<std::string> components =
        splitAbsolutePath(absolute_repo_root, "repository root");
    int current = ::open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (current < 0) fail(ErrorCode::kOpenFailed, errnoMessage("open filesystem root"));
    impl->fds.push_back(current);
    impl->identities.push_back(fstatSnapshot(current, "filesystem root fstat"));
    for (const std::string& component : components) {
        const int child = openDirectoryAt(current, component, "repository component " + component);
        impl->component_names.push_back(component);
        impl->fds.push_back(child);
        impl->identities.push_back(fstatSnapshot(child, "repository component fstat"));
        current = child;
    }
    impl->prove();
    return RepositoryAnchor(std::move(impl));
}

void RepositoryAnchor::proveStable() const {
    if (!impl_) fail(ErrorCode::kIdentityDrift, "empty repository anchor");
    impl_->prove();
}

int RepositoryAnchor::fd() const noexcept {
    return impl_ && !impl_->fds.empty() ? impl_->fds.back() : -1;
}

const std::string& RepositoryAnchor::absolutePath() const noexcept {
    static const std::string empty;
    return impl_ ? impl_->absolute_path : empty;
}

struct HeldRegularFile::Impl final {
    ExpectedRegularFile expected;
    FileIdentity identity;
    std::string payload;
    std::vector<int> directory_fds;
    std::vector<std::string> directory_names;
    std::vector<StatSnapshot> directory_identities;
    std::string leaf_name;
    int leaf_fd = -1;
    StatSnapshot leaf_identity;

    ~Impl() {
        if (leaf_fd >= 0) {
            const int saved = errno;
            static_cast<void>(::close(leaf_fd));
            errno = saved;
        }
        closeAll(directory_fds);
    }

    void prove() const {
        if (directory_fds.empty() ||
            directory_fds.size() != directory_identities.size() ||
            directory_fds.size() != directory_names.size() + 1U ||
            leaf_fd < 0) {
            fail(ErrorCode::kIdentityDrift, "held file state is invalid");
        }
        for (std::size_t index = 0; index < directory_fds.size(); ++index) {
            const StatSnapshot held = fstatSnapshot(directory_fds[index], "held path directory");
            if (!sameAnchor(held, directory_identities[index])) {
                fail(ErrorCode::kIdentityDrift, "held path directory drift");
            }
            if (index > 0U) {
                const StatSnapshot lexical = statAtSnapshot(
                    directory_fds[index - 1U], directory_names[index - 1U],
                    "held path directory lexical proof");
                if (!sameAnchor(lexical, directory_identities[index])) {
                    fail(ErrorCode::kIdentityDrift, "held path directory detached");
                }
            }
        }
        const StatSnapshot held_leaf = fstatSnapshot(leaf_fd, "held leaf fstat");
        const StatSnapshot lexical_leaf = statAtSnapshot(
            directory_fds.back(), leaf_name, "held leaf lexical proof");
        if (!sameLeaf(held_leaf, leaf_identity) ||
            !sameLeaf(lexical_leaf, leaf_identity)) {
            fail(ErrorCode::kIdentityDrift, "held leaf changed or detached");
        }
    }
};

HeldRegularFile::HeldRegularFile() = default;
HeldRegularFile::HeldRegularFile(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
HeldRegularFile::~HeldRegularFile() = default;
HeldRegularFile::HeldRegularFile(HeldRegularFile&&) noexcept = default;
HeldRegularFile& HeldRegularFile::operator=(HeldRegularFile&&) noexcept = default;

void HeldRegularFile::proveStable() const {
    if (!impl_) fail(ErrorCode::kIdentityDrift, "empty held file");
    impl_->prove();
}

int HeldRegularFile::fd() const noexcept { return impl_ ? impl_->leaf_fd : -1; }

const FileIdentity& HeldRegularFile::identity() const noexcept {
    static const FileIdentity empty;
    return impl_ ? impl_->identity : empty;
}

const std::string& HeldRegularFile::bytes() const noexcept {
    static const std::string empty;
    return impl_ ? impl_->payload : empty;
}

std::size_t HeldRegularFile::heldDirectoryCount() const noexcept {
    return impl_ ? impl_->directory_fds.size() : 0U;
}

int HeldRegularFile::heldDirectoryFd(std::size_t index) const noexcept {
    if (!impl_ || index >= impl_->directory_fds.size()) return -1;
    return impl_->directory_fds[index];
}

HeldRegularFile openExpectedRegularFile(
    const RepositoryAnchor& repository,
    const ExpectedRegularFile& expected) {
    if (!isLowerSha256(expected.sha256) || expected.nlink != 1U ||
        (expected.mode & ~07777U) != 0U) {
        fail(ErrorCode::kSchemaMismatch, "invalid expected regular-file identity");
    }
    if (expected.object_sha256.has_value() &&
        !isLowerSha256(*expected.object_sha256)) {
        fail(ErrorCode::kSchemaMismatch, "invalid expected object H0");
    }
    repository.proveStable();
    const std::vector<std::string> components =
        splitRelativePath(expected.path, expected.role);
    if (components.empty()) fail(ErrorCode::kUnsafePath, "empty regular-file path");

    auto impl = std::make_unique<HeldRegularFile::Impl>();
    impl->expected = expected;
    const int root_duplicate = ::fcntl(repository.fd(), F_DUPFD_CLOEXEC, 3);
    if (root_duplicate < 0) fail(ErrorCode::kOpenFailed, errnoMessage("dup repository root"));
    impl->directory_fds.push_back(root_duplicate);
    impl->directory_identities.push_back(fstatSnapshot(root_duplicate, "held repo duplicate"));
    int current = root_duplicate;
    for (std::size_t index = 0; index + 1U < components.size(); ++index) {
        const int child = openDirectoryAt(current, components[index], expected.role + " directory");
        impl->directory_names.push_back(components[index]);
        impl->directory_fds.push_back(child);
        impl->directory_identities.push_back(fstatSnapshot(child, expected.role + " directory fstat"));
        current = child;
    }
    impl->leaf_name = components.back();
    const StatSnapshot lexical = statAtSnapshot(current, impl->leaf_name, expected.role + " lstat");
    if (!S_ISREG(static_cast<mode_t>(lexical.mode))) {
        fail(ErrorCode::kWrongFileType, expected.role + " is not regular");
    }
    if ((lexical.mode & 07777U) != expected.mode) {
        fail(ErrorCode::kWrongFileMode, expected.role + " mode mismatch");
    }
    if (lexical.nlink != expected.nlink) {
        fail(ErrorCode::kWrongLinkCount, expected.role + " nlink mismatch");
    }
    if (lexical.size != expected.bytes) {
        fail(ErrorCode::kSizeDrift, expected.role + " size mismatch");
    }
    impl->leaf_fd = ::openat(
        current, impl->leaf_name.c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (impl->leaf_fd < 0) fail(ErrorCode::kOpenFailed, errnoMessage(expected.role + " openat"));
    impl->leaf_identity = fstatSnapshot(impl->leaf_fd, expected.role + " fstat");
    if (!sameLeaf(lexical, impl->leaf_identity)) {
        fail(ErrorCode::kIdentityDrift, expected.role + " changed during open");
    }
    impl->payload = readExactFile(impl->leaf_fd, expected.bytes, expected.role);
    const StatSnapshot after = fstatSnapshot(impl->leaf_fd, expected.role + " final fstat");
    if (!sameLeaf(after, impl->leaf_identity)) {
        fail(ErrorCode::kIdentityDrift, expected.role + " changed during read");
    }
    if (sha256Hex(impl->payload) != expected.sha256) {
        fail(ErrorCode::kHashMismatch, expected.role + " H1 mismatch");
    }
    if (expected.object_sha256.has_value()) {
        const JsonValue parsed = JsonParser(impl->payload).parse();
        if (sha256Hex(canonicalJson(parsed)) != *expected.object_sha256) {
            fail(ErrorCode::kHashMismatch, expected.role + " H0 mismatch");
        }
    }
    impl->identity = {
        expected.role,
        expected.path,
        expected.sha256,
        expected.bytes,
        expected.mode,
        expected.nlink,
        "regular",
        impl->leaf_identity.device,
        impl->leaf_identity.inode,
        expected.object_sha256,
    };
    impl->prove();
    repository.proveStable();
    return HeldRegularFile(std::move(impl));
}

}  // namespace dsm::e3e4::adapter_v1

namespace dsm::e3e4::adapter_v1 {
namespace {

std::vector<std::string_view> whitespaceTokens(std::string_view line) {
    std::vector<std::string_view> output;
    std::size_t position = 0;
    while (position < line.size()) {
        while (position < line.size() &&
               (line[position] == ' ' || line[position] == '\t' ||
                line[position] == '\r')) {
            ++position;
        }
        if (position == line.size()) break;
        const std::size_t begin = position;
        while (position < line.size() && line[position] != ' ' &&
               line[position] != '\t' && line[position] != '\r') {
            ++position;
        }
        output.push_back(line.substr(begin, position - begin));
    }
    return output;
}

std::int32_t parseNonnegativeInt32(
    std::string_view token,
    ErrorCode code,
    const std::string& context) {
    if (token.empty() || token.front() == '+' ||
        (token.size() > 1U && token.front() == '0')) {
        fail(code, context + " is not canonical nonnegative int32");
    }
    std::int64_t parsed = 0;
    const auto result = std::from_chars(
        token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size() ||
        parsed < 0 || parsed > std::numeric_limits<std::int32_t>::max()) {
        fail(code, context + " is outside nonnegative int32");
    }
    return static_cast<std::int32_t>(parsed);
}

std::int32_t parseSignedInt32(
    std::string_view token,
    ErrorCode code,
    const std::string& context) {
    if (token.empty() || token.front() == '+' || token == "-0") {
        fail(code, context + " is not canonical signed int32");
    }
    const std::size_t digit_begin = token.front() == '-' ? 1U : 0U;
    if (digit_begin == token.size() ||
        (token.size() - digit_begin > 1U && token[digit_begin] == '0')) {
        fail(code, context + " is not canonical signed int32");
    }
    std::int64_t parsed = 0;
    const auto result = std::from_chars(
        token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size() ||
        parsed < std::numeric_limits<std::int32_t>::min() ||
        parsed > std::numeric_limits<std::int32_t>::max()) {
        fail(code, context + " is outside signed int32");
    }
    return static_cast<std::int32_t>(parsed);
}

std::pair<std::string, std::uint8_t> parseQueryEndpoint(std::string_view token) {
    const std::size_t colon = token.find(':');
    if (colon == std::string_view::npos || colon == 0U ||
        colon + 2U != token.size()) {
        fail(ErrorCode::kInvalidQuery, "query endpoint must be name:A-through-E");
    }
    const std::string_view name = token.substr(0, colon);
    for (const char ch : name) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_')) {
            fail(ErrorCode::kInvalidQuery, "query vertex name is not portable ASCII");
        }
    }
    const char label = token[colon + 1U];
    if (label < 'A' || label > 'E') {
        fail(ErrorCode::kInvalidQuery, "query label is outside A-through-E");
    }
    return {std::string(name), static_cast<std::uint8_t>(label - 'A')};
}

std::string digestObjectValue(const JsonValue& value, const std::string& context) {
    const auto& object = asObject(value, context);
    exactKeys(object, {"algorithm", "value"}, context);
    if (asString(member(object, "algorithm", context), context + ".algorithm") !=
        "sha256") {
        fail(ErrorCode::kSchemaMismatch, context + " algorithm must be sha256");
    }
    const std::string result =
        asString(member(object, "value", context), context + ".value");
    if (!isLowerSha256(result)) {
        fail(ErrorCode::kSchemaMismatch, context + " value is not lowercase sha256");
    }
    return result;
}

}  // namespace

std::vector<RawTemporalEdge> parseRawTemporalGraphBytes(std::string_view bytes) {
    if (!validUtf8(bytes)) {
        fail(ErrorCode::kInvalidUtf8, "graph bytes are not UTF-8/ASCII");
    }
    std::vector<RawTemporalEdge> output;
    std::size_t begin = 0;
    std::uint64_t line_number = 0;
    while (begin <= bytes.size()) {
        const std::size_t newline = bytes.find('\n', begin);
        const std::size_t end = newline == std::string_view::npos ? bytes.size() : newline;
        std::string_view line = bytes.substr(begin, end - begin);
        ++line_number;
        const std::vector<std::string_view> tokens = whitespaceTokens(line);
        if (!tokens.empty() && tokens.front().front() != '#') {
            if (tokens.size() != 3U) {
                fail(
                    ErrorCode::kInvalidGraph,
                    "graph line " + std::to_string(line_number) +
                        " must contain exactly three integers");
            }
            RawTemporalEdge edge;
            edge.source = parseNonnegativeInt32(
                tokens[0], ErrorCode::kInvalidGraph, "graph source");
            edge.target = parseNonnegativeInt32(
                tokens[1], ErrorCode::kInvalidGraph, "graph target");
            edge.timestamp = parseSignedInt32(
                tokens[2], ErrorCode::kInvalidGraph, "graph timestamp");
            output.push_back(edge);
        }
        if (newline == std::string_view::npos) break;
        begin = newline + 1U;
    }
    if (output.empty()) fail(ErrorCode::kInvalidGraph, "graph has no edge rows");
    return output;
}

ParsedExplicitLabelMap parseExplicitLabelMapBytes(std::string_view bytes) {
    const JsonValue root = JsonParser(bytes).parse();
    const auto& object = asObject(root, "global label map");
    exactKeys(
        object,
        {"artifact_kind", "classes", "mapping", "mapping_digest",
         "normalized_entropy", "schema_version", "scheme", "vertex_count"},
        "global label map");
    if (asString(member(object, "artifact_kind", "global label map"),
                 "artifact_kind") != "dsm-explicit-vertex-label-map" ||
        nonnegativeUint64(member(object, "schema_version", "global label map"),
                          "schema_version") != 1U) {
        fail(ErrorCode::kSchemaMismatch, "unsupported global label map schema");
    }

    const auto& scheme = asObject(
        member(object, "scheme", "global label map"), "scheme");
    exactKeys(scheme, {"id", "label_count", "parameters", "seed"}, "scheme");
    if (asString(member(scheme, "id", "scheme"), "scheme.id") !=
            "stable-id-splitmix64-v1" ||
        nonnegativeUint64(member(scheme, "label_count", "scheme"),
                          "scheme.label_count") != 5U ||
        nonnegativeUint64(member(scheme, "seed", "scheme"), "scheme.seed") != 42U) {
        fail(ErrorCode::kSchemaMismatch, "global label map scheme drift");
    }
    const auto& parameters = asObject(
        member(scheme, "parameters", "scheme"), "scheme.parameters");
    exactKeys(
        parameters,
        {"assignment", "control_role", "vertex_set_invariant"},
        "scheme.parameters");
    if (asString(member(parameters, "control_role", "scheme.parameters"),
                 "control_role") != "uniform-stable-id" ||
        !asBoolean(member(parameters, "vertex_set_invariant", "scheme.parameters"),
                   "vertex_set_invariant") ||
        asString(member(parameters, "assignment", "scheme.parameters"),
                 "assignment") !=
            "splitmix64((uint32(seed) << 32) | uint32(vertex_id)) mod label_count") {
        fail(ErrorCode::kSchemaMismatch, "global label map scheme parameters drift");
    }

    ParsedExplicitLabelMap result;
    result.global_label_cardinality = 5;
    const auto& mapping = asArray(
        member(object, "mapping", "global label map"), "mapping");
    const std::uint64_t vertex_count = nonnegativeUint64(
        member(object, "vertex_count", "global label map"), "vertex_count");
    if (vertex_count == 0U || mapping.size() != vertex_count) {
        fail(ErrorCode::kSchemaMismatch, "mapping size differs from vertex_count");
    }
    result.mapping.reserve(mapping.size());
    std::array<std::uint64_t, kRuntimeLabelCount> class_counts{};
    std::int32_t previous = -1;
    for (const JsonValue& row_value : mapping) {
        const auto& row = asArray(row_value, "mapping row");
        if (row.size() != 2U) fail(ErrorCode::kSchemaMismatch, "mapping row must have two integers");
        const std::uint32_t vertex = boundedUint32(
            row[0], static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()),
            "mapping vertex");
        const std::uint32_t global_class = boundedUint32(row[1], 4U, "mapping class");
        if (static_cast<std::int32_t>(vertex) <= previous) {
            fail(ErrorCode::kSchemaMismatch, "mapping vertices are not strictly increasing");
        }
        previous = static_cast<std::int32_t>(vertex);
        result.mapping.push_back({previous, global_class});
        ++class_counts[global_class];
    }

    const auto& classes = asArray(
        member(object, "classes", "global label map"), "classes");
    if (classes.size() != 5U) fail(ErrorCode::kSchemaMismatch, "classes must contain five rows");
    std::array<bool, 5> class_seen{};
    for (const JsonValue& class_value : classes) {
        const auto& row = asObject(class_value, "class row");
        exactKeys(row, {"global_class_id", "vertex_count"}, "class row");
        const std::uint32_t global_class = boundedUint32(
            member(row, "global_class_id", "class row"), 4U, "global_class_id");
        if (class_seen[global_class]) fail(ErrorCode::kSchemaMismatch, "duplicate class row");
        class_seen[global_class] = true;
        if (nonnegativeUint64(member(row, "vertex_count", "class row"),
                              "class vertex count") != class_counts[global_class]) {
            fail(ErrorCode::kSchemaMismatch, "declared class count drift");
        }
    }

    const std::string declared_digest = digestObjectValue(
        member(object, "mapping_digest", "global label map"), "mapping_digest");
    Sha256 mapping_digest;
    mapping_digest.update("dsm-explicit-label-map-v1\t5\n");
    for (const GlobalLabelRow& row : result.mapping) {
        mapping_digest.update(
            std::to_string(row.external_vertex_id) + "\t" +
            std::to_string(row.global_class_id) + "\n");
    }
    result.mapping_sha256 = mapping_digest.finishHex();
    if (result.mapping_sha256 != declared_digest) {
        fail(ErrorCode::kHashMismatch, "canonical global mapping digest mismatch");
    }

    double entropy = 0.0;
    for (const std::uint64_t count : class_counts) {
        if (count != 0U) {
            const double probability =
                static_cast<double>(count) / static_cast<double>(vertex_count);
            entropy -= probability * std::log(probability);
        }
    }
    entropy /= std::log(5.0);
    const double declared_entropy = asNumber(
        member(object, "normalized_entropy", "global label map"),
        "normalized_entropy");
    if (declared_entropy < 0.0 || declared_entropy > 1.0 ||
        std::abs(declared_entropy - entropy) > 1e-12) {
        fail(ErrorCode::kSchemaMismatch, "normalized entropy drift");
    }
    return result;
}

ParsedQueryProjection parseQueryProjectionBytes(std::string_view bytes) {
    const JsonValue root = JsonParser(bytes).parse();
    const auto& object = asObject(root, "query projection");
    exactKeys(
        object,
        {"artifact_kind", "global_label_cardinality",
         "global_label_map_file_sha256", "global_label_map_mapping_digest",
         "invalid_runtime_class_id", "local_slots",
         "nonselected_global_class_policy", "query_sha256",
         "runtime_slot_count", "schema_version",
         "selected_global_classes_in_A_to_E_order"},
        "query projection");
    if (asString(member(object, "artifact_kind", "query projection"),
                 "artifact_kind") != "dsm-query-label-projection" ||
        nonnegativeUint64(member(object, "schema_version", "query projection"),
                          "schema_version") != 1U ||
        nonnegativeUint64(member(object, "global_label_cardinality", "query projection"),
                          "global_label_cardinality") != 5U ||
        nonnegativeUint64(member(object, "runtime_slot_count", "query projection"),
                          "runtime_slot_count") != 5U ||
        asInteger(member(object, "invalid_runtime_class_id", "query projection"),
                  "invalid_runtime_class_id") != -1 ||
        asString(member(object, "nonselected_global_class_policy", "query projection"),
                 "nonselected_global_class_policy") != "invalid") {
        fail(ErrorCode::kSchemaMismatch, "query projection fixed controls drift");
    }

    ParsedQueryProjection result;
    result.global_label_cardinality = 5;
    result.global_label_map_file_sha256 = asString(
        member(object, "global_label_map_file_sha256", "query projection"),
        "global_label_map_file_sha256");
    result.query_sha256 = asString(
        member(object, "query_sha256", "query projection"), "query_sha256");
    if (!isLowerSha256(result.global_label_map_file_sha256) ||
        !isLowerSha256(result.query_sha256)) {
        fail(ErrorCode::kSchemaMismatch, "projection file H1 fields are invalid");
    }
    result.global_label_map_mapping_sha256 = digestObjectValue(
        member(object, "global_label_map_mapping_digest", "query projection"),
        "global_label_map_mapping_digest");

    const auto& selected = asArray(
        member(object, "selected_global_classes_in_A_to_E_order", "query projection"),
        "selected_global_classes_in_A_to_E_order");
    if (selected.size() != 5U) {
        fail(ErrorCode::kSchemaMismatch, "E3/E4 projection must expose all five slots");
    }
    for (std::size_t index = 0; index < selected.size(); ++index) {
        const std::uint32_t global_class = boundedUint32(
            selected[index], 4U, "selected global class");
        if (global_class != index) {
            fail(ErrorCode::kSchemaMismatch, "projection must be exact identity classes 0..4");
        }
        result.selected_global_classes_in_a_to_e_order[index] = global_class;
    }
    const auto& slots = asArray(
        member(object, "local_slots", "query projection"), "local_slots");
    if (slots.size() != 5U) fail(ErrorCode::kSchemaMismatch, "projection must have five local slots");
    for (std::size_t index = 0; index < slots.size(); ++index) {
        const auto& slot = asObject(slots[index], "local slot");
        exactKeys(
            slot,
            {"global_class_id", "runtime_class_id", "runtime_label"},
            "local slot");
        const std::string expected_label(
            1, static_cast<char>('A' + static_cast<int>(index)));
        if (boundedUint32(member(slot, "global_class_id", "local slot"), 4U,
                          "global_class_id") != index ||
            boundedUint32(member(slot, "runtime_class_id", "local slot"), 4U,
                          "runtime_class_id") != index ||
            asString(member(slot, "runtime_label", "local slot"),
                     "runtime_label") != expected_label) {
            fail(ErrorCode::kSchemaMismatch, "local slot is not exact A-through-E identity");
        }
    }
    return result;
}

ParsedQuery parseQueryBytes(std::string_view bytes) {
    if (!validUtf8(bytes)) fail(ErrorCode::kInvalidUtf8, "query bytes are not UTF-8");
    std::vector<std::pair<std::string, std::string>> edge_names;
    std::map<std::string, std::uint8_t> label_by_name;
    std::set<std::pair<std::string, std::string>> unique_edges;
    std::size_t begin = 0;
    std::uint64_t line_number = 0;
    while (begin <= bytes.size()) {
        const std::size_t newline = bytes.find('\n', begin);
        const std::size_t end = newline == std::string_view::npos ? bytes.size() : newline;
        const std::vector<std::string_view> tokens =
            whitespaceTokens(bytes.substr(begin, end - begin));
        ++line_number;
        if (!tokens.empty() && tokens.front().front() != '#') {
            if (tokens.size() != 2U) {
                fail(
                    ErrorCode::kInvalidQuery,
                    "query line " + std::to_string(line_number) +
                        " must contain exactly two endpoints");
            }
            const auto lhs = parseQueryEndpoint(tokens[0]);
            const auto rhs = parseQueryEndpoint(tokens[1]);
            if (lhs.first == rhs.first) fail(ErrorCode::kInvalidQuery, "query self edge forbidden");
            for (const auto& endpoint : {lhs, rhs}) {
                const auto [iterator, inserted] =
                    label_by_name.emplace(endpoint.first, endpoint.second);
                if (!inserted && iterator->second != endpoint.second) {
                    fail(ErrorCode::kInvalidQuery, "query vertex label is inconsistent");
                }
            }
            if (!unique_edges.emplace(lhs.first, rhs.first).second) {
                fail(ErrorCode::kInvalidQuery, "duplicate directed query edge");
            }
            edge_names.emplace_back(lhs.first, rhs.first);
        }
        if (newline == std::string_view::npos) break;
        begin = newline + 1U;
    }
    if (edge_names.empty()) fail(ErrorCode::kInvalidQuery, "query has no directed edges");

    ParsedQuery result;
    std::map<std::string, std::uint32_t> index_by_name;
    for (const auto& [name, label] : label_by_name) {
        const std::uint32_t index = static_cast<std::uint32_t>(result.vertices.size());
        index_by_name.emplace(name, index);
        result.vertices.push_back({name, label});
    }
    for (const auto& edge : edge_names) {
        result.directed_edges.push_back({
            index_by_name.at(edge.first), index_by_name.at(edge.second)});
    }
    return result;
}

std::vector<RuntimeLabelRow> validateCoverageAndProjectLabels(
    const std::vector<RawTemporalEdge>& graph,
    const ParsedExplicitLabelMap& label_map,
    const ParsedQueryProjection& projection,
    std::string_view actual_map_file_sha256,
    std::string_view actual_query_file_sha256) {
    if (projection.global_label_map_file_sha256 != actual_map_file_sha256 ||
        projection.global_label_map_mapping_sha256 != label_map.mapping_sha256 ||
        projection.query_sha256 != actual_query_file_sha256 ||
        projection.global_label_cardinality != label_map.global_label_cardinality) {
        fail(ErrorCode::kCrossBindingMismatch, "map/projection/query cross-binding mismatch");
    }
    std::vector<std::int32_t> graph_vertices;
    graph_vertices.reserve(graph.size() * 2U);
    for (const RawTemporalEdge& edge : graph) {
        graph_vertices.push_back(edge.source);
        graph_vertices.push_back(edge.target);
    }
    std::sort(graph_vertices.begin(), graph_vertices.end());
    graph_vertices.erase(
        std::unique(graph_vertices.begin(), graph_vertices.end()),
        graph_vertices.end());
    if (graph_vertices.size() != label_map.mapping.size()) {
        fail(ErrorCode::kGraphMapCoverageMismatch, "graph-map vertex count mismatch");
    }
    std::vector<RuntimeLabelRow> output;
    output.reserve(label_map.mapping.size());
    for (std::size_t index = 0; index < label_map.mapping.size(); ++index) {
        const GlobalLabelRow& row = label_map.mapping[index];
        if (graph_vertices[index] != row.external_vertex_id) {
            fail(ErrorCode::kGraphMapCoverageMismatch, "first graph-map vertex ID mismatch");
        }
        std::uint8_t runtime = static_cast<std::uint8_t>(kRuntimeLabelCount);
        for (std::size_t slot = 0; slot < kRuntimeLabelCount; ++slot) {
            if (projection.selected_global_classes_in_a_to_e_order[slot] ==
                row.global_class_id) {
                runtime = static_cast<std::uint8_t>(slot);
                break;
            }
        }
        if (runtime == kRuntimeLabelCount) {
            fail(
                ErrorCode::kCrossBindingMismatch,
                "projection leaves a graph label without runtime slot");
        }
        output.push_back({row.external_vertex_id, row.global_class_id, runtime});
    }
    return output;
}

}  // namespace dsm::e3e4::adapter_v1

namespace dsm::e3e4::adapter_v1 {
namespace {

std::vector<std::string> selectorFor(ArmId arm) {
    switch (arm) {
        case ArmId::kTdTree: return {"--adapter-mode", "td-tree"};
        case ArmId::kTinlaReimpl: return {"--index", "tinla"};
        case ArmId::kCtinlaReimpl: return {"--index", "ctinla"};
        case ArmId::kCeciDurReimpl: return {"--adapter-mode", "cecidur"};
        case ArmId::kPriorDirect:
            return {"--engine", "gopher", "--variant", "durable-dfs"};
        case ArmId::kFinalDurableDag:
            return {"--engine", "gopher", "--variant", "dag-dep-key-memo"};
    }
    fail(ErrorCode::kInvalidInvocation, "invalid ArmId");
}

std::vector<std::string> splitRelativePath(
    std::string_view path,
    const std::string& context) {
    if (path.empty() || path.front() == '/' || path.back() == '/' ||
        path.find('\0') != std::string_view::npos) {
        fail(ErrorCode::kUnsafePath, context + " must be normalized repo-relative path");
    }
    std::vector<std::string> components;
    std::size_t begin = 0;
    while (begin < path.size()) {
        const std::size_t slash = path.find('/', begin);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        const std::string_view component = path.substr(begin, end - begin);
        if (component.empty() || component == "." || component == "..") {
            fail(ErrorCode::kUnsafePath, context + " contains unsafe component");
        }
        components.emplace_back(component);
        if (slash == std::string_view::npos) break;
        begin = slash + 1U;
    }
    return components;
}

std::uint32_t parsePositiveUint32(std::string_view text, const std::string& context) {
    if (text.empty() || (text.size() > 1U && text.front() == '0')) {
        fail(ErrorCode::kInvalidInvocation, context + " is not canonical positive uint32");
    }
    std::uint32_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        result == 0U) {
        fail(ErrorCode::kInvalidInvocation, context + " is not positive uint32");
    }
    return result;
}

std::string stringArrayH0(const std::vector<std::string>& values) {
    JsonValue::Array array;
    array.reserve(values.size());
    for (const std::string& value : values) array.emplace_back(value);
    return sha256Hex(canonicalJson(JsonValue(std::move(array))));
}

void expectArgument(
    const std::vector<std::string>& actual,
    std::size_t& position,
    std::string_view expected) {
    if (position >= actual.size() || actual[position] != expected) {
        fail(
            ErrorCode::kInvalidInvocation,
            "closed argv mismatch at token " + std::to_string(position));
    }
    ++position;
}

std::string takeValue(
    const std::vector<std::string>& actual,
    std::size_t& position,
    std::string_view option) {
    expectArgument(actual, position, option);
    if (position >= actual.size() || actual[position].empty()) {
        fail(ErrorCode::kInvalidInvocation, std::string(option) + " missing value");
    }
    return actual[position++];
}

}  // namespace

std::string_view armToken(ArmId arm) noexcept {
    switch (arm) {
        case ArmId::kTdTree: return "td-tree";
        case ArmId::kTinlaReimpl: return "tinla-reimpl";
        case ArmId::kCtinlaReimpl: return "ctinla-reimpl";
        case ArmId::kCeciDurReimpl: return "cecidur-reimpl";
        case ArmId::kPriorDirect: return "prior-direct";
        case ArmId::kFinalDurableDag: return "final-durable-dag";
    }
    return "invalid-arm";
}

std::string_view phaseToken(CommonPhase phase) noexcept {
    switch (phase) {
        case CommonPhase::kPlanning: return "planning";
        case CommonPhase::kTdConstruction: return "td_construction";
        case CommonPhase::kDagCompilation: return "dag_compilation";
        case CommonPhase::kEnumeration: return "enumeration";
        case CommonPhase::kOutputFinalization: return "output_finalization";
    }
    return "invalid-phase";
}

bool isInternalArm(ArmId arm) noexcept {
    return arm == ArmId::kPriorDirect || arm == ArmId::kFinalDurableDag;
}

ClosedInvocation parseClosedInvocation(
    int argc,
    const char* const argv[],
    ArmId expected_arm) {
    if (argc < 1 || argv == nullptr || argv[0] == nullptr) {
        fail(ErrorCode::kInvalidInvocation, "argv must contain program name");
    }
    std::vector<std::string> actual;
    actual.reserve(static_cast<std::size_t>(argc - 1));
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) {
            fail(ErrorCode::kInvalidInvocation, "invalid argv pointer");
        }
        actual.emplace_back(argv[index]);
    }
    std::size_t position = 0;
    for (const std::string& selector : selectorFor(expected_arm)) {
        expectArgument(actual, position, selector);
    }

    ClosedInvocation result;
    result.arm = expected_arm;
    result.prepared_graph_path = takeValue(actual, position, "--prepared-graph");
    result.explicit_label_map_path =
        takeValue(actual, position, "--explicit-label-map");
    result.query_projection_path =
        takeValue(actual, position, "--query-projection");
    result.query_path = takeValue(actual, position, "--query");
    const std::string minimum_duration =
        takeValue(actual, position, "--minimum-duration");
    result.minimum_duration_k =
        parsePositiveUint32(minimum_duration, "minimum duration");
    const std::string threads = takeValue(actual, position, "--threads");
    if (threads != "16") {
        fail(ErrorCode::kInvalidInvocation, "--threads must be exact 16");
    }
    result.threads = kRequiredThreads;
    expectArgument(actual, position, "--count-only");
    result.count_only = true;
    result.child_phase_receipt_path =
        takeValue(actual, position, "--phase-receipt");
    if (isInternalArm(expected_arm)) {
        result.typed_plan_path = takeValue(actual, position, "--typed-plan");
    }
    if (position != actual.size()) {
        fail(ErrorCode::kInvalidInvocation, "unknown or duplicate trailing argv");
    }
    static_cast<void>(splitRelativePath(result.prepared_graph_path, "prepared graph"));
    static_cast<void>(splitRelativePath(result.explicit_label_map_path, "label map"));
    static_cast<void>(splitRelativePath(result.query_projection_path, "projection"));
    static_cast<void>(splitRelativePath(result.query_path, "query"));
    static_cast<void>(splitRelativePath(result.child_phase_receipt_path, "child receipt"));
    static constexpr std::string_view receipt_prefix =
        "evaluation/icde2027_e3_same_input_competitive_v1/"
        "child_phase_receipts/";
    if (result.child_phase_receipt_path.rfind(receipt_prefix, 0U) != 0U ||
        result.child_phase_receipt_path.size() <= receipt_prefix.size() + 5U ||
        result.child_phase_receipt_path.substr(
            result.child_phase_receipt_path.size() - 5U) != ".json") {
        fail(
            ErrorCode::kInvalidInvocation,
            "child phase receipt is outside the exact E3 result subtree");
    }
    const CorpusRow& corpus_row = selectCorpusRow(result);
    if (result.typed_plan_path.has_value()) {
        static_cast<void>(splitRelativePath(*result.typed_plan_path, "typed plan"));
        if (*result.typed_plan_path != expectedTypedPlanPath(
                corpus_row, result.minimum_duration_k)) {
            fail(
                ErrorCode::kInvalidInvocation,
                "typed plan does not match the exact selected case");
        }
    }
    result.exact_argv_without_program = actual;
    result.exact_argv_object_sha256 = stringArrayH0(actual);
    return result;
}

}  // namespace dsm::e3e4::adapter_v1
