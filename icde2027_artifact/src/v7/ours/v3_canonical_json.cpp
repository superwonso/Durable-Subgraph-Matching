#include "v3_canonical_json.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace dsm::v3 {
namespace {

constexpr std::size_t kMaximumJsonDepth = 128;
constexpr std::string_view kIntegrityScope =
    "canonical_json_all_fields_except_integrity";

[[noreturn]] void typeError(const char* expected) {
    throw JsonError(std::string("JSON value is not ") + expected);
}

bool isContinuation(unsigned char value) noexcept {
    return (value & 0xc0U) == 0x80U;
}

void validateUtf8String(std::string_view value) {
    std::size_t position = 0;
    while (position < value.size()) {
        const unsigned char first = static_cast<unsigned char>(value[position]);
        if (first < 0x80U) {
            ++position;
            continue;
        }
        std::size_t length = 0;
        std::uint32_t code_point = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2U && first <= 0xdfU) {
            length = 2;
            code_point = first & 0x1fU;
            minimum = 0x80U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            length = 3;
            code_point = first & 0x0fU;
            minimum = 0x800U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            length = 4;
            code_point = first & 0x07U;
            minimum = 0x10000U;
        } else {
            throw JsonError("invalid UTF-8 lead byte in JSON string value");
        }
        if (value.size() - position < length) {
            throw JsonError("truncated UTF-8 in JSON string value");
        }
        for (std::size_t index = 1; index < length; ++index) {
            const unsigned char continuation =
                static_cast<unsigned char>(value[position + index]);
            if (!isContinuation(continuation)) {
                throw JsonError("invalid UTF-8 continuation in JSON string value");
            }
            code_point = static_cast<std::uint32_t>(
                (code_point << 6U) | (continuation & 0x3fU));
        }
        if (code_point < minimum || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            throw JsonError("invalid UTF-8 scalar in JSON string value");
        }
        position += length;
    }
}

