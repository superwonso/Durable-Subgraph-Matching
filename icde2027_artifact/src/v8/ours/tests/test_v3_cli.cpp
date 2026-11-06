#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../v3_canonical_json.h"

namespace {

using dsm::v3::CanonicalArtifact;
using dsm::v3::Json;
using dsm::v3::JsonError;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Action>
void requireReject(Action action, const char* message) {
    bool rejected = false;
    try {
        action();
    } catch (const JsonError&) {
        rejected = true;
    }
    require(rejected, message);
}

std::filesystem::path temporaryPath(const char* suffix) {
    const auto count = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        ("dsm_v3_json_" + std::to_string(count) + suffix);
}

void writeBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) throw std::runtime_error("could not create test artifact");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("could not write test artifact");
}

void testCanonicalParityVector() {
    const std::string input =
        "{\"z\":\"\xc3\xa9\\n\",\"a\":[0,18446744073709551615,true,false,null]}";
    const std::string expected =
        "{\"a\":[0,18446744073709551615,true,false,null],\"z\":\"\xc3\xa9\\n\"}\n";
    const Json value = Json::parse(input);
    require(dsm::v3::canonicalJsonBytes(value) == expected,
            "C++ canonical JSON differs from Python vector");
    require(
        dsm::v3::sha256Hex(expected) ==
            "e364170393bcf3f63d94a27645052844674e61d2f877de414aead2cd007660c5",
        "C++ SHA-256 differs from Python vector");

    const Json escaped = Json::parse(
        "{\"slash\":\"a\\/b\",\"control\":\"\\b\\f\\t\\u0001\","
        "\"emoji\":\"\\ud83d\\ude00\"}");
    const std::string escaped_expected =
        "{\"control\":\"\\b\\f\\t\\u0001\",\"emoji\":\"\xf0\x9f\x98\x80\","
        "\"slash\":\"a/b\"}\n";
    require(dsm::v3::canonicalJsonBytes(escaped) == escaped_expected,
            "C++ escape/Unicode canonical vector differs from Python");
    require(
        dsm::v3::sha256Hex(escaped_expected) ==
            "839d2724243886ea7a5c7f637646791b4e1dc21ab70bf8d9a7fc5c59babc2508",
        "C++ escape/Unicode SHA-256 differs from Python");

    const Json unicode_key = Json::parse(
        "{\"b\":{\"\xce\xb2\":2,\"a\":1},\"a\":[]}");
    const std::string unicode_key_expected =
        "{\"a\":[],\"b\":{\"a\":1,\"\xce\xb2\":2}}\n";
    require(dsm::v3::canonicalJsonBytes(unicode_key) == unicode_key_expected,
            "C++ Unicode-key ordering differs from Python");
    require(
        dsm::v3::sha256Hex(unicode_key_expected) ==
            "291fb5c62d89aff617949f70fb5612d680b9f0461fb8f36f88211434919d13f3",
        "C++ Unicode-key SHA-256 differs from Python");
}

void testStrictParserRejectsNumericAndUtf8Drift() {
    const std::vector<std::string> rejected{
        "{\"x\":1,\"x\":2}",
        "{\"x\":-1}",
        "{\"x\":1.0}",
        "{\"x\":1e2}",
        "{\"x\":18446744073709551616}",
        "{\"x\":01}",
        "{\"x\":\"\\ud800\"}",
        std::string("{\"x\":\"") + static_cast<char>(0xff) + "\"}"};
    for (const std::string& payload : rejected) {
        requireReject([&payload]() { (void)Json::parse(payload); },
                      "strict parser accepted forbidden JSON");
    }
    requireReject(
        []() {
            (void)dsm::v3::canonicalJsonBytes(
                Json(std::string(1, static_cast<char>(0xff))));
        },
        "canonical serializer accepted an invalid constructed UTF-8 string");
}

void testCanonicalArtifactIntegrity() {
    Json::Object payload;
    payload.emplace("schema", Json(std::string("synthetic")));
    payload.emplace("schema_version", Json(std::uint64_t{1}));
    Json unsealed = Json::object(payload);
    Json::Object integrity;
    integrity.emplace("algorithm", Json(std::string("sha256")));
    integrity.emplace(
        "canonical_payload_sha256",
        Json(dsm::v3::sha256Hex(dsm::v3::canonicalJsonBytes(unsealed))));
    integrity.emplace(
        "scope",
        Json(std::string("canonical_json_all_fields_except_integrity")));
    payload.emplace("integrity", Json::object(std::move(integrity)));
    const Json sealed = Json::object(std::move(payload));
    const std::string bytes = dsm::v3::canonicalJsonBytes(sealed);

    const std::filesystem::path good = temporaryPath("_good.json");
    const std::filesystem::path bad = temporaryPath("_bad.json");
    try {
        writeBytes(good, bytes);
        const CanonicalArtifact loaded = dsm::v3::loadCanonicalArtifact(good);
        require(loaded.value == sealed, "canonical artifact round trip drift");
        require(loaded.file_sha256 == dsm::v3::sha256Hex(bytes),
                "artifact file hash drift");
        writeBytes(bad, " " + bytes);
        requireReject(
            [&bad]() { (void)dsm::v3::loadCanonicalArtifact(bad); },
            "noncanonical artifact bytes accepted");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(good, ignored);
        std::filesystem::remove(bad, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove(good, ignored);
    std::filesystem::remove(bad, ignored);
}

} // namespace

int main() {
    try {
        testCanonicalParityVector();
        testStrictParserRejectsNumericAndUtf8Drift();
        testCanonicalArtifactIntegrity();
        std::cout << "All V3 canonical JSON tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "V3 canonical JSON test failure: " << error.what() << '\n';
        return 1;
    }
}
