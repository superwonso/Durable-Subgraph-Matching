#include "v3_formula_cap.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using dsm::v3::Json;
using dsm::v3::V3FormulaCapError;
using dsm::v3::V3FormulaCapMaterialization;

constexpr std::uint64_t kGiB = UINT64_C(1024) * UINT64_C(1024) * UINT64_C(1024);

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Callable>
void requireRejects(Callable&& callable, const std::string& message) {
    bool rejected = false;
    try {
        callable();
    } catch (const V3FormulaCapError&) {
        rejected = true;
    }
    require(rejected, message);
}

Json basePolicy() {
    return Json::parse(R"json({
        "schema":"icde2027_v3_formula_cap_policy",
        "schema_version":1,
        "policy_id":"synthetic-grid-vector",
        "formula_id":"tcc_v3_post_filter_checked_u64_caps_v1",
        "fixed":{
            "candidate_tree_tokens":1,
            "fixed_memory_headroom_bytes":4294967296,
            "metadata_payload_bytes":67108864,
            "metadata_receipt_tokens":1,
            "process_memory_max_bytes":42949672960,
            "rescore_top_l":1,
            "sample_seed":42
        },
        "candidate_build":{
            "formula_id":"candidate_alpha_b_filtered_edges_plus_root_self_loop_v2",
            "adjacency_multiplier":{"numerator":1,"denominator":2},
            "payload_ceiling_bytes":8589934592
        },
        "planner":{
            "complete_proposal_cap":16,
            "state_expansions":1024,
            "prefix_samples_per_plan":32
        },
        "lazy":{
            "formula_id":"lazy_alpha_l_filtered_edges_clamped_v1",
            "adjacency_fraction":{"numerator":1,"denominator":64},
            "adjacency_minimum":4096,
            "adjacency_maximum":16777216,
            "cache_entries":2048,
            "extension_probes":65536,
            "interval_intersections":131072
        },
        "rescore":{
            "formula_id":"symmetric_fixed_plan_pilot_v1",
            "prefix_samples_per_plan":32,
            "extension_probes":65536
        },
        "graph_validation":{
            "formula_id":"validated_snapshot_exact_structural_quote_v1",
            "accounting_model_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        },
        "accounting":{
            "candidate_value_bytes":4,
            "index_entries_per_block":1,
            "lazy_cache_entry_bytes":64,
            "semijoin_visits_per_block":3,
            "semijoin_visits_per_candidate_value":2
        },
        "cost_coefficients":{
            "formula_id":"synthetic_primitive_median_normalized_v1",
            "artifact_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            "weights":{
                "adjacency_probe":1000000,
                "emitted_entry":1200000,
                "index_visit":1100000,
                "interval_intersection":2000000,
                "reverse_lookup":1300000,
                "semijoin_visit":1400000
            }
        }
    })json");
}

Json baseRequest() {
    return Json::parse(R"json({
        "filtered_vertices":100,
        "filtered_edges":1001,
        "interval_records":1500,
        "query_vertices":6,
        "query_edges":7,
        "graph_storage_quote":{
            "accounting_model_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            "snapshot_storage_bytes":1000000,
            "temporary_validation_bytes":200000,
            "seal_peak_live_bytes":2000000
        }
    })json");
}

Json ratio(std::uint64_t numerator, std::uint64_t denominator) {
    Json::Object result;
    result.emplace("denominator", Json(denominator));
    result.emplace("numerator", Json(numerator));
    return Json::object(std::move(result));
}

void requireMaterializes(const Json& policy) {
    const V3FormulaCapMaterialization result =
        dsm::v3::materializeV3FormulaCaps(policy, baseRequest());
    require(result.value.isObject(), "allowed grid policy did not materialize");
}

