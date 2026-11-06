#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include "../ExperimentContract.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void testVariantParser() {
    struct Case {
        const char* token;
        ExperimentVariant variant;
        const char* name;
        bool uses_dag;
        bool uses_memo;
        const char* key;
        bool static_canonicalization;
        bool interns_dag;
        const char* interning;
        const char* schema;
    };
    const Case cases[]{
        {"durable-dfs", ExperimentVariant::DurableDFS,
         "DurableDFS", false, false, "not-applicable", false, false,
         "not-applicable", "candidate_dag_fair_v1"},
        {"dag-no-memo", ExperimentVariant::DagNoMemo,
         "DAG-NoMemo", true, false, "not-applicable", false, true,
         "hash-cons", "candidate_dag_fair_v1"},
        {"dag-full-prefix-memo", ExperimentVariant::DagFullPrefixMemo,
         "DAG-FullPrefixMemo", true, true, "full-prefix", false, true,
         "hash-cons", "candidate_dag_fair_v1"},
        {"dag-dep-key-memo", ExperimentVariant::DagDepKeyMemo,
         "DAG-DepKeyMemo", true, true, "required-bindings", false, true,
         "hash-cons", "candidate_dag_fair_v1"},
        {"dag-no-intern-no-memo", ExperimentVariant::DagNoInternNoMemo,
         "DAG-NoIntern-NoMemo", true, false, "not-applicable", true,
         false, "no-intern", "candidate_dag_static_canonicalization_v1"},
        {"dag-intern-no-memo", ExperimentVariant::DagInternNoMemo,
         "DAG-Intern-NoMemo", true, false, "not-applicable", true, true,
         "hash-cons", "candidate_dag_static_canonicalization_v1"},
        {"dag-intern-full-prefix-memo",
         ExperimentVariant::DagInternFullPrefixMemo,
         "DAG-Intern-FullPrefixMemo", true, true, "full-prefix", true,
         true, "hash-cons", "candidate_dag_static_canonicalization_v1"},
        {"dag-intern-dep-key-memo",
         ExperimentVariant::DagInternDepKeyMemo,
         "DAG-Intern-DepKeyMemo", true, true, "required-bindings", true,
         true, "hash-cons", "candidate_dag_static_canonicalization_v1"}};
    for (const Case& test : cases) {
        ExperimentVariant parsed = ExperimentVariant::Legacy;
        require(parseExperimentVariant(test.token, parsed),
                "valid experiment variant was rejected");
        require(parsed == test.variant, "experiment variant parsed incorrectly");
        require(std::string(experimentVariantName(parsed)) == test.name,
                "canonical experiment variant name drifted");
        require(experimentVariantUsesDag(parsed) == test.uses_dag,
                "DAG applicability drifted");
        require(experimentVariantUsesMemo(parsed) == test.uses_memo,
                "memo applicability drifted");
        require(std::string(experimentRuntimeKeyName(parsed)) == test.key,
                "runtime-key label drifted");
        require(
            experimentVariantIsStaticCanonicalization(parsed) ==
                test.static_canonicalization,
            "static-canonicalization membership drifted");
        require(experimentVariantInternsDag(parsed) == test.interns_dag,
                "DAG interning factor drifted");
        require(std::string(experimentDagInterningName(parsed)) ==
                    test.interning,
                "DAG interning label drifted");
        require(std::string(experimentContractSchemaVersion(parsed)) ==
                    test.schema,
                "experiment schema selection drifted");
    }
    ExperimentVariant unchanged = ExperimentVariant::DurableDFS;
    require(!parseExperimentVariant("static-node-only", unchanged),
            "unsound static-only key became a production experiment arm");
    require(unchanged == ExperimentVariant::DurableDFS,
            "failed parse modified its output variant");
}

void testPhaseContract() {
    PreparedQueryPhaseTimings phases;
    phases.planning_nanoseconds = 11;
    phases.td_construction_nanoseconds = 13;
    phases.dag_compilation_nanoseconds = 17;
    phases.enumeration_nanoseconds = 19;
    phases.output_finalization_nanoseconds = 23;
    phases.dag_compilation_applicable = true;
    std::uint64_t total = 0;
    require(phases.validate(), "valid DAG phase receipt was rejected");
    require(phases.totalNanoseconds(total), "valid phase total overflowed");
    require(total == 83, "phase total is not the exact disjoint sum");

    phases.dag_compilation_applicable = false;
    std::string reason;
    require(!phases.validate(&reason),
            "non-DAG phase receipt accepted compilation work");
    require(!reason.empty(), "invalid phase receipt omitted its reason");
    phases.dag_compilation_nanoseconds = 0;
    require(phases.validate(), "valid non-DAG phase receipt was rejected");

    phases.planning_nanoseconds =
        std::numeric_limits<std::uint64_t>::max();
    phases.td_construction_nanoseconds = 1;
    require(!phases.totalNanoseconds(total),
            "overflowing phase total was accepted");
}

} // namespace

int main() {
    try {
        testVariantParser();
        testPhaseContract();
        std::cout << "Experiment contract tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Experiment contract test failure: "
                  << error.what() << '\n';
        return 1;
    }
}