void appendUtf8(std::string& output, std::uint32_t code_point) {
    if (code_point <= 0x7fU) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (code_point >> 6U)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    } else if (code_point <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (code_point >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    } else if (code_point <= 0x10ffffU) {
        output.push_back(static_cast<char>(0xf0U | (code_point >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    } else {
        throw JsonError("Unicode code point exceeds U+10FFFF");
    }
}

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input) {}

    Json parse() {
        skipWhitespace();
        Json result = parseValue(0);
        skipWhitespace();
        if (position_ != input_.size()) {
            fail("trailing bytes after JSON value");
        }
        return result;
    }

private:
    [[noreturn]] void fail(const std::string& reason) const {
        throw JsonError(
            reason + " at byte " + std::to_string(position_));
    }

    char peek() const {
        if (position_ >= input_.size()) fail("unexpected end of JSON");
        return input_[position_];
    }

    char take() {
        const char value = peek();
        ++position_;
        return value;
    }

    void expect(char expected) {
        if (take() != expected) fail("unexpected JSON byte");
    }

    void skipWhitespace() noexcept {
        while (position_ < input_.size()) {
            const char value = input_[position_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
                break;
            }
            ++position_;
        }
    }

    bool consume(std::string_view literal) {
        if (input_.substr(position_, literal.size()) != literal) return false;
        position_ += literal.size();
        return true;
    }

    Json parseValue(std::size_t depth) {
        if (depth > kMaximumJsonDepth) fail("JSON nesting limit exceeded");
        const char value = peek();
        if (value == '{') return parseObject(depth + 1U);
        if (value == '[') return parseArray(depth + 1U);
        if (value == '"') return Json(parseString());
        if (value >= '0' && value <= '9') return Json(parseUnsigned());
        if (consume("true")) return Json(true);
        if (consume("false")) return Json(false);
        if (consume("null")) return Json::null();
        if (value == '-') fail("negative JSON integer is forbidden");
        fail("floating-point or malformed JSON value is forbidden");
    }

    Json parseObject(std::size_t depth) {
        expect('{');
        skipWhitespace();
        Json::Object result;
        if (peek() == '}') {
            ++position_;
            return Json::object(std::move(result));
        }
        while (true) {
            if (peek() != '"') fail("object key must be a string");
            std::string key = parseString();
            skipWhitespace();
            expect(':');
            skipWhitespace();
            Json value = parseValue(depth);
            const auto inserted = result.emplace(std::move(key), std::move(value));
            if (!inserted.second) fail("duplicate JSON object key");
            skipWhitespace();
            const char delimiter = take();
            if (delimiter == '}') break;
            if (delimiter != ',') fail("object delimiter must be comma or close brace");
            skipWhitespace();
        }
        return Json::object(std::move(result));
    }

    Json parseArray(std::size_t depth) {
        expect('[');
        skipWhitespace();
        Json::Array result;
        if (peek() == ']') {
            ++position_;
            return Json::array(std::move(result));
        }
        while (true) {
            result.push_back(parseValue(depth));
            skipWhitespace();
            const char delimiter = take();
            if (delimiter == ']') break;
            if (delimiter != ',') fail("array delimiter must be comma or close bracket");
            skipWhitespace();
        }
        return Json::array(std::move(result));
    }

    static std::uint32_t hexDigit(char value) {
        if (value >= '0' && value <= '9') {
            return static_cast<std::uint32_t>(value - '0');
        }
        if (value >= 'a' && value <= 'f') {
            return static_cast<std::uint32_t>(10 + value - 'a');
        }
        if (value >= 'A' && value <= 'F') {
            return static_cast<std::uint32_t>(10 + value - 'A');
        }
        throw JsonError("invalid hexadecimal digit in Unicode escape");
    }

    std::uint32_t parseHexQuad() {
        if (input_.size() - position_ < 4U) fail("truncated Unicode escape");
        std::uint32_t result = 0;
        for (std::size_t index = 0; index < 4U; ++index) {
            result = static_cast<std::uint32_t>(
                (result << 4U) | hexDigit(input_[position_++]));
        }
        return result;
    }

    void appendRawUtf8(std::string& result) {
        const std::size_t start = position_;
        const unsigned char first =
            static_cast<unsigned char>(input_[position_]);
        std::size_t length = 0;
        std::uint32_t code_point = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2U && first <= 0xdfU) {
            length = 2;
            code_point = first & 0x1fU;
            minimum = 0x80U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            length = 3;
            code_point = first & 0x0fU;
            minimum = 0x800U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            length = 4;
            code_point = first & 0x07U;
            minimum = 0x10000U;
        } else {
            fail("invalid UTF-8 lead byte");
        }
        if (input_.size() - position_ < length) fail("truncated UTF-8 sequence");
        ++position_;
        for (std::size_t index = 1; index < length; ++index) {
            const unsigned char continuation =
                static_cast<unsigned char>(input_[position_++]);
            if (!isContinuation(continuation)) fail("invalid UTF-8 continuation byte");
            code_point = static_cast<std::uint32_t>(
                (code_point << 6U) | (continuation & 0x3fU));
        }
        if (code_point < minimum || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            fail("noncanonical or invalid UTF-8 scalar value");
        }
        result.append(input_.substr(start, length));
    }

    std::string parseString() {
        expect('"');
        std::string result;
        while (true) {
            if (position_ >= input_.size()) fail("unterminated JSON string");
            const unsigned char value =
                static_cast<unsigned char>(input_[position_]);
            if (value == static_cast<unsigned char>('"')) {
                ++position_;
                return result;
            }
            if (value == static_cast<unsigned char>('\\')) {
                ++position_;
                if (position_ >= input_.size()) fail("truncated JSON escape");
                const char escaped = input_[position_++];
                switch (escaped) {
                    case '"': result.push_back('"'); break;
                    case '\\': result.push_back('\\'); break;
                    case '/': result.push_back('/'); break;
                    case 'b': result.push_back('\b'); break;
                    case 'f': result.push_back('\f'); break;
                    case 'n': result.push_back('\n'); break;
                    case 'r': result.push_back('\r'); break;
                    case 't': result.push_back('\t'); break;
                    case 'u': {
                        std::uint32_t code_point = parseHexQuad();
                        if (code_point >= 0xd800U && code_point <= 0xdbffU) {
                            if (input_.size() - position_ < 6U ||
                                input_[position_] != '\\' ||
                                input_[position_ + 1U] != 'u') {
                                fail("unpaired high surrogate");
                            }
                            position_ += 2U;
                            const std::uint32_t low = parseHexQuad();
                            if (low < 0xdc00U || low > 0xdfffU) {
                                fail("invalid low surrogate");
                            }
                            code_point = 0x10000U +
                                ((code_point - 0xd800U) << 10U) +
                                (low - 0xdc00U);
                        } else if (code_point >= 0xdc00U && code_point <= 0xdfffU) {
                            fail("unpaired low surrogate");
                        }
                        appendUtf8(result, code_point);
                        break;
                    }
                    default: fail("invalid JSON string escape");
                }
                continue;
            }
            if (value < 0x20U) fail("unescaped control byte in JSON string");
            if (value < 0x80U) {
                result.push_back(static_cast<char>(value));
                ++position_;
            } else {
                appendRawUtf8(result);
            }
        }
    }

    std::uint64_t parseUnsigned() {
        const std::size_t start = position_;
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && input_[position_] >= '0' &&
                input_[position_] <= '9') {
                fail("leading zero in JSON integer");
            }
            if (position_ < input_.size() &&
                (input_[position_] == '.' || input_[position_] == 'e' ||
                 input_[position_] == 'E')) {
                fail("floating-point JSON number is forbidden");
            }
            return 0;
        }
        std::uint64_t result = 0;
        while (position_ < input_.size()) {
            const char digit = input_[position_];
            if (digit < '0' || digit > '9') break;
            const std::uint64_t numeric =
                static_cast<std::uint64_t>(digit - '0');
            if (result > (std::numeric_limits<std::uint64_t>::max() - numeric) / 10U) {
                fail("JSON integer exceeds uint64");
            }
            result = result * 10U + numeric;
            ++position_;
        }
        if (position_ == start) fail("missing JSON integer");
        if (position_ < input_.size() &&
            (input_[position_] == '.' || input_[position_] == 'e' ||
             input_[position_] == 'E')) {
            fail("floating-point JSON number is forbidden");
        }
        return result;
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

