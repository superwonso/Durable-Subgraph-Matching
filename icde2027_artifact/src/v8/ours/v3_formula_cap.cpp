#include "v3_formula_cap.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <utility>

namespace dsm::v3 {
namespace {

constexpr std::uint64_t kGiB = UINT64_C(1024) * UINT64_C(1024) * UINT64_C(1024);
constexpr std::uint64_t kFixedHeadroom = UINT64_C(4) * kGiB;
constexpr std::uint64_t kProcessMaximum = UINT64_C(40) * kGiB;
constexpr std::uint64_t kLazyMinimum = UINT64_C(4096);
constexpr std::uint64_t kLazyMaximum = UINT64_C(1) << 24U;

constexpr std::string_view kPolicySchema =
    "icde2027_v3_formula_cap_policy";
constexpr std::string_view kPolicyFormula =
    "tcc_v3_post_filter_checked_u64_caps_v1";
constexpr std::string_view kCandidateFormula =
    "candidate_alpha_b_filtered_edges_plus_root_self_loop_v2";
constexpr std::string_view kLazyFormula =
    "lazy_alpha_l_filtered_edges_clamped_v1";
constexpr std::string_view kGraphFormula =
    "validated_snapshot_exact_structural_quote_v1";
constexpr std::string_view kRescoreFormula =
    "symmetric_fixed_plan_pilot_v1";
constexpr std::string_view kMemoryFormula =
    "existing_plus_candidate_plus_headroom_le_process_v1";
constexpr std::string_view kCoefficientFormula =
    "synthetic_primitive_median_normalized_v1";

[[noreturn]] void fail(const std::string& message) {
    throw V3FormulaCapError(message);
}

void requireKeys(
    const Json& value,
    std::string_view location,
    std::initializer_list<const char*> expected) {
    if (!value.isObject()) {
        fail(std::string(location) + " must be an object");
    }
    const Json::Object& object = value.objectValue();
    if (object.size() != expected.size()) {
        fail(std::string(location) + " field set drift");
    }
    for (const char* key : expected) {
        if (object.find(key) == object.end()) {
            fail(std::string(location) + " field set drift");
        }
    }
}

std::uint64_t requireUint(
    const Json& value,
    std::string_view location,
    bool positive = false) {
    // Json::Boolean and Json::UnsignedInteger are deliberately distinct.
    if (!value.isUnsignedInteger()) {
        fail(std::string(location) + " must be uint64");
    }
    const std::uint64_t result = value.unsignedInteger();
    if (positive && result == 0U) {
        fail(std::string(location) + " must be positive");
    }
    return result;
}

const std::string& requireText(
    const Json& value,
    std::string_view location,
    bool nonempty = false) {
    if (!value.isString()) {
        fail(std::string(location) + " must be text");
    }
    const std::string& result = value.string();
    if (nonempty && result.empty()) {
        fail(std::string(location) + " must be nonempty text");
    }
    return result;
}

void requireTextEquals(
    const Json& value,
    std::string_view expected,
    std::string_view error) {
    const std::string& observed = requireText(value, error);
    if (observed != expected) fail(std::string(error));
}

std::string requireSha(const Json& value, std::string_view location) {
    const std::string& observed = requireText(value, location);
    if (observed.size() != 64U) {
        fail(std::string(location) + " must be lowercase SHA-256");
    }
    for (const char character : observed) {
        const bool digit = character >= '0' && character <= '9';
        const bool lower_hex = character >= 'a' && character <= 'f';
        if (!digit && !lower_hex) {
            fail(std::string(location) + " must be lowercase SHA-256");
        }
    }
    return observed;
}

bool allowedValue(
    std::uint64_t value,
    std::initializer_list<std::uint64_t> allowed) noexcept {
    for (const std::uint64_t candidate : allowed) {
        if (value == candidate) return true;
    }
    return false;
}

V3FormulaCapRatio requireRatio(
    const Json& value,
    std::string_view location,
    std::initializer_list<std::pair<std::uint64_t, std::uint64_t>> allowed) {
    requireKeys(value, location, {"denominator", "numerator"});
    V3FormulaCapRatio ratio;
    ratio.numerator = requireUint(
        value.at("numerator"), std::string(location) + ".numerator", true);
    ratio.denominator = requireUint(
        value.at("denominator"), std::string(location) + ".denominator", true);
    for (const auto& candidate : allowed) {
        if (ratio.numerator == candidate.first &&
            ratio.denominator == candidate.second) {
            return ratio;
        }
    }
    fail(std::string(location) + " is outside the preregistered grid");
}

std::uint64_t checkedAdd(
    std::uint64_t lhs,
    std::uint64_t rhs,
    std::string_view location) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        fail(std::string(location) + " uint64 addition overflow");
    }
    return lhs + rhs;
}

