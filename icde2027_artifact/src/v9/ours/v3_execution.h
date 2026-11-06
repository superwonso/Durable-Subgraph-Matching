#ifndef V3_EXECUTION_H
#define V3_EXECUTION_H

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "fixed_plan_feasibility.h"
#include "temporal_cost_v3_planner.h"
#include "v3_input.h"
#include "validated_graph_snapshot.h"

namespace dsm::v3 {

struct ValidatedV3RunContract;

enum class V3Arm : std::uint8_t {
    IncumbentI,
    GateOnO,
    GateOffF
};

// The current authority schema carries one concrete numeric_caps object per
// ordinal.  It does not yet carry the globally selected formula parameters
// required to derive those caps from the post-filter n_f/m_f/q authority.
// Keeping that limitation in the typed projection prevents these values from
// being mistaken for matcher-verified formula materialization evidence.
enum class V3NumericCapBindingMode : std::uint8_t {
    ContractMaterializedWithoutFormulaDerivation,
    // Reserved for the not-yet-authorized protocol-V3 projection. Merely
    // setting this enum is not authority; protocol/hash/materialization gates
    // must project it together with the build-bound model SHA.
    PostFilterFormulaDerivedCheckedU64V1
};

struct V3ResourceSpec {
    std::vector<std::uint64_t> affinity_cpu_set;
    std::uint64_t memory_max_bytes = 0;
    std::uint64_t memory_swap_max_bytes = 0;
    std::uint64_t smt_active_required = 0;
    std::uint64_t threads_requested = 0;
    std::uint64_t threads_used_required = 0;
    std::uint64_t timeout_seconds = 0;
};

struct V3ExecutionSpec {
    V3Arm arm = V3Arm::IncumbentI;
    V3NumericCapBindingMode numeric_cap_binding_mode =
        V3NumericCapBindingMode::ContractMaterializedWithoutFormulaDerivation;
    std::uint64_t ordinal = 0;
    std::string arm_id;
    std::string planner_policy;
    std::string profitability_gate;
    bool profitability_gate_enabled = false;
    int minimum_duration = 0;
    std::uint32_t label_seed = 0;
    V3InputFileAuthority data;
    V3InputFileAuthority query;
    V3QueryParsedAuthority query_parsed;
    V3ResourceSpec resources;
    GraphValidationBudget graph_validation_budget;
    CandidateBuildBudget candidate_build_budget;
    V3ProposalBudget proposal_budget;
    V3LazyBudget lazy_budget;
    V3RescoreBudget rescore_budget;
    std::uint64_t metadata_payload_bytes = 0;
    std::uint64_t metadata_receipt_tokens = 0;
    std::string existing_capacity_accounting_model_sha256;
    std::string run_contract_path;
    std::string authorization_path;
    std::string matcher_receipt_path;
    std::string executable_sha256;
    std::string source_bundle_sha256;
    std::string parameter_bundle_sha256;
    std::string compiler_identity_sha256;
    std::string runner_sha256;
    std::string validator_sha256;
};

struct V3EnvironmentObservation {
    std::vector<std::uint64_t> affinity_cpu_set;
    std::uint64_t smt_active = 0;
    std::string cgroup_path;
    std::string cgroup_id;
    std::uint64_t memory_max_bytes = 0;
    std::uint64_t memory_swap_max_bytes = 0;
};

class ClassifiedV3Error : public std::runtime_error {
public:
    ClassifiedV3Error(int exit_code, std::string message);
    int exitCode() const noexcept;

private:
    int exit_code_;
};

V3ExecutionSpec projectV3ExecutionSpec(
    const ValidatedV3RunContract& authority);

V3EnvironmentObservation observeV3Environment();
void requireV3Environment(
    const V3EnvironmentObservation& observed,
    const V3ResourceSpec& required);

// Complete closed CPU execution.  On every nonzero return no success receipt
// is intentionally created (a final link already published before a later
// durability/reload failure is preserved by the publisher).
int executeValidatedV3Run(
    const ValidatedV3RunContract& authority,
    const std::string& cli_receipt_path) noexcept;

#ifdef DSM_TCC_V3_TESTING
// Synthetic-only dependency injection: authority was constructed by the test
// and environment/executable checks are supplied explicitly.  Input parsing,
// snapshot, arm execution, exact enumeration, receipt assembly and atomic
// publication are the production implementations.
int executeProjectedV3RunForTesting(
    const ValidatedV3RunContract& authority,
    const V3ExecutionSpec& spec,
    const V3EnvironmentObservation& environment) noexcept;
#endif

} // namespace dsm::v3

#endif // V3_EXECUTION_H