void testGoldenAndProjection() {
    const Json policy = basePolicy();
    const dsm::v3::V3FormulaCapPolicy typed =
        dsm::v3::validateV3FormulaCapPolicy(policy);
    require(
        typed.policy_sha256 ==
            "8ae14b8a48723a0e99c077034f5af8844b1013d2e456698974644b1cad5a7fde",
        "Python/C++ policy golden SHA drift");
    require(
        typed.cost_coefficient_artifact_sha256 == std::string(64U, 'a'),
        "cost coefficient artifact was not retained");
    require(
        typed.cost_weights.adjacency_probe == UINT64_C(1000000) &&
            typed.cost_weights.emitted_entry == UINT64_C(1200000) &&
            typed.cost_weights.index_visit == UINT64_C(1100000) &&
            typed.cost_weights.interval_intersection == UINT64_C(2000000) &&
            typed.cost_weights.reverse_lookup == UINT64_C(1300000) &&
            typed.cost_weights.semijoin_visit == UINT64_C(1400000),
        "six cost weights were not retained exactly");

    const V3FormulaCapMaterialization materialized =
        dsm::v3::materializeV3FormulaCaps(typed, dsm::v3::parseV3FormulaCapInputs(baseRequest()));
    const std::string expected =
        R"json({"candidate_build":{"adjacency_probes":501,"arc_checks":1102,"blocks":500,"candidate_tree_tokens":1,"candidate_value_occurrences":601,"fixed_memory_headroom_bytes":4294967296,"index_entries":500,"payload_ceiling_bytes":8589934592,"payload_reservation_envelope_bytes":12884901888,"relation_entries":501,"reverse_edge_lookups":501,"semijoin_entry_visits":2702},"derivation_inputs":{"filtered_edges":1001,"filtered_vertices":100,"interval_records":1500,"query_edges":7,"query_vertices":6},"formula_id":"tcc_v3_post_filter_checked_u64_caps_v1","graph_validation":{"accounting_model_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","combined_incoming_adjacency_entries":1008,"combined_outgoing_adjacency_entries":1008,"data_vertices":100,"derived_incident_interval_visits":3000,"derived_neighbor_label_updates":2002,"interval_records":1500,"query_vertices":6,"seal_peak_live_bytes":2000000,"snapshot_storage_bytes":1000000,"temporal_edges":1001,"temporary_validation_bytes":200000},"lazy":{"adjacency_probes":4096,"arc_checks":8192,"block_requests":3072,"cache_capacity_bytes":147456,"cache_entries":2048,"emitted_entries":4096,"extension_probes":65536,"interval_intersections":131072,"reverse_edge_lookups":4096},"memory_admission":{"fixed_memory_headroom_bytes":4294967296,"formula_id":"existing_plus_candidate_plus_headroom_le_process_v1","payload_ceiling_bytes":8589934592,"payload_reservation_envelope_bytes":12884901888,"process_memory_max_bytes":42949672960},"metadata":{"payload_bytes":67108864,"receipt_tokens":1},"planner":{"complete_proposal_cap":16,"prefix_samples_per_plan":32,"proposal_state_expansions":1024,"quote_cover_value_visits":8000},"policy_sha256":"8ae14b8a48723a0e99c077034f5af8844b1013d2e456698974644b1cad5a7fde","rescore":{"block_requests_per_side":192,"extension_probes_per_side":65536,"interval_intersections_per_side":458752,"prefix_samples_per_plan":32},"schema":"icde2027_v3_materialized_caps","schema_version":1}
)json";
    require(materialized.canonical_bytes == expected, "materialized golden bytes drift");
    require(
        materialized.materialized_caps_sha256 ==
            "20d5d6f9536b1123e7dd40472f0c888d1f2ef3f0b5606dbda93b20f827485156",
        "materialized golden SHA drift");

    const auto& projection = materialized.projection;
    require(
        projection.graph_validation_budget.data_vertices == 100U &&
            projection.graph_validation_budget.temporal_edges == 1001U &&
            projection.graph_validation_budget.snapshot_storage_bytes ==
                UINT64_C(1000000),
        "GraphValidationBudget projection drift");
    require(
        projection.candidate_build_budget.adjacency_probes == 501U &&
            projection.candidate_build_budget.arc_checks == 1102U &&
            projection.candidate_build_budget.payload_memory_bytes == 8U * kGiB &&
            projection.candidate_payload_ceiling_bytes == 8U * kGiB &&
            projection.candidate_payload_reservation_envelope_bytes == 12U * kGiB,
        "CandidateBuildBudget P/envelope projection drift");
    require(
        projection.proposal_budget.maximum_quote_cover_value_visits == 8000U &&
            projection.planner_prefix_samples_per_plan == 32U &&
            projection.lazy_budget.maximum_block_requests == 3072U &&
            projection.lazy_budget.maximum_relation_entries == 4096U &&
            projection.lazy_emitted_entries == 4096U &&
            projection.rescore_budget.maximum_interval_intersections == 458752U &&
            projection.metadata_budget.payload_bytes == 67108864U,
        "planner/lazy/rescore/metadata projection drift");

    const V3FormulaCapMaterialization verified =
        dsm::v3::verifyV3FormulaCapMaterialization(
            materialized.value, policy, baseRequest());
    require(
        verified.materialized_caps_sha256 == materialized.materialized_caps_sha256,
        "exact materialized verification drift");
    Json tampered = materialized.value;
    tampered.at("candidate_build").at("arc_checks") = Json(UINT64_C(1101));
    requireRejects(
        [&]() {
            static_cast<void>(dsm::v3::verifyV3FormulaCapMaterialization(
                tampered, policy, baseRequest()));
        },
        "tampered materialized object was accepted");
    Json unknown_materialized = materialized.value;
    unknown_materialized.objectValue().emplace("unexpected", Json(UINT64_C(1)));
    requireRejects(
        [&]() {
            static_cast<void>(dsm::v3::verifyV3FormulaCapMaterialization(
                unknown_materialized, policy, baseRequest()));
        },
        "unknown materialized field was accepted");
}

