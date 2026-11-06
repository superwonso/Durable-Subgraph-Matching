#include "v3_run_contract.h"

#include "v3_execution.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace dsm::v3 {
namespace {

using Keys = std::set<std::string>;

Json stringValue(std::string value) { return Json(std::move(value)); }
Json uintValue(std::uint64_t value) { return Json(value); }
Json boolValue(bool value) { return Json(value); }

Json parseConstant(std::string_view text) { return Json::parse(text); }

Keys keysOf(const Json& value) {
    if (!value.isObject()) throw V3AuthorityError("required JSON object is missing");
    Keys result;
    for (const auto& item : value.objectValue()) result.insert(item.first);
    return result;
}

Keys keys(std::initializer_list<const char*> values) {
    Keys result;
    for (const char* value : values) result.emplace(value);
    return result;
}

void requireObject(const Json& value, std::string_view location) {
    if (!value.isObject()) {
        throw V3AuthorityError(std::string(location) + " must be an object");
    }
}

void requireArray(const Json& value, std::string_view location) {
    if (!value.isArray()) {
        throw V3AuthorityError(std::string(location) + " must be an array");
    }
}

void requireExactKeys(
    const Json& value,
    const Keys& expected,
    std::string_view location) {
    requireObject(value, location);
    if (keysOf(value) != expected) {
        throw V3AuthorityError(std::string(location) + " field-set drift");
    }
}

void requireEqual(
    const Json& observed,
    const Json& expected,
    std::string_view location) {
    if (observed != expected) {
        throw V3AuthorityError(std::string(location) + " exact-value drift");
    }
}

void requireString(
    const Json& observed,
    std::string_view expected,
    std::string_view location) {
    if (!observed.isString() || observed.string() != expected) {
        throw V3AuthorityError(std::string(location) + " string drift");
    }
}

void requireUnsigned(
    const Json& observed,
    std::uint64_t expected,
    std::string_view location) {
    if (!observed.isUnsignedInteger() || observed.unsignedInteger() != expected) {
        throw V3AuthorityError(std::string(location) + " uint64 drift");
    }
}

void requireSha(const Json& value, std::string_view location) {
    if (!isLowercaseSha256(value)) {
        throw V3AuthorityError(std::string(location) + " must be lowercase SHA-256");
    }
}

bool isNormalRelativePath(std::string_view path) noexcept {
    if (path.empty() || path.front() == '/' || path.back() == '/' ||
        path.find('\\') != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        const std::string_view part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string_view::npos) break;
        start = slash + 1U;
    }
    return true;
}

std::string lowercase(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](char character) {
        if (character >= 'A' && character <= 'Z') {
            return static_cast<char>(character - 'A' + 'a');
        }
        return character;
    });
    return result;
}

bool isDeniedPath(std::string_view path) {
    if (!isNormalRelativePath(path)) return true;
    constexpr std::array<std::string_view, 2> denied_prefixes{{
        "benchmark/queries/icde2027_v2_holdout",
        "evaluation/icde2027_v2_gate1_16t_20260812"}};
    for (std::string_view prefix : denied_prefixes) {
        if (path == prefix ||
            (path.size() > prefix.size() && path.substr(0, prefix.size()) == prefix &&
             path[prefix.size()] == '/')) {
            return true;
        }
    }
    std::size_t start = 0;
    std::string last;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        last = lowercase(path.substr(start, end - start));
        if (last.find("holdout") != std::string::npos ||
            last.find("heldout") != std::string::npos) {
            return true;
        }
        if (slash == std::string_view::npos) break;
        start = slash + 1U;
    }
    return last.find("_filtered") != std::string::npos;
}

bool isExactRunContractLexicalPath(std::string_view path) noexcept {
    const std::string_view prefix =
        "evaluation/icde2027_v3_dev_16t_20260812/runs/";
    const std::string_view suffix = "/run_contract.json";
    if (path.size() != prefix.size() + 4U + suffix.size() ||
        path.substr(0, prefix.size()) != prefix ||
        path.substr(prefix.size() + 4U) != suffix) {
        return false;
    }
    std::uint64_t ordinal = 0;
    for (std::size_t index = 0; index < 4U; ++index) {
        const char digit = path[prefix.size() + index];
        if (digit < '0' || digit > '9') return false;
        ordinal = ordinal * 10U + static_cast<std::uint64_t>(digit - '0');
    }
    return ordinal >= 1U && ordinal <= 36U;
}

const Json& exactExecutionContract() {
    static const Json value = parseConstant(
        R"({"cpu_affinity":"0-15","cpu_affinity_set":[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15],"input_kind":"full_raw","internal_filter":"consecutive_pair_on","label_mode":"stable_id","label_seed":42,"match_output":"count_only","page_cache_flush":false,"prefiltered_input_allowed":false,"process_model":"one_fresh_process_per_arm_case_repeat","repeats_per_case_arm":3,"smt_active_required":0,"threads_requested":16,"threads_used_required":16,"total_fresh_processes":36,"warmup_runs":0})");
    return value;
}

