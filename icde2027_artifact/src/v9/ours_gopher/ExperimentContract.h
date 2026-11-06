#ifndef EXPERIMENT_CONTRACT_H
#define EXPERIMENT_CONTRACT_H

#include <cstdint>
#include <limits>
#include <string>

// Closed names for the four internal arms used by the Candidate-DAG E2
// factorial panel.  Only the NoIntern-NoMemo versus Intern-NoMemo pair
// isolates interning; the memo arms retain the same Intern compiler contract.
// Legacy keeps the pre-existing, independently configurable CLI behavior and
// is never emitted as an experiment arm.
enum class ExperimentVariant {
    Legacy,
    DurableDFS,
    DagNoMemo,
    DagFullPrefixMemo,
    DagDepKeyMemo,
    DagNoInternNoMemo,
    DagInternNoMemo,
    DagInternFullPrefixMemo,
    DagInternDepKeyMemo
};

inline const char* experimentVariantName(ExperimentVariant variant) {
    switch (variant) {
        case ExperimentVariant::Legacy:
            return "legacy-cli";
        case ExperimentVariant::DurableDFS:
            return "DurableDFS";
        case ExperimentVariant::DagNoMemo:
            return "DAG-NoMemo";
        case ExperimentVariant::DagFullPrefixMemo:
            return "DAG-FullPrefixMemo";
        case ExperimentVariant::DagDepKeyMemo:
            return "DAG-DepKeyMemo";
        case ExperimentVariant::DagNoInternNoMemo:
            return "DAG-NoIntern-NoMemo";
        case ExperimentVariant::DagInternNoMemo:
            return "DAG-Intern-NoMemo";
        case ExperimentVariant::DagInternFullPrefixMemo:
            return "DAG-Intern-FullPrefixMemo";
        case ExperimentVariant::DagInternDepKeyMemo:
            return "DAG-Intern-DepKeyMemo";
    }
    return "unknown";
}

inline bool parseExperimentVariant(
    const std::string& text,
    ExperimentVariant& variant) {
    if (text == "durable-dfs") {
        variant = ExperimentVariant::DurableDFS;
    } else if (text == "dag-no-memo") {
        variant = ExperimentVariant::DagNoMemo;
    } else if (text == "dag-full-prefix-memo") {
        variant = ExperimentVariant::DagFullPrefixMemo;
    } else if (text == "dag-dep-key-memo") {
        variant = ExperimentVariant::DagDepKeyMemo;
    } else if (text == "dag-no-intern-no-memo") {
        variant = ExperimentVariant::DagNoInternNoMemo;
    } else if (text == "dag-intern-no-memo") {
        variant = ExperimentVariant::DagInternNoMemo;
    } else if (text == "dag-intern-full-prefix-memo") {
        variant = ExperimentVariant::DagInternFullPrefixMemo;
    } else if (text == "dag-intern-dep-key-memo") {
        variant = ExperimentVariant::DagInternDepKeyMemo;
    } else {
        return false;
    }
    return true;
}

inline bool experimentVariantUsesDag(ExperimentVariant variant) {
    return variant == ExperimentVariant::DagNoMemo ||
        variant == ExperimentVariant::DagFullPrefixMemo ||
        variant == ExperimentVariant::DagDepKeyMemo ||
        variant == ExperimentVariant::DagNoInternNoMemo ||
        variant == ExperimentVariant::DagInternNoMemo ||
        variant == ExperimentVariant::DagInternFullPrefixMemo ||
        variant == ExperimentVariant::DagInternDepKeyMemo;
}

inline bool experimentVariantUsesMemo(ExperimentVariant variant) {
    return variant == ExperimentVariant::DagFullPrefixMemo ||
        variant == ExperimentVariant::DagDepKeyMemo ||
        variant == ExperimentVariant::DagInternFullPrefixMemo ||
        variant == ExperimentVariant::DagInternDepKeyMemo;
}

inline bool experimentVariantIsStaticCanonicalization(
    ExperimentVariant variant) {
    return variant == ExperimentVariant::DagNoInternNoMemo ||
        variant == ExperimentVariant::DagInternNoMemo ||
        variant == ExperimentVariant::DagInternFullPrefixMemo ||
        variant == ExperimentVariant::DagInternDepKeyMemo;
}

inline bool experimentVariantInternsDag(ExperimentVariant variant) {
    return experimentVariantUsesDag(variant) &&
        variant != ExperimentVariant::DagNoInternNoMemo;
}

inline const char* experimentDagInterningName(
    ExperimentVariant variant) {
    if (!experimentVariantUsesDag(variant)) return "not-applicable";
    return experimentVariantInternsDag(variant)
        ? "hash-cons"
        : "no-intern";
}

inline const char* experimentContractSchemaVersion(
    ExperimentVariant variant) {
    return experimentVariantIsStaticCanonicalization(variant)
        ? "candidate_dag_static_canonicalization_v1"
        : "candidate_dag_fair_v1";
}

inline const char* experimentRuntimeKeyName(ExperimentVariant variant) {
    switch (variant) {
        case ExperimentVariant::DagFullPrefixMemo:
        case ExperimentVariant::DagInternFullPrefixMemo:
            return "full-prefix";
        case ExperimentVariant::DagDepKeyMemo:
        case ExperimentVariant::DagInternDepKeyMemo:
            return "required-bindings";
        case ExperimentVariant::Legacy:
        case ExperimentVariant::DurableDFS:
        case ExperimentVariant::DagNoMemo:
        case ExperimentVariant::DagNoInternNoMemo:
        case ExperimentVariant::DagInternNoMemo:
            return "not-applicable";
    }
    return "unknown";
}

inline const char* experimentAdmissionName(ExperimentVariant variant) {
    return experimentVariantUsesMemo(variant)
        ? "eager-on-first-eligible-touch"
        : "not-applicable";
}

struct PreparedQueryPhaseTimings {
    static constexpr const char* schema_version =
        "prepared_query_phases_v1";

    std::uint64_t planning_nanoseconds = 0;
    std::uint64_t td_construction_nanoseconds = 0;
    std::uint64_t dag_compilation_nanoseconds = 0;
    std::uint64_t enumeration_nanoseconds = 0;
    std::uint64_t output_finalization_nanoseconds = 0;
    bool dag_compilation_applicable = false;

    bool totalNanoseconds(std::uint64_t& total) const noexcept {
        total = 0;
        const std::uint64_t values[]{
            planning_nanoseconds,
            td_construction_nanoseconds,
            dag_compilation_nanoseconds,
            enumeration_nanoseconds,
            output_finalization_nanoseconds};
        for (std::uint64_t value : values) {
            if (value > std::numeric_limits<std::uint64_t>::max() - total) {
                total = 0;
                return false;
            }
            total += value;
        }
        return true;
    }

    bool validate(std::string* reason = nullptr) const {
        if (!dag_compilation_applicable &&
            dag_compilation_nanoseconds != 0) {
            if (reason != nullptr) {
                *reason = "non-DAG arm has non-zero DAG compilation time";
            }
            return false;
        }
        std::uint64_t ignored = 0;
        if (!totalNanoseconds(ignored)) {
            if (reason != nullptr) {
                *reason = "prepared-query phase total overflow";
            }
            return false;
        }
        return true;
    }
};

#endif
