#include "E3E4PlanAuthorityV1.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace dsm::e3e4::gopher_plan_v1 {
namespace {

using adapter_v1::ClosedInvocation;
using adapter_v1::FileIdentity;
using adapter_v1::RepositoryAnchor;
using adapter_v1::ValidatedInputs;

[[noreturn]] void fail(
    TypedPlanErrorCode code,
    const std::string& message) {
    throw TypedPlanError(code, message);
}

bool validLowerSha256(std::string_view value) {
    if (value.size() != 64U) return false;
    return std::all_of(
        value.begin(), value.end(), [](char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f');
        });
}

bool validLowerHex(std::string_view value, std::size_t width) {
    if (value.size() != width) return false;
    return std::all_of(
        value.begin(), value.end(), [](char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f');
        });
}

void requireValidUtf8(std::string_view text) {
    std::size_t position = 0;
    while (position < text.size()) {
        const unsigned char first =
            static_cast<unsigned char>(text[position]);
        if (first <= 0x7fU) {
            ++position;
            continue;
        }
        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        if ((first & 0xe0U) == 0xc0U) {
            continuation_count = 1;
            code_point = first & 0x1fU;
            if (code_point == 0U) {
                fail(TypedPlanErrorCode::kInvalidUtf8, "overlong UTF-8");
            }
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation_count = 2;
            code_point = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation_count = 3;
            code_point = first & 0x07U;
        } else {
            fail(TypedPlanErrorCode::kInvalidUtf8, "invalid UTF-8 lead byte");
        }
        if (position + continuation_count >= text.size()) {
            fail(TypedPlanErrorCode::kInvalidUtf8, "truncated UTF-8");
        }
        for (std::size_t index = 1;
             index <= continuation_count;
             ++index) {
            const unsigned char continuation =
                static_cast<unsigned char>(text[position + index]);
            if ((continuation & 0xc0U) != 0x80U) {
                fail(
                    TypedPlanErrorCode::kInvalidUtf8,
                    "invalid UTF-8 continuation byte");
            }
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }
        if ((continuation_count == 1 && code_point < 0x80U) ||
            (continuation_count == 2 && code_point < 0x800U) ||
            (continuation_count == 3 && code_point < 0x10000U) ||
            code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            fail(
                TypedPlanErrorCode::kInvalidUtf8,
                "noncanonical UTF-8 code point");
        }
        position += continuation_count + 1U;
    }
}

struct JsonValue final {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;
    using Storage = std::variant<
        std::nullptr_t,
        bool,
        std::int64_t,
        std::uint64_t,
        std::string,
        Array,
        Object>;

    Storage value = nullptr;