const Json& exactResourceEnvelope() {
    static const Json value = parseConstant(
        R"({"background_observation_seconds":30,"candidate_fixed_memory_headroom_GiB":4,"candidate_payload_absolute_ceiling_GiB":8,"enforcement_required_before_execution":true,"maximum_mean_nonidle_cpu_ratio_denominator":20,"maximum_mean_nonidle_cpu_ratio_numerator":1,"maximum_other_matcher_processes":0,"maximum_preflight_wait_seconds":600,"minimum_available_memory_GiB_before_block":36,"per_process_hard_memory_GiB":40,"per_process_timeout_seconds":7200,"process_cgroup_swap_max_bytes":0,"swap_delta_required":0})");
    return value;
}

const Json& exactFixedPolicy() {
    static const Json value = parseConstant(
        R"({"candidate_tree_tokens":1,"fixed_memory_headroom_GiB":4,"max_nonincumbent_trees_materialized":1,"opportunity_margin_ratio_denominator":10,"opportunity_margin_ratio_numerator":1,"profitability_ratio_denominator":10,"profitability_ratio_numerator":9,"rescore_top_l":1,"sample_seed":42})");
    return value;
}

const Json& exactGateCThresholds() {
    static const Json value = parseConstant(
        R"({"aggregate_controllable_ratio_max":{"denominator":10,"numerator":9},"aggregate_full_process_wall_ratio_max":{"denominator":20,"numerator":21},"fallback_max_relative_positive_overhead_max":{"denominator":10,"numerator":1},"fallback_median_relative_positive_overhead_max":{"denominator":20,"numerator":1},"nonincumbent_materialization_distinct_cases_min":2,"nonincumbent_selection_distinct_cases_min":1,"selected_case_controllable_ratio_max":{"denominator":1,"numerator":1},"selected_execution_work_ratio_max":{"denominator":10,"numerator":9},"stability_inconclusive_max_over_min_gt":{"denominator":2,"numerator":3},"stability_warning_max_over_min_gt":{"denominator":5,"numerator":6},"worst_case_controllable_ratio_max":{"denominator":10,"numerator":11},"worst_case_wall_ratio_max":{"denominator":10,"numerator":11}})");
    return value;
}

const Json& exactDatasets() {
    static const Json value = parseConstant(
        R"([{"bytes":1925032024,"dataset_id":"btc_raw","input_kind":"full_raw","internal_filter":"consecutive_pair_on","path":"Dataset/bitcoin-temporal-timeinstance-unique-1w.txt","role":"development_full_raw_input","sha256":"966ad1bf987377b67fc13ce7c74cda8a8834db485bbea6e9429c83d4946d2b84"},{"bytes":18866128,"dataset_id":"su_raw","input_kind":"full_raw","internal_filter":"consecutive_pair_on","path":"Dataset/sx-superuser-temporal-1w-unique.txt","role":"development_full_raw_input","sha256":"44bef58c9dc8f7887360a604e918f0ef7397e730c7496c9ef1738bdc6fcbc0cb"}])");
    return value;
}

const Json& exactCases() {
    static const Json value = parseConstant(
        R"([{"case_id":"D01_btc_long_temporal","corpus_role":"development_existing_asset","dataset_id":"btc_raw","duration_k":5,"existing_development_asset":true,"query_bytes":50,"query_edges":5,"query_edges_directed":[[0,1],[1,2],[2,3],[3,4],[4,5]],"query_labels":["B","E","A","C","B","A"],"query_path":"benchmark/queries/bitcoin/path5_k5.qry","query_sha256":"0814a3aa7df5b273050b67c2a99a61eeba35137b066cd8cc534a743f2e75efdc","query_vertices":6,"structural_strata":["longer_tree","higher_duration"]},{"case_id":"D02_btc_cycle","corpus_role":"development_existing_asset","dataset_id":"btc_raw","duration_k":3,"existing_development_asset":true,"query_bytes":30,"query_edges":3,"query_edges_directed":[[0,1],[1,2],[2,0]],"query_labels":["B","E","A"],"query_path":"benchmark/queries/bitcoin/cycle3_k3.qry","query_sha256":"56023e66868ce197c99e4ddff7962e3d09ab51100b5c7be9b6a6ec26ce626a74","query_vertices":3,"structural_strata":["non_tree_constraint","directed_cycle","reverse_parent_orientation_witness"]},{"case_id":"D03_su_short","corpus_role":"development_existing_asset","dataset_id":"su_raw","duration_k":3,"existing_development_asset":true,"query_bytes":30,"query_edges":3,"query_edges_directed":[[0,1],[1,2],[2,3]],"query_labels":["D","A","A","D"],"query_path":"benchmark/queries/raw/sx_superuser_raw_path3_k3.qry","query_sha256":"af0313f8eab45c35ac28a9769c7600e537c852f4ba93a2675c216ea1ab9e8a88","query_vertices":4,"structural_strata":["short_tree","second_dataset_scale"]},{"case_id":"D04_su_branching","corpus_role":"development_existing_asset","dataset_id":"su_raw","duration_k":3,"existing_development_asset":true,"query_bytes":50,"query_edges":5,"query_edges_directed":[[0,1],[0,2],[0,3],[0,4],[0,5]],"query_labels":["A","C","C","E","E","E"],"query_path":"benchmark/queries/paper_parameters_v2/sx_superuser/star5_k3_rank001.qry","query_sha256":"6c8fe184b5efd325f94236235a276269ad1c092a1521d9e4b4653b5cb4c7f4f8","query_vertices":6,"structural_strata":["branching_star","second_dataset_scale"]}])");
    return value;
}