void testEveryGridValue() {
    for (const auto& item : std::array<std::pair<std::uint64_t, std::uint64_t>, 4>{{
             {1U, 4U}, {1U, 2U}, {1U, 1U}, {2U, 1U}}}) {
        Json policy = basePolicy();
        policy.at("candidate_build").at("adjacency_multiplier") =
            ratio(item.first, item.second);
        requireMaterializes(policy);
    }
    for (const std::uint64_t value :
         std::array<std::uint64_t, 4>{{kGiB, 2U * kGiB, 4U * kGiB, 8U * kGiB}}) {
        Json policy = basePolicy();
        policy.at("candidate_build").at("payload_ceiling_bytes") = Json(value);
        requireMaterializes(policy);
    }
    for (const std::uint64_t value : std::array<std::uint64_t, 3>{{8U, 16U, 32U}}) {
        Json policy = basePolicy();
        policy.at("planner").at("complete_proposal_cap") = Json(value);
        requireMaterializes(policy);
    }
    for (const std::uint64_t value :
         std::array<std::uint64_t, 3>{{256U, 1024U, 4096U}}) {
        Json policy = basePolicy();
        policy.at("planner").at("state_expansions") = Json(value);
        requireMaterializes(policy);
    }
    for (const std::uint64_t value : std::array<std::uint64_t, 3>{{16U, 32U, 64U}}) {
        Json policy = basePolicy();
        policy.at("planner").at("prefix_samples_per_plan") = Json(value);
        requireMaterializes(policy);
    }
    for (const auto& item : std::array<std::pair<std::uint64_t, std::uint64_t>, 4>{{
             {1U, 256U}, {1U, 128U}, {1U, 64U}, {1U, 32U}}}) {
        Json policy = basePolicy();
        policy.at("lazy").at("adjacency_fraction") =
            ratio(item.first, item.second);
        requireMaterializes(policy);
    }
    for (const std::uint64_t value :
         std::array<std::uint64_t, 3>{{512U, 2048U, 8192U}}) {
        Json policy = basePolicy();
        policy.at("lazy").at("cache_entries") = Json(value);
        requireMaterializes(policy);
    }
    for (const std::uint64_t value :
         std::array<std::uint64_t, 3>{{UINT64_C(1) << 14U, UINT64_C(1) << 16U,
                                      UINT64_C(1) << 18U}}) {
        Json lazy_policy = basePolicy();
        lazy_policy.at("lazy").at("extension_probes") = Json(value);
        requireMaterializes(lazy_policy);
        Json rescore_policy = basePolicy();
        rescore_policy.at("rescore").at("extension_probes") = Json(value);
        requireMaterializes(rescore_policy);
    }
    for (const std::uint64_t value :
         std::array<std::uint64_t, 3>{{UINT64_C(1) << 15U, UINT64_C(1) << 17U,
                                      UINT64_C(1) << 19U}}) {
        Json policy = basePolicy();
        policy.at("lazy").at("interval_intersections") = Json(value);
        requireMaterializes(policy);
    }
    for (const std::uint64_t value : std::array<std::uint64_t, 3>{{16U, 32U, 64U}}) {
        Json policy = basePolicy();
        policy.at("rescore").at("prefix_samples_per_plan") = Json(value);
        requireMaterializes(policy);
    }
}