std::uint64_t checkedMultiply(
    std::uint64_t lhs,
    std::uint64_t rhs,
    std::string_view location) {
    if (lhs != 0U &&
        rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        fail(std::string(location) + " uint64 multiplication overflow");
    }
    return lhs * rhs;
}

std::uint64_t checkedCeilRatio(
    std::uint64_t value,
    std::uint64_t numerator,
    std::uint64_t denominator,
    std::string_view location) {
    if (numerator == 0U || denominator == 0U) {
        fail(std::string(location) + " ratio must be positive");
    }
    const std::uint64_t product = checkedMultiply(
        value, numerator, std::string(location) + ".product");
    const std::uint64_t quotient = product / denominator;
    const std::uint64_t remainder = product % denominator;
    return checkedAdd(
        quotient,
        remainder == 0U ? UINT64_C(0) : UINT64_C(1),
        std::string(location) + ".ceil");
}

std::size_t checkedSize(std::uint64_t value, std::string_view location) {
    if (value > static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max())) {
        fail(std::string(location) + " exceeds native size_t");
    }
    return static_cast<std::size_t>(value);
}

Json uintJson(std::uint64_t value) { return Json(value); }
Json textJson(std::string value) { return Json(std::move(value)); }

Json makeDerivationInputs(const V3FormulaCapInputs& inputs) {
    Json::Object result;
    result.emplace("filtered_edges", uintJson(inputs.filtered_edges));
    result.emplace("filtered_vertices", uintJson(inputs.filtered_vertices));
    result.emplace("interval_records", uintJson(inputs.interval_records));
    result.emplace("query_edges", uintJson(inputs.query_edges));
    result.emplace("query_vertices", uintJson(inputs.query_vertices));
    return Json::object(std::move(result));
}

} // namespace