const Json& exactArms() {
    static const Json value = parseConstant(
        R"([{"arm_id":"v3dev_incumbent_16t","production":true,"role":"denominator","short_id":"I"},{"arm_id":"v3dev_cover_lazy_gate_on_16t","production":true,"role":"primary_numerator","short_id":"O"},{"arm_id":"v3dev_cover_lazy_gate_off_16t","production":false,"role":"attribution_only","short_id":"F"}])");
    return value;
}

const Json& exactPathPolicy() {
    static const Json value = parseConstant(
        R"({"declared_input_paths":["paper/icde2027/revisions/V3_PROTOCOL_FREEZE_RECOMMENDATION_20260812.md","paper/icde2027/revisions/V3_BLIND_SLATE_REREVIEW_20260812.md","Dataset/bitcoin-temporal-timeinstance-unique-1w.txt","Dataset/sx-superuser-temporal-1w-unique.txt","benchmark/queries/bitcoin/path5_k5.qry","benchmark/queries/bitcoin/cycle3_k3.qry","benchmark/queries/raw/sx_superuser_raw_path3_k3.qry","benchmark/queries/paper_parameters_v2/sx_superuser/star5_k3_rank001.qry"],"declared_output_paths":["paper/icde2027/v3_protocol/development_protocol_v2.json","paper/icde2027/v3_protocol/execution_authorization_receipt.json","evaluation/icde2027_v3_dev_16t_20260812/runs"],"denied_path_component_substrings":["holdout","heldout"],"denied_prefixes":["benchmark/queries/icde2027_v2_holdout/","evaluation/icde2027_v2_gate1_16t_20260812/"],"denied_roles":["heldout","holdout","confirmatory","sealed_holdout","v2_gate1"],"immutable_no_access_paths":["benchmark/run_benchmarks.py","benchmark/prepare_icde2027_v2_gate1.py","benchmark/validate_icde2027_v2_gate1.py","benchmark/test_icde2027_v2_gate1.py","paper/icde2027/revisions/V2_PAPER_STRATEGY.md","paper/icde2027/revisions/V2_GATE1_CORRECTED_PROTOCOL_20260812.md","paper/icde2027/revisions/V2_GATE1_CORRECTED_HARNESS_AUDIT_20260812.md"],"path_normalization":"repository_relative_posix_lexical_no_parent_segments"})");
    return value;
}

Json makeStringArray(const std::vector<std::string>& values) {
    Json::Array result;
    result.reserve(values.size());
    for (const std::string& value : values) result.emplace_back(value);
    return Json::array(std::move(result));
}

Json exactSchedule() {
    const std::array<std::array<const char*, 4>, 3> case_orders{{
        {{"D01_btc_long_temporal","D02_btc_cycle","D03_su_short","D04_su_branching"}},
        {{"D02_btc_cycle","D03_su_short","D04_su_branching","D01_btc_long_temporal"}},
        {{"D03_su_short","D04_su_branching","D01_btc_long_temporal","D02_btc_cycle"}}}};
    const std::array<std::array<const char*, 3>, 3> arm_orders{{
        {{"I","O","F"}}, {{"O","F","I"}}, {{"F","I","O"}}}};
    const std::map<std::string, std::string> arm_ids{{
        {"I", "v3dev_incumbent_16t"},
        {"O", "v3dev_cover_lazy_gate_on_16t"},
        {"F", "v3dev_cover_lazy_gate_off_16t"}}};
    Json::Object case_order_object;
    Json::Object arm_order_object;
    Json::Array positions;
    std::uint64_t ordinal = 1;
    for (std::size_t repeat_index = 0; repeat_index < 3U; ++repeat_index) {
        std::vector<std::string> case_values;
        for (const char* value : case_orders[repeat_index]) case_values.emplace_back(value);
        case_order_object.emplace(
            std::to_string(repeat_index + 1U), makeStringArray(case_values));
        std::vector<std::string> arm_values;
        for (const char* value : arm_orders[repeat_index]) arm_values.emplace_back(value);
        arm_order_object.emplace(
            std::to_string(repeat_index + 1U), makeStringArray(arm_values));
        for (std::size_t case_index = 0; case_index < 4U; ++case_index) {
            for (std::size_t arm_index = 0; arm_index < 3U; ++arm_index) {
                const std::string short_id = arm_orders[repeat_index][arm_index];
                const std::string case_id = case_orders[repeat_index][case_index];
                Json::Object row;
                row.emplace("arm_id", stringValue(arm_ids.at(short_id)));
                row.emplace("arm_position", uintValue(arm_index + 1U));
                row.emplace("arm_short_id", stringValue(short_id));
                row.emplace("attempt", uintValue(1));
                row.emplace("block_id", stringValue(
                    "R" + std::to_string(repeat_index + 1U) + "_" + case_id));
                row.emplace("case_block_position", uintValue(case_index + 1U));
                row.emplace("case_id", stringValue(case_id));
                row.emplace("fresh_process", boolValue(true));
                row.emplace("ordinal", uintValue(ordinal++));
                row.emplace("repeat", uintValue(repeat_index + 1U));
                positions.push_back(Json::object(std::move(row)));
            }
        }
    }
    Json::Object schedule;
    schedule.emplace("arm_orders_short_id", Json::object(std::move(arm_order_object)));
    schedule.emplace("case_orders", Json::object(std::move(case_order_object)));
    schedule.emplace("design", stringValue("three_arm_latin_rotation"));
    schedule.emplace("positions", Json::array(std::move(positions)));
    return Json::object(std::move(schedule));
}