void appendCanonicalString(std::string& output, const std::string& value) {
    validateUtf8String(value);
    output.push_back('"');
    constexpr char kHex[] = "0123456789abcdef";
    for (unsigned char byte : value) {
        switch (byte) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (byte < 0x20U) {
                    output += "\\u00";
                    output.push_back(kHex[(byte >> 4U) & 0x0fU]);
                    output.push_back(kHex[byte & 0x0fU]);
                } else {
                    output.push_back(static_cast<char>(byte));
                }
        }
    }
    output.push_back('"');
}

void appendCanonical(std::string& output, const Json& value) {
    switch (value.type()) {
        case Json::Type::Null:
            output += "null";
            return;
        case Json::Type::Boolean:
            output += value.boolean() ? "true" : "false";
            return;
        case Json::Type::UnsignedInteger:
            output += std::to_string(value.unsignedInteger());
            return;
        case Json::Type::String:
            appendCanonicalString(output, value.string());
            return;
        case Json::Type::Array: {
            output.push_back('[');
            bool first = true;
            for (const Json& item : value.arrayValue()) {
                if (!first) output.push_back(',');
                first = false;
                appendCanonical(output, item);
            }
            output.push_back(']');
            return;
        }
        case Json::Type::Object: {
            output.push_back('{');
            bool first = true;
            for (const auto& item : value.objectValue()) {
                if (!first) output.push_back(',');
                first = false;
                appendCanonicalString(output, item.first);
                output.push_back(':');
                appendCanonical(output, item.second);
            }
            output.push_back('}');
            return;
        }
    }
    throw JsonError("unknown JSON value type");
}

} // namespace

void Sha256Accumulator::update(std::string_view bytes) {
        if (finalized_) {
            throw JsonError("cannot update a finalized SHA-256 accumulator");
        }
        if (bytes.size() >
            std::numeric_limits<std::uint64_t>::max() - total_bytes_) {
            throw JsonError("SHA-256 byte counter overflow");
        }
        for (unsigned char byte : bytes) {
            block_[block_size_++] = byte;
            ++total_bytes_;
            if (block_size_ == block_.size()) {
                transform();
                block_size_ = 0;
            }
        }
}

std::string Sha256Accumulator::finishHex() {
        if (finalized_) {
            throw JsonError("SHA-256 accumulator was finalized twice");
        }
        finalized_ = true;
        if (total_bytes_ > std::numeric_limits<std::uint64_t>::max() / 8U) {
            throw JsonError("SHA-256 bit counter overflow");
        }
        const std::uint64_t bit_count = total_bytes_ * 8U;
        block_[block_size_++] = 0x80U;
        if (block_size_ > 56U) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_),
                      block_.end(), 0U);
            transform();
            block_size_ = 0;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_),
                  block_.begin() + 56, 0U);
        for (std::size_t index = 0; index < 8U; ++index) {
            block_[63U - index] = static_cast<std::uint8_t>(
                bit_count >> static_cast<unsigned>(index * 8U));
        }
        transform();
        std::array<std::uint8_t, 32> result{};
        for (std::size_t index = 0; index < state_.size(); ++index) {
            for (std::size_t byte = 0; byte < 4U; ++byte) {
                result[index * 4U + byte] = static_cast<std::uint8_t>(
                    state_[index] >> static_cast<unsigned>((3U - byte) * 8U));
            }
        }
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (std::uint8_t byte : result) {
            output << std::setw(2) << static_cast<unsigned>(byte);
        }
        return output.str();
}

