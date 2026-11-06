#ifndef DSM_OURS_GOPHER_E3_E4_PLAN_AUTHORITY_V1_H
#define DSM_OURS_GOPHER_E3_E4_PLAN_AUTHORITY_V1_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "CandidateDag.h"
#include "../benchmark/e3_e4_adapter_v1/E3E4AdapterCommonV1.h"

namespace dsm::e3e4::gopher_plan_v1 {

inline constexpr std::uint32_t kSchemaVersion = 1;
inline constexpr std::string_view kArtifactKind =
    "dsm-e3-e4-typed-query-plan-v1";
inline constexpr std::string_view kContractId =
    "icde2027-e3-e4-shared-contract-v1";
inline constexpr std::string_view kAuthorityRoot =
    "benchmark/icde2027_e3_e4_typed_plan_authority_v1";
inline constexpr std::string_view kTypedPlanDirectory =
    "benchmark/icde2027_e3_e4_typed_plan_authority_v1/typed_plans";
inline constexpr std::size_t kCaseCount = 14;

// Semantic object hashes (H0) are SHA-256 over sorted-key, compact UTF-8 JSON
// with no trailing LF.  A generated/consumed plan file is exactly those same
// canonical bytes followed by one LF.  No pretty-printing alternative is part
// of v1.  This source returns bytes only; it deliberately has no writer.
inline constexpr std::string_view kSemanticObjectContentContract =
    "canonical-json-sort-keys-compact-utf8-no-lf";
inline constexpr std::string_view kTypedPlanFileContentContract =
    "canonical-json-sort-keys-compact-utf8-single-lf";

enum class TypedPlanErrorCode {
    kInvalidBoundary,
    kInvalidLiveGraph,
    kInvalidFreshPlan,
    kInvalidSelection,
    kInvalidIdentity,
    kUnsafePath,
    kOpenFailed,
    kWrongFileType,
    kWrongFileMode,
    kWrongLinkCount,
    kIdentityDrift,
    kSizeDrift,
    kInvalidUtf8,
    kInvalidJson,
    kDuplicateKey,
    kUnknownOrMissingKey,
    kWrongJsonType,
    kIntegerRange,
    kNonfiniteNumber,
    kSchemaMismatch,
    kCrossBindingMismatch,
    kHashMismatch,
};

class TypedPlanError final : public std::runtime_error {
public:
    TypedPlanError(TypedPlanErrorCode code, std::string message);
    [[nodiscard]] TypedPlanErrorCode code() const noexcept;

private:
    TypedPlanErrorCode code_;
};

struct TypedDirectedQueryFingerprint final {
    std::string schema;
    std::uint32_t vertex_count = 0;
    std::vector<std::uint32_t> vertex_labels;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> directed_edges;
    std::string sha256;
};

struct TypedRootStructure final {
    std::int32_t root = -1;
    bool connected = false;
    std::string query_tree_mode;
    std::vector<std::pair<std::uint32_t, std::uint32_t>>
        spanning_tree_edges;
    std::vector<std::pair<std::uint32_t, std::uint32_t>>
        non_tree_directed_edges;
};

struct TypedLegacyFingerprints final {
    std::string fnv_plan_id;
    std::string role;
};

// The member inventory is deliberately the exact 22-key schema.  Root null
// parenthood is represented by std::nullopt; booleans can never enter an
// integer field through this typed layer.
struct TypedPlanValue final {
    std::uint32_t schema_version = 0;
    std::string artifact_kind;
    std::string contract_id;
    std::uint32_t case_index = 0;
    std::string case_id;
    std::string dataset_id;
    std::string prepared_graph_sha256;
    std::string global_label_map_file_sha256;
    std::string global_label_map_mapping_sha256;
    std::string query_projection_file_sha256;
    std::string query_sha256;
    std::uint32_t minimum_duration_k = 0;
    std::string selectivity_mode;
    std::string order_mode;
    TypedDirectedQueryFingerprint query_fingerprint;
    TypedRootStructure root_structure;
    std::vector<std::uint32_t> operator_order;
    std::vector<std::optional<std::uint32_t>> parent_relationships;
    std::vector<std::vector<std::uint32_t>> earlier_neighbors;
    std::string root_selectivity_score_binary64_hex;
    TypedLegacyFingerprints legacy_fingerprints;
    // H0 over the canonical object containing the other 21 exact fields;
    // plan_payload_sha256 is the sole excluded field.
    std::string plan_payload_sha256;
};

// Non-forgeable-by-aggregate construction result.  The factory below performs
// the label-statistics scan, paper decomposition, and TD-free paper QueryPlan
// construction after checking the caller's steady-clock boundary.  Its three
// timestamp accessors are non-authoritative same-clock ordering diagnostics;
// they are not serialized or promoted into the child phase receipt.
class FreshPaperPlanning final {
public:
    FreshPaperPlanning(const FreshPaperPlanning&) = default;
    FreshPaperPlanning& operator=(const FreshPaperPlanning&) = default;

