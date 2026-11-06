#include "v3_existing_capacity.h"

#include <v3_build_identity_generated.h>
#include "v3_canonical_json.h"

#include <array>
#include <initializer_list>
#include <limits>
#include <new>
#include <utility>

namespace {

using dsm::v3::Json;
using dsm::v3::canonicalJsonBytes;
using dsm::v3::sha256Hex;

constexpr std::uint64_t kFnvOffset = UINT64_C(14695981039346656037);
constexpr std::uint64_t kFnvPrime = UINT64_C(1099511628211);
constexpr const char* kExistingModelSchema =
    "icde2027_v3_existing_capacity_accounting_model";
constexpr std::uint64_t kExistingModelVersion = 1;

bool sizeToU64(std::size_t value, std::uint64_t& out) noexcept {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (value > static_cast<std::size_t>(
                std::numeric_limits<std::uint64_t>::max())) {
            return false;
        }
    }
    out = static_cast<std::uint64_t>(value);
    return true;
}

bool checkedAdd(
    std::uint64_t lhs,
    std::uint64_t rhs,
    std::uint64_t& out) noexcept {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) return false;
    out = lhs + rhs;
    return true;
}

bool checkedAccumulate(
    std::uint64_t& total,
    std::uint64_t value) noexcept {
    return checkedAdd(total, value, total);
}

bool checkedMultiply(
    std::uint64_t count,
    std::uint64_t width,
    std::uint64_t& out) noexcept {
    if (count != 0 &&
        width > std::numeric_limits<std::uint64_t>::max() / count) {
        return false;
    }
    out = count * width;
    return true;
}

#ifdef DSM_TCC_V3_TESTING
bool checkedAddProduct(
    std::uint64_t initial,
    std::uint64_t count,
    std::uint64_t width,
    std::uint64_t& out) noexcept {
    std::uint64_t bytes = 0;
    return checkedMultiply(count, width, bytes) &&
        checkedAdd(initial, bytes, out);
}
#endif

void appendU64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        hash ^= (value >> shift) & UINT64_C(0xff);
        hash *= kFnvPrime;
    }
}

void appendText(std::uint64_t& hash, const std::string& text) noexcept {
    appendU64(hash, static_cast<std::uint64_t>(text.size()));
    for (const unsigned char byte : text) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= kFnvPrime;
    }
}

std::uint64_t nonzeroFingerprint(std::uint64_t value) noexcept {
    return value == 0 ? UINT64_C(0x9e3779b97f4a7c15) : value;
}

