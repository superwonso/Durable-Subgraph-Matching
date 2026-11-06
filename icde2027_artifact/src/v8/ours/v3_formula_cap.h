#ifndef V3_FORMULA_CAP_H
#define V3_FORMULA_CAP_H

#include <cstdint>
#include <stdexcept>
#include <string>

#include "fixed_plan_feasibility.h"
#include "temporal_cost_v3_planner.h"
#include "v3_canonical_json.h"
#include "v3_plan_handoff.h"
#include "validated_graph_snapshot.h"

namespace dsm::v3 {

class V3FormulaCapError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct V3FormulaCapRatio {
    std::uint64_t numerator = 0;
    std::uint64_t denominator = 0;
};

struct V3FormulaCapCostWeights {
    std::uint64_t adjacency_probe = 0;
    std::uint64_t emitted_entry = 0;
    std::uint64_t index_visit = 0;
    std::uint64_t interval_intersection = 0;
    std::uint64_t reverse_lookup = 0;
    std::uint64_t semijoin_visit = 0;
};

// Closed, strongly typed projection of icde2027_v3_formula_cap_policy v1.
// canonical_policy is retained so its SHA is over the exact same canonical
// JSON bytes as the Python authority; no reconstructed policy can silently
// become a second authority.
struct V3FormulaCapPolicy {
    Json canonical_policy;
    std::string policy_sha256;
    std::string policy_id;

    std::uint64_t candidate_tree_tokens = 0;
    std::uint64_t fixed_memory_headroom_bytes = 0;
    std::uint64_t metadata_payload_bytes = 0;
    std::uint64_t metadata_receipt_tokens = 0;
    std::uint64_t process_memory_max_bytes = 0;
    std::uint64_t rescore_top_l = 0;
    std::uint64_t sample_seed = 0;

    V3FormulaCapRatio candidate_adjacency_multiplier;
    std::uint64_t candidate_payload_ceiling_bytes = 0;

    std::uint64_t complete_proposal_cap = 0;
    std::uint64_t proposal_state_expansions = 0;
    std::uint64_t planner_prefix_samples_per_plan = 0;

    V3FormulaCapRatio lazy_adjacency_fraction;
    std::uint64_t lazy_adjacency_minimum = 0;
    std::uint64_t lazy_adjacency_maximum = 0;
    std::uint64_t lazy_cache_entries = 0;
    std::uint64_t lazy_extension_probes = 0;
    std::uint64_t lazy_interval_intersections = 0;

    std::uint64_t rescore_prefix_samples_per_plan = 0;
    std::uint64_t rescore_extension_probes = 0;

    std::string graph_accounting_model_sha256;
    std::uint64_t candidate_value_bytes = 0;
    std::uint64_t index_entries_per_block = 0;
    std::uint64_t lazy_cache_entry_bytes = 0;
    std::uint64_t semijoin_visits_per_block = 0;
    std::uint64_t semijoin_visits_per_candidate_value = 0;

    std::string cost_coefficient_artifact_sha256;
    V3FormulaCapCostWeights cost_weights;
};

struct V3FormulaGraphStorageQuote {
    std::string accounting_model_sha256;
    std::uint64_t seal_peak_live_bytes = 0;
    std::uint64_t snapshot_storage_bytes = 0;
    std::uint64_t temporary_validation_bytes = 0;
};

struct V3FormulaCapInputs {
    std::uint64_t filtered_vertices = 0;
    std::uint64_t filtered_edges = 0;
    std::uint64_t interval_records = 0;
    std::uint64_t query_vertices = 0;
    std::uint64_t query_edges = 0;
    V3FormulaGraphStorageQuote graph_storage_quote;
};

// Pure, non-authorizing mapping into the native budget types.  In this v1
// projection CandidateBuildBudget::payload_memory_bytes has P semantics (the
// candidate payload ceiling), while the separate envelope below is P+H.
// Production wiring must preserve that distinction.
struct V3FormulaCapProjection {
    GraphValidationBudget graph_validation_budget;
    CandidateBuildBudget candidate_build_budget;
    std::uint64_t candidate_payload_ceiling_bytes = 0;
    std::uint64_t candidate_payload_reservation_envelope_bytes = 0;

    V3ProposalBudget proposal_budget;
    std::uint64_t planner_prefix_samples_per_plan = 0;
    V3LazyBudget lazy_budget;
    std::uint64_t lazy_emitted_entries = 0;
    V3RescoreBudget rescore_budget;
    V3MetadataBudget metadata_budget;

    std::uint64_t fixed_memory_headroom_bytes = 0;
    std::uint64_t process_memory_max_bytes = 0;
    std::uint64_t payload_ceiling_bytes = 0;
    std::uint64_t payload_reservation_envelope_bytes = 0;
};

struct V3FormulaCapMaterialization {
    Json value;
    std::string canonical_bytes;
    std::string materialized_caps_sha256;
    V3FormulaCapProjection projection;
};

V3FormulaCapPolicy validateV3FormulaCapPolicy(const Json& policy);

// The request is a closed helper object with the five Python keyword inputs
// plus graph_storage_quote.  It exists only to make parser/type rejection and
// Python/C++ differential tests mechanically checkable.
V3FormulaCapInputs parseV3FormulaCapInputs(const Json& request);

V3FormulaCapMaterialization materializeV3FormulaCaps(
    const V3FormulaCapPolicy& policy,
    const V3FormulaCapInputs& inputs);

V3FormulaCapMaterialization materializeV3FormulaCaps(
    const Json& policy,
    const Json& request);

// Recompute from the policy and derivation request, then require exact Json
// equality.  This intentionally does not trust hashes or local invariants in
// the supplied materialized object.
V3FormulaCapMaterialization verifyV3FormulaCapMaterialization(
    const Json& materialized,
    const Json& policy,
    const Json& request);

} // namespace dsm::v3

#endif // V3_FORMULA_CAP_H
