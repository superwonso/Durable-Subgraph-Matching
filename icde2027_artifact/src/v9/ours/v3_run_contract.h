#ifndef V3_RUN_CONTRACT_H
#define V3_RUN_CONTRACT_H

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "v3_canonical_json.h"

namespace dsm::v3 {

constexpr int kV3CompleteSuccess = 0;
constexpr int kV3CliOrSchemaError = 10;
constexpr int kV3AuthorizationOrHashError = 11;
constexpr int kV3EnvironmentError = 12;
constexpr int kV3InputOrFilterError = 13;
constexpr int kV3SnapshotValidationError = 14;
constexpr int kV3PlannerNotIntegrated = 15;
constexpr int kV3ExecutionExactnessError = 16;
constexpr int kV3ReceiptIoError = 17;

constexpr const char* kV3ProtocolArtifactPath =
    "paper/icde2027/v3_protocol/development_protocol_v2.json";
constexpr const char* kV3AuthorizationArtifactPath =
    "paper/icde2027/v3_protocol/execution_authorization_receipt.json";
constexpr const char* kV3ExecutionOutputRoot =
    "evaluation/icde2027_v3_dev_16t_20260812/runs";
constexpr const char* kV3SelectivityFormulaId =
    "label_population_over_product_query_degree_mean_vertex_activity";

class V3AuthorityError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ValidatedV3RunContract {
    CanonicalArtifact protocol;
    CanonicalArtifact authorization;
    CanonicalArtifact contract;
    std::uint64_t ordinal = 0;
    std::string matcher_receipt_path;
};

// This function is read-only.  It opens exactly the three lexical authority
// paths and never opens a declared data/query path.
ValidatedV3RunContract validateV3AuthorityFiles(
    const std::filesystem::path& run_contract_path,
    const std::filesystem::path& authorization_path);

// True if any token attempts to enter the protocol-only option namespace.
// main() uses this before legacy argument parsing or graph I/O.
bool containsV3CliToken(int argc, char* argv[]) noexcept;

// Closed protocol surface.  --check-v3-run-contract is read-only and may
// return success after exact authority validation.  --v3-run-contract enters
// the typed snapshot/planner/exact-count executor and publishes a matcher
// receipt only after every native invariant succeeds.
int runClosedV3Cli(int argc, char* argv[]) noexcept;

} // namespace dsm::v3

#endif // V3_RUN_CONTRACT_H