bool lowercaseSha256(const std::string& value) noexcept {
    if (value.size() != 64U) return false;
    for (const char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

Json u64(std::uint64_t value) { return Json(value); }
Json boolean(bool value) { return Json(value); }
Json text(const std::string& value) { return Json(value); }

Json strings(std::initializer_list<const char*> values) {
    Json::Array result;
    result.reserve(values.size());
    for (const char* value : values) result.emplace_back(std::string(value));
    return Json::array(std::move(result));
}

Json fixedPlanJson(const FixedPlanCapacityReceiptV1& r) {
    Json::Object o;
#define FIELD(name) o.emplace(#name, u64(r.name))
    o.emplace("complete", boolean(r.complete));
    o.emplace("overflowed", boolean(r.overflowed));
    FIELD(object_bytes); FIELD(order_count); FIELD(order_capacity);
    FIELD(order_bytes); FIELD(parent_count); FIELD(parent_capacity);
    FIELD(parent_bytes); FIELD(dynamic_bytes); FIELD(total_bytes);
    FIELD(query_fingerprint); FIELD(plan_fingerprint);
    FIELD(ownership_fingerprint);
#undef FIELD
    return Json::object(std::move(o));
}

Json planContainerJson(const PlanContainerCapacityDetailV1& r) {
    Json::Object o;
#define FIELD(name) o.emplace(#name, u64(r.name))
    o.emplace("complete", boolean(r.complete));
    o.emplace("overflowed", boolean(r.overflowed));
    FIELD(container_object_bytes); FIELD(element_count);
    FIELD(element_capacity); FIELD(element_width_bytes);
    FIELD(outer_elements_bytes); FIELD(plan_count); FIELD(plan_order_count);
    FIELD(plan_order_capacity); FIELD(plan_order_bytes);
    FIELD(plan_parent_count); FIELD(plan_parent_capacity);
    FIELD(plan_parent_bytes); FIELD(score_row_count);
    FIELD(score_row_capacity); FIELD(score_row_width_bytes);
    FIELD(score_row_bytes); FIELD(selected_plan_fingerprint);
    FIELD(dynamic_bytes); FIELD(total_bytes); FIELD(ownership_fingerprint);
#undef FIELD
    return Json::object(std::move(o));
}

Json releasedPlansJson(const ReleasedPlanContainerCapacityReceiptV1& r) {
    Json::Object o;
    o.emplace("complete", boolean(r.complete));
    o.emplace("current_owned_bytes", u64(r.current_owned_bytes));
    o.emplace("last_live_capacity", planContainerJson(r.last_live_capacity));
    o.emplace("overflowed", boolean(r.overflowed));
    o.emplace(
        "released_before_existing_capture",
        boolean(r.released_before_existing_capture));
    o.emplace("release_fingerprint", u64(r.release_fingerprint));
    return Json::object(std::move(o));
}

Json lazyDetailJson(const LazyCacheCapacityDetailV1& r) {
    Json::Object o;
#define FIELD(name) o.emplace(#name, u64(r.name))
    o.emplace("complete", boolean(r.complete));
    o.emplace("overflowed", boolean(r.overflowed));
    FIELD(container_object_bytes); FIELD(entry_count); FIELD(entry_capacity);
    FIELD(entry_width_bytes); FIELD(outer_entries_bytes);
    FIELD(nested_value_count); FIELD(nested_value_capacity);
    FIELD(nested_value_width_bytes); FIELD(nested_value_bytes);
    FIELD(dynamic_bytes); FIELD(total_bytes); FIELD(ownership_fingerprint);
#undef FIELD
    return Json::object(std::move(o));
}

Json releasedLazyJson(const ReleasedLazyCacheCapacityReceiptV1& r) {
    Json::Object o;
    o.emplace("complete", boolean(r.complete));
    o.emplace("current_owned_bytes", u64(r.current_owned_bytes));
    o.emplace("last_live_capacity", lazyDetailJson(r.last_live_capacity));
    o.emplace("overflowed", boolean(r.overflowed));
    o.emplace(
        "released_before_existing_capture",
        boolean(r.released_before_existing_capture));
    o.emplace("release_fingerprint", u64(r.release_fingerprint));
    return Json::object(std::move(o));
}

Json traceJson(const PlannerTraceCapacityReceiptV1& r) {
    Json::Object o;
#define FIELD(name) o.emplace(#name, u64(r.name))
    o.emplace("complete", boolean(r.complete));
    o.emplace("overflowed", boolean(r.overflowed));
    o.emplace(
        "frozen_before_reservation",
        boolean(r.frozen_before_reservation));
    FIELD(object_bytes); FIELD(entry_count); FIELD(entry_capacity);
    FIELD(entry_width_bytes); FIELD(dynamic_bytes); FIELD(total_bytes);
    FIELD(content_fingerprint); FIELD(ownership_fingerprint);
#undef FIELD
    return Json::object(std::move(o));
}

Json sourceJson(const SourceGraphReleaseReceiptV1& r) {
    Json::Object o;
#define FIELD(name) o.emplace(#name, u64(r.name))
    o.emplace("complete", boolean(r.complete));
    o.emplace("overflowed", boolean(r.overflowed));
    o.emplace(
        "released_before_incumbent", boolean(r.released_before_incumbent));
    o.emplace("owner_id", text(r.owner_id));
    FIELD(vertex_count); FIELD(adjacency_outer_capacity);
    FIELD(adjacency_outer_bytes); FIELD(adjacency_nested_capacity);
    FIELD(adjacency_nested_bytes); FIELD(incoming_outer_capacity);
    FIELD(incoming_outer_bytes); FIELD(incoming_nested_capacity);
    FIELD(incoming_nested_bytes); FIELD(vertex_labels_capacity);
    FIELD(vertex_labels_bytes); FIELD(external_ids_capacity);
    FIELD(external_ids_bytes); FIELD(temporal_edges_capacity);
    FIELD(temporal_edges_bytes); FIELD(temporal_intervals_capacity);
    FIELD(temporal_intervals_bytes); FIELD(vertex_active_durations_capacity);
    FIELD(vertex_active_durations_bytes); FIELD(neighbor_label_counts_capacity);
    FIELD(neighbor_label_counts_bytes);
    FIELD(incoming_neighbor_label_counts_capacity);
    FIELD(incoming_neighbor_label_counts_bytes); FIELD(total_owned_bytes);
    FIELD(release_fingerprint);
#undef FIELD
    return Json::object(std::move(o));
}

Json snapshotJson(const SnapshotCapacityComponentReceiptV1& r) {
    Json::Object o;
    o.emplace("complete", boolean(r.complete));
    o.emplace("data_fingerprint", u64(r.data_fingerprint));
    o.emplace(
        "expected_quote_sha256", text(r.expected_quote_sha256));
    o.emplace(
        "graph_accounting_model_sha256",
        text(r.graph_accounting_model_sha256));
    o.emplace("overflowed", boolean(r.overflowed));
    o.emplace("ownership_fingerprint", u64(r.ownership_fingerprint));
    o.emplace(
        "query_fingerprint", u64(r.query_fingerprint));
    o.emplace(
        "quoted_snapshot_storage_bytes",
        u64(r.quoted_snapshot_storage_bytes));
    o.emplace(
        "sealed_snapshot_storage_bytes",
        u64(r.sealed_snapshot_storage_bytes));
    o.emplace("snapshot_token", u64(r.snapshot_token));
    o.emplace("total_bytes", u64(r.total_bytes));
    return Json::object(std::move(o));
}

Json tdNativeJson(const TDTreeCapacityReceipt& r) {
    Json::Object o;
#define FIELD(name) o.emplace(#name, u64(r.name))
    o.emplace("complete", boolean(r.complete));
    o.emplace("overflowed", boolean(r.overflowed));
    FIELD(object_bytes); FIELD(node_count); FIELD(node_capacity);
    FIELD(nodes_bytes); FIELD(root_candidate_count);
    FIELD(root_candidate_capacity); FIELD(root_candidates_bytes);
    FIELD(block_count); FIELD(block_capacity); FIELD(blocks_bytes);
    FIELD(block_candidate_count); FIELD(block_candidate_capacity);
    FIELD(block_candidates_bytes); FIELD(query_out_requirements_count);
    FIELD(query_out_requirements_capacity); FIELD(query_out_requirements_bytes);
    FIELD(query_in_requirements_count); FIELD(query_in_requirements_capacity);
    FIELD(query_in_requirements_bytes);
    FIELD(decomposition_spanning_outer_count);
    FIELD(decomposition_spanning_outer_capacity);
    FIELD(decomposition_spanning_outer_bytes);
    FIELD(decomposition_spanning_nested_count);
    FIELD(decomposition_spanning_nested_capacity);
    FIELD(decomposition_spanning_nested_bytes);
    FIELD(decomposition_non_tree_edges_count);
    FIELD(decomposition_non_tree_edges_capacity);
    FIELD(decomposition_non_tree_edges_bytes);
    FIELD(decomposition_vertex_labels_count);
    FIELD(decomposition_vertex_labels_capacity);
    FIELD(decomposition_vertex_labels_bytes); FIELD(decomposition_parent_count);
    FIELD(decomposition_parent_capacity); FIELD(decomposition_parent_bytes);
    FIELD(decomposition_dfs_order_count); FIELD(decomposition_dfs_order_capacity);
    FIELD(decomposition_dfs_order_bytes);
    FIELD(decomposition_temporal_selectivity_count);
    FIELD(decomposition_temporal_selectivity_capacity);
    FIELD(decomposition_temporal_selectivity_bytes);
    FIELD(decomposition_structural_candidate_counts_count);
    FIELD(decomposition_structural_candidate_counts_capacity);
    FIELD(decomposition_structural_candidate_counts_bytes);
    FIELD(decomposition_compatible_edge_counts_count);
    FIELD(decomposition_compatible_edge_counts_capacity);
    FIELD(decomposition_compatible_edge_counts_bytes);
    FIELD(decomposition_durable_compatible_edge_counts_count);
    FIELD(decomposition_durable_compatible_edge_counts_capacity);
    FIELD(decomposition_durable_compatible_edge_counts_bytes);
    FIELD(decomposition_durability_survival_rates_count);
    FIELD(decomposition_durability_survival_rates_capacity);
    FIELD(decomposition_durability_survival_rates_bytes);
    FIELD(query_decomposition_bytes); FIELD(dynamic_bytes); FIELD(total_bytes);
#undef FIELD
    return Json::object(std::move(o));
}

Json tdComponentJson(const TDTreeCapacityComponentReceiptV1& r) {
    Json::Object o;
    o.emplace("complete", boolean(r.complete));
    o.emplace("native", tdNativeJson(r.native));
    o.emplace("overflowed", boolean(r.overflowed));
    o.emplace("ownership_fingerprint", u64(r.ownership_fingerprint));
    o.emplace("plan_fingerprint", u64(r.plan_fingerprint));
    o.emplace("snapshot_token", u64(r.snapshot_token));
    o.emplace(
        "sorted_flat_blocks_only", boolean(r.sorted_flat_blocks_only));
    o.emplace(
        "tree_content_fingerprint", u64(r.tree_content_fingerprint));
    o.emplace("total_bytes", u64(r.total_bytes));
    return Json::object(std::move(o));
}

Json coverNativeJson(const CandidateCoverCapacityReceipt& r) {
    Json::Object o;
#define FIELD(name) o.emplace(#name, u64(r.name))
    o.emplace("complete", boolean(r.complete));
    o.emplace("overflowed", boolean(r.overflowed));
    FIELD(object_bytes); FIELD(domain_count); FIELD(domain_capacity);
    FIELD(outer_domains_bytes); FIELD(value_count); FIELD(value_capacity);
    FIELD(nested_values_bytes); FIELD(dynamic_bytes); FIELD(total_bytes);
#undef FIELD
    return Json::object(std::move(o));
}

Json coverComponentJson(const CandidateCoverCapacityComponentReceiptV1& r) {
    Json::Object o;
    o.emplace("complete", boolean(r.complete));
    o.emplace("cover_fingerprint", u64(r.cover_fingerprint));
    o.emplace(
        "incumbent_plan_fingerprint", u64(r.incumbent_plan_fingerprint));
    o.emplace("native", coverNativeJson(r.native));
    o.emplace("overflowed", boolean(r.overflowed));
    o.emplace("ownership_fingerprint", u64(r.ownership_fingerprint));
    o.emplace("snapshot_token", u64(r.snapshot_token));
    o.emplace("total_bytes", u64(r.total_bytes));
    return Json::object(std::move(o));
}

std::uint64_t fixedPlanFingerprintForReceipt(
    const FixedPlanCapacityReceiptV1& r) noexcept {
    std::uint64_t hash = kFnvOffset;
    appendText(hash, "fixed_plan_capacity_v1");
    appendU64(hash, r.object_bytes); appendU64(hash, r.order_count);
    appendU64(hash, r.order_capacity); appendU64(hash, r.order_bytes);
    appendU64(hash, r.parent_count); appendU64(hash, r.parent_capacity);
    appendU64(hash, r.parent_bytes); appendU64(hash, r.query_fingerprint);
    appendU64(hash, r.plan_fingerprint);
    return nonzeroFingerprint(hash);
}

std::uint64_t planContainerFingerprint(
    const PlanContainerCapacityDetailV1& r) noexcept {
    std::uint64_t hash = kFnvOffset;
    appendText(hash, "plan_container_capacity_v1");
    appendU64(hash, r.container_object_bytes); appendU64(hash, r.element_count);
    appendU64(hash, r.element_capacity); appendU64(hash, r.element_width_bytes);
    appendU64(hash, r.plan_count); appendU64(hash, r.plan_order_count);
    appendU64(hash, r.plan_order_capacity); appendU64(hash, r.plan_parent_count);
    appendU64(hash, r.plan_parent_capacity); appendU64(hash, r.score_row_count);
    appendU64(hash, r.score_row_capacity); appendU64(hash, r.score_row_width_bytes);
    appendU64(hash, r.selected_plan_fingerprint);
    return nonzeroFingerprint(hash);
}

std::uint64_t lazyFingerprint(const LazyCacheCapacityDetailV1& r) noexcept {
    std::uint64_t hash = kFnvOffset;
    appendText(hash, "lazy_cache_capacity_v1");
    appendU64(hash, r.container_object_bytes); appendU64(hash, r.entry_count);
    appendU64(hash, r.entry_capacity); appendU64(hash, r.entry_width_bytes);
    appendU64(hash, r.nested_value_count);
    appendU64(hash, r.nested_value_capacity);
    appendU64(hash, r.nested_value_width_bytes);
    return nonzeroFingerprint(hash);
}

std::uint64_t traceContentFingerprint(
    const std::vector<std::uint64_t>& trace) noexcept {
    std::uint64_t hash = kFnvOffset;
    appendText(hash, "planner_trace_content_v1");
    appendU64(hash, static_cast<std::uint64_t>(trace.size()));
    for (const std::uint64_t value : trace) appendU64(hash, value);
    return nonzeroFingerprint(hash);
}

Json existingModelJson() {
    Json::Object widths;
    widths.emplace("candidate_cover", u64(sizeof(CandidateCover)));
    widths.emplace("edge", u64(sizeof(Edge)));
    widths.emplace("fixed_plan", u64(sizeof(FixedPlan)));
    widths.emplace("int", u64(sizeof(int)));
    widths.emplace("label", u64(sizeof(Label)));
    widths.emplace("lazy_cache_entry", u64(sizeof(V3LazyCacheEntryStorageV1)));
    widths.emplace(
        "neighbor_label_array", u64(sizeof(std::array<int, kLabelCount>)));
    widths.emplace("selected_candidate_carrier", u64(sizeof(V3SelectedCandidateCarrierV1)));
    widths.emplace("tdtree", u64(sizeof(TDTree)));
    widths.emplace("tdtree_block", u64(sizeof(TDTreeBlock)));
    widths.emplace("tdtree_node", u64(sizeof(TDTreeNode)));
    widths.emplace("temporal_edge", u64(sizeof(TemporalEdge)));
    widths.emplace("time_interval", u64(sizeof(TimeInterval)));
    widths.emplace("uint64", u64(sizeof(std::uint64_t)));
    widths.emplace(
        "weighted_score_row", u64(sizeof(V3RetainedPlanScoreRowV1)));
    widths.emplace("vector_edge", u64(sizeof(std::vector<Edge>)));
    widths.emplace("vector_fixed_plan", u64(sizeof(std::vector<FixedPlan>)));
    widths.emplace("vector_int", u64(sizeof(std::vector<int>)));
    widths.emplace("vector_uint64", u64(sizeof(std::vector<std::uint64_t>)));

    Json::Object equations;
    equations.emplace(
        "existing_capacity",
        text("checked_sum_exactly_11_closed_components_v1"));
    equations.emplace(
        "global_memory_admission",
        text("checked_add(checked_add(E,Q_B),H)<=M_v1"));
    equations.emplace(
        "local_payload_admission", text("Q_B<=P_v1"));
    equations.emplace(
        "nested_vector_storage", text("capacity*native_value_width_v1"));
    equations.emplace(
        "released_owner", text("current_owned_bytes==0_with_last_live_receipt_v1"));

    Json::Object root;
    root.emplace("compiler_identity_sha256", text(DSM_V3_COMPILER_IDENTITY_SHA256));
    root.emplace(
        "closed_component_names",
        strings({
            "validated_snapshot_bytes",
            "incumbent_flat_tdtree_bytes",
            "incumbent_fixed_plan_owned_bytes",
            "candidate_cover_owned_bytes",
            "proposal_plans_owned_bytes",
            "admitted_candidates_owned_bytes",
            "selected_candidate_plan_owned_bytes",
            "lazy_cache_owned_bytes",
            "planner_trace_owned_bytes",
            "source_data_graph_owned_bytes",
            "source_query_graph_owned_bytes"}));
    root.emplace(
        "excluded_ownership",
        strings({
            "allocator_metadata",
            "executable_and_static_storage",
            "library_and_runtime_state",
            "stack_frames_and_thread_stacks",
            "small_untracked_control_objects_covered_by_fixed_headroom"}));
    root.emplace(
        "ownership_rules",
        strings({
            "snapshot_pointee_and_control_counted_once_v1",
            "embedded_shared_ptr_handle_counted_only_through_inline_object_v1",
            "actual_vector_capacity_never_size_estimate_v1",
            "proposal_admitted_lazy_destroyed_before_capture_v1",
            "selected_plan_score_rows_and_trace_retained_across_reservation_v1",
            "source_graph_dynamic_owners_explicitly_zero_after_release_v1",
            "candidate_quote_and_headroom_excluded_from_E_v1",
            "no_rss_or_allocator_global_statistics_v1"}));
    root.emplace("capacity_equations", Json::object(std::move(equations)));
    root.emplace("native_size_bytes", Json::object(std::move(widths)));
    root.emplace("schema", text(kExistingModelSchema));
    root.emplace("schema_version", u64(kExistingModelVersion));
    root.emplace("source_bundle_sha256", text(DSM_V3_SOURCE_BUNDLE_SHA256));
    return Json::object(std::move(root));
}

Json existingReceiptCoreJson(const ExistingCapacityReceiptV1& r) {
    Json::Object components;
#define COMPONENT(name) components.emplace(#name, u64(r.name))
    COMPONENT(validated_snapshot_bytes);
    COMPONENT(incumbent_flat_tdtree_bytes);
    COMPONENT(incumbent_fixed_plan_owned_bytes);
    COMPONENT(candidate_cover_owned_bytes);
    COMPONENT(proposal_plans_owned_bytes);
    COMPONENT(admitted_candidates_owned_bytes);
    COMPONENT(selected_candidate_plan_owned_bytes);
    COMPONENT(lazy_cache_owned_bytes);
    COMPONENT(planner_trace_owned_bytes);
    COMPONENT(source_data_graph_owned_bytes);
    COMPONENT(source_query_graph_owned_bytes);
#undef COMPONENT

    Json::Object detail;
    detail.emplace("validated_snapshot_bytes", snapshotJson(r.validated_snapshot_detail));
    detail.emplace("incumbent_flat_tdtree_bytes", tdComponentJson(r.incumbent_flat_tdtree_detail));
    detail.emplace("incumbent_fixed_plan_owned_bytes", fixedPlanJson(r.incumbent_fixed_plan_detail));
    detail.emplace("candidate_cover_owned_bytes", coverComponentJson(r.candidate_cover_detail));
    detail.emplace("proposal_plans_owned_bytes", releasedPlansJson(r.proposal_plans_detail));
    detail.emplace("admitted_candidates_owned_bytes", releasedPlansJson(r.admitted_candidates_detail));
    detail.emplace("selected_candidate_plan_owned_bytes", planContainerJson(r.selected_candidate_plan_detail));
    detail.emplace("lazy_cache_owned_bytes", releasedLazyJson(r.lazy_cache_detail));
    detail.emplace("planner_trace_owned_bytes", traceJson(r.planner_trace_detail));
    detail.emplace("source_data_graph_owned_bytes", sourceJson(r.source_data_graph_detail));
    detail.emplace("source_query_graph_owned_bytes", sourceJson(r.source_query_graph_detail));

    Json::Object root;
    root.emplace(
        "accounted_existing_capacity_bytes",
        u64(r.accounted_existing_capacity_bytes));
    root.emplace("accounting_model_sha256", text(r.accounting_model_sha256));
    root.emplace("complete", boolean(r.complete));
    root.emplace("component_details", Json::object(std::move(detail)));
    root.emplace("components", Json::object(std::move(components)));
    root.emplace("overflowed", boolean(r.overflowed));
    root.emplace("phase_generation", u64(r.phase_generation));
    root.emplace("schema", text(r.schema));
    root.emplace("schema_version", u64(r.schema_version));
    return Json::object(std::move(root));
}

Json admissionCoreJson(const V3GlobalMemoryAdmissionReceiptV1& r) {
    Json::Object o;
    o.emplace("admission_scope", text(r.admission_scope));
    o.emplace("accounted_existing_capacity_bytes", u64(r.accounted_existing_capacity_bytes));
    o.emplace("candidate_payload_ceiling_bytes", u64(r.candidate_payload_ceiling_bytes));
    o.emplace("candidate_payload_quote_bytes", u64(r.candidate_payload_quote_bytes));
    o.emplace("candidate_quote_contract_fingerprint", u64(r.candidate_quote_contract_fingerprint));
    o.emplace("complete", boolean(r.complete));
    o.emplace("exact_boundary", boolean(r.exact_boundary));
    o.emplace("existing_capacity_accounting_model_sha256", text(r.existing_capacity_accounting_model_sha256));
    o.emplace("existing_capacity_receipt_sha256", text(r.existing_capacity_receipt_sha256));
    o.emplace("fits", boolean(r.fits));
    o.emplace("fixed_memory_headroom_bytes", u64(r.fixed_memory_headroom_bytes));
    o.emplace("overflowed", boolean(r.overflowed));
    o.emplace(
        "full_process_memory_proof",
        boolean(r.full_process_memory_proof));
    o.emplace("process_memory_max_bytes", u64(r.process_memory_max_bytes));
    o.emplace(
        "production_authorization",
        boolean(r.production_authorization));
    o.emplace("reason", text(v3GlobalMemoryAdmissionReasonName(r.reason)));
    o.emplace("required_capacity_bytes", u64(r.required_capacity_bytes));
    o.emplace("schema", text(r.schema));
    o.emplace("schema_version", u64(r.schema_version));
    o.emplace("selected_plan_fingerprint", u64(r.selected_plan_fingerprint));
    return Json::object(std::move(o));
}

} // namespace

bool FixedPlanCapacityReceiptV1::invariantHolds() const noexcept {
    std::uint64_t expected_order = 0;
    std::uint64_t expected_parent = 0;
    std::uint64_t expected_dynamic = 0;
    std::uint64_t expected_total = 0;
    return complete && !overflowed && object_bytes == sizeof(FixedPlan) &&
        order_count <= order_capacity && parent_count <= parent_capacity &&
        checkedMultiply(order_capacity, sizeof(int), expected_order) &&
        expected_order == order_bytes &&
        checkedMultiply(parent_capacity, sizeof(int), expected_parent) &&
        expected_parent == parent_bytes &&
        checkedAdd(order_bytes, parent_bytes, expected_dynamic) &&
        expected_dynamic == dynamic_bytes &&
        checkedAdd(object_bytes, dynamic_bytes, expected_total) &&
        expected_total == total_bytes && plan_fingerprint != 0 &&
        ownership_fingerprint == fixedPlanFingerprintForReceipt(*this);
}

FixedPlanCapacityReceiptV1 fixedPlanCapacityReceiptV1(
    const FixedPlan& plan) noexcept {
    FixedPlanCapacityReceiptV1 r;
    r.object_bytes = sizeof(FixedPlan);
    r.query_fingerprint = plan.query_fingerprint;
    r.plan_fingerprint = plan.fingerprint;
    if (!sizeToU64(plan.order.size(), r.order_count) ||
        !sizeToU64(plan.order.capacity(), r.order_capacity) ||
        !checkedMultiply(r.order_capacity, sizeof(int), r.order_bytes) ||
        !sizeToU64(plan.parent.size(), r.parent_count) ||
        !sizeToU64(plan.parent.capacity(), r.parent_capacity) ||
        !checkedMultiply(r.parent_capacity, sizeof(int), r.parent_bytes) ||
        !checkedAdd(r.order_bytes, r.parent_bytes, r.dynamic_bytes) ||
        !checkedAdd(r.object_bytes, r.dynamic_bytes, r.total_bytes)) {
        r.overflowed = true;
        return r;
    }
    r.ownership_fingerprint = fixedPlanFingerprintForReceipt(r);
    r.complete = r.plan_fingerprint != 0;
    if (r.complete && !r.invariantHolds()) r.complete = false;
    return r;
}

bool PlanContainerCapacityDetailV1::invariantHolds() const noexcept {
    std::uint64_t outer = 0;
    std::uint64_t order = 0;
    std::uint64_t parent = 0;
    std::uint64_t rows = 0;
    std::uint64_t dynamic = 0;
    std::uint64_t total = 0;
    return complete && !overflowed && element_count <= element_capacity &&
        plan_count <= element_count + (element_capacity == 0 ? 1U : 0U) &&
        plan_order_count <= plan_order_capacity &&
        plan_parent_count <= plan_parent_capacity &&
        score_row_count <= score_row_capacity &&
        ((element_capacity == 0 && outer_elements_bytes == 0) ||
         (checkedMultiply(element_capacity, element_width_bytes, outer) &&
          outer == outer_elements_bytes)) &&
        checkedMultiply(plan_order_capacity, sizeof(int), order) &&
        order == plan_order_bytes &&
        checkedMultiply(plan_parent_capacity, sizeof(int), parent) &&
        parent == plan_parent_bytes &&
        checkedMultiply(score_row_capacity, score_row_width_bytes, rows) &&
        rows == score_row_bytes &&
        checkedAdd(outer_elements_bytes, plan_order_bytes, dynamic) &&
        checkedAccumulate(dynamic, plan_parent_bytes) &&
        checkedAccumulate(dynamic, score_row_bytes) &&
        dynamic == dynamic_bytes &&
        checkedAdd(container_object_bytes, dynamic_bytes, total) &&
        total == total_bytes &&
        ownership_fingerprint == planContainerFingerprint(*this);
}

namespace {

bool addPlanDetail(
    PlanContainerCapacityDetailV1& r,
    const FixedPlan& plan) noexcept {
    std::uint64_t count = 0;
    std::uint64_t capacity = 0;
    return sizeToU64(plan.order.size(), count) &&
        checkedAccumulate(r.plan_order_count, count) &&
        sizeToU64(plan.order.capacity(), capacity) &&
        checkedAccumulate(r.plan_order_capacity, capacity) &&
        sizeToU64(plan.parent.size(), count) &&
        checkedAccumulate(r.plan_parent_count, count) &&
        sizeToU64(plan.parent.capacity(), capacity) &&
        checkedAccumulate(r.plan_parent_capacity, capacity) &&
        checkedAccumulate(r.plan_count, 1);
}

bool finalizePlanDetail(PlanContainerCapacityDetailV1& r) noexcept {
    if (!checkedMultiply(
            r.element_capacity, r.element_width_bytes,
            r.outer_elements_bytes) ||
        !checkedMultiply(
            r.plan_order_capacity, sizeof(int), r.plan_order_bytes) ||
        !checkedMultiply(
            r.plan_parent_capacity, sizeof(int), r.plan_parent_bytes) ||
        !checkedMultiply(
            r.score_row_capacity, r.score_row_width_bytes,
            r.score_row_bytes) ||
        !checkedAdd(
            r.outer_elements_bytes, r.plan_order_bytes, r.dynamic_bytes) ||
        !checkedAccumulate(r.dynamic_bytes, r.plan_parent_bytes) ||
        !checkedAccumulate(r.dynamic_bytes, r.score_row_bytes) ||
        !checkedAdd(
            r.container_object_bytes, r.dynamic_bytes, r.total_bytes)) {
        r.overflowed = true;
        return false;
    }
    r.ownership_fingerprint = planContainerFingerprint(r);
    r.complete = true;
    if (!r.invariantHolds()) r.complete = false;
    return r.complete;
}

} // namespace

PlanContainerCapacityDetailV1 proposalPlanCapacityDetailV1(
    const std::vector<FixedPlan>& plans) noexcept {
    PlanContainerCapacityDetailV1 r;
    r.container_object_bytes = sizeof(plans);
    r.element_width_bytes = sizeof(FixedPlan);
    r.score_row_width_bytes = sizeof(V3RetainedPlanScoreRowV1);
    if (!sizeToU64(plans.size(), r.element_count) ||
        !sizeToU64(plans.capacity(), r.element_capacity)) {
        r.overflowed = true;
        return r;
    }
    for (const FixedPlan& plan : plans) {
        if (!addPlanDetail(r, plan)) {
            r.overflowed = true;
            return r;
        }
    }
    finalizePlanDetail(r);
    return r;
}

PlanContainerCapacityDetailV1 admittedCandidateCapacityDetailV1(
    const std::vector<V3SelectedCandidateCarrierV1>& candidates) noexcept {
    PlanContainerCapacityDetailV1 r;
    r.container_object_bytes = sizeof(candidates);
    r.element_width_bytes = sizeof(V3SelectedCandidateCarrierV1);
    r.score_row_width_bytes = sizeof(V3RetainedPlanScoreRowV1);
    if (!sizeToU64(candidates.size(), r.element_count) ||
        !sizeToU64(candidates.capacity(), r.element_capacity)) {
        r.overflowed = true;
        return r;
    }
    for (const V3SelectedCandidateCarrierV1& candidate : candidates) {
        if (!candidate.available || !addPlanDetail(r, candidate.plan)) {
            r.overflowed = true;
            return r;
        }
        std::uint64_t count = 0;
        std::uint64_t capacity = 0;
        if (!sizeToU64(candidate.retained_score_rows.size(), count) ||
            !sizeToU64(candidate.retained_score_rows.capacity(), capacity) ||
            !checkedAccumulate(r.score_row_count, count) ||
            !checkedAccumulate(r.score_row_capacity, capacity)) {
            r.overflowed = true;
            return r;
        }
    }
    finalizePlanDetail(r);
    return r;
}

PlanContainerCapacityDetailV1 selectedCandidateCapacityDetailV1(
    const V3SelectedCandidateCarrierV1& candidate) noexcept {
    PlanContainerCapacityDetailV1 r;
    r.container_object_bytes = sizeof(candidate);
    r.element_width_bytes = sizeof(V3SelectedCandidateCarrierV1);
    r.score_row_width_bytes = sizeof(V3RetainedPlanScoreRowV1);
    if (!candidate.available) return r;
    r.selected_plan_fingerprint = candidate.plan.fingerprint;
    if (!addPlanDetail(r, candidate.plan) ||
        !sizeToU64(
            candidate.retained_score_rows.size(), r.score_row_count) ||
        !sizeToU64(
            candidate.retained_score_rows.capacity(), r.score_row_capacity)) {
        r.overflowed = true;
        return r;
    }
    finalizePlanDetail(r);
    return r;
}

bool ReleasedPlanContainerCapacityReceiptV1::invariantHolds() const noexcept {
    std::uint64_t expected = kFnvOffset;
    appendText(expected, "released_plan_container_v1");
    appendU64(expected, last_live_capacity.ownership_fingerprint);
    appendU64(expected, current_owned_bytes);
    expected = nonzeroFingerprint(expected);
    return complete && !overflowed && released_before_existing_capture &&
        last_live_capacity.invariantHolds() && current_owned_bytes == 0 &&
        release_fingerprint == expected;
}

ReleasedPlanContainerCapacityReceiptV1
releasedPlanContainerCapacityReceiptV1(
    const PlanContainerCapacityDetailV1& last_live) noexcept {
    ReleasedPlanContainerCapacityReceiptV1 r;
    r.last_live_capacity = last_live;
    r.overflowed = last_live.overflowed;
    r.released_before_existing_capture = last_live.invariantHolds();
    std::uint64_t hash = kFnvOffset;
    appendText(hash, "released_plan_container_v1");
    appendU64(hash, last_live.ownership_fingerprint);
    appendU64(hash, r.current_owned_bytes);
    r.release_fingerprint = nonzeroFingerprint(hash);
    r.complete = r.released_before_existing_capture && !r.overflowed;
    return r;
}

bool LazyCacheCapacityDetailV1::invariantHolds() const noexcept {
    std::uint64_t outer = 0;
    std::uint64_t nested = 0;
    std::uint64_t dynamic = 0;
    std::uint64_t total = 0;
    return complete && !overflowed && entry_count <= entry_capacity &&
        nested_value_count <= nested_value_capacity &&
        checkedMultiply(entry_capacity, entry_width_bytes, outer) &&
        outer == outer_entries_bytes &&
        checkedMultiply(
            nested_value_capacity, nested_value_width_bytes, nested) &&
        nested == nested_value_bytes &&
        checkedAdd(outer_entries_bytes, nested_value_bytes, dynamic) &&
        dynamic == dynamic_bytes &&
        checkedAdd(container_object_bytes, dynamic_bytes, total) &&
        total == total_bytes &&
        ownership_fingerprint == lazyFingerprint(*this);
}

LazyCacheCapacityDetailV1 lazyCacheCapacityDetailV1(
    const std::vector<V3LazyCacheEntryStorageV1>& cache) noexcept {
    LazyCacheCapacityDetailV1 r;
    r.container_object_bytes = sizeof(cache);
    r.entry_width_bytes = sizeof(V3LazyCacheEntryStorageV1);
    r.nested_value_width_bytes = sizeof(int);
    if (!sizeToU64(cache.size(), r.entry_count) ||
        !sizeToU64(cache.capacity(), r.entry_capacity)) {
        r.overflowed = true;
        return r;
    }
    for (const V3LazyCacheEntryStorageV1& entry : cache) {
        std::uint64_t count = 0;
        std::uint64_t capacity = 0;
        if (!sizeToU64(entry.values.size(), count) ||
            !sizeToU64(entry.values.capacity(), capacity) ||
            !checkedAccumulate(r.nested_value_count, count) ||
            !checkedAccumulate(r.nested_value_capacity, capacity)) {
            r.overflowed = true;
            return r;
        }
    }
    if (!checkedMultiply(
            r.entry_capacity, r.entry_width_bytes, r.outer_entries_bytes) ||
        !checkedMultiply(
            r.nested_value_capacity, r.nested_value_width_bytes,
            r.nested_value_bytes) ||
        !checkedAdd(
            r.outer_entries_bytes, r.nested_value_bytes, r.dynamic_bytes) ||
        !checkedAdd(
            r.container_object_bytes, r.dynamic_bytes, r.total_bytes)) {
        r.overflowed = true;
        return r;
    }
    r.ownership_fingerprint = lazyFingerprint(r);
    r.complete = true;
    if (!r.invariantHolds()) r.complete = false;
    return r;
}

bool ReleasedLazyCacheCapacityReceiptV1::invariantHolds() const noexcept {
    std::uint64_t expected = kFnvOffset;
    appendText(expected, "released_lazy_cache_v1");
    appendU64(expected, last_live_capacity.ownership_fingerprint);
    appendU64(expected, current_owned_bytes);
    expected = nonzeroFingerprint(expected);
    return complete && !overflowed && released_before_existing_capture &&
        last_live_capacity.invariantHolds() && current_owned_bytes == 0 &&
        release_fingerprint == expected;
}

ReleasedLazyCacheCapacityReceiptV1 releasedLazyCacheCapacityReceiptV1(
    const LazyCacheCapacityDetailV1& last_live) noexcept {
    ReleasedLazyCacheCapacityReceiptV1 r;
    r.last_live_capacity = last_live;
    r.overflowed = last_live.overflowed;
    r.released_before_existing_capture = last_live.invariantHolds();
    std::uint64_t hash = kFnvOffset;
    appendText(hash, "released_lazy_cache_v1");
    appendU64(hash, last_live.ownership_fingerprint);
    appendU64(hash, r.current_owned_bytes);
    r.release_fingerprint = nonzeroFingerprint(hash);
    r.complete = r.released_before_existing_capture && !r.overflowed;
    return r;
}

bool PlannerTraceCapacityReceiptV1::invariantHolds() const noexcept {
    std::uint64_t dynamic = 0;
    std::uint64_t total = 0;
    std::uint64_t owner = kFnvOffset;
    appendText(owner, "planner_trace_capacity_v1");
    appendU64(owner, object_bytes); appendU64(owner, entry_count);
    appendU64(owner, entry_capacity); appendU64(owner, entry_width_bytes);
    appendU64(owner, content_fingerprint);
    owner = nonzeroFingerprint(owner);
    return complete && !overflowed && frozen_before_reservation &&
        object_bytes == sizeof(std::vector<std::uint64_t>) &&
        entry_count <= entry_capacity && entry_width_bytes == sizeof(std::uint64_t) &&
        checkedMultiply(entry_capacity, entry_width_bytes, dynamic) &&
        dynamic == dynamic_bytes && checkedAdd(object_bytes, dynamic, total) &&
        total == total_bytes && content_fingerprint != 0 &&
        ownership_fingerprint == owner;
}

PlannerTraceCapacityReceiptV1 plannerTraceCapacityReceiptV1(
    const std::vector<std::uint64_t>& trace,
    bool frozen_before_reservation) noexcept {
    PlannerTraceCapacityReceiptV1 r;
    r.frozen_before_reservation = frozen_before_reservation;
    r.object_bytes = sizeof(trace);
    r.entry_width_bytes = sizeof(std::uint64_t);
    r.content_fingerprint = traceContentFingerprint(trace);
    if (!sizeToU64(trace.size(), r.entry_count) ||
        !sizeToU64(trace.capacity(), r.entry_capacity) ||
        !checkedMultiply(r.entry_capacity, r.entry_width_bytes, r.dynamic_bytes) ||
        !checkedAdd(r.object_bytes, r.dynamic_bytes, r.total_bytes)) {
        r.overflowed = true;
        return r;
    }
    std::uint64_t owner = kFnvOffset;
    appendText(owner, "planner_trace_capacity_v1");
    appendU64(owner, r.object_bytes); appendU64(owner, r.entry_count);
    appendU64(owner, r.entry_capacity); appendU64(owner, r.entry_width_bytes);
    appendU64(owner, r.content_fingerprint);
    r.ownership_fingerprint = nonzeroFingerprint(owner);
    r.complete = frozen_before_reservation;
    if (r.complete && !r.invariantHolds()) r.complete = false;
    return r;
}

bool SourceGraphReleaseReceiptV1::invariantHolds() const noexcept {
    std::uint64_t total = 0;
    std::uint64_t hash = kFnvOffset;
    appendText(hash, "source_graph_release_v1");
    appendText(hash, owner_id);
    appendU64(hash, total_owned_bytes);
    hash = nonzeroFingerprint(hash);
    return complete && !overflowed && released_before_incumbent &&
        !owner_id.empty() && vertex_count == 0 &&
        adjacency_outer_capacity == 0 && adjacency_outer_bytes == 0 &&
        adjacency_nested_capacity == 0 && adjacency_nested_bytes == 0 &&
        incoming_outer_capacity == 0 && incoming_outer_bytes == 0 &&
        incoming_nested_capacity == 0 && incoming_nested_bytes == 0 &&
        vertex_labels_capacity == 0 && vertex_labels_bytes == 0 &&
        external_ids_capacity == 0 && external_ids_bytes == 0 &&
        temporal_edges_capacity == 0 && temporal_edges_bytes == 0 &&
        temporal_intervals_capacity == 0 && temporal_intervals_bytes == 0 &&
        vertex_active_durations_capacity == 0 &&
        vertex_active_durations_bytes == 0 &&
        neighbor_label_counts_capacity == 0 &&
        neighbor_label_counts_bytes == 0 &&
        incoming_neighbor_label_counts_capacity == 0 &&
        incoming_neighbor_label_counts_bytes == 0 &&
        total_owned_bytes == total && release_fingerprint == hash;
}

SourceGraphReleaseReceiptV1 sourceGraphReleaseReceiptV1(
    const Graph& graph,
    const std::string& owner_id) noexcept {
    SourceGraphReleaseReceiptV1 r;
    r.owner_id = owner_id;
    if (graph.num_vertices < 0) {
        r.overflowed = true;
        return r;
    }
    r.vertex_count = static_cast<std::uint64_t>(graph.num_vertices);
    const auto recordCapacity = [&](std::size_t capacity,
                                    std::uint64_t width,
                                    std::uint64_t& cap_out,
                                    std::uint64_t& bytes_out) noexcept {
        return sizeToU64(capacity, cap_out) &&
            checkedMultiply(cap_out, width, bytes_out) &&
            checkedAccumulate(r.total_owned_bytes, bytes_out);
    };
    if (!recordCapacity(
            graph.adj.capacity(), sizeof(std::vector<Edge>),
            r.adjacency_outer_capacity, r.adjacency_outer_bytes) ||
        !recordCapacity(
            graph.in_adj.capacity(), sizeof(std::vector<Edge>),
            r.incoming_outer_capacity, r.incoming_outer_bytes) ||
        !recordCapacity(
            graph.vertex_labels.capacity(), sizeof(Label),
            r.vertex_labels_capacity, r.vertex_labels_bytes) ||
        !recordCapacity(
            graph.external_ids.capacity(), sizeof(int),
            r.external_ids_capacity, r.external_ids_bytes) ||
        !recordCapacity(
            graph.temporal_edges.capacity(), sizeof(TemporalEdge),
            r.temporal_edges_capacity, r.temporal_edges_bytes) ||
        !recordCapacity(
            graph.vertex_active_durations.capacity(), sizeof(int),
            r.vertex_active_durations_capacity,
            r.vertex_active_durations_bytes) ||
        !recordCapacity(
            graph.neighbor_label_counts.capacity(),
            sizeof(std::array<int, kLabelCount>),
            r.neighbor_label_counts_capacity,
            r.neighbor_label_counts_bytes) ||
        !recordCapacity(
            graph.incoming_neighbor_label_counts.capacity(),
            sizeof(std::array<int, kLabelCount>),
            r.incoming_neighbor_label_counts_capacity,
            r.incoming_neighbor_label_counts_bytes)) {
        r.overflowed = true;
        return r;
    }
    for (const std::vector<Edge>& edges : graph.adj) {
        std::uint64_t capacity = 0;
        std::uint64_t bytes = 0;
        if (!sizeToU64(edges.capacity(), capacity) ||
            !checkedAccumulate(r.adjacency_nested_capacity, capacity) ||
            !checkedMultiply(capacity, sizeof(Edge), bytes) ||
            !checkedAccumulate(r.adjacency_nested_bytes, bytes) ||
            !checkedAccumulate(r.total_owned_bytes, bytes)) {
            r.overflowed = true;
            return r;
        }
    }
    for (const std::vector<Edge>& edges : graph.in_adj) {
        std::uint64_t capacity = 0;
        std::uint64_t bytes = 0;
        if (!sizeToU64(edges.capacity(), capacity) ||
            !checkedAccumulate(r.incoming_nested_capacity, capacity) ||
            !checkedMultiply(capacity, sizeof(Edge), bytes) ||
            !checkedAccumulate(r.incoming_nested_bytes, bytes) ||
            !checkedAccumulate(r.total_owned_bytes, bytes)) {
            r.overflowed = true;
            return r;
        }
    }
    for (const TemporalEdge& edge : graph.temporal_edges) {
        std::uint64_t capacity = 0;
        std::uint64_t bytes = 0;
        if (!sizeToU64(edge.activeIntervals().capacity(), capacity) ||
            !checkedAccumulate(r.temporal_intervals_capacity, capacity) ||
            !checkedMultiply(capacity, sizeof(TimeInterval), bytes) ||
            !checkedAccumulate(r.temporal_intervals_bytes, bytes) ||
            !checkedAccumulate(r.total_owned_bytes, bytes)) {
            r.overflowed = true;
            return r;
        }
    }
    r.released_before_incumbent =
        graph.num_vertices == 0 && graph.adj.empty() && graph.in_adj.empty() &&
        graph.vertex_labels.empty() && graph.external_ids.empty() &&
        graph.temporal_edges.empty() && graph.vertex_active_durations.empty() &&
        graph.neighbor_label_counts.empty() &&
        graph.incoming_neighbor_label_counts.empty() &&
        r.total_owned_bytes == 0;
    std::uint64_t hash = kFnvOffset;
    appendText(hash, "source_graph_release_v1");
    appendText(hash, owner_id);
    appendU64(hash, r.total_owned_bytes);
    r.release_fingerprint = nonzeroFingerprint(hash);
    r.complete = r.released_before_incumbent;
    if (r.complete && !r.invariantHolds()) r.complete = false;
    return r;
}

bool SnapshotCapacityComponentReceiptV1::invariantHolds() const noexcept {
    std::uint64_t hash = kFnvOffset;
    appendText(hash, "snapshot_capacity_component_v1");
    appendU64(hash, snapshot_token); appendU64(hash, data_fingerprint);
    appendU64(hash, query_fingerprint);
    appendU64(hash, sealed_snapshot_storage_bytes);
    appendText(hash, graph_accounting_model_sha256);
    appendText(hash, expected_quote_sha256);
    hash = nonzeroFingerprint(hash);
    const bool evidence_hashes =
        (graph_accounting_model_sha256.empty() &&
         expected_quote_sha256.empty()) ||
        (lowercaseSha256(graph_accounting_model_sha256) &&
         lowercaseSha256(expected_quote_sha256));
    return complete && !overflowed && snapshot_token != 0 &&
        data_fingerprint != 0 && query_fingerprint != 0 &&
        sealed_snapshot_storage_bytes > 0 &&
        sealed_snapshot_storage_bytes == total_bytes &&
        sealed_snapshot_storage_bytes <= quoted_snapshot_storage_bytes &&
        evidence_hashes && ownership_fingerprint == hash;
}

SnapshotCapacityComponentReceiptV1 snapshotCapacityComponentReceiptV1(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot) noexcept {
    SnapshotCapacityComponentReceiptV1 r;
    if (!snapshot || !snapshot->receipt().complete()) return r;
    const GraphValidationReceipt& receipt = snapshot->receipt();
    r.snapshot_token = snapshot->snapshotToken();
    r.data_fingerprint = snapshot->dataFingerprint();
    r.query_fingerprint = snapshot->queryFingerprint();
    r.sealed_snapshot_storage_bytes = receipt.snapshot_storage_bytes;
    r.quoted_snapshot_storage_bytes = receipt.quoted_snapshot_storage_bytes;
    r.graph_accounting_model_sha256 = receipt.graph_accounting_model_sha256;
    r.expected_quote_sha256 = receipt.expected_quote_sha256;
    r.total_bytes = receipt.snapshot_storage_bytes;
    std::uint64_t hash = kFnvOffset;
    appendText(hash, "snapshot_capacity_component_v1");
    appendU64(hash, r.snapshot_token); appendU64(hash, r.data_fingerprint);
    appendU64(hash, r.query_fingerprint);
    appendU64(hash, r.sealed_snapshot_storage_bytes);
    appendText(hash, r.graph_accounting_model_sha256);
    appendText(hash, r.expected_quote_sha256);
    r.ownership_fingerprint = nonzeroFingerprint(hash);
    r.complete = true;
    if (!r.invariantHolds()) r.complete = false;
    return r;
}

bool TDTreeCapacityComponentReceiptV1::invariantHolds() const noexcept {
    std::uint64_t hash = kFnvOffset;
    appendText(hash, "tdtree_capacity_component_v1");
    appendU64(hash, snapshot_token); appendU64(hash, plan_fingerprint);
    appendU64(hash, tree_content_fingerprint); appendU64(hash, total_bytes);
    hash = nonzeroFingerprint(hash);
    return complete && !overflowed && sorted_flat_blocks_only &&
        snapshot_token != 0 && plan_fingerprint != 0 &&
        tree_content_fingerprint != 0 && native.invariantHolds() &&
        total_bytes == native.total_bytes && ownership_fingerprint == hash;
}

TDTreeCapacityComponentReceiptV1 tdTreeCapacityComponentReceiptV1(
    const TDTree& tree) noexcept {
    TDTreeCapacityComponentReceiptV1 r;
    r.sorted_flat_blocks_only = tree.hasOnlySortedFlatBlocks();
    r.snapshot_token = tree.validationSnapshotToken();
    r.plan_fingerprint = tree.executionPlanFingerprint();
    r.tree_content_fingerprint = tree.candidateContentFingerprint();
    r.native = tree.capacityReceipt();
    r.total_bytes = r.native.total_bytes;
    r.overflowed = r.native.overflowed;
    std::uint64_t hash = kFnvOffset;
    appendText(hash, "tdtree_capacity_component_v1");
    appendU64(hash, r.snapshot_token); appendU64(hash, r.plan_fingerprint);
    appendU64(hash, r.tree_content_fingerprint); appendU64(hash, r.total_bytes);
    r.ownership_fingerprint = nonzeroFingerprint(hash);
    r.complete = r.native.invariantHolds() && r.sorted_flat_blocks_only;
    if (r.complete && !r.invariantHolds()) r.complete = false;
    return r;
}

bool CandidateCoverCapacityComponentReceiptV1::invariantHolds() const noexcept {
    std::uint64_t hash = kFnvOffset;
    appendText(hash, "candidate_cover_capacity_component_v1");
    appendU64(hash, snapshot_token); appendU64(hash, cover_fingerprint);
    appendU64(hash, incumbent_plan_fingerprint); appendU64(hash, total_bytes);
    hash = nonzeroFingerprint(hash);
    return complete && !overflowed && snapshot_token != 0 &&
        cover_fingerprint != 0 && incumbent_plan_fingerprint != 0 &&
        native.invariantHolds() && total_bytes == native.total_bytes &&
        ownership_fingerprint == hash;
}

CandidateCoverCapacityComponentReceiptV1
candidateCoverCapacityComponentReceiptV1(
    const CandidateCover& cover) noexcept {
    CandidateCoverCapacityComponentReceiptV1 r;
    r.snapshot_token = cover.snapshotToken();
    r.cover_fingerprint = cover.coverFingerprint();
    r.incumbent_plan_fingerprint = cover.incumbentPlanFingerprint();
    r.native = cover.capacityReceipt();
    r.total_bytes = r.native.total_bytes;
    r.overflowed = r.native.overflowed;
    std::uint64_t hash = kFnvOffset;
    appendText(hash, "candidate_cover_capacity_component_v1");
    appendU64(hash, r.snapshot_token); appendU64(hash, r.cover_fingerprint);
    appendU64(hash, r.incumbent_plan_fingerprint); appendU64(hash, r.total_bytes);
    r.ownership_fingerprint = nonzeroFingerprint(hash);
    r.complete = r.native.invariantHolds() &&
        cover.hasValidatedSnapshotAuthority();
    if (r.complete && !r.invariantHolds()) r.complete = false;
    return r;
}

bool ExistingCapacityAccountingModelArtifactV1::complete() const noexcept {
    try {
        return lowercaseSha256(sha256) && !canonical_bytes.empty() &&
            canonical_bytes == canonicalJsonBytes(existingModelJson()) &&
            sha256 == sha256Hex(canonical_bytes);
    } catch (...) {
        return false;
    }
}

ExistingCapacityAccountingModelArtifactV1
existingCapacityAccountingModelArtifactV1() {
    ExistingCapacityAccountingModelArtifactV1 result;
    result.canonical_bytes = canonicalJsonBytes(existingModelJson());
    result.sha256 = sha256Hex(result.canonical_bytes);
    return result;
}

bool ExistingCapacityReceiptV1::invariantHolds() const noexcept {
    try {
        std::uint64_t total = 0;
        const ExistingCapacityAccountingModelArtifactV1 model =
            existingCapacityAccountingModelArtifactV1();
        return schema == "icde2027_v3_existing_capacity_receipt" &&
            schema_version == 1 && complete && !overflowed &&
            phase_generation != 0 && model.complete() &&
            accounting_model_sha256 == model.sha256 &&
            validated_snapshot_detail.invariantHolds() &&
            incumbent_flat_tdtree_detail.invariantHolds() &&
            incumbent_fixed_plan_detail.invariantHolds() &&
            candidate_cover_detail.invariantHolds() &&
            proposal_plans_detail.invariantHolds() &&
            admitted_candidates_detail.invariantHolds() &&
            selected_candidate_plan_detail.invariantHolds() &&
            lazy_cache_detail.invariantHolds() &&
            planner_trace_detail.invariantHolds() &&
            source_data_graph_detail.invariantHolds() &&
            source_query_graph_detail.invariantHolds() &&
            validated_snapshot_bytes == validated_snapshot_detail.total_bytes &&
            incumbent_flat_tdtree_bytes == incumbent_flat_tdtree_detail.total_bytes &&
            incumbent_fixed_plan_owned_bytes == incumbent_fixed_plan_detail.total_bytes &&
            candidate_cover_owned_bytes == candidate_cover_detail.total_bytes &&
            proposal_plans_owned_bytes == proposal_plans_detail.current_owned_bytes &&
            admitted_candidates_owned_bytes == admitted_candidates_detail.current_owned_bytes &&
            selected_candidate_plan_owned_bytes == selected_candidate_plan_detail.total_bytes &&
            lazy_cache_owned_bytes == lazy_cache_detail.current_owned_bytes &&
            planner_trace_owned_bytes == planner_trace_detail.total_bytes &&
            source_data_graph_owned_bytes == source_data_graph_detail.total_owned_bytes &&
            source_query_graph_owned_bytes == source_query_graph_detail.total_owned_bytes &&
            checkedAccumulate(total, validated_snapshot_bytes) &&
            checkedAccumulate(total, incumbent_flat_tdtree_bytes) &&
            checkedAccumulate(total, incumbent_fixed_plan_owned_bytes) &&
            checkedAccumulate(total, candidate_cover_owned_bytes) &&
            checkedAccumulate(total, proposal_plans_owned_bytes) &&
            checkedAccumulate(total, admitted_candidates_owned_bytes) &&
            checkedAccumulate(total, selected_candidate_plan_owned_bytes) &&
            checkedAccumulate(total, lazy_cache_owned_bytes) &&
            checkedAccumulate(total, planner_trace_owned_bytes) &&
            checkedAccumulate(total, source_data_graph_owned_bytes) &&
            checkedAccumulate(total, source_query_graph_owned_bytes) &&
            total == accounted_existing_capacity_bytes &&
            receipt_sha256 == existingCapacityReceiptSha256V1(*this);
    } catch (...) {
        return false;
    }
}

ExistingCapacityReceiptV1 captureExistingCapacityReceiptV1(
    const ExistingCapacityCaptureInputsV1& inputs) noexcept {
    ExistingCapacityReceiptV1 r;
    try {
        const ExistingCapacityAccountingModelArtifactV1 model =
            existingCapacityAccountingModelArtifactV1();
        if (!model.complete() || inputs.expected_accounting_model_sha256 != model.sha256 ||
            inputs.phase_generation == 0 || !inputs.snapshot ||
            inputs.incumbent_tree == nullptr || inputs.incumbent_plan == nullptr ||
            inputs.cover == nullptr || inputs.selected_candidate == nullptr ||
            inputs.planner_trace == nullptr) {
            return r;
        }
        r.accounting_model_sha256 = model.sha256;
        r.phase_generation = inputs.phase_generation;
        r.validated_snapshot_detail = snapshotCapacityComponentReceiptV1(inputs.snapshot);
        r.incumbent_flat_tdtree_detail = tdTreeCapacityComponentReceiptV1(*inputs.incumbent_tree);
        r.incumbent_fixed_plan_detail = fixedPlanCapacityReceiptV1(*inputs.incumbent_plan);
        r.candidate_cover_detail = candidateCoverCapacityComponentReceiptV1(*inputs.cover);
        r.proposal_plans_detail = inputs.proposal_plans;
        r.admitted_candidates_detail = inputs.admitted_candidates;
        r.selected_candidate_plan_detail = selectedCandidateCapacityDetailV1(*inputs.selected_candidate);
        r.lazy_cache_detail = inputs.lazy_cache;
        r.planner_trace_detail = plannerTraceCapacityReceiptV1(*inputs.planner_trace, true);
        r.source_data_graph_detail = inputs.source_data_graph;
        r.source_query_graph_detail = inputs.source_query_graph;

        if (!r.validated_snapshot_detail.invariantHolds() ||
            !r.incumbent_flat_tdtree_detail.invariantHolds() ||
            !r.incumbent_fixed_plan_detail.invariantHolds() ||
            !r.candidate_cover_detail.invariantHolds() ||
            !r.proposal_plans_detail.invariantHolds() ||
            !r.admitted_candidates_detail.invariantHolds() ||
            !r.selected_candidate_plan_detail.invariantHolds() ||
            !r.lazy_cache_detail.invariantHolds() ||
            !r.planner_trace_detail.invariantHolds() ||
            !r.source_data_graph_detail.invariantHolds() ||
            !r.source_query_graph_detail.invariantHolds() ||
            r.validated_snapshot_detail.snapshot_token !=
                r.incumbent_flat_tdtree_detail.snapshot_token ||
            r.validated_snapshot_detail.snapshot_token !=
                r.candidate_cover_detail.snapshot_token ||
            r.incumbent_fixed_plan_detail.plan_fingerprint !=
                r.incumbent_flat_tdtree_detail.plan_fingerprint ||
            r.candidate_cover_detail.incumbent_plan_fingerprint !=
                r.incumbent_fixed_plan_detail.plan_fingerprint ||
            r.selected_candidate_plan_detail.selected_plan_fingerprint !=
                inputs.selected_candidate->quote.plan_fingerprint ||
            r.source_data_graph_detail.owner_id != "source_data_graph" ||
            r.source_query_graph_detail.owner_id != "source_query_graph") {
            return r;
        }

        r.validated_snapshot_bytes = r.validated_snapshot_detail.total_bytes;
        r.incumbent_flat_tdtree_bytes = r.incumbent_flat_tdtree_detail.total_bytes;
        r.incumbent_fixed_plan_owned_bytes = r.incumbent_fixed_plan_detail.total_bytes;
        r.candidate_cover_owned_bytes = r.candidate_cover_detail.total_bytes;
        r.proposal_plans_owned_bytes = r.proposal_plans_detail.current_owned_bytes;
        r.admitted_candidates_owned_bytes = r.admitted_candidates_detail.current_owned_bytes;
        r.selected_candidate_plan_owned_bytes = r.selected_candidate_plan_detail.total_bytes;
        r.lazy_cache_owned_bytes = r.lazy_cache_detail.current_owned_bytes;
        r.planner_trace_owned_bytes = r.planner_trace_detail.total_bytes;
        r.source_data_graph_owned_bytes = r.source_data_graph_detail.total_owned_bytes;
        r.source_query_graph_owned_bytes = r.source_query_graph_detail.total_owned_bytes;
        if (!checkedAccumulate(r.accounted_existing_capacity_bytes, r.validated_snapshot_bytes) ||
            !checkedAccumulate(r.accounted_existing_capacity_bytes, r.incumbent_flat_tdtree_bytes) ||
            !checkedAccumulate(r.accounted_existing_capacity_bytes, r.incumbent_fixed_plan_owned_bytes) ||
            !checkedAccumulate(r.accounted_existing_capacity_bytes, r.candidate_cover_owned_bytes) ||
            !checkedAccumulate(r.accounted_existing_capacity_bytes, r.proposal_plans_owned_bytes) ||
            !checkedAccumulate(r.accounted_existing_capacity_bytes, r.admitted_candidates_owned_bytes) ||
            !checkedAccumulate(r.accounted_existing_capacity_bytes, r.selected_candidate_plan_owned_bytes) ||
            !checkedAccumulate(r.accounted_existing_capacity_bytes, r.lazy_cache_owned_bytes) ||
            !checkedAccumulate(r.accounted_existing_capacity_bytes, r.planner_trace_owned_bytes) ||
            !checkedAccumulate(r.accounted_existing_capacity_bytes, r.source_data_graph_owned_bytes) ||
            !checkedAccumulate(r.accounted_existing_capacity_bytes, r.source_query_graph_owned_bytes)) {
            r.overflowed = true;
            return r;
        }
        r.complete = true;
        r.receipt_sha256 = existingCapacityReceiptSha256V1(r);
        if (!r.invariantHolds()) r.complete = false;
        return r;
    } catch (...) {
        r.complete = false;
        return r;
    }
}

std::string canonicalExistingCapacityReceiptCoreV1(
    const ExistingCapacityReceiptV1& receipt) {
    return canonicalJsonBytes(existingReceiptCoreJson(receipt));
}

std::string existingCapacityReceiptSha256V1(
    const ExistingCapacityReceiptV1& receipt) {
    return sha256Hex(canonicalExistingCapacityReceiptCoreV1(receipt));
}

const char* v3GlobalMemoryAdmissionReasonName(
    V3GlobalMemoryAdmissionReason reason) noexcept {
    switch (reason) {
        case V3GlobalMemoryAdmissionReason::Admitted: return "admitted";
        case V3GlobalMemoryAdmissionReason::ExistingReceiptIncomplete:
            return "existing-receipt-incomplete";
        case V3GlobalMemoryAdmissionReason::AccountingModelMismatch:
            return "accounting-model-mismatch";
        case V3GlobalMemoryAdmissionReason::CandidateQuoteIncomplete:
            return "candidate-quote-incomplete";
        case V3GlobalMemoryAdmissionReason::CandidateQuoteBindingMismatch:
            return "candidate-quote-binding-mismatch";
        case V3GlobalMemoryAdmissionReason::CandidatePayloadCeilingExceeded:
            return "candidate-payload-ceiling-exceeded";
        case V3GlobalMemoryAdmissionReason::ArithmeticOverflow:
            return "arithmetic-overflow";
        case V3GlobalMemoryAdmissionReason::ProcessMemoryLimitExceeded:
            return "process-memory-limit-exceeded";
        case V3GlobalMemoryAdmissionReason::AlreadyConsumed:
            return "already-consumed";
    }
    return "existing-receipt-incomplete";
}

bool V3GlobalMemoryAdmissionReceiptV1::invariantHolds() const noexcept {
    try {
        if (schema != "icde2027_v3_global_memory_admission" ||
            schema_version != 1 || !complete || overflowed ||
            admission_scope !=
                "pre_reservation_closed_11_components_v1" ||
            full_process_memory_proof || production_authorization ||
            !lowercaseSha256(existing_capacity_receipt_sha256) ||
            !lowercaseSha256(existing_capacity_accounting_model_sha256) ||
            candidate_quote_contract_fingerprint == 0 ||
            selected_plan_fingerprint == 0) {
            return false;
        }
        std::uint64_t required = 0;
        if (!checkedAdd(
                accounted_existing_capacity_bytes,
                candidate_payload_quote_bytes, required)) {
            return false;
        }
        if (!checkedAccumulate(required, fixed_memory_headroom_bytes) ||
            required != required_capacity_bytes ||
            admission_sha256 != v3GlobalMemoryAdmissionSha256V1(*this)) {
            return false;
        }
        switch (reason) {
            case V3GlobalMemoryAdmissionReason::Admitted:
                return fits && candidate_payload_quote_bytes <=
                    candidate_payload_ceiling_bytes &&
                    required <= process_memory_max_bytes &&
                    exact_boundary == (required == process_memory_max_bytes);
            case V3GlobalMemoryAdmissionReason::
                    CandidatePayloadCeilingExceeded:
                return !fits && !exact_boundary &&
                    candidate_payload_quote_bytes >
                        candidate_payload_ceiling_bytes;
            case V3GlobalMemoryAdmissionReason::ProcessMemoryLimitExceeded:
                return !fits && !exact_boundary &&
                    candidate_payload_quote_bytes <=
                        candidate_payload_ceiling_bytes &&
                    required > process_memory_max_bytes;
            case V3GlobalMemoryAdmissionReason::ExistingReceiptIncomplete:
            case V3GlobalMemoryAdmissionReason::AccountingModelMismatch:
            case V3GlobalMemoryAdmissionReason::CandidateQuoteIncomplete:
            case V3GlobalMemoryAdmissionReason::CandidateQuoteBindingMismatch:
            case V3GlobalMemoryAdmissionReason::ArithmeticOverflow:
            case V3GlobalMemoryAdmissionReason::AlreadyConsumed:
                return false;
        }
        return false;
    } catch (...) {
        return false;
    }
}

V3GlobalMemoryAdmissionReceiptV1 evaluateV3GlobalMemoryAdmissionV1(
    const ExistingCapacityReceiptV1& existing,
    const CandidateBuildQuote& quote,
    std::uint64_t candidate_payload_ceiling_bytes,
    std::uint64_t fixed_memory_headroom_bytes,
    std::uint64_t process_memory_max_bytes) noexcept {
    V3GlobalMemoryAdmissionReceiptV1 r;
    try {
        r.existing_capacity_receipt_sha256 = existing.receipt_sha256;
        r.existing_capacity_accounting_model_sha256 =
            existing.accounting_model_sha256;
        r.candidate_quote_contract_fingerprint = quote.contractFingerprint();
        r.selected_plan_fingerprint = quote.plan_fingerprint;
        r.accounted_existing_capacity_bytes =
            existing.accounted_existing_capacity_bytes;
        r.candidate_payload_quote_bytes = quote.maximum_payload_bytes;
        r.candidate_payload_ceiling_bytes = candidate_payload_ceiling_bytes;
        r.fixed_memory_headroom_bytes = fixed_memory_headroom_bytes;
        r.process_memory_max_bytes = process_memory_max_bytes;
        const ExistingCapacityAccountingModelArtifactV1 model =
            existingCapacityAccountingModelArtifactV1();
        if (!existing.invariantHolds()) {
            r.reason = V3GlobalMemoryAdmissionReason::ExistingReceiptIncomplete;
            return r;
        }
        if (!model.complete() || existing.accounting_model_sha256 != model.sha256) {
            r.reason = V3GlobalMemoryAdmissionReason::AccountingModelMismatch;
            return r;
        }
        if (!quote.complete || quote.overflowed ||
            !candidateBuildQuoteContractIsValid(quote)) {
            r.reason = V3GlobalMemoryAdmissionReason::CandidateQuoteIncomplete;
            return r;
        }
        if (existing.selected_candidate_plan_detail.selected_plan_fingerprint !=
                quote.plan_fingerprint ||
            existing.validated_snapshot_detail.snapshot_token !=
                quote.validation_snapshot_token) {
            r.reason = V3GlobalMemoryAdmissionReason::CandidateQuoteBindingMismatch;
            return r;
        }
        if (!checkedAdd(
                r.accounted_existing_capacity_bytes,
                r.candidate_payload_quote_bytes,
                r.required_capacity_bytes) ||
            !checkedAccumulate(
                r.required_capacity_bytes,
                r.fixed_memory_headroom_bytes)) {
            r.overflowed = true;
            r.reason = V3GlobalMemoryAdmissionReason::ArithmeticOverflow;
            return r;
        }
        if (quote.maximum_payload_bytes > candidate_payload_ceiling_bytes) {
            r.complete = true;
            r.reason =
                V3GlobalMemoryAdmissionReason::CandidatePayloadCeilingExceeded;
            r.admission_sha256 = v3GlobalMemoryAdmissionSha256V1(r);
            return r;
        }
        if (r.required_capacity_bytes > r.process_memory_max_bytes) {
            r.complete = true;
            r.reason = V3GlobalMemoryAdmissionReason::ProcessMemoryLimitExceeded;
            r.admission_sha256 = v3GlobalMemoryAdmissionSha256V1(r);
            return r;
        }
        r.complete = true;
        r.fits = true;
        r.reason = V3GlobalMemoryAdmissionReason::Admitted;
        r.exact_boundary =
            r.required_capacity_bytes == r.process_memory_max_bytes;
        r.admission_sha256 = v3GlobalMemoryAdmissionSha256V1(r);
        if (!r.invariantHolds()) {
            r.complete = false;
            r.fits = false;
        }
        return r;
    } catch (...) {
        r.complete = false;
        r.fits = false;
        return r;
    }
}

std::string canonicalV3GlobalMemoryAdmissionCoreV1(
    const V3GlobalMemoryAdmissionReceiptV1& receipt) {
    return canonicalJsonBytes(admissionCoreJson(receipt));
}

std::string v3GlobalMemoryAdmissionSha256V1(
    const V3GlobalMemoryAdmissionReceiptV1& receipt) {
    return sha256Hex(canonicalV3GlobalMemoryAdmissionCoreV1(receipt));
}

#ifdef DSM_TCC_V3_TESTING
V3GlobalMemoryAdmissionReceiptV1
evaluateV3GlobalMemoryAdmissionValuesForTesting(
    std::uint64_t accounted_existing_capacity_bytes,
    std::uint64_t candidate_payload_quote_bytes,
    std::uint64_t candidate_payload_ceiling_bytes,
    std::uint64_t fixed_memory_headroom_bytes,
    std::uint64_t process_memory_max_bytes) {
    V3GlobalMemoryAdmissionReceiptV1 r;
    r.existing_capacity_receipt_sha256 = std::string(64U, '1');
    r.existing_capacity_accounting_model_sha256 = std::string(64U, '2');
    r.candidate_quote_contract_fingerprint = 1;
    r.selected_plan_fingerprint = 1;
    r.accounted_existing_capacity_bytes =
        accounted_existing_capacity_bytes;
    r.candidate_payload_quote_bytes = candidate_payload_quote_bytes;
    r.candidate_payload_ceiling_bytes = candidate_payload_ceiling_bytes;
    r.fixed_memory_headroom_bytes = fixed_memory_headroom_bytes;
    r.process_memory_max_bytes = process_memory_max_bytes;
    if (!checkedAdd(
            accounted_existing_capacity_bytes,
            candidate_payload_quote_bytes,
            r.required_capacity_bytes) ||
        !checkedAccumulate(
            r.required_capacity_bytes,
            fixed_memory_headroom_bytes)) {
        r.overflowed = true;
        r.reason = V3GlobalMemoryAdmissionReason::ArithmeticOverflow;
        return r;
    }
    r.complete = true;
    if (candidate_payload_quote_bytes > candidate_payload_ceiling_bytes) {
        r.reason =
            V3GlobalMemoryAdmissionReason::CandidatePayloadCeilingExceeded;
    } else if (r.required_capacity_bytes > process_memory_max_bytes) {
        r.reason =
            V3GlobalMemoryAdmissionReason::ProcessMemoryLimitExceeded;
    } else {
        r.fits = true;
        r.reason = V3GlobalMemoryAdmissionReason::Admitted;
        r.exact_boundary =
            r.required_capacity_bytes == process_memory_max_bytes;
    }
    r.admission_sha256 = v3GlobalMemoryAdmissionSha256V1(r);
    return r;
}
#endif

ExistingCapacityAdmissionSealV1::ExistingCapacityAdmissionSealV1(
    ExistingCapacityReceiptV1 existing,
    V3GlobalMemoryAdmissionReceiptV1 admission)
    : existing_(std::move(existing)), admission_(std::move(admission)) {}

const ExistingCapacityReceiptV1&
ExistingCapacityAdmissionSealV1::existing() const noexcept {
    return existing_;
}

const V3GlobalMemoryAdmissionReceiptV1&
ExistingCapacityAdmissionSealV1::admission() const noexcept {
    return admission_;
}

bool ExistingCapacityAdmissionSealV1::consumableFor(
    const CandidateBuildQuote& quote) const noexcept {
    return !consumed_ && existing_.invariantHolds() &&
        admission_.invariantHolds() && admission_.fits &&
        admission_.existing_capacity_receipt_sha256 == existing_.receipt_sha256 &&
        admission_.existing_capacity_accounting_model_sha256 ==
            existing_.accounting_model_sha256 &&
        admission_.candidate_quote_contract_fingerprint ==
            quote.contractFingerprint() &&
        admission_.selected_plan_fingerprint == quote.plan_fingerprint &&
        admission_.candidate_payload_quote_bytes == quote.maximum_payload_bytes;
}

bool ExistingCapacityAdmissionSealV1::consumed() const noexcept {
    return consumed_;
}

void ExistingCapacityAdmissionSealV1::consume() noexcept {
    consumed_ = true;
}

#ifdef DSM_TCC_V3_TESTING
bool checkedExistingCapacityAddProductForTesting(
    std::uint64_t initial,
    std::uint64_t count,
    std::uint64_t width,
    std::uint64_t& result) noexcept {
    return checkedAddProduct(initial, count, width, result);
}
#endif