void testEmptyAndCeilBoundaries() {
    Json empty = baseRequest();
    empty.at("filtered_vertices") = Json(UINT64_C(0));
    empty.at("filtered_edges") = Json(UINT64_C(0));
    empty.at("interval_records") = Json(UINT64_C(0));
    empty.at("graph_storage_quote").at("snapshot_storage_bytes") = Json(UINT64_C(1));
    empty.at("graph_storage_quote").at("temporary_validation_bytes") = Json(UINT64_C(0));
    empty.at("graph_storage_quote").at("seal_peak_live_bytes") = Json(UINT64_C(1));
    const V3FormulaCapMaterialization empty_result =
        dsm::v3::materializeV3FormulaCaps(basePolicy(), empty);
    require(
        empty_result.value.at("candidate_build").at("adjacency_probes").unsignedInteger() == 0U &&
            empty_result.value.at("candidate_build").at("blocks").unsignedInteger() == 0U &&
            empty_result.value.at("graph_validation").at("data_vertices").unsignedInteger() == 0U &&
            empty_result.value.at("graph_validation").at("temporary_validation_bytes").unsignedInteger() == 0U,
        "empty filtered graph did not preserve exact zero semantics");
    require(
        empty_result.value.at("lazy").at("adjacency_probes").unsignedInteger() == 4096U,
        "empty graph must retain the frozen lazy minimum");

    Json quarter = basePolicy();
    quarter.at("candidate_build").at("adjacency_multiplier") = ratio(1U, 4U);
    for (const auto& item :
         std::array<std::pair<std::uint64_t, std::uint64_t>, 4>{{
             {0U, 0U}, {1U, 1U}, {4U, 1U}, {5U, 2U}}}) {
        Json request = baseRequest();
        request.at("filtered_edges") = Json(item.first);
        const V3FormulaCapMaterialization result =
            dsm::v3::materializeV3FormulaCaps(quarter, request);
        require(
            result.value.at("candidate_build").at("adjacency_probes").unsignedInteger() ==
                item.second,
            "checked ceil-ratio boundary drift");
    }

    Json large = baseRequest();
    large.at("filtered_edges") = Json(UINT64_C(1) << 40U);
    const V3FormulaCapMaterialization large_result =
        dsm::v3::materializeV3FormulaCaps(basePolicy(), large);
    require(
        large_result.value.at("lazy").at("adjacency_probes").unsignedInteger() ==
            (UINT64_C(1) << 24U),
        "lazy maximum clamp drift");
}