V3FormulaCapPolicy validateV3FormulaCapPolicy(const Json& policy) {
    requireKeys(
        policy,
        "cap_policy",
        {"accounting", "candidate_build", "cost_coefficients", "fixed",
         "formula_id", "graph_validation", "lazy", "planner", "policy_id",
         "rescore", "schema", "schema_version"});
    requireTextEquals(
        policy.at("schema"), kPolicySchema, "cap-policy schema/version drift");
    if (requireUint(policy.at("schema_version"), "cap_policy.schema_version") !=
        1U) {
        fail("cap-policy schema/version drift");
    }
    requireTextEquals(
        policy.at("formula_id"), kPolicyFormula,
        "cap-policy formula ID drift");

    V3FormulaCapPolicy result;
    result.policy_id = requireText(
        policy.at("policy_id"), "cap-policy ID", true);

    const Json& fixed = policy.at("fixed");
    requireKeys(
        fixed,
        "cap_policy.fixed",
        {"candidate_tree_tokens", "fixed_memory_headroom_bytes",
         "metadata_payload_bytes", "metadata_receipt_tokens",
         "process_memory_max_bytes", "rescore_top_l", "sample_seed"});
    result.candidate_tree_tokens = requireUint(
        fixed.at("candidate_tree_tokens"),
        "cap_policy.fixed.candidate_tree_tokens", true);
    result.fixed_memory_headroom_bytes = requireUint(
        fixed.at("fixed_memory_headroom_bytes"),
        "cap_policy.fixed.fixed_memory_headroom_bytes", true);
    result.metadata_payload_bytes = requireUint(
        fixed.at("metadata_payload_bytes"),
        "cap_policy.fixed.metadata_payload_bytes", true);
    result.metadata_receipt_tokens = requireUint(
        fixed.at("metadata_receipt_tokens"),
        "cap_policy.fixed.metadata_receipt_tokens", true);
    result.process_memory_max_bytes = requireUint(
        fixed.at("process_memory_max_bytes"),
        "cap_policy.fixed.process_memory_max_bytes", true);
    result.rescore_top_l = requireUint(
        fixed.at("rescore_top_l"), "cap_policy.fixed.rescore_top_l", true);
    result.sample_seed = requireUint(
        fixed.at("sample_seed"), "cap_policy.fixed.sample_seed", true);
    if (result.candidate_tree_tokens != 1U) {
        fail("cap_policy.fixed.candidate_tree_tokens drift");
    }
    if (result.fixed_memory_headroom_bytes != kFixedHeadroom) {
        fail("cap_policy.fixed.fixed_memory_headroom_bytes drift");
    }
    if (result.metadata_receipt_tokens != 1U) {
        fail("cap_policy.fixed.metadata_receipt_tokens drift");
    }
    if (result.process_memory_max_bytes != kProcessMaximum) {
        fail("cap_policy.fixed.process_memory_max_bytes drift");
    }
    if (result.rescore_top_l != 1U) {
        fail("cap_policy.fixed.rescore_top_l drift");
    }
    if (result.sample_seed != 42U) {
        fail("cap_policy.fixed.sample_seed drift");
    }

    const Json& candidate = policy.at("candidate_build");
    requireKeys(
        candidate,
        "cap_policy.candidate_build",
        {"adjacency_multiplier", "formula_id", "payload_ceiling_bytes"});
    requireTextEquals(
        candidate.at("formula_id"), kCandidateFormula,
        "candidate-build formula ID drift");
    result.candidate_adjacency_multiplier = requireRatio(
        candidate.at("adjacency_multiplier"),
        "cap_policy.candidate_build.adjacency_multiplier",
        {{1U, 4U}, {1U, 2U}, {1U, 1U}, {2U, 1U}});
    result.candidate_payload_ceiling_bytes = requireUint(
        candidate.at("payload_ceiling_bytes"),
        "cap_policy.candidate_build.payload_ceiling_bytes", true);
    if (!allowedValue(
            result.candidate_payload_ceiling_bytes,
            {kGiB, 2U * kGiB, 4U * kGiB, 8U * kGiB})) {
        fail("candidate payload ceiling is outside the grid");
    }

    const Json& planner = policy.at("planner");
    requireKeys(
        planner,
        "cap_policy.planner",
        {"complete_proposal_cap", "prefix_samples_per_plan",
         "state_expansions"});
    result.complete_proposal_cap = requireUint(
        planner.at("complete_proposal_cap"), "planner.complete", true);
    result.proposal_state_expansions = requireUint(
        planner.at("state_expansions"), "planner.states", true);
    result.planner_prefix_samples_per_plan = requireUint(
        planner.at("prefix_samples_per_plan"), "planner.prefix", true);
    if (!allowedValue(result.complete_proposal_cap, {8U, 16U, 32U})) {
        fail("complete proposal cap is outside the grid");
    }
    if (!allowedValue(
            result.proposal_state_expansions, {256U, 1024U, 4096U})) {
        fail("proposal state cap is outside the grid");
    }
    if (!allowedValue(
            result.planner_prefix_samples_per_plan, {16U, 32U, 64U})) {
        fail("planner prefix cap is outside the grid");
    }

    const Json& lazy = policy.at("lazy");
    requireKeys(
        lazy,
        "cap_policy.lazy",
        {"adjacency_fraction", "adjacency_maximum", "adjacency_minimum",
         "cache_entries", "extension_probes", "formula_id",
         "interval_intersections"});
    requireTextEquals(
        lazy.at("formula_id"), kLazyFormula, "lazy formula ID drift");
    result.lazy_adjacency_fraction = requireRatio(
        lazy.at("adjacency_fraction"),
        "cap_policy.lazy.adjacency_fraction",
        {{1U, 256U}, {1U, 128U}, {1U, 64U}, {1U, 32U}});
    result.lazy_adjacency_minimum = requireUint(
        lazy.at("adjacency_minimum"), "lazy.minimum", true);
    result.lazy_adjacency_maximum = requireUint(
        lazy.at("adjacency_maximum"), "lazy.maximum", true);
    result.lazy_cache_entries = requireUint(
        lazy.at("cache_entries"), "lazy.cache_entries", true);
    result.lazy_extension_probes = requireUint(
        lazy.at("extension_probes"), "lazy.extensions", true);
    result.lazy_interval_intersections = requireUint(
        lazy.at("interval_intersections"), "lazy.intervals", true);
    if (result.lazy_adjacency_minimum != kLazyMinimum) {
        fail("lazy adjacency minimum drift");
    }
    if (result.lazy_adjacency_maximum != kLazyMaximum) {
        fail("lazy adjacency maximum drift");
    }
    if (!allowedValue(result.lazy_cache_entries, {512U, 2048U, 8192U})) {
        fail("lazy cache entries are outside the grid");
    }
    if (!allowedValue(
            result.lazy_extension_probes,
            {UINT64_C(1) << 14U, UINT64_C(1) << 16U,
             UINT64_C(1) << 18U})) {
        fail("lazy extension cap is outside the grid");
    }
    if (!allowedValue(
            result.lazy_interval_intersections,
            {UINT64_C(1) << 15U, UINT64_C(1) << 17U,
             UINT64_C(1) << 19U})) {
        fail("lazy interval cap is outside the grid");
    }

    const Json& rescore = policy.at("rescore");
    requireKeys(
        rescore,
        "cap_policy.rescore",
        {"extension_probes", "formula_id", "prefix_samples_per_plan"});
    requireTextEquals(
        rescore.at("formula_id"), kRescoreFormula,
        "rescore formula ID drift");
    result.rescore_prefix_samples_per_plan = requireUint(
        rescore.at("prefix_samples_per_plan"), "rescore.prefix", true);
    result.rescore_extension_probes = requireUint(
        rescore.at("extension_probes"), "rescore.extensions", true);
    if (!allowedValue(
            result.rescore_prefix_samples_per_plan, {16U, 32U, 64U})) {
        fail("rescore prefix is outside the pilot grid");
    }
    if (!allowedValue(
            result.rescore_extension_probes,
            {UINT64_C(1) << 14U, UINT64_C(1) << 16U,
             UINT64_C(1) << 18U})) {
        fail("rescore extension cap is outside the pilot grid");
    }

    const Json& graph = policy.at("graph_validation");
    requireKeys(
        graph,
        "cap_policy.graph_validation",
        {"accounting_model_sha256", "formula_id"});
    requireTextEquals(
        graph.at("formula_id"), kGraphFormula,
        "graph-validation formula ID drift");
    result.graph_accounting_model_sha256 = requireSha(
        graph.at("accounting_model_sha256"), "graph accounting model");

    const Json& accounting = policy.at("accounting");
    requireKeys(
        accounting,
        "cap_policy.accounting",
        {"candidate_value_bytes", "index_entries_per_block",
         "lazy_cache_entry_bytes", "semijoin_visits_per_block",
         "semijoin_visits_per_candidate_value"});
    result.candidate_value_bytes = requireUint(
        accounting.at("candidate_value_bytes"),
        "accounting.candidate_value_bytes", true);
    result.index_entries_per_block = requireUint(
        accounting.at("index_entries_per_block"),
        "accounting.index_entries_per_block", true);
    result.lazy_cache_entry_bytes = requireUint(
        accounting.at("lazy_cache_entry_bytes"),
        "accounting.lazy_cache_entry_bytes", true);
    result.semijoin_visits_per_block = requireUint(
        accounting.at("semijoin_visits_per_block"),
        "accounting.semijoin_visits_per_block", true);
    result.semijoin_visits_per_candidate_value = requireUint(
        accounting.at("semijoin_visits_per_candidate_value"),
        "accounting.semijoin_visits_per_candidate_value", true);
    if (result.candidate_value_bytes != 4U) {
        fail("accounting.candidate_value_bytes drift");
    }
    if (result.index_entries_per_block != 1U) {
        fail("accounting.index_entries_per_block drift");
    }
    if (result.semijoin_visits_per_block != 3U) {
        fail("accounting.semijoin_visits_per_block drift");
    }
    if (result.semijoin_visits_per_candidate_value != 2U) {
        fail("accounting.semijoin_visits_per_candidate_value drift");
    }

    const Json& coefficients = policy.at("cost_coefficients");
    requireKeys(
        coefficients,
        "cap_policy.cost_coefficients",
        {"artifact_sha256", "formula_id", "weights"});
    requireTextEquals(
        coefficients.at("formula_id"), kCoefficientFormula,
        "cost-coefficient formula ID drift");
    result.cost_coefficient_artifact_sha256 = requireSha(
        coefficients.at("artifact_sha256"), "cost coefficient artifact");
    const Json& weights = coefficients.at("weights");
    requireKeys(
        weights,
        "cap_policy.cost_coefficients.weights",
        {"adjacency_probe", "emitted_entry", "index_visit",
         "interval_intersection", "reverse_lookup", "semijoin_visit"});
    result.cost_weights.adjacency_probe = requireUint(
        weights.at("adjacency_probe"),
        "cost coefficient adjacency_probe", true);
    result.cost_weights.emitted_entry = requireUint(
        weights.at("emitted_entry"), "cost coefficient emitted_entry", true);
    result.cost_weights.index_visit = requireUint(
        weights.at("index_visit"), "cost coefficient index_visit", true);
    result.cost_weights.interval_intersection = requireUint(
        weights.at("interval_intersection"),
        "cost coefficient interval_intersection", true);
    result.cost_weights.reverse_lookup = requireUint(
        weights.at("reverse_lookup"),
        "cost coefficient reverse_lookup", true);
    result.cost_weights.semijoin_visit = requireUint(
        weights.at("semijoin_visit"),
        "cost coefficient semijoin_visit", true);
    if (result.cost_weights.adjacency_probe != UINT64_C(1000000)) {
        fail("adjacency coefficient normalization drift");
    }

    try {
        result.canonical_policy = policy;
        result.policy_sha256 = sha256Hex(canonicalJsonBytes(policy));
    } catch (const JsonError& error) {
        fail(std::string("cap_policy is not canonical-JSON representable: ") +
             error.what());
    }
    return result;
}