    [[nodiscard]] std::uint64_t boundaryNanoseconds() const noexcept;
    [[nodiscard]] std::uint64_t startedNanoseconds() const noexcept;
    [[nodiscard]] std::uint64_t finishedNanoseconds() const noexcept;
    [[nodiscard]] const std::array<std::size_t, kLabelCount>&
        labelVertexCounts() const noexcept;
    [[nodiscard]] const std::array<double, kLabelCount>&
        labelMeanVertexActiveSnapshots() const noexcept;
    [[nodiscard]] const std::string& queryTypedObjectSha256() const noexcept;
    [[nodiscard]] const std::string&
        preparedGraphFileSha256Binding() const noexcept;
    [[nodiscard]] const QueryDecomposition& decomposition() const noexcept;
    [[nodiscard]] const QueryPlan& queryPlan() const noexcept;

private:
    friend FreshPaperPlanning buildFreshPaperPlanAfterBoundary(
        const adapter_v1::ValidatedInputs&,
        const Graph&,
        const Graph&,
        std::uint64_t);
    FreshPaperPlanning() = default;

    std::uint64_t boundary_ns_ = 0;
    std::uint64_t started_ns_ = 0;
    std::uint64_t finished_ns_ = 0;
    std::array<std::size_t, kLabelCount> label_vertex_counts_{};
    std::array<double, kLabelCount>
        label_mean_vertex_active_snapshots_{};
    std::string query_typed_object_sha256_;
    std::string prepared_graph_file_sha256_binding_;
    QueryDecomposition decomposition_;
    QueryPlan query_plan_;
};

// Caller must start the common PhaseLedger planning interval at boundary_ns
// before entering this function.  Phase authority comes only from the sealed
// adapter clock readings that lexically surround this complete call and the
// later typed-plan load; the returned diagnostic start/end values neither
// determine nor prove receipt durations.
[[nodiscard]] FreshPaperPlanning buildFreshPaperPlanAfterBoundary(
    const adapter_v1::ValidatedInputs& inputs,
    const Graph& temporal_graph,
    const Graph& query_graph,
    std::uint64_t boundary_ns);

struct ValidatedTypedPlan final {
    TypedPlanValue value;
    QueryPlan executable_plan;
    QueryDecomposition executable_decomposition;
    std::string canonical_no_lf_bytes;
    std::string payload_object_sha256;
    std::string runtime_object_sha256;
};

struct GeneratedTypedPlan final {
    ValidatedTypedPlan validated;
    std::string canonical_lf_bytes;
    std::string canonical_lf_sha256;
};

// Untimed generation path.  exact_case_output_path is validated against the
// closed fourteen-case registry, but no filesystem operation is provided.
// The bytes are returned only after a strict parse round trip and full equality
// with the freshly computed live paper plan.
[[nodiscard]] GeneratedTypedPlan generateTypedPlanInMemory(
    const adapter_v1::ValidatedInputs& inputs,
    std::uint32_t minimum_duration_k,
    std::string_view exact_case_output_path,
    const Graph& query_graph,
    const FreshPaperPlanning& fresh_planning);

// Pure hostile-input entry point for exhaustive source tests.  It requires the
// exact canonical LF byte contract, exact 22 keys, all live input/case/K/mode
// bindings, a full directed-arc partition, exact binary64 score bits, payload
// H0, and full equality with fresh_planning.
[[nodiscard]] ValidatedTypedPlan parseAndValidateTypedPlanBytes(
    std::string_view canonical_lf_bytes,
    const adapter_v1::ValidatedInputs& inputs,
    std::uint32_t minimum_duration_k,
    std::string_view exact_case_path,
    const Graph& query_graph,
    const FreshPaperPlanning& fresh_planning);

struct TypedPlanConsumptionRecord final {
    std::string lexical_path;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::string file_sha256;
    std::uint64_t bytes = 0;
    std::uint32_t mode = 0;
    std::uint64_t nlink = 0;
    std::string file_type;
    std::string payload_object_sha256;
    std::string runtime_object_sha256;
    // The loader always leaves this false: validation/opening is not backend
    // consumption. Positive evidence belongs to the later runtime adapter
    // after the same executablePlan() object is handed to a backend.
    bool plan_consumed = false;
};

// Owns the held plan leaf and each no-follow directory anchor.  Both internal
// backends must receive executablePlan() from this same object; validating a
// plan and independently rebuilding a different QueryPlan is outside the API.
class ConsumedTypedPlan final {
public:
    struct Impl;