    JsonValue() = default;
    explicit JsonValue(std::nullptr_t) : value(nullptr) {}
    explicit JsonValue(bool input) : value(input) {}
    explicit JsonValue(std::int64_t input) : value(input) {}
    explicit JsonValue(std::uint64_t input) : value(input) {}
    explicit JsonValue(std::string input) : value(std::move(input)) {}
    explicit JsonValue(const char* input) : value(std::string(input)) {}
    explicit JsonValue(Array input) : value(std::move(input)) {}
    explicit JsonValue(Object input) : value(std::move(input)) {}
};

void appendUtf8(std::string& output, std::uint32_t code_point) {
    if (code_point <= 0x7fU) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (code_point >> 6U)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    } else if (code_point <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (code_point >> 12U)));
        output.push_back(static_cast<char>(
            0x80U | ((code_point >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (code_point >> 18U)));
        output.push_back(static_cast<char>(
            0x80U | ((code_point >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(
            0x80U | ((code_point >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    }
}

class JsonParser final {
public:
    explicit JsonParser(std::string_view text) : text_(text) {
        requireValidUtf8(text_);
        if (text_.find('\0') != std::string_view::npos) {
            fail(TypedPlanErrorCode::kInvalidJson, "JSON contains NUL");
        }
    }

    JsonValue parse() {
        skipWhitespace();
        JsonValue result = parseValue();
        skipWhitespace();
        if (position_ != text_.size()) {
            fail(TypedPlanErrorCode::kInvalidJson, "trailing JSON bytes");
        }
        return result;
    }

private:
    void skipWhitespace() {
        while (position_ < text_.size()) {
            const char character = text_[position_];
            if (character != ' ' && character != '\t' &&
                character != '\n' && character != '\r') {
                break;
            }
            ++position_;
        }
    }

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
            fail(TypedPlanErrorCode::kInvalidJson, "JSON punctuation mismatch");
        }
    }

    bool beginsWith(std::string_view token) const {
        return position_ <= text_.size() &&
            text_.substr(position_, token.size()) == token;
    }

    void literal(std::string_view token) {
        if (!beginsWith(token)) {
            fail(TypedPlanErrorCode::kInvalidJson, "invalid JSON literal");
        }
        position_ += token.size();
    }

    JsonValue parseValue() {
        struct DepthGuard final {
            explicit DepthGuard(std::size_t& depth) : depth_(depth) {
                ++depth_;
                if (depth_ > 64U) {
                    fail(
                        TypedPlanErrorCode::kInvalidJson,
                        "typed-plan JSON nesting exceeds 64");
                }
            }
            ~DepthGuard() { --depth_; }
            std::size_t& depth_;
        } guard(depth_);
        skipWhitespace();
        if (position_ >= text_.size()) {
            fail(TypedPlanErrorCode::kInvalidJson, "truncated JSON value");
        }
        if (beginsWith("NaN") || beginsWith("Infinity") ||
            beginsWith("-Infinity")) {
            fail(
                TypedPlanErrorCode::kNonfiniteNumber,
                "nonfinite JSON number");
        }
        switch (text_[position_]) {
            case '{': return parseObject();
            case '[': return parseArray();
            case '"': return JsonValue(parseString());
            case 't': literal("true"); return JsonValue(true);
            case 'f': literal("false"); return JsonValue(false);
            case 'n': literal("null"); return JsonValue(nullptr);
            default:
                if (text_[position_] == '-' ||
                    (text_[position_] >= '0' && text_[position_] <= '9')) {
                    return parseInteger();
                }
                fail(TypedPlanErrorCode::kInvalidJson, "invalid JSON value");
        }
    }

    std::uint32_t parseHexQuad() {
        if (position_ + 4U > text_.size()) {
            fail(TypedPlanErrorCode::kInvalidJson, "truncated JSON unicode escape");
        }
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4U; ++index) {
            const char character = text_[position_++];
            std::uint32_t digit = 0;
            if (character >= '0' && character <= '9') {
                digit = static_cast<std::uint32_t>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                digit = static_cast<std::uint32_t>(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                digit = static_cast<std::uint32_t>(character - 'A' + 10);
            } else {
                fail(TypedPlanErrorCode::kInvalidJson, "invalid JSON unicode escape");
            }
            value = (value << 4U) | digit;
        }
        return value;
    }

    std::string parseString() {
        if (position_ >= text_.size() || text_[position_] != '"') {
            fail(TypedPlanErrorCode::kInvalidJson, "expected JSON string");
        }
        ++position_;
        std::string output;
        while (position_ < text_.size()) {
            const unsigned char character =
                static_cast<unsigned char>(text_[position_++]);
            if (character == '"') return output;
            if (character < 0x20U) {
                fail(TypedPlanErrorCode::kInvalidJson, "unescaped JSON control byte");
            }
            if (character != '\\') {
                output.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ >= text_.size()) {
                fail(TypedPlanErrorCode::kInvalidJson, "truncated JSON escape");
            }
            const char escape = text_[position_++];
            switch (escape) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    std::uint32_t code_point = parseHexQuad();
                    if (code_point >= 0xd800U && code_point <= 0xdbffU) {
                        if (position_ + 2U > text_.size() ||
                            text_[position_] != '\\' ||
                            text_[position_ + 1U] != 'u') {
                            fail(
                                TypedPlanErrorCode::kInvalidJson,
                                "unpaired JSON high surrogate");
                        }
                        position_ += 2U;
                        const std::uint32_t low = parseHexQuad();
                        if (low < 0xdc00U || low > 0xdfffU) {
                            fail(
                                TypedPlanErrorCode::kInvalidJson,
                                "invalid JSON low surrogate");
                        }
                        code_point = 0x10000U +
                            ((code_point - 0xd800U) << 10U) +
                            (low - 0xdc00U);
                    } else if (code_point >= 0xdc00U &&
                               code_point <= 0xdfffU) {
                        fail(
                            TypedPlanErrorCode::kInvalidJson,
                            "unpaired JSON low surrogate");
                    }
                    appendUtf8(output, code_point);
                    break;
                }
                default:
                    fail(TypedPlanErrorCode::kInvalidJson, "invalid JSON escape");
            }
        }
        fail(TypedPlanErrorCode::kInvalidJson, "unterminated JSON string");
    }

    JsonValue parseInteger() {
        const std::size_t begin = position_;
        const bool negative = text_[position_] == '-';
        if (negative) {
            ++position_;
            if (position_ >= text_.size()) {
                fail(TypedPlanErrorCode::kInvalidJson, "truncated JSON number");
            }
        }
        if (text_[position_] == '0') {
            ++position_;
            if (position_ < text_.size() &&
                text_[position_] >= '0' && text_[position_] <= '9') {
                fail(TypedPlanErrorCode::kInvalidJson, "JSON leading zero");
            }
        } else {
            if (text_[position_] < '1' || text_[position_] > '9') {
                fail(TypedPlanErrorCode::kInvalidJson, "invalid JSON number");
            }
            while (position_ < text_.size() &&
                   text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < text_.size() &&
            (text_[position_] == '.' || text_[position_] == 'e' ||
             text_[position_] == 'E')) {
            fail(
                TypedPlanErrorCode::kWrongJsonType,
                "typed-plan JSON numbers must be integers");
        }
        const std::string_view token = text_.substr(begin, position_ - begin);
        if (negative) {
            std::int64_t parsed = 0;
            const auto result = std::from_chars(
                token.data(), token.data() + token.size(), parsed);
            if (result.ec != std::errc{} ||
                result.ptr != token.data() + token.size()) {
                fail(TypedPlanErrorCode::kIntegerRange, "signed JSON integer overflow");
            }
            return JsonValue(parsed);
        }
        std::uint64_t parsed = 0;
        const auto result = std::from_chars(
            token.data(), token.data() + token.size(), parsed);
        if (result.ec != std::errc{} ||
            result.ptr != token.data() + token.size()) {
            fail(TypedPlanErrorCode::kIntegerRange, "unsigned JSON integer overflow");
        }
        return JsonValue(parsed);
    }

    JsonValue parseArray() {
        expect('[');
        JsonValue::Array result;
        if (consume(']')) return JsonValue(std::move(result));
        while (true) {
            result.push_back(parseValue());
            if (consume(']')) return JsonValue(std::move(result));
            expect(',');
        }
    }

    JsonValue parseObject() {
        expect('{');
        JsonValue::Object result;
        if (consume('}')) return JsonValue(std::move(result));
        while (true) {
            skipWhitespace();
            std::string key = parseString();
            expect(':');
            JsonValue child = parseValue();
            if (!result.emplace(std::move(key), std::move(child)).second) {
                fail(TypedPlanErrorCode::kDuplicateKey, "duplicate JSON object key");
            }
            if (consume('}')) return JsonValue(std::move(result));
            expect(',');
        }
    }

    std::string_view text_;
    std::size_t position_ = 0;
    std::size_t depth_ = 0;
};

void appendJsonString(std::string& output, std::string_view input) {
    output.push_back('"');
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char character : input) {
        switch (character) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (character < 0x20U) {
                    output += "\\u00";
                    output.push_back(hex[character >> 4U]);
                    output.push_back(hex[character & 0x0fU]);
                } else {
                    output.push_back(static_cast<char>(character));
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
    } else if (const auto* signed_integer =
               std::get_if<std::int64_t>(&value.value)) {
        char buffer[32];
        const auto result = std::to_chars(
            std::begin(buffer), std::end(buffer), *signed_integer);
        if (result.ec != std::errc{}) {
            fail(TypedPlanErrorCode::kIntegerRange, "integer serialization failed");
        }
        output.append(buffer, result.ptr);
    } else if (const auto* unsigned_integer =
               std::get_if<std::uint64_t>(&value.value)) {
        char buffer[32];
        const auto result = std::to_chars(
            std::begin(buffer), std::end(buffer), *unsigned_integer);
        if (result.ec != std::errc{}) {
            fail(TypedPlanErrorCode::kIntegerRange, "integer serialization failed");
        }
        output.append(buffer, result.ptr);
    } else if (const auto* string = std::get_if<std::string>(&value.value)) {
        appendJsonString(output, *string);
    } else if (const auto* array =
               std::get_if<JsonValue::Array>(&value.value)) {
        output.push_back('[');
        for (std::size_t index = 0; index < array->size(); ++index) {
            if (index != 0U) output.push_back(',');
            appendCanonicalJson(output, (*array)[index]);
        }
        output.push_back(']');
    } else {
        const auto& object = std::get<JsonValue::Object>(value.value);
        output.push_back('{');
        std::size_t index = 0;
        for (const auto& [key, child] : object) {
            if (index++ != 0U) output.push_back(',');
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

const JsonValue::Object& asObject(
    const JsonValue& value,
    std::string_view context) {
    const auto* object = std::get_if<JsonValue::Object>(&value.value);
    if (object == nullptr) {
        fail(
            TypedPlanErrorCode::kWrongJsonType,
            std::string(context) + " must be an object");
    }
    return *object;
}

const JsonValue::Array& asArray(
    const JsonValue& value,
    std::string_view context) {
    const auto* array = std::get_if<JsonValue::Array>(&value.value);
    if (array == nullptr) {
        fail(
            TypedPlanErrorCode::kWrongJsonType,
            std::string(context) + " must be an array");
    }
    return *array;
}

const std::string& asString(
    const JsonValue& value,
    std::string_view context) {
    const auto* string = std::get_if<std::string>(&value.value);
    if (string == nullptr) {
        fail(
            TypedPlanErrorCode::kWrongJsonType,
            std::string(context) + " must be a string");
    }
    return *string;
}

bool asBoolean(const JsonValue& value, std::string_view context) {
    const auto* boolean = std::get_if<bool>(&value.value);
    if (boolean == nullptr) {
        fail(
            TypedPlanErrorCode::kWrongJsonType,
            std::string(context) + " must be a boolean");
    }
    return *boolean;
}

std::uint64_t asUnsigned(
    const JsonValue& value,
    std::string_view context) {
    if (const auto* result = std::get_if<std::uint64_t>(&value.value)) {
        return *result;
    }
    if (const auto* result = std::get_if<std::int64_t>(&value.value)) {
        if (*result >= 0) return static_cast<std::uint64_t>(*result);
    }
    fail(
        TypedPlanErrorCode::kWrongJsonType,
        std::string(context) + " must be a nonnegative integer (not bool)");
}

std::int64_t asSigned(const JsonValue& value, std::string_view context) {
    if (const auto* result = std::get_if<std::int64_t>(&value.value)) {
        return *result;
    }
    if (const auto* result = std::get_if<std::uint64_t>(&value.value)) {
        if (*result <= static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(*result);
        }
    }
    fail(
        TypedPlanErrorCode::kWrongJsonType,
        std::string(context) + " must be an integer (not bool)");
}

bool isNull(const JsonValue& value) {
    return std::holds_alternative<std::nullptr_t>(value.value);
}

const JsonValue& member(
    const JsonValue::Object& object,
    std::string_view key) {
    const auto found = object.find(std::string(key));
    if (found == object.end()) {
        fail(
            TypedPlanErrorCode::kUnknownOrMissingKey,
            "missing JSON key: " + std::string(key));
    }
    return found->second;
}

void requireExactKeys(
    const JsonValue::Object& object,
    std::initializer_list<std::string_view> keys,
    std::string_view context) {
    if (object.size() != keys.size()) {
        fail(
            TypedPlanErrorCode::kUnknownOrMissingKey,
            std::string(context) + " key count mismatch");
    }
    for (std::string_view key : keys) {
        if (object.find(std::string(key)) == object.end()) {
            fail(
                TypedPlanErrorCode::kUnknownOrMissingKey,
                std::string(context) + " missing key " + std::string(key));
        }
    }
}

std::uint32_t checkedUint32(
    const JsonValue& value,
    std::string_view context) {
    const std::uint64_t parsed = asUnsigned(value, context);
    if (parsed > std::numeric_limits<std::uint32_t>::max()) {
        fail(
            TypedPlanErrorCode::kIntegerRange,
            std::string(context) + " exceeds uint32");
    }
    return static_cast<std::uint32_t>(parsed);
}

std::int32_t checkedInt32(
    const JsonValue& value,
    std::string_view context) {
    const std::int64_t parsed = asSigned(value, context);
    if (parsed < std::numeric_limits<std::int32_t>::min() ||
        parsed > std::numeric_limits<std::int32_t>::max()) {
        fail(
            TypedPlanErrorCode::kIntegerRange,
            std::string(context) + " exceeds int32");
    }
    return static_cast<std::int32_t>(parsed);
}

JsonValue stringValue(std::string_view value) {
    return JsonValue(std::string(value));
}

JsonValue uintValue(std::uint64_t value) {
    return JsonValue(value);
}

JsonValue intValue(std::int64_t value) {
    return value < 0
        ? JsonValue(value)
        : JsonValue(static_cast<std::uint64_t>(value));
}

}  // namespace

namespace {

struct CaseRow final {
    std::uint32_t case_index;
    const char* case_id;
    const char* dataset_id;
    std::uint32_t minimum_duration_k;
    const char* graph_path;
    const char* map_path;
    const char* projection_path;
    const char* query_path;
};

struct LivePlanTopology final {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> tree_arcs;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> serialized_non_tree;
    std::vector<std::pair<int, int>> engine_non_tree;
    std::vector<std::vector<std::uint32_t>> earlier_neighbors;
    std::vector<std::vector<int>> spanning_tree_adj;
};

JsonValue typedPlanJson(const TypedPlanValue&, bool);
TypedPlanValue parseTypedPlanJson(const JsonValue&);
const CaseRow& selectCase(
    const ValidatedInputs&, std::uint32_t, std::string_view);
TypedDirectedQueryFingerprint makeQueryFingerprint(const Graph&);
void bindLiveQueryToLoadedBytes(
    const Graph&,
    const ValidatedInputs&,
    const TypedDirectedQueryFingerprint&);
LivePlanTopology deriveLiveTopology(
    const Graph&,
    const QueryPlan&,
    const TypedDirectedQueryFingerprint&);
void validateFreshPlanning(
    const Graph&,
    const FreshPaperPlanning&,
    const TypedDirectedQueryFingerprint&,
    const LivePlanTopology&);
TypedPlanValue buildExpectedValue(
    const ValidatedInputs&,
    const CaseRow&,
    const TypedDirectedQueryFingerprint&,
    const FreshPaperPlanning&,
    const LivePlanTopology&);
ValidatedTypedPlan validateParsedAgainstLive(
    TypedPlanValue,
    std::string,
    const ValidatedInputs&,
    const CaseRow&,
    const Graph&,
    const FreshPaperPlanning&);
void validateTemporalGraphAgainstInputs(
    const Graph&,
    const ValidatedInputs&);

struct HeldNode final {
    int fd = -1;
    int parent_fd = -1;
    std::string name;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t size = 0;
    std::uint64_t nlink = 0;
    std::uint32_t mode = 0;
    std::int64_t modification_seconds = 0;
    std::int64_t modification_nanoseconds = 0;
    std::int64_t change_seconds = 0;
    std::int64_t change_nanoseconds = 0;
    bool directory = false;
};

std::uint32_t permissionMode(const struct stat& status) {
    return static_cast<std::uint32_t>(status.st_mode & 07777U);
}

HeldNode snapshotNode(
    int fd,
    int parent_fd,
    std::string name,
    bool directory) {
    struct stat status {};
    if (::fstat(fd, &status) != 0) {
        fail(TypedPlanErrorCode::kOpenFailed, "fstat failed for held plan node");
    }
    if ((directory && !S_ISDIR(status.st_mode)) ||
        (!directory && !S_ISREG(status.st_mode))) {
        fail(
            TypedPlanErrorCode::kWrongFileType,
            "held plan path component has wrong file type");
    }
    HeldNode result;
    result.fd = fd;
    result.parent_fd = parent_fd;
    result.name = std::move(name);
    result.device = static_cast<std::uint64_t>(status.st_dev);
    result.inode = static_cast<std::uint64_t>(status.st_ino);
    result.size = static_cast<std::uint64_t>(status.st_size);
    result.nlink = static_cast<std::uint64_t>(status.st_nlink);
    result.mode = permissionMode(status);
    result.modification_seconds = status.st_mtim.tv_sec;
    result.modification_nanoseconds = status.st_mtim.tv_nsec;
    result.change_seconds = status.st_ctim.tv_sec;
    result.change_nanoseconds = status.st_ctim.tv_nsec;
    result.directory = directory;
    return result;
}

bool sameNode(const HeldNode& expected, const struct stat& status) {
    return expected.device == static_cast<std::uint64_t>(status.st_dev) &&
        expected.inode == static_cast<std::uint64_t>(status.st_ino) &&
        expected.size == static_cast<std::uint64_t>(status.st_size) &&
        expected.nlink == static_cast<std::uint64_t>(status.st_nlink) &&
        expected.mode == permissionMode(status) &&
        expected.modification_seconds == status.st_mtim.tv_sec &&
        expected.modification_nanoseconds == status.st_mtim.tv_nsec &&
        expected.change_seconds == status.st_ctim.tv_sec &&
        expected.change_nanoseconds == status.st_ctim.tv_nsec &&
        (expected.directory ? S_ISDIR(status.st_mode) : S_ISREG(status.st_mode));
}

void proveHeldNode(const HeldNode& node) {
    struct stat held {};
    if (::fstat(node.fd, &held) != 0 || !sameNode(node, held)) {
        fail(
            TypedPlanErrorCode::kIdentityDrift,
            "held typed-plan node identity drifted");
    }
    if (node.parent_fd >= 0) {
        struct stat rebound {};
        if (::fstatat(
                node.parent_fd,
                node.name.c_str(),
                &rebound,
                AT_SYMLINK_NOFOLLOW) != 0 ||
            !sameNode(node, rebound)) {
            fail(
                TypedPlanErrorCode::kIdentityDrift,
                "typed-plan parent-to-child final rebind failed");
        }
    }
}

std::vector<std::string> splitExactPlanPath(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.back() == '/') {
        fail(TypedPlanErrorCode::kUnsafePath, "typed-plan path is not relative");
    }
    std::vector<std::string> components;
    std::size_t start = 0;
    while (start < path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end =
            slash == std::string_view::npos ? path.size() : slash;
        const std::string_view component = path.substr(start, end - start);
        if (component.empty() || component == "." || component == ".." ||
            component.find('\0') != std::string_view::npos) {
            fail(TypedPlanErrorCode::kUnsafePath, "unsafe typed-plan path component");
        }
        components.emplace_back(component);
        if (slash == std::string_view::npos) break;
        start = slash + 1U;
    }
    if (components.size() != 4U || components[0] != "benchmark" ||
        components[1] != "icde2027_e3_e4_typed_plan_authority_v1" ||
        components[2] != "typed_plans" ||
        components[3].size() <= 5U ||
        components[3].substr(components[3].size() - 5U) != ".json") {
        fail(
            TypedPlanErrorCode::kUnsafePath,
            "typed-plan path is outside the exact disjoint authority root");
    }
    return components;
}

std::string readExactHeldFile(const HeldNode& leaf) {
    static constexpr std::uint64_t kMaximumPlanBytes = 1U << 20U;
    if (leaf.size < 3U || leaf.size > kMaximumPlanBytes) {
        fail(
            TypedPlanErrorCode::kSizeDrift,
            "typed-plan file size is outside the closed v1 bound");
    }
    std::string bytes(static_cast<std::size_t>(leaf.size), '\0');
    std::size_t completed = 0;
    while (completed < bytes.size()) {
        const ssize_t count = ::pread(
            leaf.fd,
            bytes.data() + completed,
            bytes.size() - completed,
            static_cast<off_t>(completed));
        if (count < 0) {
            if (errno == EINTR) continue;
            fail(TypedPlanErrorCode::kOpenFailed, "typed-plan pread failed");
        }
        if (count == 0) {
            fail(TypedPlanErrorCode::kSizeDrift, "typed-plan short read");
        }
        completed += static_cast<std::size_t>(count);
    }
    char extra = 0;
    ssize_t count = 0;
    do {
        count = ::pread(
            leaf.fd, &extra, 1, static_cast<off_t>(bytes.size()));
    } while (count < 0 && errno == EINTR);
    if (count != 0) {
        fail(TypedPlanErrorCode::kSizeDrift, "typed-plan grew during read");
    }
    return bytes;
}

}  // namespace

struct ConsumedTypedPlan::Impl final {
    std::vector<HeldNode> directories;
    HeldNode leaf;
    ValidatedTypedPlan validated;
    FileIdentity identity;
    TypedPlanConsumptionRecord record;

    ~Impl() {
        if (leaf.fd >= 0) {
            static_cast<void>(::close(leaf.fd));
            leaf.fd = -1;
        }
        for (auto iterator = directories.rbegin();
             iterator != directories.rend();
             ++iterator) {
            if (iterator->fd >= 0) {
                static_cast<void>(::close(iterator->fd));
                iterator->fd = -1;
            }
        }
    }

    void prove() const {
        for (const HeldNode& directory : directories) {
            proveHeldNode(directory);
        }
        proveHeldNode(leaf);
    }
};

namespace {

std::unique_ptr<ConsumedTypedPlan::Impl> openHeldTypedPlan(
    const RepositoryAnchor& repository,
    std::string_view exact_path) {
    repository.proveStable();
    const std::vector<std::string> components = splitExactPlanPath(exact_path);
    auto impl = std::make_unique<ConsumedTypedPlan::Impl>();

    const int root_fd = ::fcntl(repository.fd(), F_DUPFD_CLOEXEC, 3);
    if (root_fd < 0) {
        fail(TypedPlanErrorCode::kOpenFailed, "repository fd duplication failed");
    }
    try {
        try {
            impl->directories.push_back(
                snapshotNode(root_fd, -1, std::string(), true));
        } catch (...) {
            static_cast<void>(::close(root_fd));
            throw;
        }
        int parent_fd = root_fd;
        for (std::size_t index = 0; index + 1U < components.size(); ++index) {
            const int directory_fd = ::openat(
                parent_fd,
                components[index].c_str(),
                O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK);
            if (directory_fd < 0) {
                fail(TypedPlanErrorCode::kOpenFailed, "typed-plan directory open failed");
            }
            try {
                impl->directories.push_back(snapshotNode(
                    directory_fd,
                    parent_fd,
                    components[index],
                    true));
            } catch (...) {
                static_cast<void>(::close(directory_fd));
                throw;
            }
            parent_fd = directory_fd;
        }
        const std::string& leaf_name = components.back();
        const int leaf_fd = ::openat(
            parent_fd,
            leaf_name.c_str(),
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (leaf_fd < 0) {
            fail(TypedPlanErrorCode::kOpenFailed, "typed-plan leaf open failed");
        }
        try {
            impl->leaf = snapshotNode(leaf_fd, parent_fd, leaf_name, false);
        } catch (...) {
            static_cast<void>(::close(leaf_fd));
            throw;
        }
        if (impl->leaf.mode != 0644U) {
            fail(TypedPlanErrorCode::kWrongFileMode, "typed-plan mode must be exact 0644");
        }
        if (impl->leaf.nlink != 1U) {
            fail(TypedPlanErrorCode::kWrongLinkCount, "typed-plan nlink must be exact 1");
        }
        impl->prove();
        repository.proveStable();
        return impl;
    } catch (...) {
        // Ownership already transferred into impl for every successfully
        // opened descriptor; its destructor closes the exact partial chain.
        throw;
    }
}

void validateInvocationBindings(
    const ClosedInvocation& invocation,
    const ValidatedInputs& inputs) {
    if (!adapter_v1::isInternalArm(invocation.arm) ||
        !invocation.typed_plan_path.has_value() ||
        invocation.threads != adapter_v1::kRequiredThreads ||
        !invocation.count_only || invocation.minimum_duration_k == 0U ||
        invocation.prepared_graph_path != inputs.prepared_graph.identity().path ||
        invocation.explicit_label_map_path !=
            inputs.explicit_label_map.identity().path ||
        invocation.query_projection_path !=
            inputs.query_projection.identity().path ||
        invocation.query_path != inputs.query.identity().path) {
        fail(
            TypedPlanErrorCode::kInvalidSelection,
            "runtime invocation is not one exact internal gopher selector/input tuple");
    }
}

}  // namespace

TypedPlanError::TypedPlanError(
    TypedPlanErrorCode code,
    std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

TypedPlanErrorCode TypedPlanError::code() const noexcept { return code_; }

std::uint64_t FreshPaperPlanning::boundaryNanoseconds() const noexcept {
    return boundary_ns_;
}

std::uint64_t FreshPaperPlanning::startedNanoseconds() const noexcept {
    return started_ns_;
}

std::uint64_t FreshPaperPlanning::finishedNanoseconds() const noexcept {
    return finished_ns_;
}

const std::array<std::size_t, kLabelCount>&
FreshPaperPlanning::labelVertexCounts() const noexcept {
    return label_vertex_counts_;
}

const std::array<double, kLabelCount>&
FreshPaperPlanning::labelMeanVertexActiveSnapshots() const noexcept {
    return label_mean_vertex_active_snapshots_;
}

const std::string& FreshPaperPlanning::queryTypedObjectSha256() const noexcept {
    return query_typed_object_sha256_;
}

const std::string&
FreshPaperPlanning::preparedGraphFileSha256Binding() const noexcept {
    return prepared_graph_file_sha256_binding_;
}

const QueryDecomposition& FreshPaperPlanning::decomposition() const noexcept {
    return decomposition_;
}

const QueryPlan& FreshPaperPlanning::queryPlan() const noexcept {
    return query_plan_;
}

FreshPaperPlanning buildFreshPaperPlanAfterBoundary(
    const ValidatedInputs& inputs,
    const Graph& temporal_graph,
    const Graph& query_graph,
    std::uint64_t boundary_ns) {
    const std::uint64_t started = adapter_v1::steadyClockNowNanoseconds();
    if (boundary_ns == 0U || started < boundary_ns) {
        fail(
            TypedPlanErrorCode::kInvalidBoundary,
            "fresh paper planning must begin after the controllable boundary");
    }
    inputs.proveStable();
    validateTemporalGraphAgainstInputs(temporal_graph, inputs);
    const TypedDirectedQueryFingerprint fingerprint =
        makeQueryFingerprint(query_graph);
    bindLiveQueryToLoadedBytes(query_graph, inputs, fingerprint);

    FreshPaperPlanning result;
    result.boundary_ns_ = boundary_ns;
    result.started_ns_ = started;
    result.query_typed_object_sha256_ = fingerprint.sha256;
    result.prepared_graph_file_sha256_binding_ =
        inputs.prepared_graph.identity().sha256;

    if (temporal_graph.vertex_labels.size() !=
            temporal_graph.vertex_active_durations.size()) {
        fail(
            TypedPlanErrorCode::kInvalidLiveGraph,
            "temporal graph label/duration arrays differ in size");
    }
    std::array<std::int64_t, kLabelCount> duration_sums{};
    for (std::size_t vertex = 0;
         vertex < temporal_graph.vertex_labels.size();
         ++vertex) {
        const Label label = temporal_graph.vertex_labels[vertex];
        const int duration = temporal_graph.vertex_active_durations[vertex];
        if (label >= kLabelCount || duration < 0) {
            fail(
                TypedPlanErrorCode::kInvalidLiveGraph,
                "temporal graph contains invalid planning statistic");
        }
        if (duration > 0 &&
            duration_sums[label] >
                std::numeric_limits<std::int64_t>::max() - duration) {
            fail(
                TypedPlanErrorCode::kIntegerRange,
                "label active-duration sum overflow");
        }
        ++result.label_vertex_counts_[label];
        duration_sums[label] += duration;
    }
    for (std::size_t label = 0; label < kLabelCount; ++label) {
        if (result.label_vertex_counts_[label] != 0U) {
            result.label_mean_vertex_active_snapshots_[label] =
                static_cast<double>(duration_sums[label]) /
                static_cast<double>(result.label_vertex_counts_[label]);
            if (!std::isfinite(
                    result.label_mean_vertex_active_snapshots_[label])) {
                fail(
                    TypedPlanErrorCode::kNonfiniteNumber,
                    "label mean activity is nonfinite");
            }
        }
    }
    result.decomposition_ = decomposeQuery(
        query_graph,
        result.label_vertex_counts_,
        result.label_mean_vertex_active_snapshots_,
        SelectivityMode::Paper,
        nullptr);
    result.query_plan_ = makeQueryPlan(
        query_graph, result.decomposition_, OrderMode::Paper);

    const LivePlanTopology topology = deriveLiveTopology(
        query_graph, result.query_plan_, fingerprint);
    result.finished_ns_ = adapter_v1::steadyClockNowNanoseconds();
    validateFreshPlanning(query_graph, result, fingerprint, topology);
    inputs.proveStable();
    result.finished_ns_ = adapter_v1::steadyClockNowNanoseconds();
    if (result.finished_ns_ < result.started_ns_) {
        fail(TypedPlanErrorCode::kInvalidBoundary, "planning clock regressed");
    }
    return result;
}

ValidatedTypedPlan parseAndValidateTypedPlanBytes(
    std::string_view canonical_lf_bytes,
    const ValidatedInputs& inputs,
    std::uint32_t minimum_duration_k,
    std::string_view exact_case_path,
    const Graph& query_graph,
    const FreshPaperPlanning& fresh_planning) {
    static constexpr std::size_t kMaximumPlanBytes = 1U << 20U;
    if (canonical_lf_bytes.size() < 3U ||
        canonical_lf_bytes.size() > kMaximumPlanBytes ||
        canonical_lf_bytes.back() != '\n' ||
        (canonical_lf_bytes.size() >= 2U &&
         canonical_lf_bytes[canonical_lf_bytes.size() - 2U] == '\n')) {
        fail(
            TypedPlanErrorCode::kInvalidJson,
            "typed-plan bytes must have exactly one terminal LF");
    }
    inputs.proveStable();
    const CaseRow& row = selectCase(
        inputs, minimum_duration_k, exact_case_path);
    JsonValue root = JsonParser(canonical_lf_bytes).parse();
    const std::string canonical_no_lf = canonicalJson(root);
    const std::string expected_file_bytes = canonical_no_lf + "\n";
    if (canonical_lf_bytes != std::string_view(expected_file_bytes)) {
        fail(
            TypedPlanErrorCode::kInvalidJson,
            "typed-plan bytes violate canonical compact sorted-key UTF-8+LF contract");
    }
    TypedPlanValue parsed = parseTypedPlanJson(root);
    ValidatedTypedPlan result = validateParsedAgainstLive(
        std::move(parsed),
        canonical_no_lf,
        inputs,
        row,
        query_graph,
        fresh_planning);
    inputs.proveStable();
    return result;
}

GeneratedTypedPlan generateTypedPlanInMemory(
    const ValidatedInputs& inputs,
    std::uint32_t minimum_duration_k,
    std::string_view exact_case_output_path,
    const Graph& query_graph,
    const FreshPaperPlanning& fresh_planning) {
    inputs.proveStable();
    const CaseRow& row = selectCase(
        inputs, minimum_duration_k, exact_case_output_path);
    const TypedDirectedQueryFingerprint fingerprint =
        makeQueryFingerprint(query_graph);
    bindLiveQueryToLoadedBytes(query_graph, inputs, fingerprint);
    if (fresh_planning.queryTypedObjectSha256() != fingerprint.sha256 ||
        fresh_planning.preparedGraphFileSha256Binding() !=
            inputs.prepared_graph.identity().sha256) {
        fail(
            TypedPlanErrorCode::kCrossBindingMismatch,
            "fresh planning graph binding differs at typed-plan generation");
    }
    const LivePlanTopology topology = deriveLiveTopology(
        query_graph, fresh_planning.queryPlan(), fingerprint);
    validateFreshPlanning(
        query_graph, fresh_planning, fingerprint, topology);
    const TypedPlanValue expected = buildExpectedValue(
        inputs, row, fingerprint, fresh_planning, topology);
    const std::string canonical_no_lf =
        canonicalJson(typedPlanJson(expected, true));
    const std::string canonical_lf = canonical_no_lf + "\n";

    // Return only the independently strict-parsed value.  This prevents an
    // emitter-only serialization path from bypassing consumer validation.
    GeneratedTypedPlan result;
    result.validated = parseAndValidateTypedPlanBytes(
        canonical_lf,
        inputs,
        minimum_duration_k,
        exact_case_output_path,
        query_graph,
        fresh_planning);
    result.canonical_lf_bytes = canonical_lf;
    result.canonical_lf_sha256 = adapter_v1::sha256Hex(canonical_lf);
    inputs.proveStable();
    return result;
}

ConsumedTypedPlan::ConsumedTypedPlan() = default;

ConsumedTypedPlan::ConsumedTypedPlan(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ConsumedTypedPlan::~ConsumedTypedPlan() = default;
ConsumedTypedPlan::ConsumedTypedPlan(ConsumedTypedPlan&&) noexcept = default;
ConsumedTypedPlan& ConsumedTypedPlan::operator=(
    ConsumedTypedPlan&&) noexcept = default;

void ConsumedTypedPlan::proveStable() const {
    if (!impl_) {
        fail(TypedPlanErrorCode::kIdentityDrift, "empty consumed typed plan");
    }
    impl_->prove();
}

const TypedPlanValue& ConsumedTypedPlan::typedValue() const noexcept {
    static const TypedPlanValue empty;
    return impl_ ? impl_->validated.value : empty;
}

const QueryPlan& ConsumedTypedPlan::executablePlan() const noexcept {
    static const QueryPlan empty;
    return impl_ ? impl_->validated.executable_plan : empty;
}

const QueryDecomposition&
ConsumedTypedPlan::executableDecomposition() const noexcept {
    static const QueryDecomposition empty;
    return impl_ ? impl_->validated.executable_decomposition : empty;
}

const FileIdentity& ConsumedTypedPlan::identity() const noexcept {
    static const FileIdentity empty;
    return impl_ ? impl_->identity : empty;
}

const TypedPlanConsumptionRecord&
ConsumedTypedPlan::record() const noexcept {
    static const TypedPlanConsumptionRecord empty;
    return impl_ ? impl_->record : empty;
}

ConsumedTypedPlan consumeTypedPlanAfterBoundary(
    const RepositoryAnchor& repository,
    const ClosedInvocation& invocation,
    const ValidatedInputs& inputs,
    const Graph& query_graph,
    const FreshPaperPlanning& fresh_planning) {
    validateInvocationBindings(invocation, inputs);
    if (adapter_v1::steadyClockNowNanoseconds() <
        fresh_planning.finishedNanoseconds()) {
        fail(
            TypedPlanErrorCode::kInvalidBoundary,
            "typed-plan read began before fresh paper planning finished");
    }
    const std::string& exact_path = *invocation.typed_plan_path;
    static_cast<void>(selectCase(
        inputs, invocation.minimum_duration_k, exact_path));
    inputs.proveStable();
    std::unique_ptr<ConsumedTypedPlan::Impl> impl =
        openHeldTypedPlan(repository, exact_path);
    const std::string bytes = readExactHeldFile(impl->leaf);
    impl->prove();
    repository.proveStable();

    impl->validated = parseAndValidateTypedPlanBytes(
        bytes,
        inputs,
        invocation.minimum_duration_k,
        exact_path,
        query_graph,
        fresh_planning);
    if (readExactHeldFile(impl->leaf) != bytes) {
        fail(
            TypedPlanErrorCode::kIdentityDrift,
            "typed-plan held readback changed during strict validation");
    }
    impl->prove();
    inputs.proveStable();
    repository.proveStable();

    impl->identity.role = "typed_plan";
    impl->identity.path = exact_path;
    impl->identity.sha256 = adapter_v1::sha256Hex(bytes);
    impl->identity.bytes = impl->leaf.size;
    impl->identity.mode = impl->leaf.mode;
    impl->identity.nlink = impl->leaf.nlink;
    impl->identity.file_type = "regular";
    impl->identity.device = impl->leaf.device;
    impl->identity.inode = impl->leaf.inode;
    impl->identity.object_sha256 = impl->validated.runtime_object_sha256;

    impl->record.lexical_path = exact_path;
    impl->record.device = impl->leaf.device;
    impl->record.inode = impl->leaf.inode;
    impl->record.file_sha256 = impl->identity.sha256;
    impl->record.bytes = impl->leaf.size;
    impl->record.mode = impl->leaf.mode;
    impl->record.nlink = impl->leaf.nlink;
    impl->record.file_type = "regular";
    impl->record.payload_object_sha256 =
        impl->validated.payload_object_sha256;
    impl->record.runtime_object_sha256 =
        impl->validated.runtime_object_sha256;
    // A held, strictly validated plan is not yet an executed plan.  The
    // future internal runtime adapter must create separate positive
    // consumption evidence only after it passes this exact QueryPlan object
    // to the selected direct or DAG backend.
    impl->record.plan_consumed = false;
    impl->prove();
    return ConsumedTypedPlan(std::move(impl));
}

namespace {

JsonValue edgeArrayJson(
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& edges) {
    JsonValue::Array output;
    output.reserve(edges.size());
    for (const auto& [source, target] : edges) {
        JsonValue::Array edge;
        edge.emplace_back(uintValue(source));
        edge.emplace_back(uintValue(target));
        output.emplace_back(JsonValue(std::move(edge)));
    }
    return JsonValue(std::move(output));
}

JsonValue uintArrayJson(const std::vector<std::uint32_t>& values) {
    JsonValue::Array output;
    output.reserve(values.size());
    for (std::uint32_t value : values) {
        output.emplace_back(uintValue(value));
    }
    return JsonValue(std::move(output));
}

JsonValue nestedUintArrayJson(
    const std::vector<std::vector<std::uint32_t>>& values) {
    JsonValue::Array output;
    output.reserve(values.size());
    for (const auto& row : values) {
        output.emplace_back(uintArrayJson(row));
    }
    return JsonValue(std::move(output));
}

JsonValue queryFingerprintJson(
    const TypedDirectedQueryFingerprint& value,
    bool include_sha256) {
    JsonValue::Object output;
    output.emplace("schema", stringValue(value.schema));
    output.emplace("vertex_count", uintValue(value.vertex_count));
    output.emplace("vertex_labels", uintArrayJson(value.vertex_labels));
    output.emplace("directed_edges", edgeArrayJson(value.directed_edges));
    if (include_sha256) {
        output.emplace("sha256", stringValue(value.sha256));
    }
    return JsonValue(std::move(output));
}

JsonValue rootStructureJson(const TypedRootStructure& value) {
    JsonValue::Object output;
    output.emplace("root", intValue(value.root));
    output.emplace("connected", JsonValue(value.connected));
    output.emplace("query_tree_mode", stringValue(value.query_tree_mode));
    output.emplace(
        "spanning_tree_edges",
        edgeArrayJson(value.spanning_tree_edges));
    output.emplace(
        "non_tree_directed_edges",
        edgeArrayJson(value.non_tree_directed_edges));
    return JsonValue(std::move(output));
}

JsonValue parentArrayJson(
    const std::vector<std::optional<std::uint32_t>>& parents) {
    JsonValue::Array output;
    output.reserve(parents.size());
    for (const auto& parent : parents) {
        if (parent.has_value()) {
            output.emplace_back(uintValue(*parent));
        } else {
            output.emplace_back(JsonValue(nullptr));
        }
    }
    return JsonValue(std::move(output));
}

JsonValue legacyJson(const TypedLegacyFingerprints& value) {
    JsonValue::Object output;
    output.emplace("fnv_plan_id", stringValue(value.fnv_plan_id));
    output.emplace("role", stringValue(value.role));
    return JsonValue(std::move(output));
}

JsonValue typedPlanJson(const TypedPlanValue& value, bool include_payload) {
    JsonValue::Object output;
    output.emplace("schema_version", uintValue(value.schema_version));
    output.emplace("artifact_kind", stringValue(value.artifact_kind));
    output.emplace("contract_id", stringValue(value.contract_id));
    output.emplace("case_index", uintValue(value.case_index));
    output.emplace("case_id", stringValue(value.case_id));
    output.emplace("dataset_id", stringValue(value.dataset_id));
    output.emplace(
        "prepared_graph_sha256",
        stringValue(value.prepared_graph_sha256));
    output.emplace(
        "global_label_map_file_sha256",
        stringValue(value.global_label_map_file_sha256));
    output.emplace(
        "global_label_map_mapping_sha256",
        stringValue(value.global_label_map_mapping_sha256));
    output.emplace(
        "query_projection_file_sha256",
        stringValue(value.query_projection_file_sha256));
    output.emplace("query_sha256", stringValue(value.query_sha256));
    output.emplace(
        "minimum_duration_k",
        uintValue(value.minimum_duration_k));
    output.emplace("selectivity_mode", stringValue(value.selectivity_mode));
    output.emplace("order_mode", stringValue(value.order_mode));
    output.emplace(
        "query_fingerprint",
        queryFingerprintJson(value.query_fingerprint, true));
    output.emplace("root_structure", rootStructureJson(value.root_structure));
    output.emplace("operator_order", uintArrayJson(value.operator_order));
    output.emplace(
        "parent_relationships",
        parentArrayJson(value.parent_relationships));
    output.emplace(
        "earlier_neighbors",
        nestedUintArrayJson(value.earlier_neighbors));
    output.emplace(
        "root_selectivity_score_binary64_hex",
        stringValue(value.root_selectivity_score_binary64_hex));
    output.emplace(
        "legacy_fingerprints",
        legacyJson(value.legacy_fingerprints));
    if (include_payload) {
        output.emplace(
            "plan_payload_sha256",
            stringValue(value.plan_payload_sha256));
    }
    return JsonValue(std::move(output));
}

std::vector<std::uint32_t> parseUintArray(
    const JsonValue& value,
    std::string_view context) {
    const auto& array = asArray(value, context);
    std::vector<std::uint32_t> result;
    result.reserve(array.size());
    for (std::size_t index = 0; index < array.size(); ++index) {
        result.push_back(checkedUint32(
            array[index],
            std::string(context) + "[" + std::to_string(index) + "]"));
    }
    return result;
}

std::vector<std::vector<std::uint32_t>> parseNestedUintArray(
    const JsonValue& value,
    std::string_view context) {
    const auto& array = asArray(value, context);
    std::vector<std::vector<std::uint32_t>> result;
    result.reserve(array.size());
    for (std::size_t index = 0; index < array.size(); ++index) {
        result.push_back(parseUintArray(
            array[index],
            std::string(context) + "[" + std::to_string(index) + "]"));
    }
    return result;
}

std::vector<std::pair<std::uint32_t, std::uint32_t>> parseEdgeArray(
    const JsonValue& value,
    std::string_view context) {
    const auto& array = asArray(value, context);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> result;
    result.reserve(array.size());
    for (std::size_t index = 0; index < array.size(); ++index) {
        const auto& edge = asArray(
            array[index],
            std::string(context) + "[" + std::to_string(index) + "]");
        if (edge.size() != 2U) {
            fail(
                TypedPlanErrorCode::kSchemaMismatch,
                std::string(context) + " edge must have two vertices");
        }
        result.emplace_back(
            checkedUint32(edge[0], std::string(context) + " source"),
            checkedUint32(edge[1], std::string(context) + " target"));
    }
    return result;
}

TypedDirectedQueryFingerprint parseQueryFingerprint(
    const JsonValue& value) {
    const auto& object = asObject(value, "query_fingerprint");
    requireExactKeys(
        object,
        {"schema", "vertex_count", "vertex_labels", "directed_edges", "sha256"},
        "query_fingerprint");
    TypedDirectedQueryFingerprint result;
    result.schema = asString(member(object, "schema"), "query schema");
    result.vertex_count = checkedUint32(
        member(object, "vertex_count"), "query vertex_count");
    result.vertex_labels = parseUintArray(
        member(object, "vertex_labels"), "query vertex_labels");
    result.directed_edges = parseEdgeArray(
        member(object, "directed_edges"), "query directed_edges");
    result.sha256 = asString(member(object, "sha256"), "query sha256");
    return result;
}

TypedRootStructure parseRootStructure(const JsonValue& value) {
    const auto& object = asObject(value, "root_structure");
    requireExactKeys(
        object,
        {"root", "connected", "query_tree_mode", "spanning_tree_edges",
         "non_tree_directed_edges"},
        "root_structure");
    TypedRootStructure result;
    result.root = checkedInt32(member(object, "root"), "root_structure.root");
    result.connected = asBoolean(
        member(object, "connected"), "root_structure.connected");
    result.query_tree_mode = asString(
        member(object, "query_tree_mode"),
        "root_structure.query_tree_mode");
    result.spanning_tree_edges = parseEdgeArray(
        member(object, "spanning_tree_edges"),
        "root_structure.spanning_tree_edges");
    result.non_tree_directed_edges = parseEdgeArray(
        member(object, "non_tree_directed_edges"),
        "root_structure.non_tree_directed_edges");
    return result;
}

std::vector<std::optional<std::uint32_t>> parseParentArray(
    const JsonValue& value) {
    const auto& array = asArray(value, "parent_relationships");
    std::vector<std::optional<std::uint32_t>> result;
    result.reserve(array.size());
    for (std::size_t index = 0; index < array.size(); ++index) {
        if (isNull(array[index])) {
            result.emplace_back(std::nullopt);
        } else {
            result.emplace_back(checkedUint32(
                array[index],
                "parent_relationships[" + std::to_string(index) + "]"));
        }
    }
    return result;
}

TypedLegacyFingerprints parseLegacy(const JsonValue& value) {
    const auto& object = asObject(value, "legacy_fingerprints");
    requireExactKeys(
        object, {"fnv_plan_id", "role"}, "legacy_fingerprints");
    TypedLegacyFingerprints result;
    result.fnv_plan_id = asString(
        member(object, "fnv_plan_id"), "legacy fnv_plan_id");
    result.role = asString(member(object, "role"), "legacy role");
    return result;
}

TypedPlanValue parseTypedPlanJson(const JsonValue& root) {
    const auto& object = asObject(root, "typed plan");
    requireExactKeys(
        object,
        {"schema_version", "artifact_kind", "contract_id", "case_index",
         "case_id", "dataset_id", "prepared_graph_sha256",
         "global_label_map_file_sha256", "global_label_map_mapping_sha256",
         "query_projection_file_sha256", "query_sha256", "minimum_duration_k",
         "selectivity_mode", "order_mode", "query_fingerprint",
         "root_structure", "operator_order", "parent_relationships",
         "earlier_neighbors", "root_selectivity_score_binary64_hex",
         "legacy_fingerprints", "plan_payload_sha256"},
        "typed plan");

    TypedPlanValue result;
    result.schema_version = checkedUint32(
        member(object, "schema_version"), "schema_version");
    result.artifact_kind = asString(
        member(object, "artifact_kind"), "artifact_kind");
    result.contract_id = asString(
        member(object, "contract_id"), "contract_id");
    result.case_index = checkedUint32(
        member(object, "case_index"), "case_index");
    result.case_id = asString(member(object, "case_id"), "case_id");
    result.dataset_id = asString(member(object, "dataset_id"), "dataset_id");
    result.prepared_graph_sha256 = asString(
        member(object, "prepared_graph_sha256"), "prepared_graph_sha256");
    result.global_label_map_file_sha256 = asString(
        member(object, "global_label_map_file_sha256"),
        "global_label_map_file_sha256");
    result.global_label_map_mapping_sha256 = asString(
        member(object, "global_label_map_mapping_sha256"),
        "global_label_map_mapping_sha256");
    result.query_projection_file_sha256 = asString(
        member(object, "query_projection_file_sha256"),
        "query_projection_file_sha256");
    result.query_sha256 = asString(
        member(object, "query_sha256"), "query_sha256");
    result.minimum_duration_k = checkedUint32(
        member(object, "minimum_duration_k"), "minimum_duration_k");
    result.selectivity_mode = asString(
        member(object, "selectivity_mode"), "selectivity_mode");
    result.order_mode = asString(
        member(object, "order_mode"), "order_mode");
    result.query_fingerprint = parseQueryFingerprint(
        member(object, "query_fingerprint"));
    result.root_structure = parseRootStructure(
        member(object, "root_structure"));
    result.operator_order = parseUintArray(
        member(object, "operator_order"), "operator_order");
    result.parent_relationships = parseParentArray(
        member(object, "parent_relationships"));
    result.earlier_neighbors = parseNestedUintArray(
        member(object, "earlier_neighbors"), "earlier_neighbors");
    result.root_selectivity_score_binary64_hex = asString(
        member(object, "root_selectivity_score_binary64_hex"),
        "root_selectivity_score_binary64_hex");
    result.legacy_fingerprints = parseLegacy(
        member(object, "legacy_fingerprints"));
    result.plan_payload_sha256 = asString(
        member(object, "plan_payload_sha256"), "plan_payload_sha256");
    return result;
}

std::string sha256Object(const JsonValue& value) {
    return adapter_v1::sha256Hex(canonicalJson(value));
}

std::string uint64Hex(std::uint64_t value) {
    char buffer[16];
    std::fill(std::begin(buffer), std::end(buffer), '0');
    char digits[16];
    const auto result = std::to_chars(
        std::begin(digits), std::end(digits), value, 16);
    if (result.ec != std::errc{}) {
        fail(TypedPlanErrorCode::kIntegerRange, "uint64 hex serialization failed");
    }
    const std::size_t digit_count =
        static_cast<std::size_t>(result.ptr - std::begin(digits));
    std::copy(
        std::begin(digits), result.ptr,
        std::end(buffer) - static_cast<std::ptrdiff_t>(digit_count));
    return std::string(std::begin(buffer), std::end(buffer));
}

std::uint64_t parseUint64Hex(std::string_view value, std::string_view context) {
    if (!validLowerHex(value, 16U)) {
        fail(
            TypedPlanErrorCode::kSchemaMismatch,
            std::string(context) + " must be 16 lowercase hex digits");
    }
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), result, 16);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size()) {
        fail(TypedPlanErrorCode::kIntegerRange, "uint64 hex parse failed");
    }
    return result;
}

std::string binary64Hex(double value) {
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    if (!std::isfinite(value)) {
        fail(
            TypedPlanErrorCode::kNonfiniteNumber,
            "root selectivity score is nonfinite");
    }
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return uint64Hex(bits);
}

double binary64FromHex(std::string_view value) {
    const std::uint64_t bits = parseUint64Hex(value, "binary64 score");
    double result = 0.0;
    std::memcpy(&result, &bits, sizeof(result));
    if (!std::isfinite(result)) {
        fail(
            TypedPlanErrorCode::kNonfiniteNumber,
            "root selectivity score bits encode nonfinite value");
    }
    return result;
}

constexpr const char* kBitcoinGraph =
    "Dataset/bitcoin-temporal-timeinstance-unique-1w_filtered.txt";
constexpr const char* kBitcoinMap =
    "benchmark/icde2027_m4_candidate_pool_v2/label_maps/bitcoin/uniform_c005.json";
constexpr const char* kSuperuserGraph =
    "Dataset/sx-superuser-temporal-1w-unique_filtered.txt";
constexpr const char* kSuperuserMap =
    "benchmark/icde2027_m4_candidate_pool_v2/label_maps/sx_superuser/uniform_c005.json";

const std::array<CaseRow, kCaseCount>& caseRows() {
    static const std::array<CaseRow, kCaseCount> rows{{
        {0, "icde27_full_fair_sx_superuser_path3_e3_k3_r001", "sx_superuser", 3,
         kSuperuserGraph, kSuperuserMap,
         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/sx_superuser/path3_projection.json",
         "benchmark/queries/paper_parameters_v2/sx_superuser/path3_k3_rank001.qry"},
        {1, "icde27_full_fair_wiki_talk_path3_e3_k3_r001", "wiki_talk", 3,
         "Dataset/wiki-talk-temporal-timeinstance-unique-1w_filtered.txt",
         "benchmark/icde2027_m4_candidate_pool_v2/label_maps/wiki_talk/uniform_c005.json",
         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/wiki_talk/path3_projection.json",
         "benchmark/queries/paper_parameters_v2/wiki_talk/path3_k3_rank001.qry"},
        {2, "icde27_full_fair_sx_stackoverflow_path3_e3_k3_r001", "sx_stackoverflow", 3,
         "Dataset/sx-stackoverflow-timeinstance-unique-1w_filtered.txt",
         "benchmark/icde2027_m4_candidate_pool_v2/label_maps/sx_stackoverflow/uniform_c005.json",
         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/sx_stackoverflow/path3_projection.json",
         "benchmark/queries/paper_parameters_v2/sx_stackoverflow/path3_k3_rank001.qry"},
        {3, "icde27_full_fair_bitcoin_path3_e3_k3_r001", "bitcoin", 3,
         kBitcoinGraph, kBitcoinMap,
         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/bitcoin/path3_projection.json",
         "benchmark/queries/paper_parameters_v2/bitcoin/path3_k3_rank001.qry"},
        {4, "icde27_full_fair_bitcoin_cycle3_e3_k3_r001", "bitcoin", 3,
         kBitcoinGraph, kBitcoinMap,
         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/bitcoin/cycle3_projection.json",
         "benchmark/queries/paper_parameters_v2/bitcoin/cycle3_k3_rank001.qry"},
        {5, "icde27_full_fair_bitcoin_tailed4_e4_k3_r001", "bitcoin", 3,
         kBitcoinGraph, kBitcoinMap,
         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/bitcoin/tailed4_projection.json",
         "benchmark/queries/paper_parameters_v2/bitcoin/tailed4_k3_rank001.qry"},
        {6, "icde27_full_fair_bitcoin_path4_e4_k3_r001", "bitcoin", 3,
         kBitcoinGraph, kBitcoinMap,
         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/bitcoin/path4_projection.json",
         "benchmark/queries/paper_parameters_v2/bitcoin/path4_k3_rank001.qry"},
        {7, "icde27_full_fair_bitcoin_path5_e5_k3_r001", "bitcoin", 3,
         kBitcoinGraph, kBitcoinMap,
         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/bitcoin/path5_projection.json",
         "benchmark/queries/paper_parameters_v2/bitcoin/path5_k3_rank001.qry"},
        {8, "icde27_full_fair_bitcoin_path3_e3_k4_r001", "bitcoin", 4,
         kBitcoinGraph, kBitcoinMap,
         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/bitcoin/path3_projection.json",
         "benchmark/queries/paper_parameters_v2/bitcoin/path3_k3_rank001.qry"},
        {9, "icde27_full_fair_bitcoin_path3_e3_k5_r001", "bitcoin", 5,
         kBitcoinGraph, kBitcoinMap,
         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/bitcoin/path3_projection.json",
         "benchmark/queries/paper_parameters_v2/bitcoin/path3_k3_rank001.qry"},
        {10, "icde27_full_fair_sx_superuser_path4_e4_k3_r001", "sx_superuser", 3,
         kSuperuserGraph, kSuperuserMap,
         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/sx_superuser/path4_projection.json",
         "benchmark/queries/paper_parameters_v2/sx_superuser/path4_k3_rank001.qry"},
        {11, "icde27_full_fair_sx_superuser_star5_e5_k3_r001", "sx_superuser", 3,
         kSuperuserGraph, kSuperuserMap,
         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/sx_superuser/star5_projection.json",
         "benchmark/queries/paper_parameters_v2/sx_superuser/star5_k3_rank001.qry"},
        {12, "icde27_full_fair_sx_superuser_path3_e3_k4_r001", "sx_superuser", 4,
         kSuperuserGraph, kSuperuserMap,
         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/sx_superuser/path3_projection.json",
         "benchmark/queries/paper_parameters_v2/sx_superuser/path3_k3_rank001.qry"},
        {13, "icde27_full_fair_sx_superuser_path3_e3_k5_r001", "sx_superuser", 5,
         kSuperuserGraph, kSuperuserMap,
         "benchmark/icde2027_e3_e4_shared_inputs_v1/query_projections/sx_superuser/path3_projection.json",
         "benchmark/queries/paper_parameters_v2/sx_superuser/path3_k3_rank001.qry"},
    }};
    return rows;
}

std::string planPath(const CaseRow& row) {
    return std::string(kTypedPlanDirectory) + "/" + row.case_id + ".json";
}

void validateInputIdentity(
    const FileIdentity& identity,
    std::string_view role,
    std::string_view path) {
    if (identity.role != std::string(role) ||
        identity.path != std::string(path) ||
        !validLowerSha256(identity.sha256) ||
        identity.file_type != "regular" || identity.nlink != 1U ||
        identity.bytes == 0U) {
        fail(
            TypedPlanErrorCode::kInvalidIdentity,
            "validated input identity/corpus path mismatch for " +
                std::string(role));
    }
}

const CaseRow& selectCase(
    const ValidatedInputs& inputs,
    std::uint32_t minimum_duration_k,
    std::string_view exact_plan_path) {
    const CaseRow* selected = nullptr;
    for (const CaseRow& row : caseRows()) {
        const std::string expected_plan_path = planPath(row);
        if (minimum_duration_k == row.minimum_duration_k &&
            exact_plan_path == std::string_view(expected_plan_path) &&
            inputs.prepared_graph.identity().path == row.graph_path &&
            inputs.explicit_label_map.identity().path == row.map_path &&
            inputs.query_projection.identity().path == row.projection_path &&
            inputs.query.identity().path == row.query_path) {
            if (selected != nullptr) {
                fail(
                    TypedPlanErrorCode::kInvalidSelection,
                    "ambiguous typed-plan case binding");
            }
            selected = &row;
        }
    }
    if (selected == nullptr) {
        fail(
            TypedPlanErrorCode::kInvalidSelection,
            "path/input/K tuple is not one exact fourteen-case binding");
    }
    validateInputIdentity(
        inputs.prepared_graph.identity(), "prepared_graph", selected->graph_path);
    validateInputIdentity(
        inputs.explicit_label_map.identity(), "explicit_label_map", selected->map_path);
    validateInputIdentity(
        inputs.query_projection.identity(), "query_projection", selected->projection_path);
    validateInputIdentity(
        inputs.query.identity(), "query", selected->query_path);
    if (!validLowerSha256(inputs.global_label_map_mapping_sha256) ||
        !inputs.loads.all_completed_before_boundary) {
        fail(
            TypedPlanErrorCode::kInvalidIdentity,
            "map mapping identity or preboundary load proof is invalid");
    }
    return *selected;
}

std::vector<std::pair<std::uint32_t, std::uint32_t>> liveDirectedEdges(
    const Graph& query) {
    if (query.num_vertices < 1 ||
        query.vertex_labels.size() !=
            static_cast<std::size_t>(query.num_vertices) ||
        query.adj.size() != static_cast<std::size_t>(query.num_vertices) ||
        query.in_adj.size() != static_cast<std::size_t>(query.num_vertices)) {
        fail(TypedPlanErrorCode::kInvalidLiveGraph, "query graph shape mismatch");
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> outgoing;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> incoming;
    for (int source = 0; source < query.num_vertices; ++source) {
        if (query.vertex_labels[static_cast<std::size_t>(source)] >= kLabelCount) {
            fail(TypedPlanErrorCode::kInvalidLiveGraph, "query label outside A--E");
        }
        for (const Edge& edge : query.adj[static_cast<std::size_t>(source)]) {
            if (edge.to < 0 || edge.to >= query.num_vertices || edge.to == source) {
                fail(TypedPlanErrorCode::kInvalidLiveGraph, "invalid query directed arc");
            }
            outgoing.emplace_back(
                static_cast<std::uint32_t>(source),
                static_cast<std::uint32_t>(edge.to));
        }
        for (const Edge& edge : query.in_adj[static_cast<std::size_t>(source)]) {
            if (edge.to < 0 || edge.to >= query.num_vertices || edge.to == source) {
                fail(TypedPlanErrorCode::kInvalidLiveGraph, "invalid query incoming arc");
            }
            incoming.emplace_back(
                static_cast<std::uint32_t>(edge.to),
                static_cast<std::uint32_t>(source));
        }
    }
    std::sort(outgoing.begin(), outgoing.end());
    std::sort(incoming.begin(), incoming.end());
    if (std::adjacent_find(outgoing.begin(), outgoing.end()) != outgoing.end() ||
        std::adjacent_find(incoming.begin(), incoming.end()) != incoming.end() ||
        outgoing != incoming || outgoing.empty()) {
        fail(
            TypedPlanErrorCode::kInvalidLiveGraph,
            "query arcs must be nonempty, unique, and adj/in_adj mirrored");
    }
    return outgoing;
}

TypedDirectedQueryFingerprint makeQueryFingerprint(const Graph& query) {
    TypedDirectedQueryFingerprint result;
    result.schema = "typed-directed-query-v1";
    result.vertex_count = static_cast<std::uint32_t>(query.num_vertices);
    result.vertex_labels.reserve(query.vertex_labels.size());
    for (Label label : query.vertex_labels) {
        result.vertex_labels.push_back(static_cast<std::uint32_t>(label));
    }
    result.directed_edges = liveDirectedEdges(query);
    result.sha256 = sha256Object(queryFingerprintJson(result, false));
    return result;
}

void bindLiveQueryToLoadedBytes(
    const Graph& query,
    const ValidatedInputs& inputs,
    const TypedDirectedQueryFingerprint& fingerprint) {
    if (inputs.parsed_query.vertices.size() != fingerprint.vertex_count) {
        fail(
            TypedPlanErrorCode::kCrossBindingMismatch,
            "live query vertex count differs from held query bytes");
    }
    for (std::size_t vertex = 0;
         vertex < inputs.parsed_query.vertices.size();
         ++vertex) {
        if (inputs.parsed_query.vertices[vertex].runtime_class_id !=
            fingerprint.vertex_labels[vertex]) {
            fail(
                TypedPlanErrorCode::kCrossBindingMismatch,
                "live query label differs from held query bytes");
        }
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> parsed_edges;
    parsed_edges.reserve(inputs.parsed_query.directed_edges.size());
    for (const auto& edge : inputs.parsed_query.directed_edges) {
        if (edge.source_index >= fingerprint.vertex_count ||
            edge.target_index >= fingerprint.vertex_count ||
            edge.source_index == edge.target_index) {
            fail(
                TypedPlanErrorCode::kCrossBindingMismatch,
                "held parsed query contains an invalid directed edge");
        }
        parsed_edges.emplace_back(edge.source_index, edge.target_index);
    }
    std::sort(parsed_edges.begin(), parsed_edges.end());
    if (std::adjacent_find(parsed_edges.begin(), parsed_edges.end()) !=
            parsed_edges.end() ||
        parsed_edges != fingerprint.directed_edges) {
        fail(
            TypedPlanErrorCode::kCrossBindingMismatch,
            "live query arcs differ from held query bytes");
    }
    static_cast<void>(query);
}

std::pair<std::uint32_t, std::uint32_t> weakKey(
    std::uint32_t first,
    std::uint32_t second) {
    return std::minmax(first, second);
}

LivePlanTopology deriveLiveTopology(
    const Graph& query,
    const QueryPlan& plan,
    const TypedDirectedQueryFingerprint& fingerprint) {
    const std::size_t vertex_count = fingerprint.vertex_count;
    if (plan.root < 0 ||
        static_cast<std::size_t>(plan.root) >= vertex_count ||
        plan.order.size() != vertex_count ||
        plan.parent.size() != vertex_count ||
        plan.earlier_neighbors.size() != vertex_count ||
        plan.order.front() != plan.root ||
        plan.selectivity_mode != SelectivityMode::Paper ||
        !std::isfinite(plan.root_selectivity_score)) {
        fail(TypedPlanErrorCode::kInvalidFreshPlan, "fresh paper plan shape invalid");
    }
    std::vector<int> position(vertex_count, -1);
    for (std::size_t index = 0; index < plan.order.size(); ++index) {
        const int vertex = plan.order[index];
        if (vertex < 0 || static_cast<std::size_t>(vertex) >= vertex_count ||
            position[static_cast<std::size_t>(vertex)] != -1) {
            fail(TypedPlanErrorCode::kInvalidFreshPlan, "operator order is not a permutation");
        }
        position[static_cast<std::size_t>(vertex)] = static_cast<int>(index);
    }
    std::set<std::pair<std::uint32_t, std::uint32_t>> edge_set(
        fingerprint.directed_edges.begin(), fingerprint.directed_edges.end());
    std::set<std::pair<std::uint32_t, std::uint32_t>> weak_edges;
    for (const auto& [source, target] : fingerprint.directed_edges) {
        weak_edges.insert(weakKey(source, target));
    }

    LivePlanTopology result;
    result.earlier_neighbors.resize(vertex_count);
    result.spanning_tree_adj.resize(vertex_count);
    std::set<std::pair<std::uint32_t, std::uint32_t>> tree_weak_edges;
    std::set<std::pair<std::uint32_t, std::uint32_t>> selected_tree_arcs;

    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        const int parent = plan.parent[vertex];
        if (static_cast<int>(vertex) == plan.root) {
            if (parent != -1) {
                fail(TypedPlanErrorCode::kInvalidFreshPlan, "root parent must be -1");
            }
            continue;
        }
        if (parent < 0 || static_cast<std::size_t>(parent) >= vertex_count ||
            position[static_cast<std::size_t>(parent)] >= position[vertex] ||
            weak_edges.find(weakKey(
                static_cast<std::uint32_t>(parent),
                static_cast<std::uint32_t>(vertex))) == weak_edges.end()) {
            fail(
                TypedPlanErrorCode::kInvalidFreshPlan,
                "nonroot parent must be an earlier weak neighbor");
        }
        const auto forward = std::make_pair(
            static_cast<std::uint32_t>(parent),
            static_cast<std::uint32_t>(vertex));
        const auto reverse = std::make_pair(
            static_cast<std::uint32_t>(vertex),
            static_cast<std::uint32_t>(parent));
        const auto chosen = edge_set.find(forward) != edge_set.end()
            ? forward
            : reverse;
        result.tree_arcs.push_back(chosen);
        selected_tree_arcs.insert(chosen);
        tree_weak_edges.insert(weakKey(chosen.first, chosen.second));
    }

    // Preserve DFS child visitation order, not numeric vertex order.
    for (int vertex : plan.order) {
        if (vertex == plan.root) continue;
        result.spanning_tree_adj[
            static_cast<std::size_t>(
                plan.parent[static_cast<std::size_t>(vertex)])]
            .push_back(vertex);
    }

    for (const auto& edge : fingerprint.directed_edges) {
        if (selected_tree_arcs.find(edge) == selected_tree_arcs.end()) {
            // In a reciprocal tree pair, exactly the chosen original directed
            // arc is the serialized tree arc; the reverse remains non-tree.
            result.serialized_non_tree.push_back(edge);
        }
    }
    for (int source = 0; source < query.num_vertices; ++source) {
        for (const Edge& edge : query.adj[static_cast<std::size_t>(source)]) {
            const auto weak = weakKey(
                static_cast<std::uint32_t>(source),
                static_cast<std::uint32_t>(edge.to));
            if (tree_weak_edges.find(weak) == tree_weak_edges.end()) {
                // Engine decomposition intentionally excludes both directions
                // of a weak tree pair; it never copies serialized non-tree.
                result.engine_non_tree.emplace_back(source, edge.to);
            }
        }
    }
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        std::vector<std::uint32_t>& expected = result.earlier_neighbors[vertex];
        for (std::uint32_t neighbor = 0;
             neighbor < fingerprint.vertex_count;
             ++neighbor) {
            if (neighbor != vertex &&
                weak_edges.find(weakKey(
                    static_cast<std::uint32_t>(vertex), neighbor)) !=
                    weak_edges.end() &&
                position[neighbor] < position[vertex]) {
                expected.push_back(neighbor);
            }
        }
        std::vector<int> actual = plan.earlier_neighbors[vertex];
        std::vector<int> expected_int;
        expected_int.reserve(expected.size());
        for (std::uint32_t neighbor : expected) {
            expected_int.push_back(static_cast<int>(neighbor));
        }
        if (actual != expected_int) {
            fail(
                TypedPlanErrorCode::kInvalidFreshPlan,
                "fresh earlier-neighbor rows are not exact");
        }
    }
    if (result.tree_arcs.size() + 1U != vertex_count ||
        result.tree_arcs.size() + result.serialized_non_tree.size() !=
            fingerprint.directed_edges.size()) {
        fail(
            TypedPlanErrorCode::kInvalidFreshPlan,
            "directed arc partition cardinality mismatch");
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> partition =
        result.tree_arcs;
    partition.insert(
        partition.end(),
        result.serialized_non_tree.begin(),
        result.serialized_non_tree.end());
    std::sort(partition.begin(), partition.end());
    if (partition != fingerprint.directed_edges) {
        fail(
            TypedPlanErrorCode::kInvalidFreshPlan,
            "directed arc partition is not exhaustive and disjoint");
    }
    return result;
}

void validateFreshPlanning(
    const Graph& query,
    const FreshPaperPlanning& fresh,
    const TypedDirectedQueryFingerprint& fingerprint,
    const LivePlanTopology& topology) {
    if (fresh.boundaryNanoseconds() == 0U ||
        fresh.startedNanoseconds() < fresh.boundaryNanoseconds() ||
        fresh.finishedNanoseconds() < fresh.startedNanoseconds() ||
        fresh.queryTypedObjectSha256() != fingerprint.sha256 ||
        !validLowerSha256(fresh.preparedGraphFileSha256Binding())) {
        fail(
            TypedPlanErrorCode::kInvalidBoundary,
            "fresh paper planning is not postboundary");
    }
    const QueryDecomposition& decomposition = fresh.decomposition();
    const QueryPlan& plan = fresh.queryPlan();
    if (!decomposition.connected ||
        decomposition.selectivity_mode != SelectivityMode::Paper ||
        decomposition.root != plan.root ||
        decomposition.parent != plan.parent ||
        decomposition.dfs_order != plan.order ||
        decomposition.spanning_tree_adj != topology.spanning_tree_adj ||
        decomposition.non_tree_edges != topology.engine_non_tree ||
        decomposition.temporal_selectivity.size() != fingerprint.vertex_count ||
        decomposition.vertex_labels != query.vertex_labels) {
        fail(
            TypedPlanErrorCode::kInvalidFreshPlan,
            "fresh decomposition does not equal live reconstructed engine topology");
    }
    if (decomposition.temporal_selectivity[
            static_cast<std::size_t>(plan.root)] !=
        plan.root_selectivity_score) {
        fail(
            TypedPlanErrorCode::kInvalidFreshPlan,
            "fresh root score differs between decomposition and plan");
    }
}

TypedPlanValue buildExpectedValue(
    const ValidatedInputs& inputs,
    const CaseRow& row,
    const TypedDirectedQueryFingerprint& fingerprint,
    const FreshPaperPlanning& fresh,
    const LivePlanTopology& topology) {
    const QueryPlan& plan = fresh.queryPlan();
    TypedPlanValue result;
    result.schema_version = kSchemaVersion;
    result.artifact_kind = std::string(kArtifactKind);
    result.contract_id = std::string(kContractId);
    result.case_index = row.case_index;
    result.case_id = row.case_id;
    result.dataset_id = row.dataset_id;
    result.prepared_graph_sha256 = inputs.prepared_graph.identity().sha256;
    result.global_label_map_file_sha256 =
        inputs.explicit_label_map.identity().sha256;
    result.global_label_map_mapping_sha256 =
        inputs.global_label_map_mapping_sha256;
    result.query_projection_file_sha256 =
        inputs.query_projection.identity().sha256;
    result.query_sha256 = inputs.query.identity().sha256;
    result.minimum_duration_k = row.minimum_duration_k;
    result.selectivity_mode = "paper";
    result.order_mode = "paper";
    result.query_fingerprint = fingerprint;
    result.root_structure.root = plan.root;
    result.root_structure.connected = true;
    result.root_structure.query_tree_mode = "dfs";
    result.root_structure.spanning_tree_edges = topology.tree_arcs;
    result.root_structure.non_tree_directed_edges =
        topology.serialized_non_tree;
    result.operator_order.reserve(plan.order.size());
    result.parent_relationships.reserve(plan.parent.size());
    for (int vertex : plan.order) {
        result.operator_order.push_back(static_cast<std::uint32_t>(vertex));
    }
    for (std::size_t vertex = 0; vertex < plan.parent.size(); ++vertex) {
        if (static_cast<int>(vertex) == plan.root) {
            result.parent_relationships.emplace_back(std::nullopt);
        } else {
            result.parent_relationships.emplace_back(
                static_cast<std::uint32_t>(plan.parent[vertex]));
        }
    }
    result.earlier_neighbors = topology.earlier_neighbors;
    result.root_selectivity_score_binary64_hex =
        binary64Hex(plan.root_selectivity_score);
    result.legacy_fingerprints.fnv_plan_id = uint64Hex(plan.plan_id);
    result.legacy_fingerprints.role = "context-only-not-authority";
    result.plan_payload_sha256 =
        sha256Object(typedPlanJson(result, false));
    return result;
}

bool samePlan(const QueryPlan& lhs, const QueryPlan& rhs) {
    return lhs.root == rhs.root && lhs.order == rhs.order &&
        lhs.parent == rhs.parent &&
        lhs.earlier_neighbors == rhs.earlier_neighbors &&
        lhs.selectivity_mode == rhs.selectivity_mode &&
        binary64Hex(lhs.root_selectivity_score) ==
            binary64Hex(rhs.root_selectivity_score) &&
        lhs.query_fingerprint == rhs.query_fingerprint &&
        lhs.plan_id == rhs.plan_id;
}

QueryPlan executablePlanFromParsed(
    const TypedPlanValue& value,
    const QueryPlan& fresh_plan) {
    QueryPlan result;
    result.root = value.root_structure.root;
    result.order.reserve(value.operator_order.size());
    for (std::uint32_t vertex : value.operator_order) {
        result.order.push_back(static_cast<int>(vertex));
    }
    result.parent.reserve(value.parent_relationships.size());
    for (const auto& parent : value.parent_relationships) {
        result.parent.push_back(
            parent.has_value() ? static_cast<int>(*parent) : -1);
    }
    result.earlier_neighbors.reserve(value.earlier_neighbors.size());
    for (const auto& row : value.earlier_neighbors) {
        std::vector<int> converted;
        converted.reserve(row.size());
        for (std::uint32_t neighbor : row) {
            converted.push_back(static_cast<int>(neighbor));
        }
        result.earlier_neighbors.push_back(std::move(converted));
    }
    result.selectivity_mode = SelectivityMode::Paper;
    result.root_selectivity_score =
        binary64FromHex(value.root_selectivity_score_binary64_hex);
    // The typed SHA-256 query object was already proven exactly equal to the
    // live query.  QueryPlan's legacy scalar has no 22-key slot, so derive it
    // from that equal live query and treat it as context-only.
    result.query_fingerprint = fresh_plan.query_fingerprint;
    result.plan_id = parseUint64Hex(
        value.legacy_fingerprints.fnv_plan_id,
        "legacy_fingerprints.fnv_plan_id");
    if (!samePlan(result, fresh_plan)) {
        fail(
            TypedPlanErrorCode::kCrossBindingMismatch,
            "parsed executable QueryPlan differs from fresh live paper plan");
    }
    return result;
}

QueryDecomposition executableDecompositionFromParsed(
    const TypedPlanValue& value,
    const Graph& query,
    const FreshPaperPlanning& fresh,
    const LivePlanTopology& topology) {
    QueryDecomposition result = fresh.decomposition();
    result.root = value.root_structure.root;
    result.connected = value.root_structure.connected;
    result.parent.clear();
    result.dfs_order.clear();
    for (const auto& parent : value.parent_relationships) {
        result.parent.push_back(
            parent.has_value() ? static_cast<int>(*parent) : -1);
    }
    for (std::uint32_t vertex : value.operator_order) {
        result.dfs_order.push_back(static_cast<int>(vertex));
    }
    result.spanning_tree_adj = topology.spanning_tree_adj;
    result.non_tree_edges.clear();

    std::set<std::pair<std::uint32_t, std::uint32_t>> tree_weak;
    for (std::size_t vertex = 0; vertex < result.parent.size(); ++vertex) {
        const int parent = result.parent[vertex];
        if (parent >= 0) {
            tree_weak.insert(weakKey(
                static_cast<std::uint32_t>(parent),
                static_cast<std::uint32_t>(vertex)));
        }
    }
    for (int source = 0; source < query.num_vertices; ++source) {
        for (const Edge& edge : query.adj[static_cast<std::size_t>(source)]) {
            if (tree_weak.find(weakKey(
                    static_cast<std::uint32_t>(source),
                    static_cast<std::uint32_t>(edge.to))) == tree_weak.end()) {
                result.non_tree_edges.emplace_back(source, edge.to);
            }
        }
    }
    // This equality proves that reconstruction used live query+parents and did
    // not import serialized non_tree_directed_edges into engine semantics.
    if (result.root != fresh.decomposition().root ||
        result.connected != fresh.decomposition().connected ||
        result.parent != fresh.decomposition().parent ||
        result.dfs_order != fresh.decomposition().dfs_order ||
        result.spanning_tree_adj != fresh.decomposition().spanning_tree_adj ||
        result.non_tree_edges != fresh.decomposition().non_tree_edges) {
        fail(
            TypedPlanErrorCode::kCrossBindingMismatch,
            "parsed plan cannot reconstruct the fresh engine decomposition");
    }
    return result;
}

void validateParsedSchema(const TypedPlanValue& value) {
    if (value.schema_version != kSchemaVersion ||
        value.artifact_kind != std::string(kArtifactKind) ||
        value.contract_id != std::string(kContractId) ||
        value.selectivity_mode != "paper" || value.order_mode != "paper") {
        fail(TypedPlanErrorCode::kSchemaMismatch, "typed-plan constants drifted");
    }
    if (!validLowerSha256(value.prepared_graph_sha256) ||
        !validLowerSha256(value.global_label_map_file_sha256) ||
        !validLowerSha256(value.global_label_map_mapping_sha256) ||
        !validLowerSha256(value.query_projection_file_sha256) ||
        !validLowerSha256(value.query_sha256) ||
        !validLowerSha256(value.query_fingerprint.sha256) ||
        !validLowerSha256(value.plan_payload_sha256)) {
        fail(TypedPlanErrorCode::kSchemaMismatch, "typed-plan digest encoding drifted");
    }
    if (value.query_fingerprint.schema != "typed-directed-query-v1" ||
        value.query_fingerprint.vertex_count < 1U ||
        value.query_fingerprint.vertex_labels.size() !=
            value.query_fingerprint.vertex_count ||
        value.query_fingerprint.directed_edges.empty()) {
        fail(TypedPlanErrorCode::kSchemaMismatch, "typed query shape drifted");
    }
    for (std::uint32_t label : value.query_fingerprint.vertex_labels) {
        if (label >= kLabelCount) {
            fail(TypedPlanErrorCode::kSchemaMismatch, "typed query label drifted");
        }
    }
    const auto& edges = value.query_fingerprint.directed_edges;
    for (const auto& [source, target] : edges) {
        if (source >= value.query_fingerprint.vertex_count ||
            target >= value.query_fingerprint.vertex_count || source == target) {
            fail(TypedPlanErrorCode::kSchemaMismatch, "typed query edge drifted");
        }
    }
    if (!std::is_sorted(edges.begin(), edges.end()) ||
        std::adjacent_find(edges.begin(), edges.end()) != edges.end()) {
        fail(
            TypedPlanErrorCode::kSchemaMismatch,
            "typed query edges must be sorted and unique");
    }
    if (sha256Object(queryFingerprintJson(value.query_fingerprint, false)) !=
        value.query_fingerprint.sha256) {
        fail(TypedPlanErrorCode::kHashMismatch, "typed query object H0 mismatch");
    }
    const std::size_t vertex_count = value.query_fingerprint.vertex_count;
    if (value.root_structure.root < 0 ||
        static_cast<std::size_t>(value.root_structure.root) >= vertex_count ||
        !value.root_structure.connected ||
        value.root_structure.query_tree_mode != "dfs" ||
        value.operator_order.size() != vertex_count ||
        value.parent_relationships.size() != vertex_count ||
        value.earlier_neighbors.size() != vertex_count ||
        value.operator_order.empty() ||
        value.operator_order.front() !=
            static_cast<std::uint32_t>(value.root_structure.root)) {
        fail(TypedPlanErrorCode::kSchemaMismatch, "typed root/order shape drifted");
    }
    std::vector<std::uint32_t> permutation = value.operator_order;
    std::sort(permutation.begin(), permutation.end());
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        if (permutation[vertex] != vertex) {
            fail(TypedPlanErrorCode::kSchemaMismatch, "operator order is not exact permutation");
        }
    }
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        const auto& parent = value.parent_relationships[vertex];
        if (static_cast<int>(vertex) == value.root_structure.root) {
            if (parent.has_value()) {
                fail(TypedPlanErrorCode::kSchemaMismatch, "typed root parent must be null");
            }
        } else if (!parent.has_value() || *parent >= vertex_count) {
            fail(TypedPlanErrorCode::kSchemaMismatch, "typed nonroot parent invalid");
        }
        const auto& earlier = value.earlier_neighbors[vertex];
        if (!std::is_sorted(earlier.begin(), earlier.end()) ||
            std::adjacent_find(earlier.begin(), earlier.end()) != earlier.end() ||
            std::any_of(earlier.begin(), earlier.end(), [&](std::uint32_t item) {
                return item >= vertex_count;
            })) {
            fail(TypedPlanErrorCode::kSchemaMismatch, "earlier-neighbor row invalid");
        }
    }
    static_cast<void>(binary64FromHex(
        value.root_selectivity_score_binary64_hex));
    if (!validLowerHex(value.legacy_fingerprints.fnv_plan_id, 16U) ||
        value.legacy_fingerprints.role != "context-only-not-authority") {
        fail(TypedPlanErrorCode::kSchemaMismatch, "legacy fingerprint contract drifted");
    }
    if (sha256Object(typedPlanJson(value, false)) !=
        value.plan_payload_sha256) {
        fail(TypedPlanErrorCode::kHashMismatch, "plan payload H0 mismatch");
    }
}

ValidatedTypedPlan validateParsedAgainstLive(
    TypedPlanValue parsed,
    std::string canonical_no_lf,
    const ValidatedInputs& inputs,
    const CaseRow& row,
    const Graph& query,
    const FreshPaperPlanning& fresh) {
    validateParsedSchema(parsed);
    const TypedDirectedQueryFingerprint fingerprint = makeQueryFingerprint(query);
    bindLiveQueryToLoadedBytes(query, inputs, fingerprint);
    if (fresh.preparedGraphFileSha256Binding() !=
        inputs.prepared_graph.identity().sha256) {
        fail(
            TypedPlanErrorCode::kCrossBindingMismatch,
            "fresh planning is bound to a different prepared graph H1");
    }
    const LivePlanTopology topology = deriveLiveTopology(
        query, fresh.queryPlan(), fingerprint);
    validateFreshPlanning(query, fresh, fingerprint, topology);
    const TypedPlanValue expected = buildExpectedValue(
        inputs, row, fingerprint, fresh, topology);
    if (canonicalJson(typedPlanJson(parsed, true)) !=
        canonicalJson(typedPlanJson(expected, true))) {
        fail(
            TypedPlanErrorCode::kCrossBindingMismatch,
            "typed plan differs from exact live input/case/K/paper-plan value");
    }

    ValidatedTypedPlan result;
    result.value = std::move(parsed);
    result.executable_plan = executablePlanFromParsed(
        result.value, fresh.queryPlan());
    result.executable_decomposition = executableDecompositionFromParsed(
        result.value, query, fresh, topology);
    result.canonical_no_lf_bytes = std::move(canonical_no_lf);
    result.payload_object_sha256 = result.value.plan_payload_sha256;
    result.runtime_object_sha256 = adapter_v1::sha256Hex(
        result.canonical_no_lf_bytes);
    return result;
}

struct ExpectedTemporalEdge final {
    std::int32_t external_source = 0;
    std::int32_t external_target = 0;
    std::vector<TimeInterval> intervals;
    std::int64_t active_snapshot_count = 0;
};

bool rawEdgeLess(
    const adapter_v1::RawTemporalEdge& lhs,
    const adapter_v1::RawTemporalEdge& rhs) {
    return std::tie(lhs.source, lhs.target, lhs.timestamp) <
        std::tie(rhs.source, rhs.target, rhs.timestamp);
}

int checkedUnionDuration(std::vector<TimeInterval> intervals) {
    if (intervals.empty()) return 0;
    std::sort(
        intervals.begin(), intervals.end(),
        [](const TimeInterval& lhs, const TimeInterval& rhs) {
            return std::tie(lhs.start, lhs.end) < std::tie(rhs.start, rhs.end);
        });
    std::int64_t total = 0;
    int current_start = intervals.front().start;
    int current_end = intervals.front().end;
    if (current_end < current_start) {
        fail(TypedPlanErrorCode::kInvalidLiveGraph, "invalid temporal interval");
    }
    auto commit = [&]() {
        total += static_cast<std::int64_t>(current_end) - current_start + 1;
        if (total > std::numeric_limits<int>::max()) {
            fail(
                TypedPlanErrorCode::kIntegerRange,
                "vertex active duration exceeds int");
        }
    };
    for (std::size_t index = 1; index < intervals.size(); ++index) {
        const TimeInterval& interval = intervals[index];
        if (interval.end < interval.start) {
            fail(TypedPlanErrorCode::kInvalidLiveGraph, "invalid temporal interval");
        }
        if (static_cast<std::int64_t>(interval.start) <=
            static_cast<std::int64_t>(current_end) + 1) {
            current_end = std::max(current_end, interval.end);
        } else {
            commit();
            current_start = interval.start;
            current_end = interval.end;
        }
    }
    commit();
    return static_cast<int>(total);
}

void validateTemporalGraphAgainstInputs(
    const Graph& graph,
    const ValidatedInputs& inputs) {
    if (inputs.raw_temporal_edges.empty() ||
        inputs.runtime_labels_by_external_id.empty()) {
        fail(
            TypedPlanErrorCode::kCrossBindingMismatch,
            "held prepared graph or runtime label projection is empty");
    }
    std::vector<adapter_v1::RawTemporalEdge> raw =
        inputs.raw_temporal_edges;
    std::sort(raw.begin(), raw.end(), rawEdgeLess);

    std::vector<ExpectedTemporalEdge> expected_edges;
    std::vector<std::int32_t> filtered_external_vertices;
    std::size_t unique_edge_count = 0;
    std::size_t position = 0;
    while (position < raw.size()) {
        const std::int32_t source = raw[position].source;
        const std::int32_t target = raw[position].target;
        ++unique_edge_count;
        bool have_timestamp = false;
        bool has_consecutive_pair = false;
        std::int32_t run_start = 0;
        std::int32_t previous = 0;
        std::int64_t active_count = 0;
        std::vector<TimeInterval> intervals;
        while (position < raw.size() && raw[position].source == source &&
               raw[position].target == target) {
            const std::int32_t timestamp = raw[position++].timestamp;
            if (have_timestamp && timestamp == previous) continue;
            ++active_count;
            if (!have_timestamp) {
                have_timestamp = true;
                run_start = previous = timestamp;
            } else if (previous != std::numeric_limits<std::int32_t>::max() &&
                       timestamp == previous + 1) {
                has_consecutive_pair = true;
                previous = timestamp;
            } else {
                intervals.push_back({run_start, previous});
                run_start = previous = timestamp;
            }
        }
        if (have_timestamp) intervals.push_back({run_start, previous});
        if (has_consecutive_pair) {
            expected_edges.push_back(
                {source, target, std::move(intervals), active_count});
            filtered_external_vertices.push_back(source);
            filtered_external_vertices.push_back(target);
        }
    }
    std::sort(
        filtered_external_vertices.begin(), filtered_external_vertices.end());
    filtered_external_vertices.erase(
        std::unique(
            filtered_external_vertices.begin(),
            filtered_external_vertices.end()),
        filtered_external_vertices.end());

    if (graph.num_vertices < 0 ||
        graph.external_ids.size() != filtered_external_vertices.size() ||
        !std::equal(
            graph.external_ids.begin(), graph.external_ids.end(),
            filtered_external_vertices.begin(), filtered_external_vertices.end()) ||
        graph.num_vertices != static_cast<int>(graph.external_ids.size()) ||
        graph.vertex_labels.size() != graph.external_ids.size() ||
        graph.adj.size() != graph.external_ids.size() ||
        graph.in_adj.size() != graph.external_ids.size() ||
        graph.temporal_edges.size() != expected_edges.size() ||
        graph.input_occurrence_count != raw.size() ||
        graph.input_unique_edge_count != unique_edge_count ||
        graph.filtered_edge_count != expected_edges.size()) {
        fail(
            TypedPlanErrorCode::kCrossBindingMismatch,
            "live temporal graph inventory differs from held prepared graph bytes");
    }

    const auto& label_rows = inputs.runtime_labels_by_external_id;
    if (!std::is_sorted(
            label_rows.begin(), label_rows.end(),
            [](const adapter_v1::RuntimeLabelRow& lhs,
               const adapter_v1::RuntimeLabelRow& rhs) {
                return lhs.external_vertex_id < rhs.external_vertex_id;
            }) ||
        std::adjacent_find(
            label_rows.begin(), label_rows.end(),
            [](const adapter_v1::RuntimeLabelRow& lhs,
               const adapter_v1::RuntimeLabelRow& rhs) {
                return lhs.external_vertex_id == rhs.external_vertex_id;
            }) != label_rows.end()) {
        fail(
            TypedPlanErrorCode::kCrossBindingMismatch,
            "runtime label rows are not sorted and unique");
    }
    for (std::size_t vertex = 0; vertex < graph.external_ids.size(); ++vertex) {
        const int external_id = graph.external_ids[vertex];
        const auto found = std::lower_bound(
            label_rows.begin(), label_rows.end(), external_id,
            [](const adapter_v1::RuntimeLabelRow& row, int value) {
                return row.external_vertex_id < value;
            });
        if (found == label_rows.end() ||
            found->external_vertex_id != external_id ||
            found->runtime_class_id >= kLabelCount ||
            graph.vertex_labels[vertex] != found->runtime_class_id) {
            fail(
                TypedPlanErrorCode::kCrossBindingMismatch,
                "live temporal graph label differs from held projection");
        }
    }

    std::vector<std::vector<Edge>> expected_adj(graph.external_ids.size());
    std::vector<std::vector<Edge>> expected_in_adj(graph.external_ids.size());
    for (std::size_t edge_index = 0;
         edge_index < expected_edges.size();
         ++edge_index) {
        const ExpectedTemporalEdge& expected = expected_edges[edge_index];
        const int source = static_cast<int>(std::lower_bound(
            graph.external_ids.begin(), graph.external_ids.end(),
            expected.external_source) - graph.external_ids.begin());
        const int target = static_cast<int>(std::lower_bound(
            graph.external_ids.begin(), graph.external_ids.end(),
            expected.external_target) - graph.external_ids.begin());
        const TemporalEdge& actual = graph.temporal_edges[edge_index];
        if (source < 0 || target < 0 || source >= graph.num_vertices ||
            target >= graph.num_vertices || actual.u != source ||
            actual.v != target || actual.activeIntervals().size() !=
                expected.intervals.size() ||
            actual.active_snapshot_count != expected.active_snapshot_count) {
            fail(
                TypedPlanErrorCode::kCrossBindingMismatch,
                "live temporal edge differs from held prepared graph bytes");
        }
        for (std::size_t interval = 0;
             interval < expected.intervals.size();
             ++interval) {
            if (actual.activeIntervals()[interval].start !=
                    expected.intervals[interval].start ||
                actual.activeIntervals()[interval].end !=
                    expected.intervals[interval].end) {
                fail(
                    TypedPlanErrorCode::kCrossBindingMismatch,
                    "live temporal interval differs from held graph bytes");
            }
        }
        expected_adj[static_cast<std::size_t>(source)].push_back(
            {target, static_cast<int>(edge_index)});
        expected_in_adj[static_cast<std::size_t>(target)].push_back(
            {source, static_cast<int>(edge_index)});
    }
    auto edgeLess = [](const Edge& lhs, const Edge& rhs) {
        return std::tie(lhs.to, lhs.temporal_edge_id) <
            std::tie(rhs.to, rhs.temporal_edge_id);
    };
    for (auto& row : expected_adj) std::sort(row.begin(), row.end(), edgeLess);
    for (auto& row : expected_in_adj) std::sort(row.begin(), row.end(), edgeLess);
    auto sameEdgeRows = [&](const std::vector<std::vector<Edge>>& lhs,
                            const std::vector<std::vector<Edge>>& rhs) {
        if (lhs.size() != rhs.size()) return false;
        for (std::size_t row = 0; row < lhs.size(); ++row) {
            if (lhs[row].size() != rhs[row].size()) return false;
            for (std::size_t index = 0; index < lhs[row].size(); ++index) {
                if (lhs[row][index].to != rhs[row][index].to ||
                    lhs[row][index].temporal_edge_id !=
                        rhs[row][index].temporal_edge_id) {
                    return false;
                }
            }
        }
        return true;
    };
    if (!sameEdgeRows(graph.adj, expected_adj) ||
        !sameEdgeRows(graph.in_adj, expected_in_adj)) {
        fail(
            TypedPlanErrorCode::kCrossBindingMismatch,
            "live temporal adjacency differs from held graph bytes");
    }

    if (graph.vertex_active_durations.size() != graph.external_ids.size() ||
        graph.neighbor_label_counts.size() != graph.external_ids.size() ||
        graph.incoming_neighbor_label_counts.size() != graph.external_ids.size()) {
        fail(
            TypedPlanErrorCode::kCrossBindingMismatch,
            "live temporal derived-stat arrays have wrong shape");
    }
    for (std::size_t vertex = 0; vertex < graph.external_ids.size(); ++vertex) {
        std::vector<TimeInterval> incident;
        std::array<int, kLabelCount> out_counts{};
        std::array<int, kLabelCount> in_counts{};
        for (const Edge& edge : graph.adj[vertex]) {
            const TemporalEdge& temporal = graph.temporal_edges[
                static_cast<std::size_t>(edge.temporal_edge_id)];
            incident.insert(
                incident.end(),
                temporal.activeIntervals().begin(),
                temporal.activeIntervals().end());
            ++out_counts[graph.vertex_labels[static_cast<std::size_t>(edge.to)]];
        }
        for (const Edge& edge : graph.in_adj[vertex]) {
            const TemporalEdge& temporal = graph.temporal_edges[
                static_cast<std::size_t>(edge.temporal_edge_id)];
            incident.insert(
                incident.end(),
                temporal.activeIntervals().begin(),
                temporal.activeIntervals().end());
            ++in_counts[graph.vertex_labels[static_cast<std::size_t>(edge.to)]];
        }
        if (graph.vertex_active_durations[vertex] !=
                checkedUnionDuration(std::move(incident)) ||
            graph.neighbor_label_counts[vertex] != out_counts ||
            graph.incoming_neighbor_label_counts[vertex] != in_counts) {
            fail(
                TypedPlanErrorCode::kCrossBindingMismatch,
                "live temporal derived statistics differ from held graph bytes");
        }
    }
}

}  // namespace

}  // namespace dsm::e3e4::gopher_plan_v1