const Json& expectedResources() {
    static const Json value = parseConstant(
        R"({"affinity_cpu_set":[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15],"memory_max_bytes":42949672960,"memory_swap_max_bytes":0,"smt_active_required":0,"threads_requested":16,"threads_used_required":16,"timeout_seconds":7200,"worker_count_semantics":"executor_pool_size"})");
    return value;
}

void validateNumericCaps(const Json& caps) {
    requireExactKeys(
        caps,
        keys({"candidate_build","graph_validation","lazy","metadata","planner"}),
        "protocol.numeric_caps");
    const std::map<std::string, Keys> dimensions{{
        {"graph_validation", keys({
            "combined_incoming_adjacency_entries","combined_outgoing_adjacency_entries",
            "data_vertices","derived_incident_interval_visits",
            "derived_neighbor_label_updates","interval_records","query_vertices",
            "seal_peak_live_bytes","snapshot_storage_bytes",
            "temporary_validation_bytes","temporal_edges"})},
        {"candidate_build", keys({
            "adjacency_probes","arc_checks","blocks","candidate_tree_tokens",
            "candidate_value_occurrences","fixed_memory_headroom_bytes","index_entries",
            "payload_memory_bytes","relation_entries","reverse_edge_lookups",
            "semijoin_entry_visits"})},
        {"lazy", keys({
            "adjacency_probes","arc_checks","cache_capacity_bytes","emitted_entries",
            "extension_probes","interval_intersections","reverse_edge_lookups"})},
        {"metadata", keys({"payload_bytes","receipt_tokens"})},
        {"planner", keys({
            "complete_proposal_cap","prefix_samples_per_plan",
            "proposal_state_expansions","quote_cover_value_visits"})}}};
    for (const auto& group : dimensions) {
        const Json& object = caps.at(group.first);
        requireExactKeys(object, group.second, "protocol.numeric_caps." + group.first);
        for (const auto& field : object.objectValue()) {
            if (!field.second.isUnsignedInteger() || field.second.unsignedInteger() == 0) {
                throw V3AuthorityError(
                    "numeric cap must be positive uint64: " + group.first + "." + field.first);
            }
        }
    }
    requireUnsigned(caps.at("candidate_build").at("candidate_tree_tokens"), 1,
                    "candidate tree token cap");
    requireUnsigned(caps.at("candidate_build").at("fixed_memory_headroom_bytes"),
                    4ULL * 1024ULL * 1024ULL * 1024ULL,
                    "candidate fixed headroom cap");
    const Json& payload = caps.at("candidate_build").at("payload_memory_bytes");
    if (payload.unsignedInteger() > 8ULL * 1024ULL * 1024ULL * 1024ULL) {
        throw V3AuthorityError("candidate payload cap exceeds eight GiB");
    }
    requireUnsigned(caps.at("metadata").at("receipt_tokens"), 1,
                    "metadata receipt token cap");
}

void validateBuildHashes(const Json& build, std::string_view location) {
    requireExactKeys(
        build,
        keys({"compiler_identity_sha256","executable_sha256","parameter_bundle_sha256",
              "runner_sha256","source_bundle_sha256","validator_sha256"}),
        location);
    for (const auto& field : build.objectValue()) {
        requireSha(field.second, std::string(location) + "." + field.first);
    }
}

void validateProtocol(const CanonicalArtifact& artifact) {
    const Json& protocol = artifact.value;
    requireExactKeys(
        protocol,
        keys({"schema","schema_version","protocol_id","status","authorization",
              "authorization_artifact_path","protocol_artifact_path",
              "execution_output_root","path_policy","execution_contract",
              "resource_envelope","datasets","cases","arms","schedule",
              "fixed_policy","gate_c_thresholds","numeric_caps","build_provenance",
              "readiness","integrity"}),
        "protocol");
    requireString(protocol.at("schema"), "icde2027_v3_executable_protocol", "protocol.schema");
    requireUnsigned(protocol.at("schema_version"), 2, "protocol.schema_version");
    requireString(protocol.at("status"), "frozen_execution_authorized", "protocol.status");
    if (!protocol.at("protocol_id").isString() || protocol.at("protocol_id").string().empty()) {
        throw V3AuthorityError("protocol id is missing");
    }
    requireEqual(protocol.at("authorization"),
                 parseConstant(R"({"execution_authorized":true})"),
                 "protocol.authorization");
    requireString(protocol.at("protocol_artifact_path"), kV3ProtocolArtifactPath,
                  "protocol.protocol_artifact_path");
    requireString(protocol.at("authorization_artifact_path"), kV3AuthorizationArtifactPath,
                  "protocol.authorization_artifact_path");
    requireString(protocol.at("execution_output_root"), kV3ExecutionOutputRoot,
                  "protocol.execution_output_root");
    requireEqual(protocol.at("path_policy"), exactPathPolicy(), "protocol.path_policy");
    requireEqual(protocol.at("execution_contract"), exactExecutionContract(),
                 "protocol.execution_contract");
    requireEqual(protocol.at("resource_envelope"), exactResourceEnvelope(),
                 "protocol.resource_envelope");
    requireEqual(protocol.at("datasets"), exactDatasets(), "protocol.datasets");
    requireEqual(protocol.at("cases"), exactCases(), "protocol.cases");
    requireEqual(protocol.at("arms"), exactArms(), "protocol.arms");
    requireEqual(protocol.at("schedule"), exactSchedule(), "protocol.schedule");
    requireEqual(protocol.at("fixed_policy"), exactFixedPolicy(), "protocol.fixed_policy");
    requireEqual(protocol.at("gate_c_thresholds"), exactGateCThresholds(),
                 "protocol.gate_c_thresholds");
    validateNumericCaps(protocol.at("numeric_caps"));
    validateBuildHashes(protocol.at("build_provenance"), "protocol.build_provenance");

    const Json& readiness = protocol.at("readiness");
    requireExactKeys(
        readiness,
        keys({"gate_a_status","gate_b_status","gate_a_receipt_sha256",
              "gate_b_receipt_sha256","patch_4_5_independent_review_receipt_sha256",
              "patch_5_independent_review_receipt_sha256",
              "patch_6_cli_telemetry_receipt_sha256","system_enforcement_receipt_sha256",
              "artifact_hash_revalidation_receipt_sha256",
              "input_revalidation_receipt_sha256",
              "independent_preregistration_signature_sha256"}),
        "protocol.readiness");
    requireString(readiness.at("gate_a_status"), "pass", "protocol Gate A status");
    requireString(readiness.at("gate_b_status"), "pass", "protocol Gate B status");
    for (const auto& field : readiness.objectValue()) {
        if (field.first == "gate_a_status" || field.first == "gate_b_status") continue;
        requireSha(field.second, "protocol.readiness." + field.first);
    }
}