void testClosedSchemaTypesAndInvariants() {
    require(
        Json(true).isBoolean() && !Json(true).isUnsignedInteger(),
        "Json bool/integer type distinction regressed");

    Json unknown = basePolicy();
    unknown.objectValue().emplace("unexpected", Json(UINT64_C(1)));
    requireRejects(
        [&]() { static_cast<void>(dsm::v3::validateV3FormulaCapPolicy(unknown)); },
        "unknown policy field was accepted");

    Json missing = basePolicy();
    missing.at("planner").objectValue().erase("state_expansions");
    requireRejects(
        [&]() { static_cast<void>(dsm::v3::validateV3FormulaCapPolicy(missing)); },
        "missing nested policy field was accepted");

    Json boolean = basePolicy();
    boolean.at("planner").at("complete_proposal_cap") = Json(true);
    requireRejects(
        [&]() { static_cast<void>(dsm::v3::validateV3FormulaCapPolicy(boolean)); },
        "boolean policy integer was accepted");

    Json outside = basePolicy();
    outside.at("candidate_build").at("adjacency_multiplier") = ratio(3U, 4U);
    requireRejects(
        [&]() { static_cast<void>(dsm::v3::validateV3FormulaCapPolicy(outside)); },
        "out-of-grid ratio was accepted");

    Json zero_ratio = basePolicy();
    zero_ratio.at("candidate_build").at("adjacency_multiplier") = ratio(1U, 0U);
    requireRejects(
        [&]() { static_cast<void>(dsm::v3::validateV3FormulaCapPolicy(zero_ratio)); },
        "zero ratio denominator was accepted");
    Json zero_numerator = basePolicy();
    zero_numerator.at("candidate_build").at("adjacency_multiplier") =
        ratio(0U, 1U);
    requireRejects(
        [&]() {
            static_cast<void>(
                dsm::v3::validateV3FormulaCapPolicy(zero_numerator));
        },
        "zero ratio numerator was accepted");

    Json fixed_drift = basePolicy();
    fixed_drift.at("fixed").at("fixed_memory_headroom_bytes") =
        Json(UINT64_C(1));
    requireRejects(
        [&]() { static_cast<void>(dsm::v3::validateV3FormulaCapPolicy(fixed_drift)); },
        "fixed headroom drift was accepted");

    const std::array<std::pair<const char*, std::uint64_t>, 6> fixed_mutations{{
        {"candidate_tree_tokens", UINT64_C(2)},
        {"metadata_payload_bytes", UINT64_C(0)},
        {"metadata_receipt_tokens", UINT64_C(2)},
        {"process_memory_max_bytes", UINT64_C(39) * kGiB},
        {"rescore_top_l", UINT64_C(2)},
        {"sample_seed", UINT64_C(41)},
    }};
    for (const auto& mutation : fixed_mutations) {
        Json value = basePolicy();
        value.at("fixed").at(mutation.first) = Json(mutation.second);
        requireRejects(
            [&]() {
                static_cast<void>(dsm::v3::validateV3FormulaCapPolicy(value));
            },
            std::string("fixed invariant was accepted: ") + mutation.first);
    }

    Json accounting_drift = basePolicy();
    accounting_drift.at("accounting").at("candidate_value_bytes") =
        Json(UINT64_C(8));
    requireRejects(
        [&]() { static_cast<void>(dsm::v3::validateV3FormulaCapPolicy(accounting_drift)); },
        "fixed accounting drift was accepted");

    const std::array<std::pair<const char*, std::uint64_t>, 4>
        accounting_mutations{{
            {"index_entries_per_block", UINT64_C(2)},
            {"lazy_cache_entry_bytes", UINT64_C(0)},
            {"semijoin_visits_per_block", UINT64_C(2)},
            {"semijoin_visits_per_candidate_value", UINT64_C(3)},
        }};
    for (const auto& mutation : accounting_mutations) {
        Json value = basePolicy();
        value.at("accounting").at(mutation.first) = Json(mutation.second);
        requireRejects(
            [&]() {
                static_cast<void>(dsm::v3::validateV3FormulaCapPolicy(value));
            },
            std::string("accounting invariant was accepted: ") + mutation.first);
    }

    Json lazy_minimum_drift = basePolicy();
    lazy_minimum_drift.at("lazy").at("adjacency_minimum") = Json(UINT64_C(4095));
    requireRejects(
        [&]() {
            static_cast<void>(
                dsm::v3::validateV3FormulaCapPolicy(lazy_minimum_drift));
        },
        "lazy minimum invariant was accepted");
    Json lazy_maximum_drift = basePolicy();
    lazy_maximum_drift.at("lazy").at("adjacency_maximum") =
        Json((UINT64_C(1) << 24U) - 1U);
    requireRejects(
        [&]() {
            static_cast<void>(
                dsm::v3::validateV3FormulaCapPolicy(lazy_maximum_drift));
        },
        "lazy maximum invariant was accepted");

    Json weight_drift = basePolicy();
    weight_drift.at("cost_coefficients").at("weights").at("adjacency_probe") =
        Json(UINT64_C(999999));
    requireRejects(
        [&]() { static_cast<void>(dsm::v3::validateV3FormulaCapPolicy(weight_drift)); },
        "cost normalization drift was accepted");

    Json uppercase_sha = basePolicy();
    uppercase_sha.at("graph_validation").at("accounting_model_sha256") =
        Json(std::string(64U, 'A'));
    requireRejects(
        [&]() { static_cast<void>(dsm::v3::validateV3FormulaCapPolicy(uppercase_sha)); },
        "noncanonical SHA was accepted");

    Json request_unknown = baseRequest();
    request_unknown.objectValue().emplace("unexpected", Json(UINT64_C(1)));
    requireRejects(
        [&]() { static_cast<void>(dsm::v3::parseV3FormulaCapInputs(request_unknown)); },
        "unknown request field was accepted");

    Json request_bool = baseRequest();
    request_bool.at("interval_records") = Json(true);
    requireRejects(
        [&]() { static_cast<void>(dsm::v3::parseV3FormulaCapInputs(request_bool)); },
        "boolean derivation input was accepted");

    Json quote_bool = baseRequest();
    quote_bool.at("graph_storage_quote").at("temporary_validation_bytes") = Json(false);
    requireRejects(
        [&]() { static_cast<void>(dsm::v3::parseV3FormulaCapInputs(quote_bool)); },
        "boolean storage quote was accepted");

    Json quote_unknown = baseRequest();
    quote_unknown.at("graph_storage_quote")
        .objectValue()
        .emplace("unexpected", Json(UINT64_C(1)));
    requireRejects(
        [&]() {
            static_cast<void>(dsm::v3::parseV3FormulaCapInputs(quote_unknown));
        },
        "unknown storage-quote field was accepted");

    Json one_vertex = baseRequest();
    one_vertex.at("query_vertices") = Json(UINT64_C(1));
    requireRejects(
        [&]() { static_cast<void>(dsm::v3::materializeV3FormulaCaps(basePolicy(), one_vertex)); },
        "one-vertex query was accepted");

    Json zero_edges = baseRequest();
    zero_edges.at("query_edges") = Json(UINT64_C(0));
    requireRejects(
        [&]() { static_cast<void>(dsm::v3::materializeV3FormulaCaps(basePolicy(), zero_edges)); },
        "zero-edge query was accepted");

    Json zero_storage = baseRequest();
    zero_storage.at("graph_storage_quote").at("snapshot_storage_bytes") =
        Json(UINT64_C(0));
    requireRejects(
        [&]() { static_cast<void>(dsm::v3::materializeV3FormulaCaps(basePolicy(), zero_storage)); },
        "zero snapshot storage quote was accepted");

    Json wrong_hash = baseRequest();
    wrong_hash.at("graph_storage_quote").at("accounting_model_sha256") =
        Json(std::string(64U, 'b'));
    requireRejects(
        [&]() { static_cast<void>(dsm::v3::materializeV3FormulaCaps(basePolicy(), wrong_hash)); },
        "cross-model graph quote was accepted");

    dsm::v3::V3FormulaCapPolicy typed =
        dsm::v3::validateV3FormulaCapPolicy(basePolicy());
    typed.policy_sha256 = std::string(64U, 'b');
    requireRejects(
        [&]() {
            static_cast<void>(dsm::v3::materializeV3FormulaCaps(
                typed, dsm::v3::parseV3FormulaCapInputs(baseRequest())));
        },
        "mutated typed policy hash was accepted");
}

