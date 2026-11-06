#ifndef DSM_E3_E4_ADAPTER_COMMON_V1_H
#define DSM_E3_E4_ADAPTER_COMMON_V1_H

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

namespace dsm::e3e4::adapter_v1 {

inline constexpr std::string_view kGovernanceId =
    "icde2027-e3-e4-adapter-plan-source-governance-v1";
inline constexpr std::string_view kChildLedgerArtifactKind =
    "dsm-e3-e4-child-phase-ledger-v1";
inline constexpr std::string_view kProjectionAuthorityReceiptPath =
    "benchmark/icde2027_e3_e4_shared_inputs_v1/"
    "query_projection_authority_receipt_v1.json";
inline constexpr std::string_view kProjectionAuthorityReceiptSha256 =
    "97e731492b704badc48ba7d02fefa018a293020b1ad8d2d3a898822af1b4478f";
inline constexpr std::string_view kProjectionAuthorityReceiptObjectSha256 =
    "3f32dda5808c4f82a8924863af1546eadcfcc33e3046ca88ba4011ae374f0974";
inline constexpr std::uint64_t kProjectionAuthorityReceiptBytes = 262686;
#ifndef DSM_ARTIFACT_REPOSITORY_ROOT
#define DSM_ARTIFACT_REPOSITORY_ROOT "."
#endif
inline constexpr std::string_view kCanonicalRepositoryRoot = DSM_ARTIFACT_REPOSITORY_ROOT;
// This successor source contract intentionally tightens the historical dry
// governance's effective-positive rule: requested and effective are both 16,
// parallel mode is "roots", and affinity is exactly CPUs 0..15.  The future
// exhaustive source receipt must bind this tightening before any product build.
inline constexpr std::uint32_t kRequiredThreads = 16;
inline constexpr std::size_t kRuntimeLabelCount = 5;

enum class ArmId {
    kTdTree,
    kTinlaReimpl,
    kCtinlaReimpl,
    kCeciDurReimpl,
    kPriorDirect,
    kFinalDurableDag,
};

enum class CommonPhase {
    kPlanning,
    kTdConstruction,
    kDagCompilation,
    kEnumeration,
    kOutputFinalization,
};

enum class ErrorCode {
    kInvalidInvocation,
    kUnsafePath,
    kOpenFailed,
    kWrongFileType,
    kWrongFileMode,
    kWrongLinkCount,
    kIdentityDrift,
    kSizeDrift,
    kHashMismatch,
    kInvalidUtf8,
    kInvalidJson,
    kSchemaMismatch,
    kCrossBindingMismatch,
    kInvalidGraph,
    kInvalidQuery,
    kGraphMapCoverageMismatch,
    kClockRegression,
    kIntegerOverflow,
    kInvalidPhaseLedger,
    kInvalidChildReceipt,
};

class AdapterError final : public std::runtime_error {
public:
    AdapterError(ErrorCode code, std::string message);
    [[nodiscard]] ErrorCode code() const noexcept;

private:
    ErrorCode code_;
};

[[nodiscard]] std::string_view armToken(ArmId arm) noexcept;
[[nodiscard]] std::string_view phaseToken(CommonPhase phase) noexcept;
[[nodiscard]] bool isInternalArm(ArmId arm) noexcept;
[[nodiscard]] std::string sha256Hex(std::string_view bytes);

struct ClosedInvocation {
    ArmId arm = ArmId::kTdTree;
    std::string prepared_graph_path;
    std::string explicit_label_map_path;
    std::string query_projection_path;
    std::string query_path;
    std::uint32_t minimum_duration_k = 0;
    std::uint32_t threads = 0;
    bool count_only = false;
    std::string child_phase_receipt_path;
    std::optional<std::string> typed_plan_path;
    std::vector<std::string> exact_argv_without_program;
    std::string exact_argv_object_sha256;
};

// Parses only the audited selector + common-argument order. Unknown, missing,
// duplicate, reordered, ambient-label, raw-input, seed, and legacy-output
// options are rejected. --typed-plan is required only for the two internal
// arms and is never opened by the pre-boundary loader below.
[[nodiscard]] ClosedInvocation parseClosedInvocation(
    int argc,
    const char* const argv[],
    ArmId expected_arm);

struct FileIdentity {
    std::string role;
    std::string path;
    std::string sha256;
    std::uint64_t bytes = 0;
    std::uint32_t mode = 0;
    std::uint64_t nlink = 0;
    std::string file_type;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::optional<std::string> object_sha256;
};

struct ExpectedRegularFile {
    std::string role;
    std::string path;
    std::string sha256;
    std::uint64_t bytes = 0;
    std::uint32_t mode = 0;
    std::uint64_t nlink = 1;
    std::optional<std::string> object_sha256;
};

class RepositoryAnchor final {
public:
    RepositoryAnchor();
    ~RepositoryAnchor();
    RepositoryAnchor(RepositoryAnchor&&) noexcept;
    RepositoryAnchor& operator=(RepositoryAnchor&&) noexcept;
    RepositoryAnchor(const RepositoryAnchor&) = delete;
    RepositoryAnchor& operator=(const RepositoryAnchor&) = delete;