V3FormulaCapInputs parseV3FormulaCapInputs(const Json& request) {
    requireKeys(
        request,
        "materialization_request",
        {"filtered_edges", "filtered_vertices", "graph_storage_quote",
         "interval_records", "query_edges", "query_vertices"});
    V3FormulaCapInputs result;
    result.filtered_vertices = requireUint(
        request.at("filtered_vertices"), "filtered_vertices");
    result.filtered_edges = requireUint(
        request.at("filtered_edges"), "filtered_edges");
    result.interval_records = requireUint(
        request.at("interval_records"), "interval_records");
    result.query_vertices = requireUint(
        request.at("query_vertices"), "query_vertices", true);
    result.query_edges = requireUint(
        request.at("query_edges"), "query_edges", true);
    if (result.query_vertices < 2U) {
        fail("query must contain at least two vertices");
    }

    const Json& quote = request.at("graph_storage_quote");
    requireKeys(
        quote,
        "graph_storage_quote",
        {"accounting_model_sha256", "seal_peak_live_bytes",
         "snapshot_storage_bytes", "temporary_validation_bytes"});
    result.graph_storage_quote.accounting_model_sha256 = requireSha(
        quote.at("accounting_model_sha256"),
        "graph_storage_quote.accounting_model_sha256");
    result.graph_storage_quote.seal_peak_live_bytes = requireUint(
        quote.at("seal_peak_live_bytes"),
        "graph_storage_quote.seal_peak_live_bytes", true);
    result.graph_storage_quote.snapshot_storage_bytes = requireUint(
        quote.at("snapshot_storage_bytes"),
        "graph_storage_quote.snapshot_storage_bytes", true);
    result.graph_storage_quote.temporary_validation_bytes = requireUint(
        quote.at("temporary_validation_bytes"),
        "graph_storage_quote.temporary_validation_bytes");
    return result;
}