void testCheckedOverflowRejection() {
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();

    Json multiply_policy = basePolicy();
    multiply_policy.at("candidate_build").at("adjacency_multiplier") = ratio(2U, 1U);
    Json multiply_request = baseRequest();
    multiply_request.at("filtered_edges") = Json(maximum / 2U + 1U);
    requireRejects(
        [&]() {
            static_cast<void>(dsm::v3::materializeV3FormulaCaps(
                multiply_policy, multiply_request));
        },
        "candidate ceil multiplication overflow was accepted");

    Json addition_request = baseRequest();
    addition_request.at("filtered_edges") = Json(maximum);
    requireRejects(
        [&]() {
            static_cast<void>(dsm::v3::materializeV3FormulaCaps(
                basePolicy(), addition_request));
        },
        "candidate addition overflow was accepted");

    Json interval_request = baseRequest();
    interval_request.at("interval_records") = Json(maximum);
    requireRejects(
        [&]() {
            static_cast<void>(dsm::v3::materializeV3FormulaCaps(
                basePolicy(), interval_request));
        },
        "derived interval multiplication overflow was accepted");

    Json block_request = baseRequest();
    block_request.at("filtered_vertices") = Json(maximum);
    block_request.at("filtered_edges") = Json(UINT64_C(0));
    block_request.at("interval_records") = Json(UINT64_C(0));
    requireRejects(
        [&]() {
            static_cast<void>(dsm::v3::materializeV3FormulaCaps(
                basePolicy(), block_request));
        },
        "candidate semijoin/block overflow was accepted");

    Json query_request = baseRequest();
    query_request.at("filtered_vertices") = Json(UINT64_C(2));
    query_request.at("query_vertices") = Json(maximum);
    requireRejects(
        [&]() {
            static_cast<void>(dsm::v3::materializeV3FormulaCaps(
                basePolicy(), query_request));
        },
        "q-by-n multiplication overflow was accepted");

    Json cache_policy = basePolicy();
    cache_policy.at("accounting").at("lazy_cache_entry_bytes") = Json(maximum);
    requireRejects(
        [&]() {
            static_cast<void>(dsm::v3::materializeV3FormulaCaps(
                cache_policy, baseRequest()));
        },
        "lazy cache multiplication overflow was accepted");

    Json rescore_request = baseRequest();
    rescore_request.at("query_edges") = Json(maximum);
    requireRejects(
        [&]() {
            static_cast<void>(dsm::v3::materializeV3FormulaCaps(
                basePolicy(), rescore_request));
        },
        "rescore/combined-edge overflow was accepted");
}

} // namespace

int main() {
    try {
        testGoldenAndProjection();
        testEveryGridValue();
        testEmptyAndCeilBoundaries();
        testClosedSchemaTypesAndInvariants();
        testCheckedOverflowRejection();
        std::cout << "v3 formula-cap C++ oracle tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "v3 formula-cap C++ oracle test failure: "
                  << error.what() << '\n';
        return 1;
    }
}