void validateAuthorization(
    const CanonicalArtifact& authorization_artifact,
    const CanonicalArtifact& protocol_artifact) {
    const Json& authorization = authorization_artifact.value;
    const Json& protocol = protocol_artifact.value;
    requireExactKeys(
        authorization,
        keys({"schema","schema_version","protocol_binding","authorization",
              "independent_reviews","synthetic_gates","readiness_receipts",
              "build_provenance","integrity"}),
        "authorization");
    requireString(authorization.at("schema"),
                  "icde2027_v3_execution_authorization", "authorization.schema");
    requireUnsigned(authorization.at("schema_version"), 1,
                    "authorization.schema_version");
    requireEqual(
        authorization.at("authorization"),
        parseConstant(
            R"({"blocking_reasons":[],"execution_authorized":true,"headline_authorized":false})"),
        "authorization.decision");

    Json::Object binding;
    binding.emplace("protocol_file_sha256", stringValue(protocol_artifact.file_sha256));
    binding.emplace("protocol_id", protocol.at("protocol_id"));
    binding.emplace("protocol_payload_sha256", stringValue(protocol_artifact.payload_sha256));
    requireEqual(authorization.at("protocol_binding"), Json::object(std::move(binding)),
                 "authorization.protocol_binding");

    const Json& reviews = authorization.at("independent_reviews");
    requireExactKeys(
        reviews,
        keys({"patch_4_5_independent_review","patch_5_independent_review",
              "patch_6_cli_telemetry_review"}),
        "authorization.independent_reviews");
    for (const auto& review : reviews.objectValue()) {
        requireExactKeys(review.second, keys({"receipt_sha256","status"}),
                         "authorization review");
        requireString(review.second.at("status"), "pass", "authorization review status");
        requireSha(review.second.at("receipt_sha256"), "authorization review hash");
    }
    const Json& gates = authorization.at("synthetic_gates");
    requireExactKeys(gates, keys({"gate_a","gate_b"}), "authorization.synthetic_gates");
    for (const auto& gate : gates.objectValue()) {
        requireExactKeys(gate.second, keys({"receipt_sha256","status"}),
                         "authorization synthetic gate");
        requireString(gate.second.at("status"), "pass", "authorization gate status");
        requireSha(gate.second.at("receipt_sha256"), "authorization gate hash");
    }
    const Json& receipts = authorization.at("readiness_receipts");
    requireExactKeys(
        receipts,
        keys({"artifact_hash_revalidation","input_revalidation",
              "independent_preregistration_signature","system_enforcement"}),
        "authorization.readiness_receipts");
    for (const auto& receipt : receipts.objectValue()) {
        requireSha(receipt.second, "authorization readiness hash");
    }
    requireEqual(authorization.at("build_provenance"), protocol.at("build_provenance"),
                 "authorization.build_provenance");

    const Json& readiness = protocol.at("readiness");
    const std::array<std::pair<const char*, const char*>, 9> cross_bindings{{
        {"patch_4_5_independent_review_receipt_sha256", "patch_4_5_independent_review"},
        {"patch_5_independent_review_receipt_sha256", "patch_5_independent_review"},
        {"patch_6_cli_telemetry_receipt_sha256", "patch_6_cli_telemetry_review"},
        {"gate_a_receipt_sha256", "gate_a"},
        {"gate_b_receipt_sha256", "gate_b"},
        {"system_enforcement_receipt_sha256", "system_enforcement"},
        {"artifact_hash_revalidation_receipt_sha256", "artifact_hash_revalidation"},
        {"input_revalidation_receipt_sha256", "input_revalidation"},
        {"independent_preregistration_signature_sha256", "independent_preregistration_signature"}}};
    for (std::size_t index = 0; index < cross_bindings.size(); ++index) {
        const auto& binding_pair = cross_bindings[index];
        const Json* observed = nullptr;
        if (index < 3U) observed = &reviews.at(binding_pair.second).at("receipt_sha256");
        else if (index < 5U) observed = &gates.at(binding_pair.second).at("receipt_sha256");
        else observed = &receipts.at(binding_pair.second);
        requireEqual(readiness.at(binding_pair.first), *observed,
                     "protocol/authorization readiness cross-binding");
    }
}