V3FormulaCapMaterialization materializeV3FormulaCaps(
    const V3FormulaCapPolicy& policy,
    const V3FormulaCapInputs& inputs) {
    // canonical_policy is the sole authority. Revalidate it so a caller cannot
    // mutate a public typed field and create a second policy source.
    const V3FormulaCapPolicy selected =
        validateV3FormulaCapPolicy(policy.canonical_policy);
    if (policy.policy_sha256 != selected.policy_sha256) {
        fail("typed cap-policy hash drift");
    }
    if (inputs.query_vertices < 2U) {
        fail("query must contain at least two vertices");
    }
    if (inputs.query_edges == 0U) {
        fail("query_edges must be positive");
    }
    if (inputs.graph_storage_quote.accounting_model_sha256 !=
        selected.graph_accounting_model_sha256) {
        fail("graph storage quote accounting-model hash drift");
    }
    if (inputs.graph_storage_quote.snapshot_storage_bytes == 0U) {
        fail("graph_storage_quote.snapshot_storage_bytes must be positive");
    }
    if (inputs.graph_storage_quote.seal_peak_live_bytes == 0U) {
        fail("graph_storage_quote.seal_peak_live_bytes must be positive");
    }

    const std::uint64_t n_f = inputs.filtered_vertices;
    const std::uint64_t m_f = inputs.filtered_edges;
    const std::uint64_t interval_count = inputs.interval_records;
    const std::uint64_t q = inputs.query_vertices;
    const std::uint64_t e_q = inputs.query_edges;

    const std::uint64_t candidate_adjacency = checkedCeilRatio(
        m_f,
        selected.candidate_adjacency_multiplier.numerator,
        selected.candidate_adjacency_multiplier.denominator,
        "candidate adjacency");
    const std::uint64_t candidate_reverse = candidate_adjacency;
    const std::uint64_t candidate_pair_arc = checkedAdd(
        candidate_adjacency, candidate_reverse, "candidate pair arc");
    const std::uint64_t candidate_arc = checkedAdd(
        candidate_pair_arc, n_f, "candidate root-self-loop arc");
    const std::uint64_t candidate_relation = candidate_adjacency;
    const std::uint64_t candidate_occurrences = checkedAdd(
        n_f, candidate_relation, "candidate occurrences");
    const std::uint64_t candidate_blocks = checkedMultiply(
        q - 1U, n_f, "candidate blocks");
    const std::uint64_t candidate_semijoin = checkedAdd(
        checkedMultiply(
            candidate_occurrences,
            selected.semijoin_visits_per_candidate_value,
            "candidate semijoin values"),
        checkedMultiply(
            candidate_blocks,
            selected.semijoin_visits_per_block,
            "candidate semijoin blocks"),
        "candidate semijoin total");
    const std::uint64_t candidate_index = checkedMultiply(
        candidate_blocks,
        selected.index_entries_per_block,
        "candidate index entries");
    const std::uint64_t payload_ceiling =
        selected.candidate_payload_ceiling_bytes;
    const std::uint64_t headroom = selected.fixed_memory_headroom_bytes;
    const std::uint64_t reservation_envelope = checkedAdd(
        payload_ceiling, headroom, "candidate envelope");

    const std::uint64_t quote_cover_visits = checkedMultiply(
        checkedMultiply(
            selected.complete_proposal_cap,
            q - 1U,
            "quote cover plans/depths"),
        n_f,
        "quote cover visits");

    const std::uint64_t lazy_fraction = checkedCeilRatio(
        m_f,
        selected.lazy_adjacency_fraction.numerator,
        selected.lazy_adjacency_fraction.denominator,
        "lazy adjacency");
    const std::uint64_t lazy_adjacency = std::max(
        selected.lazy_adjacency_minimum,
        std::min(lazy_fraction, selected.lazy_adjacency_maximum));
    const std::uint64_t lazy_reverse = lazy_adjacency;
    const std::uint64_t lazy_arc = checkedAdd(
        lazy_adjacency, lazy_reverse, "lazy arc");
    const std::uint64_t lazy_block_requests = checkedMultiply(
        checkedMultiply(
            selected.complete_proposal_cap,
            selected.planner_prefix_samples_per_plan,
            "lazy plans/prefix"),
        q,
        "lazy block requests");
    const std::uint64_t cache_entry_bytes = checkedMultiply(
        selected.lazy_cache_entries,
        selected.lazy_cache_entry_bytes,
        "lazy cache entry bytes");
    const std::uint64_t cache_value_bytes = checkedMultiply(
        lazy_adjacency,
        selected.candidate_value_bytes,
        "lazy cache value bytes");
    const std::uint64_t lazy_cache_bytes = checkedAdd(
        cache_entry_bytes, cache_value_bytes, "lazy cache bytes");

    const std::uint64_t rescore_blocks = checkedMultiply(
        selected.rescore_prefix_samples_per_plan,
        q,
        "rescore block requests");
    const std::uint64_t rescore_intervals = checkedMultiply(
        selected.rescore_extension_probes,
        e_q,
        "rescore interval intersections");

    const std::uint64_t combined_edges = checkedAdd(
        m_f, e_q, "combined graph/query adjacency");
    const std::uint64_t derived_intervals = checkedMultiply(
        interval_count, 2U, "derived incident intervals");
    const std::uint64_t derived_labels = checkedMultiply(
        m_f, 2U, "derived label updates");

    Json::Object candidate_json;
    candidate_json.emplace(
        "adjacency_probes", uintJson(candidate_adjacency));
    candidate_json.emplace("arc_checks", uintJson(candidate_arc));
    candidate_json.emplace("blocks", uintJson(candidate_blocks));
    candidate_json.emplace(
        "candidate_tree_tokens", uintJson(selected.candidate_tree_tokens));
    candidate_json.emplace(
        "candidate_value_occurrences", uintJson(candidate_occurrences));
    candidate_json.emplace(
        "fixed_memory_headroom_bytes", uintJson(headroom));
    candidate_json.emplace("index_entries", uintJson(candidate_index));
    candidate_json.emplace(
        "payload_ceiling_bytes", uintJson(payload_ceiling));
    candidate_json.emplace(
        "payload_reservation_envelope_bytes",
        uintJson(reservation_envelope));
    candidate_json.emplace(
        "relation_entries", uintJson(candidate_relation));
    candidate_json.emplace(
        "reverse_edge_lookups", uintJson(candidate_reverse));
    candidate_json.emplace(
        "semijoin_entry_visits", uintJson(candidate_semijoin));

    Json::Object graph_json;
    graph_json.emplace(
        "accounting_model_sha256",
        textJson(inputs.graph_storage_quote.accounting_model_sha256));
    graph_json.emplace(
        "combined_incoming_adjacency_entries", uintJson(combined_edges));
    graph_json.emplace(
        "combined_outgoing_adjacency_entries", uintJson(combined_edges));
    graph_json.emplace("data_vertices", uintJson(n_f));
    graph_json.emplace(
        "derived_incident_interval_visits", uintJson(derived_intervals));
    graph_json.emplace(
        "derived_neighbor_label_updates", uintJson(derived_labels));
    graph_json.emplace("interval_records", uintJson(interval_count));
    graph_json.emplace("query_vertices", uintJson(q));
    graph_json.emplace(
        "seal_peak_live_bytes",
        uintJson(inputs.graph_storage_quote.seal_peak_live_bytes));
    graph_json.emplace(
        "snapshot_storage_bytes",
        uintJson(inputs.graph_storage_quote.snapshot_storage_bytes));
    graph_json.emplace("temporal_edges", uintJson(m_f));
    graph_json.emplace(
        "temporary_validation_bytes",
        uintJson(inputs.graph_storage_quote.temporary_validation_bytes));

    Json::Object lazy_json;
    lazy_json.emplace("adjacency_probes", uintJson(lazy_adjacency));
    lazy_json.emplace("arc_checks", uintJson(lazy_arc));
    lazy_json.emplace("block_requests", uintJson(lazy_block_requests));
    lazy_json.emplace("cache_capacity_bytes", uintJson(lazy_cache_bytes));
    lazy_json.emplace(
        "cache_entries", uintJson(selected.lazy_cache_entries));
    lazy_json.emplace("emitted_entries", uintJson(lazy_adjacency));
    lazy_json.emplace(
        "extension_probes", uintJson(selected.lazy_extension_probes));
    lazy_json.emplace(
        "interval_intersections",
        uintJson(selected.lazy_interval_intersections));
    lazy_json.emplace("reverse_edge_lookups", uintJson(lazy_reverse));

    Json::Object memory_json;
    memory_json.emplace(
        "fixed_memory_headroom_bytes", uintJson(headroom));
    memory_json.emplace("formula_id", textJson(std::string(kMemoryFormula)));
    memory_json.emplace("payload_ceiling_bytes", uintJson(payload_ceiling));
    memory_json.emplace(
        "payload_reservation_envelope_bytes",
        uintJson(reservation_envelope));
    memory_json.emplace(
        "process_memory_max_bytes",
        uintJson(selected.process_memory_max_bytes));

    Json::Object metadata_json;
    metadata_json.emplace(
        "payload_bytes", uintJson(selected.metadata_payload_bytes));
    metadata_json.emplace(
        "receipt_tokens", uintJson(selected.metadata_receipt_tokens));

    Json::Object planner_json;
    planner_json.emplace(
        "complete_proposal_cap",
        uintJson(selected.complete_proposal_cap));
    planner_json.emplace(
        "prefix_samples_per_plan",
        uintJson(selected.planner_prefix_samples_per_plan));
    planner_json.emplace(
        "proposal_state_expansions",
        uintJson(selected.proposal_state_expansions));
    planner_json.emplace(
        "quote_cover_value_visits", uintJson(quote_cover_visits));

    Json::Object rescore_json;
    rescore_json.emplace(
        "block_requests_per_side", uintJson(rescore_blocks));
    rescore_json.emplace(
        "extension_probes_per_side",
        uintJson(selected.rescore_extension_probes));
    rescore_json.emplace(
        "interval_intersections_per_side", uintJson(rescore_intervals));
    rescore_json.emplace(
        "prefix_samples_per_plan",
        uintJson(selected.rescore_prefix_samples_per_plan));

    Json::Object root;
    root.emplace(
        "candidate_build", Json::object(std::move(candidate_json)));
    root.emplace("derivation_inputs", makeDerivationInputs(inputs));
    root.emplace("formula_id", textJson(std::string(kPolicyFormula)));
    root.emplace("graph_validation", Json::object(std::move(graph_json)));
    root.emplace("lazy", Json::object(std::move(lazy_json)));
    root.emplace("memory_admission", Json::object(std::move(memory_json)));
    root.emplace("metadata", Json::object(std::move(metadata_json)));
    root.emplace("planner", Json::object(std::move(planner_json)));
    root.emplace("policy_sha256", textJson(selected.policy_sha256));
    root.emplace("rescore", Json::object(std::move(rescore_json)));
    root.emplace(
        "schema", textJson("icde2027_v3_materialized_caps"));
    root.emplace("schema_version", uintJson(1U));

    V3FormulaCapMaterialization result;
    result.value = Json::object(std::move(root));
    result.canonical_bytes = canonicalJsonBytes(result.value);
    result.materialized_caps_sha256 = sha256Hex(result.canonical_bytes);

    GraphValidationBudget& graph_budget =
        result.projection.graph_validation_budget;
    graph_budget.data_vertices = n_f;
    graph_budget.query_vertices = q;
    graph_budget.combined_outgoing_adjacency_entries = combined_edges;
    graph_budget.combined_incoming_adjacency_entries = combined_edges;
    graph_budget.temporal_edges = m_f;
    graph_budget.interval_records = interval_count;
    graph_budget.derived_incident_interval_visits = derived_intervals;
    graph_budget.derived_neighbor_label_updates = derived_labels;
    graph_budget.snapshot_storage_bytes =
        inputs.graph_storage_quote.snapshot_storage_bytes;
    graph_budget.temporary_validation_bytes =
        inputs.graph_storage_quote.temporary_validation_bytes;
    graph_budget.seal_peak_live_bytes =
        inputs.graph_storage_quote.seal_peak_live_bytes;

    CandidateBuildBudget& candidate_budget =
        result.projection.candidate_build_budget;
    candidate_budget.adjacency_probes = candidate_adjacency;
    candidate_budget.reverse_edge_lookups = candidate_reverse;
    candidate_budget.arc_checks = candidate_arc;
    candidate_budget.relation_entries = candidate_relation;
    candidate_budget.candidate_value_occurrences = candidate_occurrences;
    candidate_budget.blocks = candidate_blocks;
    candidate_budget.semijoin_entry_visits = candidate_semijoin;
    candidate_budget.index_entries = candidate_index;
    candidate_budget.payload_memory_bytes = payload_ceiling;
    candidate_budget.fixed_memory_headroom_bytes = headroom;
    candidate_budget.candidate_tree_tokens = selected.candidate_tree_tokens;
    result.projection.candidate_payload_ceiling_bytes = payload_ceiling;
    result.projection.candidate_payload_reservation_envelope_bytes =
        reservation_envelope;

    result.projection.proposal_budget.maximum_states_expanded =
        selected.proposal_state_expansions;
    result.projection.proposal_budget.maximum_complete_plans =
        selected.complete_proposal_cap;
    result.projection.proposal_budget.maximum_quote_cover_value_visits =
        quote_cover_visits;
    result.projection.planner_prefix_samples_per_plan =
        selected.planner_prefix_samples_per_plan;

    result.projection.lazy_budget.maximum_block_requests =
        lazy_block_requests;
    result.projection.lazy_budget.maximum_adjacency_probes = lazy_adjacency;
    result.projection.lazy_budget.maximum_reverse_lookups = lazy_reverse;
    result.projection.lazy_budget.maximum_arc_checks = lazy_arc;
    // The current native name is relation_entries; the formula schema calls
    // the same emitted lazy values emitted_entries.
    result.projection.lazy_budget.maximum_relation_entries = lazy_adjacency;
    result.projection.lazy_budget.maximum_cache_entries =
        selected.lazy_cache_entries;
    result.projection.lazy_budget.maximum_cache_capacity_bytes =
        lazy_cache_bytes;
    result.projection.lazy_budget.maximum_extension_probes =
        selected.lazy_extension_probes;
    result.projection.lazy_budget.maximum_interval_intersections =
        selected.lazy_interval_intersections;
    result.projection.lazy_budget.prefix_sample_cap = checkedSize(
        selected.planner_prefix_samples_per_plan,
        "planner prefix_samples_per_plan");
    result.projection.lazy_emitted_entries = lazy_adjacency;

    result.projection.rescore_budget.maximum_block_requests = rescore_blocks;
    result.projection.rescore_budget.maximum_extension_probes =
        selected.rescore_extension_probes;
    result.projection.rescore_budget.maximum_interval_intersections =
        rescore_intervals;
    result.projection.rescore_budget.prefix_sample_cap = checkedSize(
        selected.rescore_prefix_samples_per_plan,
        "rescore prefix_samples_per_plan");

    result.projection.metadata_budget.payload_bytes =
        selected.metadata_payload_bytes;
    result.projection.metadata_budget.receipt_tokens =
        selected.metadata_receipt_tokens;
    result.projection.fixed_memory_headroom_bytes = headroom;
    result.projection.process_memory_max_bytes =
        selected.process_memory_max_bytes;
    result.projection.payload_ceiling_bytes = payload_ceiling;
    result.projection.payload_reservation_envelope_bytes =
        reservation_envelope;
    return result;
}

V3FormulaCapMaterialization materializeV3FormulaCaps(
    const Json& policy,
    const Json& request) {
    return materializeV3FormulaCaps(
        validateV3FormulaCapPolicy(policy),
        parseV3FormulaCapInputs(request));
}

V3FormulaCapMaterialization verifyV3FormulaCapMaterialization(
    const Json& materialized,
    const Json& policy,
    const Json& request) {
    V3FormulaCapMaterialization expected =
        materializeV3FormulaCaps(policy, request);
    if (materialized != expected.value) {
        fail("materialized caps differ from independent global-policy derivation");
    }
    return expected;
}

} // namespace dsm::v3
