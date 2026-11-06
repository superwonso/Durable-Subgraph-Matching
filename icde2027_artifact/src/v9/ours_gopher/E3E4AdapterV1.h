#ifndef DSM_OURS_GOPHER_E3_E4_ADAPTER_V1_H
#define DSM_OURS_GOPHER_E3_E4_ADAPTER_V1_H

#if !defined(DSM_E3E4_CORE_ONLY)
#error "E3E4AdapterV1 requires the DSM_E3E4_CORE_ONLY source profile"
#endif

#include <cstdint>
#include <string>

#include "E3E4PlanAuthorityV1.h"
#include "GopherMatcher.h"
#include "../benchmark/e3_e4_adapter_v1/E3E4AdapterCommonV1.h"

namespace dsm::e3e4::gopher_adapter_v1 {

// This pair is source-only integration evidence.  It has no main(), no child
// receipt writer, no typed-plan writer, and no authority publication API.
inline constexpr bool kSourceReceiptAuthority = false;
inline constexpr bool kBuildOrExecutionAuthority = false;
inline constexpr bool kMayWriteTypedPlan = false;
inline constexpr bool kMayWriteChildReceipt = false;

// Kept separate from the typed-plan loader record by construction.  The
// loader's record.plan_consumed remains false.  This record becomes positive
// only at the native matcher callback where the selected backend is observed
// using the exact executablePlan() object owned by ConsumedTypedPlan.
struct InternalPlanConsumptionEvidence final {
    bool plan_consumed = false;
    bool loader_record_plan_consumed = false;
    bool same_executable_plan_object = false;
    bool exact_executable_decomposition_passed_to_td_tree = false;
    adapter_v1::ArmId backend_arm = adapter_v1::ArmId::kPriorDirect;
    std::string lexical_path;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    // H1 over the exact held canonical-LF file bytes.
    std::string file_sha256;
    std::uint64_t bytes = 0;
    std::uint32_t mode = 0;
    std::uint64_t nlink = 0;
    std::string file_type;
    std::string payload_object_sha256;  // typed-plan payload H0
    std::string runtime_object_sha256;  // executable runtime-object H0
};

struct InternalSourceOnlyResult final {
    adapter_v1::ClosedInvocation invocation;
    adapter_v1::InputIdentitySet input_identities;
    adapter_v1::ExcludedLoadDurations loads;
    adapter_v1::PhaseLedgerSnapshot phase_ledger;
    // enumeration_nanoseconds/milliseconds are broad legacy diagnostics and
    // are never timing authority; phase_ledger is the sole phase authority.
    GopherMatchSummary match_summary;
    adapter_v1::ThreadObservation threads;
    InternalPlanConsumptionEvidence plan_consumption;
    adapter_v1::CanonicalChildReceipt canonical_child_receipt;
};

// Exact closed entrypoint for only:
//   --engine gopher --variant durable-dfs
//   --engine gopher --variant dag-dep-key-memo
// It captures child_start before opening the compiled canonical repository,
// performs all four common held loads before the boundary, forces count-only
// roots/16-worker execution, and returns canonical receipt bytes in memory.
// There is intentionally no caller-controlled repository root, clock, binder,
// output stream/writer, graph object, query object, or plan object.  The closed
// argv receipt path is validated as invocation data but is never opened here.
[[nodiscard]] InternalSourceOnlyResult runInternalClosedCountOnly(
    int argc,
    const char* const argv[]);

// Remaining HOLD after this source slice and its separately bound main/writer/
// hostile-test sources: the exhaustive canonical source receipt, one-shot
// shared build receipt, 14-plan authority plus live dual-arm equality run, and
// the supervising shared-contract/protocol/launch lifecycle.

}  // namespace dsm::e3e4::gopher_adapter_v1

#endif  // DSM_OURS_GOPHER_E3_E4_ADAPTER_V1_H