std::uint64_t Sha256Accumulator::observedBytes() const noexcept {
    return total_bytes_;
}

std::uint32_t Sha256Accumulator::rotateRight(
    std::uint32_t value, unsigned count) noexcept {
        return (value >> count) | (value << (32U - count));
}

void Sha256Accumulator::transform() noexcept {
        static constexpr std::array<std::uint32_t, 64> constants{{
            0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,
            0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
            0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,
            0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
            0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,
            0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
            0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,
            0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
            0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,
            0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
            0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,
            0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
            0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,
            0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
            0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,
            0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U}};
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16U; ++index) {
            const std::size_t offset = index * 4U;
            words[index] =
                (static_cast<std::uint32_t>(block_[offset]) << 24U) |
                (static_cast<std::uint32_t>(block_[offset + 1U]) << 16U) |
                (static_cast<std::uint32_t>(block_[offset + 2U]) << 8U) |
                static_cast<std::uint32_t>(block_[offset + 3U]);
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            const std::uint32_t x = words[index - 15U];
            const std::uint32_t y = words[index - 2U];
            const std::uint32_t s0 = rotateRight(x, 7U) ^ rotateRight(x, 18U) ^ (x >> 3U);
            const std::uint32_t s1 = rotateRight(y, 17U) ^ rotateRight(y, 19U) ^ (y >> 10U);
            words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
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
            const std::uint32_t sum1 = rotateRight(e, 6U) ^ rotateRight(e, 11U) ^ rotateRight(e, 25U);
            const std::uint32_t choice = (e & f) ^ ((~e) & g);
            const std::uint32_t temporary1 = h + sum1 + choice + constants[index] + words[index];
            const std::uint32_t sum0 = rotateRight(a, 2U) ^ rotateRight(a, 13U) ^ rotateRight(a, 22U);
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

namespace {

bool hasExactKeys(
    const Json::Object& object,
    std::initializer_list<std::string_view> expected) {
    if (object.size() != expected.size()) return false;
    for (std::string_view key : expected) {
        if (object.find(std::string(key)) == object.end()) return false;
    }
    return true;
}

} // namespace

Json::Json() noexcept = default;
Json::Json(bool value) noexcept : type_(Type::Boolean), boolean_(value) {}
Json::Json(std::uint64_t value) noexcept
    : type_(Type::UnsignedInteger), unsigned_integer_(value) {}
Json::Json(std::string value)
    : type_(Type::String), string_(std::move(value)) {}

Json Json::null() noexcept { return Json(); }
Json Json::array(Array value) {
    Json result;
    result.type_ = Type::Array;
    result.array_ = std::move(value);
    return result;
}
Json Json::object(Object value) {
    Json result;
    result.type_ = Type::Object;
    result.object_ = std::move(value);
    return result;
}
Json Json::parse(std::string_view bytes) { return Parser(bytes).parse(); }

Json::Type Json::type() const noexcept { return type_; }
bool Json::isNull() const noexcept { return type_ == Type::Null; }
bool Json::isBoolean() const noexcept { return type_ == Type::Boolean; }
bool Json::isUnsignedInteger() const noexcept { return type_ == Type::UnsignedInteger; }
bool Json::isString() const noexcept { return type_ == Type::String; }
bool Json::isArray() const noexcept { return type_ == Type::Array; }
bool Json::isObject() const noexcept { return type_ == Type::Object; }

bool Json::boolean() const {
    if (!isBoolean()) typeError("a boolean");
    return boolean_;
}
std::uint64_t Json::unsignedInteger() const {
    if (!isUnsignedInteger()) typeError("an unsigned integer");
    return unsigned_integer_;
}
const std::string& Json::string() const {
    if (!isString()) typeError("a string");
    return string_;
}
const Json::Array& Json::arrayValue() const {
    if (!isArray()) typeError("an array");
    return array_;
}
Json::Array& Json::arrayValue() {
    if (!isArray()) typeError("an array");
    return array_;
}
const Json::Object& Json::objectValue() const {
    if (!isObject()) typeError("an object");
    return object_;
}
Json::Object& Json::objectValue() {
    if (!isObject()) typeError("an object");
    return object_;
}

const Json* Json::find(std::string_view key) const {
    if (!isObject()) return nullptr;
    const auto found = object_.find(std::string(key));
    return found == object_.end() ? nullptr : &found->second;
}
Json* Json::find(std::string_view key) {
    if (!isObject()) return nullptr;
    const auto found = object_.find(std::string(key));
    return found == object_.end() ? nullptr : &found->second;
}
const Json& Json::at(std::string_view key) const {
    const Json* value = find(key);
    if (value == nullptr) throw JsonError("missing JSON object field: " + std::string(key));
    return *value;
}
Json& Json::at(std::string_view key) {
    Json* value = find(key);
    if (value == nullptr) throw JsonError("missing JSON object field: " + std::string(key));
    return *value;
}

bool Json::operator==(const Json& other) const noexcept {
    if (type_ != other.type_) return false;
    switch (type_) {
        case Type::Null: return true;
        case Type::Boolean: return boolean_ == other.boolean_;
        case Type::UnsignedInteger: return unsigned_integer_ == other.unsigned_integer_;
        case Type::String: return string_ == other.string_;
        case Type::Array: return array_ == other.array_;
        case Type::Object: return object_ == other.object_;
    }
    return false;
}
bool Json::operator!=(const Json& other) const noexcept { return !(*this == other); }

std::string canonicalJsonBytes(const Json& value) {
    std::string result;
    appendCanonical(result, value);
    result.push_back('\n');
    return result;
}

std::string sha256Hex(std::string_view bytes) {
    Sha256Accumulator sha;
    sha.update(bytes);
    return sha.finishHex();
}

Json withoutIntegrity(const Json& value) {
    if (!value.isObject()) throw JsonError("artifact root must be an object");
    Json payload = value;
    payload.objectValue().erase("integrity");
    return payload;
}

std::string canonicalPayloadSha256(const Json& value) {
    return sha256Hex(canonicalJsonBytes(withoutIntegrity(value)));
}

bool isLowercaseSha256(const Json& value) noexcept {
    if (!value.isString() || value.string().size() != 64U) return false;
    return std::all_of(
        value.string().begin(), value.string().end(), [](char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f');
        });
}

CanonicalArtifact loadCanonicalArtifact(
    const std::filesystem::path& path,
    std::size_t maximum_bytes) {
    std::error_code size_error;
    const std::uintmax_t size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        throw ArtifactIoError("cannot stat canonical artifact: " + path.string());
    }
    if (size > static_cast<std::uintmax_t>(maximum_bytes)) {
        throw JsonError("canonical artifact exceeds byte cap: " + path.string());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw ArtifactIoError("cannot open canonical artifact: " + path.string());
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!bytes.empty()) {
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    if (!input || input.peek() != std::ifstream::traits_type::eof()) {
        throw ArtifactIoError("short or drifting canonical artifact read: " + path.string());
    }
    Json value = Json::parse(bytes);
    if (!value.isObject()) throw JsonError("artifact root must be an object");
    if (canonicalJsonBytes(value) != bytes) {
        throw JsonError("artifact bytes are not canonical JSON: " + path.string());
    }
    const Json* integrity = value.find("integrity");
    if (integrity == nullptr || !integrity->isObject() ||
        !hasExactKeys(
            integrity->objectValue(),
            {"algorithm", "canonical_payload_sha256", "scope"})) {
        throw ArtifactIntegrityError("artifact integrity field-set drift: " + path.string());
    }
    const Json& algorithm = integrity->at("algorithm");
    const Json& scope = integrity->at("scope");
    const Json& observed = integrity->at("canonical_payload_sha256");
    if (!algorithm.isString() || algorithm.string() != "sha256" ||
        !scope.isString() || scope.string() != kIntegrityScope ||
        !isLowercaseSha256(observed)) {
        throw ArtifactIntegrityError("artifact integrity metadata drift: " + path.string());
    }
    const std::string expected = canonicalPayloadSha256(value);
    if (observed.string() != expected) {
        throw ArtifactIntegrityError(
            "artifact canonical payload SHA-256 mismatch: " + path.string());
    }
    const std::string file_hash = sha256Hex(bytes);
    return CanonicalArtifact{
        std::move(value), std::move(bytes), file_hash, std::move(expected)};
}

} // namespace dsm::v3