const Json& findByStringField(
    const Json& array,
    std::string_view field,
    std::string_view expected,
    std::string_view location) {
    requireArray(array, location);
    const Json* result = nullptr;
    for (const Json& item : array.arrayValue()) {
        requireObject(item, location);
        const Json* value = item.find(field);
        if (value != nullptr && value->isString() && value->string() == expected) {
            if (result != nullptr) throw V3AuthorityError("duplicate declaration in " + std::string(location));
            result = &item;
        }
    }
    if (result == nullptr) throw V3AuthorityError("missing declaration in " + std::string(location));
    return *result;
}

std::string fourDigitOrdinal(std::uint64_t ordinal) {
    if (ordinal == 0 || ordinal > 9999U) throw V3AuthorityError("ordinal path width overflow");
    std::ostringstream output;
    output.width(4);
    output.fill('0');
    output << ordinal;
    return output.str();
}

std::string armPlannerPolicy(std::string_view arm_id) {
    if (arm_id == "v3dev_incumbent_16t") return "validated_incumbent_only";
    if (arm_id == "v3dev_cover_lazy_gate_on_16t" ||
        arm_id == "v3dev_cover_lazy_gate_off_16t") return "cover_lazy_v3";
    throw V3AuthorityError("unknown arm id");
}

std::string armProfitabilityGate(std::string_view arm_id) {
    if (arm_id == "v3dev_incumbent_16t") return "not_applicable";
    if (arm_id == "v3dev_cover_lazy_gate_on_16t") {
        return "candidate_le_9_over_10_incumbent";
    }
    if (arm_id == "v3dev_cover_lazy_gate_off_16t") {
        return "bypass_profitability_only";
    }
    throw V3AuthorityError("unknown arm id");
}

Json expectedContractAlgorithm(const Json& case_value, std::string_view arm_id) {
    Json::Object result;
    result.emplace("formula_id", stringValue(kV3SelectivityFormulaId));
    result.emplace("internal_filter", stringValue("consecutive_pair_on"));
    result.emplace("label_mode", stringValue("stable_id"));
    result.emplace("label_seed", uintValue(42));
    result.emplace("match_output", stringValue("count_only"));
    result.emplace("minimum_duration", case_value.at("duration_k"));
    result.emplace("planner_policy", stringValue(armPlannerPolicy(arm_id)));
    result.emplace("profitability_gate", stringValue(armProfitabilityGate(arm_id)));
    result.emplace("profitability_ratio_denominator", uintValue(10));
    result.emplace("profitability_ratio_numerator", uintValue(9));
    return Json::object(std::move(result));
}

Json expectedContractInput(const Json& dataset, const Json& case_value) {
    Json::Object result;
    result.emplace("data_bytes", dataset.at("bytes"));
    result.emplace("data_kind", stringValue("full_raw"));
    result.emplace("data_path", dataset.at("path"));
    result.emplace("data_sha256", dataset.at("sha256"));
    result.emplace("query_bytes", case_value.at("query_bytes"));
    result.emplace("query_edges", case_value.at("query_edges"));
    result.emplace("query_edges_directed", case_value.at("query_edges_directed"));
    result.emplace("query_labels", case_value.at("query_labels"));
    result.emplace("query_path", case_value.at("query_path"));
    result.emplace("query_sha256", case_value.at("query_sha256"));
    result.emplace("query_vertices", case_value.at("query_vertices"));
    return Json::object(std::move(result));
}

Json expectedContractOutput(std::uint64_t ordinal) {
    const std::string directory =
        std::string(kV3ExecutionOutputRoot) + "/" + fourDigitOrdinal(ordinal);
    Json::Object result;
    result.emplace("matcher_receipt_path", stringValue(directory + "/matcher_receipt.json"));
    result.emplace("run_contract_path", stringValue(directory + "/run_contract.json"));
    result.emplace("run_directory", stringValue(directory));
    result.emplace("runner_receipt_path", stringValue(directory + "/runner_receipt.json"));
    return Json::object(std::move(result));
}

std::string runContractPayloadHash(const Json& contract) {
    Json payload = withoutIntegrity(contract);
    Json& identity = payload.at("identity");
    requireObject(identity, "contract identity");
    identity.objectValue().erase("run_contract_sha256");
    identity.objectValue().erase("run_signature_sha256");
    return sha256Hex(canonicalJsonBytes(payload));
}

std::string expectedRunSignature(
    std::string_view protocol_payload_sha256,
    std::string_view contract_sha256,
    std::uint64_t ordinal,
    const Json& executable_sha256) {
    Json::Object signature;
    signature.emplace("attempt", uintValue(1));
    signature.emplace("executable_sha256", executable_sha256);
    signature.emplace("ordinal", uintValue(ordinal));
    signature.emplace("protocol_payload_sha256", stringValue(std::string(protocol_payload_sha256)));
    signature.emplace("run_contract_sha256", stringValue(std::string(contract_sha256)));
    return sha256Hex(canonicalJsonBytes(Json::object(std::move(signature))));
}

