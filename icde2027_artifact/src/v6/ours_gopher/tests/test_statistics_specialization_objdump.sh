#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "Usage: $0 <objdump> <probe-executable> <core-archive>" >&2
    exit 2
fi

objdump_tool="$1"
probe_executable="$2"
core_archive="$3"

if [[ ! -x "$objdump_tool" ]]; then
    echo "objdump is not executable: $objdump_tool" >&2
    exit 2
fi

extract_symbol() {
    local artifact="$1"
    local symbol_fragment="$2"
    "$objdump_tool" -drC "$artifact" | awk -v pattern="$symbol_fragment" '
        /^[[:xdigit:]]+ <.*>:/ {
            selected = index($0, pattern) > 0
        }
        selected { print }
    '
}

require_symbol() {
    local artifact="$1"
    local symbol_fragment="$2"
    local body
    body="$(extract_symbol "$artifact" "$symbol_fragment")"
    if [[ -z "$body" ]]; then
        echo "missing disassembly symbol: $symbol_fragment" >&2
        exit 1
    fi
    printf '%s' "$body"
}

reject_statistics_hooks() {
    local artifact="$1"
    local symbol_fragment="$2"
    local body
    body="$(require_symbol "$artifact" "$symbol_fragment")"
    if grep -Eq \
        'steady_clock::now|SearchStatisticsCollector::(enabled|add|set)|record(Operator|Cache)|ExactRuntimeKeyCollector' \
        <<<"$body"; then
        echo "statistics-OFF symbol retained a diagnostic hook: $symbol_fragment" >&2
        exit 1
    fi
}

probe_body="$(require_symbol \
    "$probe_executable" \
    'gopherStatisticsOffAtomicProbe')"
if grep -Eq 'call|lock|xadd|xchg|cmpxchg' <<<"$probe_body"; then
    echo "statistics-OFF atomic probe was not erased" >&2
    exit 1
fi
if ! grep -Eq '[[:space:]]ret[q]?[[:space:]]*$' <<<"$probe_body"; then
    echo "statistics-OFF atomic probe did not reduce to a return" >&2
    exit 1
fi

for symbol in \
    'DirectEvaluator::structuralCandidatesSpecialized<false>' \
    'DirectEvaluator::temporalSupportSpecialized<false>' \
    'DagExecutor::evaluateCandidateNode<false, false>' \
    'DagExecutor::evaluateIntervalNode<false, false>' \
    'DagExecutor::evaluateLongestNode<false, false>' \
    'DagExecutor::structuralCandidatesSpecialized<false, false>' \
    'DagExecutor::temporalSupportSpecialized<false, false>'
do
    reject_statistics_hooks "$core_archive" "$symbol"
done

# Worker bodies retain scheduler atomics and locks, but statistics-OFF must
# not retain worker timing or SearchStatistics hooks.
worker_bodies="$($objdump_tool -drC "$core_archive" | awk '
    /^[[:xdigit:]]+ <.*>:/ {
        selected = index($0, "runSpecialized<false, false>") > 0 &&
            index($0, "::_M_run()") > 0
    }
    selected { print }
')"
if [[ -z "$worker_bodies" ]]; then
    echo "missing statistics-OFF worker disassembly" >&2
    exit 1
fi
if grep -Eq \
    'steady_clock::now|SearchStatisticsCollector::(enabled|add|set)|workerMeasurement|record(Operator|Cache)|TDTree::candidatesForParent' \
    <<<"$worker_bodies"; then
    echo "statistics-OFF worker retained diagnostic timing/hooks" >&2
    exit 1
fi

gopher_off_bodies="$($objdump_tool -drC "$core_archive" | awk '
    /^[[:xdigit:]]+ <.*>:/ {
        selected = index($0, "GopherMatcher::runSpecialized<false, false>") > 0
    }
    selected { print }
')"
if [[ -z "$gopher_off_bodies" ]]; then
    echo "missing statistics-OFF matcher disassembly" >&2
    exit 1
fi
if grep -Eq \
    'SearchStatisticsCollector::(enabled|add|set)|record(Operator|Cache)|TDTree::candidatesForParent' \
    <<<"$gopher_off_bodies"; then
    echo "statistics-OFF matcher retained a statistics-only call" >&2
    exit 1
fi
for specialized_evaluator in \
    'DagExecutor::structuralCandidatesSpecialized<false, false>' \
    'DagExecutor::temporalSupportSpecialized<false, false>' \
    'DirectEvaluator::structuralCandidatesSpecialized<false>' \
    'DirectEvaluator::temporalSupportSpecialized<false>'
do
    if ! grep -Fq "$specialized_evaluator" <<<"$gopher_off_bodies"; then
        echo "statistics-OFF matcher missed specialized evaluator: $specialized_evaluator" >&2
        exit 1
    fi
done

echo "Statistics specialization disassembly tests passed."