    ConsumedTypedPlan();
    ~ConsumedTypedPlan();
    ConsumedTypedPlan(ConsumedTypedPlan&&) noexcept;
    ConsumedTypedPlan& operator=(ConsumedTypedPlan&&) noexcept;
    ConsumedTypedPlan(const ConsumedTypedPlan&) = delete;
    ConsumedTypedPlan& operator=(const ConsumedTypedPlan&) = delete;

    void proveStable() const;
    [[nodiscard]] const TypedPlanValue& typedValue() const noexcept;
    [[nodiscard]] const QueryPlan& executablePlan() const noexcept;
    [[nodiscard]] const QueryDecomposition&
        executableDecomposition() const noexcept;
    [[nodiscard]] const adapter_v1::FileIdentity& identity() const noexcept;
    [[nodiscard]] const TypedPlanConsumptionRecord& record() const noexcept;

private:
    explicit ConsumedTypedPlan(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend ConsumedTypedPlan consumeTypedPlanAfterBoundary(
        const adapter_v1::RepositoryAnchor&,
        const adapter_v1::ClosedInvocation&,
        const adapter_v1::ValidatedInputs&,
        const Graph&,
        const FreshPaperPlanning&);
};

// Runtime path for only `--engine gopher --variant durable-dfs|dag-dep-key-memo`.
// Common argv parsing has already closed the selector and exact --typed-plan
// suffix.  This function rechecks those bindings, opens/reads/H1-hashes the
// exact held plan after fresh planning, strictly validates it, and returns the
// sole parsed QueryPlan/decomposition objects to carry through TD and either
// direct or DAG execution.
[[nodiscard]] ConsumedTypedPlan consumeTypedPlanAfterBoundary(
    const adapter_v1::RepositoryAnchor& repository,
    const adapter_v1::ClosedInvocation& invocation,
    const adapter_v1::ValidatedInputs& inputs,
    const Graph& query_graph,
    const FreshPaperPlanning& fresh_planning);

// This is a source-only integration layer.  Affinity observation, native phase
// provenance, child receipt writing, one-shot plan publication, exhaustive
// source receipt, and source/build identity remain explicit HOLD obligations.

}  // namespace dsm::e3e4::gopher_plan_v1

#endif  // DSM_OURS_GOPHER_E3_E4_PLAN_AUTHORITY_V1_H