std::uint64_t validateContract(
    const CanonicalArtifact& contract_artifact,
    const CanonicalArtifact& protocol_artifact,
    const CanonicalArtifact& authorization_artifact,
    std::string_view contract_path) {
    const Json& contract = contract_artifact.value;
    const Json& protocol = protocol_artifact.value;
    requireExactKeys(
        contract,
        keys({"schema","schema_version","identity","authorization","algorithm","input",
              "resources","numeric_caps","build","output","integrity"}),
        "run contract");
    requireString(contract.at("schema"), "icde2027_v3_run_contract", "contract.schema");
    requireUnsigned(contract.at("schema_version"), 1, "contract.schema_version");

    const Json& identity = contract.at("identity");
    requireExactKeys(
        identity,
        keys({"arm_id","arm_position","attempt","block_id","case_id","ordinal",
              "protocol_file_sha256","protocol_id","protocol_payload_sha256","repeat",
              "run_contract_sha256","run_signature_sha256"}),
        "contract.identity");
    if (!identity.at("ordinal").isUnsignedInteger()) {
        throw V3AuthorityError("contract ordinal must be uint64");
    }
    const std::uint64_t ordinal = identity.at("ordinal").unsignedInteger();
    const Json& positions = protocol.at("schedule").at("positions");
    if (ordinal == 0 || ordinal > positions.arrayValue().size()) {
        throw V3AuthorityError("contract ordinal outside exact schedule");
    }
    const Json& row = positions.arrayValue()[static_cast<std::size_t>(ordinal - 1U)];
    const std::string arm_id = row.at("arm_id").string();
    const std::string case_id = row.at("case_id").string();
    const Json& case_value = findByStringField(protocol.at("cases"), "case_id", case_id,
                                               "protocol.cases");
    const Json& dataset = findByStringField(
        protocol.at("datasets"), "dataset_id", case_value.at("dataset_id").string(),
        "protocol.datasets");

    requireEqual(contract.at("algorithm"), expectedContractAlgorithm(case_value, arm_id),
                 "contract.algorithm/selectivity-formula binding");
    requireEqual(contract.at("input"), expectedContractInput(dataset, case_value),
                 "contract.input");
    requireEqual(contract.at("resources"), expectedResources(), "contract.resources");
    requireEqual(contract.at("numeric_caps"), protocol.at("numeric_caps"),
                 "contract.numeric_caps");
    requireEqual(contract.at("build"), protocol.at("build_provenance"), "contract.build");
    const Json expected_output = expectedContractOutput(ordinal);
    requireEqual(contract.at("output"), expected_output, "contract.output");
    requireString(expected_output.at("run_contract_path"), contract_path,
                  "CLI/run-contract path binding");

    Json::Object expected_authorization;
    expected_authorization.emplace("authorization_path", stringValue(kV3AuthorizationArtifactPath));
    expected_authorization.emplace(
        "authorization_receipt_sha256", stringValue(authorization_artifact.file_sha256));
    expected_authorization.emplace("execution_authorized_observed", boolValue(true));
    expected_authorization.emplace("headline_authorized", boolValue(false));
    requireEqual(contract.at("authorization"), Json::object(std::move(expected_authorization)),
                 "contract.authorization");

    const std::map<std::string, const Json*> row_identity{{
        {"arm_id", &row.at("arm_id")},
        {"arm_position", &row.at("arm_position")},
        {"attempt", &row.at("attempt")},
        {"block_id", &row.at("block_id")},
        {"case_id", &row.at("case_id")},
        {"repeat", &row.at("repeat")}}};
    for (const auto& field : row_identity) {
        requireEqual(identity.at(field.first), *field.second, "contract.identity." + field.first);
    }
    requireUnsigned(identity.at("attempt"), 1, "contract.identity.attempt");
    requireString(identity.at("protocol_file_sha256"), protocol_artifact.file_sha256,
                  "contract protocol file hash");
    requireEqual(identity.at("protocol_id"), protocol.at("protocol_id"),
                 "contract protocol id");
    requireString(identity.at("protocol_payload_sha256"), protocol_artifact.payload_sha256,
                  "contract protocol payload hash");
    const std::string payload_hash = runContractPayloadHash(contract);
    requireString(identity.at("run_contract_sha256"), payload_hash,
                  "contract payload hash");
    const std::string signature = expectedRunSignature(
        protocol_artifact.payload_sha256,
        payload_hash,
        ordinal,
        protocol.at("build_provenance").at("executable_sha256"));
    requireString(identity.at("run_signature_sha256"), signature,
                  "contract run signature");
    return ordinal;
}

struct ParsedClosedCli {
    bool check_only = false;
    std::string contract_path;
    std::string authorization_path;
    std::string receipt_path;
};