    // absolute_repo_root must be lexical, normalized, and free of symlinks.
    [[nodiscard]] static RepositoryAnchor openAbsolute(
        const std::string& absolute_repo_root);
    void proveStable() const;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] const std::string& absolutePath() const noexcept;

private:
    struct Impl;
    explicit RepositoryAnchor(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

struct ValidatedInputs;

class ProjectionAuthorityGuard final {
public:
    ProjectionAuthorityGuard();
    ~ProjectionAuthorityGuard();
    ProjectionAuthorityGuard(ProjectionAuthorityGuard&&) noexcept;
    ProjectionAuthorityGuard& operator=(ProjectionAuthorityGuard&&) noexcept;
    ProjectionAuthorityGuard(const ProjectionAuthorityGuard&) = delete;
    ProjectionAuthorityGuard& operator=(const ProjectionAuthorityGuard&) = delete;

    // Opens only the compiled, non-overridable v1 projection authority tree.
    // This is not a registry override or a publication API.
    [[nodiscard]] static ProjectionAuthorityGuard open(
        const RepositoryAnchor& repository);

    void proveStable() const;
    [[nodiscard]] const FileIdentity& receiptIdentity() const noexcept;

private:
    friend struct ValidatedInputs;
    friend ValidatedInputs loadValidatedInputs(
        const RepositoryAnchor&, const ClosedInvocation&);
    struct Impl;
    explicit ProjectionAuthorityGuard(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class HeldRegularFile final {
public:
    HeldRegularFile();
    ~HeldRegularFile();
    HeldRegularFile(HeldRegularFile&&) noexcept;
    HeldRegularFile& operator=(HeldRegularFile&&) noexcept;
    HeldRegularFile(const HeldRegularFile&) = delete;
    HeldRegularFile& operator=(const HeldRegularFile&) = delete;

    void proveStable() const;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] const FileIdentity& identity() const noexcept;
    [[nodiscard]] const std::string& bytes() const noexcept;

private:
    friend struct ValidatedInputs;
    friend class ProjectionAuthorityGuard;
    friend HeldRegularFile openExpectedRegularFile(
        const RepositoryAnchor&, const ExpectedRegularFile&);
    friend ValidatedInputs loadValidatedInputs(
        const RepositoryAnchor&, const ClosedInvocation&);
    [[nodiscard]] std::size_t heldDirectoryCount() const noexcept;
    [[nodiscard]] int heldDirectoryFd(std::size_t index) const noexcept;
    struct Impl;
    explicit HeldRegularFile(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Safe generic primitive used by the fixed corpus loader and by temp-only
// path-race tests. It validates the caller-supplied full identity; it does not
// relax or override the non-configurable production corpus registry.
[[nodiscard]] HeldRegularFile openExpectedRegularFile(
    const RepositoryAnchor& repository,
    const ExpectedRegularFile& expected);

struct RawTemporalEdge {
    std::int32_t source = 0;
    std::int32_t target = 0;
    std::int32_t timestamp = 0;
};

struct RuntimeLabelRow {
    std::int32_t external_vertex_id = 0;
    std::uint32_t global_class_id = 0;
    std::uint8_t runtime_class_id =
        static_cast<std::uint8_t>(kRuntimeLabelCount);
};

struct QueryVertex {
    std::string name;
    std::uint8_t runtime_class_id = 0;
};

struct QueryDirectedEdge {
    std::uint32_t source_index = 0;
    std::uint32_t target_index = 0;
};

struct ParsedQuery {
    std::vector<QueryVertex> vertices;
    std::vector<QueryDirectedEdge> directed_edges;
};

struct GlobalLabelRow {
    std::int32_t external_vertex_id = 0;
    std::uint32_t global_class_id = 0;
};

struct ParsedExplicitLabelMap {
    std::uint32_t global_label_cardinality = 0;
    std::vector<GlobalLabelRow> mapping;
    std::string mapping_sha256;
};

struct ParsedQueryProjection {
    std::uint32_t global_label_cardinality = 0;
    std::string global_label_map_file_sha256;
    std::string global_label_map_mapping_sha256;
    std::string query_sha256;
    std::array<std::uint32_t, kRuntimeLabelCount>
        selected_global_classes_in_a_to_e_order{};
};

// Pure, strict byte-level functions are intentionally public so the hostile
// synthetic test TU can exercise schema and cross-binding failures without a
// production registry override or inclusion of this .cpp file.
[[nodiscard]] std::vector<RawTemporalEdge> parseRawTemporalGraphBytes(
    std::string_view bytes);
[[nodiscard]] ParsedExplicitLabelMap parseExplicitLabelMapBytes(
    std::string_view bytes);
[[nodiscard]] ParsedQueryProjection parseQueryProjectionBytes(
    std::string_view bytes);
[[nodiscard]] ParsedQuery parseQueryBytes(std::string_view bytes);
[[nodiscard]] std::vector<RuntimeLabelRow> validateCoverageAndProjectLabels(
    const std::vector<RawTemporalEdge>& graph,
    const ParsedExplicitLabelMap& label_map,
    const ParsedQueryProjection& projection,
    std::string_view actual_map_file_sha256,
    std::string_view actual_query_file_sha256);

struct ExcludedLoadDurations {
    std::uint64_t prepared_graph_load_ns = 0;
    std::uint64_t explicit_label_map_load_ns = 0;
    std::uint64_t query_projection_load_ns = 0;
    std::uint64_t query_load_ns = 0;
    bool all_completed_before_boundary = false;
};

struct ValidatedInputs {
    ProjectionAuthorityGuard projection_authority;
    HeldRegularFile prepared_graph;
    HeldRegularFile explicit_label_map;
    HeldRegularFile query_projection;
    HeldRegularFile query;
    std::vector<RawTemporalEdge> raw_temporal_edges;
    std::vector<RuntimeLabelRow> runtime_labels_by_external_id;
    ParsedQuery parsed_query;
    std::string global_label_map_mapping_sha256;
    ExcludedLoadDurations loads;

    void proveStable() const;
};

// Exact order and charge policy:
//  1. graph read/hash/strict three-int parse and unique vertex set;
//  2. map read/hash/strict schema/mapping digest;
//  3. projection read/hash/strict schema and map cross-binding;
//  4. query read/hash/strict A--E parse, query cross-binding, exact graph-map
//     coverage, and construction of the projected runtime-label vector.
// All four durations finish before this function returns. Typed-plan work is
// deliberately absent and must later be charged to planning_ns. No class
// cardinalities or planner statistics are returned: integrations must freshly
// scan runtime_labels_by_external_id after PhaseLedger::start and charge that
// scan to planning_ns.
[[nodiscard]] ValidatedInputs loadValidatedInputs(
    const RepositoryAnchor& repository,
    const ClosedInvocation& invocation);

[[nodiscard]] std::uint64_t steadyClockNowNanoseconds();

struct NativeInterval {
    std::uint64_t interval_index = 0;
    std::string native_name;
    CommonPhase common_phase = CommonPhase::kPlanning;
    std::uint64_t start_offset_ns = 0;
    std::uint64_t end_offset_ns = 0;
    std::uint64_t duration_ns = 0;
    bool applicable = true;
};

struct PhaseTotals {
    std::uint64_t planning_ns = 0;
    std::uint64_t td_construction_ns = 0;
    std::uint64_t dag_compilation_ns = 0;
    std::uint64_t enumeration_ns = 0;
    std::uint64_t output_finalization_ns = 0;
    std::uint64_t prepared_query_total_ns = 0;
};

struct PhaseLedgerSnapshot {
    std::vector<NativeInterval> native_intervals;
    PhaseTotals totals;
};

// Timestamps are absolute steady-clock nanoseconds. The ledger derives all
// offsets from boundary_ns, closes one interval at each transition, permits
// repeated phase intervals, and requires an explicit zero-duration,
// non-applicable DAG interval for every arm except final-durable-dag.
class PhaseLedger final {
public:
    PhaseLedger(ArmId arm, std::uint64_t boundary_ns);
    ~PhaseLedger();
    PhaseLedger(PhaseLedger&&) noexcept;
    PhaseLedger& operator=(PhaseLedger&&) noexcept;
    PhaseLedger(const PhaseLedger&) = delete;
    PhaseLedger& operator=(const PhaseLedger&) = delete;
    void start(
        CommonPhase phase,
        std::string native_name,
        bool applicable,
        std::uint64_t now_ns);
    void transition(
        CommonPhase next_phase,
        std::string next_native_name,
        bool next_applicable,
        std::uint64_t now_ns);
    [[nodiscard]] PhaseLedgerSnapshot finish(std::uint64_t now_ns);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct InputIdentitySet {
    FileIdentity prepared_graph;
    FileIdentity explicit_label_map;
    FileIdentity query_projection;
    FileIdentity query;
    std::uint32_t minimum_duration_k = 0;
    std::optional<FileIdentity> typed_plan;
};

struct ThreadObservation {
    std::uint32_t requested = 0;
    std::uint32_t effective = 0;
    std::string parallel_mode;
    std::vector<std::uint32_t> affinity_cpu_set;
};

struct ChildWallSegments {
    std::string clock;
    std::string unit;
    std::uint64_t preboundary_ns = 0;
    std::uint64_t primary_total_ns = 0;
    std::uint64_t postboundary_ns = 0;
};

// Internal-only evidence that the exact held typed-plan runtime object—not an
// independently rebuilt equivalent—reached the selected native backend.  The
// loader record itself must remain false; opening/validating a plan is not
// consumption.  External native-planner arms must serialize null instead.
struct InternalPlanConsumptionObservation {
    ArmId backend_arm = ArmId::kPriorDirect;
    bool plan_consumed = false;
    bool loader_record_plan_consumed = false;
    bool same_executable_plan_object = false;
    bool exact_executable_decomposition_passed_to_td_tree = false;
    std::string payload_object_sha256;
    std::string runtime_object_sha256;
};

struct ChildReceiptValue {
    std::string governance_id;
    ArmId arm = ArmId::kTdTree;
    InputIdentitySet input_identities;
    ExcludedLoadDurations loads;
    PhaseLedgerSnapshot ledger;
    std::uint64_t match_count = 0;
    bool count_overflow = false;
    ThreadObservation threads;
    ChildWallSegments child_wall_segments;
    std::optional<InternalPlanConsumptionObservation> plan_consumption;
};

struct CanonicalChildReceipt {
    std::string canonical_lf_bytes;
    std::string receipt_preimage_object_sha256;
    std::string object_sha256;
    std::string canonical_lf_sha256;
};

// Validates exact corpus pins and the child-only ownership/schema, then
// serializes canonical sorted-key compact UTF-8 JSON plus one LF. The structs
// remain untrusted observations: this canonicalizer does not by itself prove
// clock/affinity/core-hook provenance, and the later integration source receipt
// must bind their construction. No filesystem write or supervisor field is
// provided by this source-only common layer.
[[nodiscard]] CanonicalChildReceipt serializeCanonicalChildReceipt(
    const ChildReceiptValue& value);

}  // namespace dsm::e3e4::adapter_v1

#endif  // DSM_E3_E4_ADAPTER_COMMON_V1_H