ParsedClosedCli parseClosedCli(int argc, char* argv[]) {
    ParsedClosedCli parsed;
    if (argc >= 2 && std::string_view(argv[1]) == "--check-v3-run-contract") {
        if (argc != 5 || std::string_view(argv[3]) != "--v3-authorization") {
            throw JsonError(
                "check mode requires exactly --check-v3-run-contract RUN "
                "--v3-authorization AUTH");
        }
        parsed.check_only = true;
        parsed.contract_path = argv[2];
        parsed.authorization_path = argv[4];
    } else if (argc >= 2 && std::string_view(argv[1]) == "--v3-run-contract") {
        if (argc != 7 || std::string_view(argv[3]) != "--v3-authorization" ||
            std::string_view(argv[5]) != "--v3-receipt") {
            throw JsonError(
                "protocol mode requires exactly --v3-run-contract RUN "
                "--v3-authorization AUTH --v3-receipt RECEIPT");
        }
        parsed.contract_path = argv[2];
        parsed.authorization_path = argv[4];
        parsed.receipt_path = argv[6];
    } else {
        throw JsonError("legacy and V3 protocol arguments cannot be mixed");
    }
    if (isDeniedPath(parsed.contract_path) || isDeniedPath(parsed.authorization_path) ||
        (!parsed.check_only && isDeniedPath(parsed.receipt_path))) {
        throw JsonError("V3 CLI path is noncanonical or denied");
    }
    if (!isExactRunContractLexicalPath(parsed.contract_path)) {
        throw JsonError("run-contract CLI path is outside the exact output namespace");
    }
    if (parsed.authorization_path != kV3AuthorizationArtifactPath) {
        throw JsonError("authorization CLI path differs from frozen path");
    }
    return parsed;
}

} // namespace

ValidatedV3RunContract validateV3AuthorityFiles(
    const std::filesystem::path& run_contract_path,
    const std::filesystem::path& authorization_path) {
    const std::string contract_lexical = run_contract_path.generic_string();
    const std::string authorization_lexical = authorization_path.generic_string();
    if (isDeniedPath(contract_lexical) || isDeniedPath(authorization_lexical)) {
        throw V3AuthorityError("authority path is noncanonical or denied");
    }
    if (!isExactRunContractLexicalPath(contract_lexical)) {
        throw V3AuthorityError("run-contract path is outside the exact output namespace");
    }
    if (authorization_lexical != kV3AuthorizationArtifactPath) {
        throw V3AuthorityError("authorization path differs from exact frozen path");
    }
    CanonicalArtifact protocol = loadCanonicalArtifact(kV3ProtocolArtifactPath);
    validateProtocol(protocol);
    CanonicalArtifact authorization = loadCanonicalArtifact(authorization_path);
    validateAuthorization(authorization, protocol);
    CanonicalArtifact contract = loadCanonicalArtifact(run_contract_path);
    const std::uint64_t ordinal = validateContract(
        contract, protocol, authorization, contract_lexical);
    const std::string receipt_path = contract.value.at("output")
        .at("matcher_receipt_path").string();
    return ValidatedV3RunContract{
        std::move(protocol), std::move(authorization), std::move(contract),
        ordinal, receipt_path};
}

bool containsV3CliToken(int argc, char* argv[]) noexcept {
    constexpr std::array<std::string_view, 4> names{{
        "--check-v3-run-contract", "--v3-run-contract",
        "--v3-authorization", "--v3-receipt"}};
    for (int index = 1; index < argc; ++index) {
        const std::string_view token(argv[index]);
        for (std::string_view name : names) {
            if (token == name ||
                (token.size() > name.size() &&
                 token.substr(0, name.size()) == name &&
                 token[name.size()] == '=')) {
                return true;
            }
        }
    }
    return false;
}

int runClosedV3Cli(int argc, char* argv[]) noexcept {
    try {
        const ParsedClosedCli cli = parseClosedCli(argc, argv);
        ValidatedV3RunContract authority = validateV3AuthorityFiles(
            cli.contract_path, cli.authorization_path);
        if (cli.check_only) {
            std::cout << "V3 authority check passed for ordinal "
                      << authority.ordinal
                      << "; graph and matcher were not executed.\n";
            return kV3CompleteSuccess;
        }
        if (cli.receipt_path != authority.matcher_receipt_path) {
            std::cerr << "V3 authority error: receipt path differs from exact contract.\n";
            return kV3AuthorizationOrHashError;
        }
        std::error_code exists_error;
        const bool exists = std::filesystem::exists(cli.receipt_path, exists_error);
        if (exists_error || exists) {
            std::cerr << "V3 receipt I/O error: target exists or cannot be inspected.\n";
            return kV3ReceiptIoError;
        }
        return executeValidatedV3Run(authority, cli.receipt_path);
    } catch (const V3AuthorityError& error) {
        std::cerr << "V3 authorization error: " << error.what() << '\n';
        return kV3AuthorizationOrHashError;
    } catch (const ArtifactIntegrityError& error) {
        std::cerr << "V3 authorization/hash error: " << error.what() << '\n';
        return kV3AuthorizationOrHashError;
    } catch (const ArtifactIoError& error) {
        std::cerr << "V3 authority I/O error: " << error.what() << '\n';
        return kV3AuthorizationOrHashError;
    } catch (const JsonError& error) {
        std::cerr << "V3 CLI/schema error: " << error.what() << '\n';
        return kV3CliOrSchemaError;
    } catch (const std::exception& error) {
        std::cerr << "V3 authorization error: " << error.what() << '\n';
        return kV3AuthorizationOrHashError;
    }
}

} // namespace dsm::v3
