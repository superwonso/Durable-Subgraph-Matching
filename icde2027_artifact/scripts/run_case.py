#!/usr/bin/env python3
"""Run a frozen case in one fresh process; default only prints native command.

No correctness mismatch is accepted; no retries, substitutions, or hidden warmups.
Linux GNU time/task affinity/resource limits reproduce the recorded protocol.
"""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import time
from repro_common import (ROOT, bindings, cases, check_file, graph_binding, label_paths,
                          read, save, sha, store_path)

BINARIES = {'v7': 'td_tree_direct_v7', 'v8': 'td_tree_bitset_v8',
            'v9': 'td_tree_followup_v9', 'profile_v9': 'td_tree_profile_v9'}
ARMS = ['td_raw', 'td_f2', 'tinla_raw', 'tinla_f2', 'dfs', 'probe', 'bitset',
        'win_s', 'final', 'win_s_w6', 'win_s_union']
VERIFIED = set()


def verify(path, digest):
    stat = Path(path).stat()
    key = (str(path), digest, stat.st_size, stat.st_mtime_ns)
    if key not in VERIFIED:
        check_file(path, digest)
        VERIFIED.add(key)


def command(case, spec, args, folder, verify_inputs=False):
    arm = spec['arm']
    baseline = arm.startswith(('td_', 'tinla_'))
    snapshot = spec.get('snapshot') or ('baselines' if baseline else 'v9' if arm in ('win_s_w6', 'win_s_union') else 'v8' if arm == 'bitset' else 'v7')
    binary_name = ('tinla_baseline_v8' if arm.startswith('tinla') else 'td_tree_baseline_v8') if snapshot == 'baselines' else BINARIES[snapshot]
    binary = (args.build_root / snapshot / binary_name).resolve()
    boundary = spec.get('boundary', 'raw' if arm.endswith('_raw') else 'prepared')
    graph, bound = graph_binding(args.data_root, case, boundary)
    query = ROOT / case['inputs']['query']['path']
    if verify_inputs:
        verify(graph, bound['sha256'])
        verify(query, case['inputs']['query']['sha256'])
    label_args = []
    if case['label_mode'] == 'explicit':
        maps = label_paths(args.data_root, case, boundary)
        if verify_inputs:
            for path, digest in maps:
                verify(path, digest)
            role = 'raw' if boundary in ('raw', 'raw_f2') else 'prepared'
            local = bindings(args.data_root)['labels'].get(case['case_id'] + ':' + role)
            if local and local['graph_sha256'] != bound['sha256']:
                raise RuntimeError('Labels belong to a different graph input SHA')
        label_args = ['--global-label-map', str(maps[0][0]), '--query-label-projection', str(maps[1][0])]
    cmd = [str(binary), str(graph), str(query), str(case['k'])]
    if baseline:
        cmd += [str(case['label_seed']), '--count-only', '--threads', '16', '--duration-mode',
                spec.get('duration', 'consecutive'), '--label-mode', 'stable-id', '--prefilter',
                'on' if boundary == 'raw_f2' else 'off', '--output-dir', str(folder), *label_args]
    else:
        cmd += label_args or [str(case['label_seed']), '--label-mode', 'stable-id']
        cmd += ['--engine', 'gopher', '--selectivity', 'paper', '--order', 'paper', '--parallel', 'roots',
                '--threads', '16', '--count-only', '--experiment-variant', 'durable-dfs', '--dag-passes', 'none',
                '--direct-p4', 'off' if arm == 'dfs' else 'on',
                '--window-algebra', 'off' if arm in ('dfs', 'probe') else 'on', '--window-build', 'eager',
                '--early-prune', 'on', '--statistics', 'off', '--runtime-key-diagnostics', 'off',
                '--adaptive-diagnostics', 'off', '--neighbor-copy-timing', 'off',
                '--ops-counters', 'on' if spec.get('counters') else 'off', '--result-digest', 'off',
                '--output-dir', str(folder), '--metrics', str(folder / 'metrics.json')]
        if boundary == 'prepared' and arm not in ('dfs', 'probe'):
            kind = 'raw_bitset' if arm == 'bitset' else 'window'
            store = store_path(args.data_root, bound['sha256'], kind)
            if verify_inputs and not store.is_file():
                raise RuntimeError('Missing store; run build_stores.py: ' + str(store))
            cmd += ['--wstore', str(store)]
        if arm == 'bitset':
            if boundary != 'prepared':
                raise RuntimeError('Frozen bitset arm requires prepared input')
            cmd += ['--raw-bitset', 'on']
        if arm == 'final':
            cmd += ['--bindex', 'always', '--bindex-alpha', '0.5', '--bindex-c', '1',
                    '--bindex-block-size', '16', '--bindex-min-size', '64']
        if arm == 'win_s_w6':
            cmd += ['--window-pad-raw-words', 'on']
        if arm == 'win_s_union':
            cmd += ['--window-block-union', 'on']
    return cmd, baseline, bound


def swap():
    return {a: int(b) for a, b in (s.split() for s in Path('/proc/vmstat').read_text().splitlines())
            if a in ('pswpin', 'pswpout')}


def execute(cmd, folder, timeout, args):
    affinity = set(range(16))
    if not affinity <= os.sched_getaffinity(0):
        raise RuntimeError('Required CPUs 0-15 unavailable; use a suitable host (do not silently change protocol)')
    smt = Path('/sys/devices/system/cpu/smt/active')
    if smt.exists() and smt.read_text().strip() != '0':
        raise RuntimeError('Recorded protocol requires SMT off; change host setup before executing')
    before = swap()
    wrapper = ['/usr/bin/time', '-f', 'max_rss_kb=%M', '-o', str(folder / 'child_rss.txt'), *cmd]
    def setup():
        os.setsid()
        os.sched_setaffinity(0, affinity)
        limit = 28672 * 1024 * 1024
        resource.setrlimit(resource.RLIMIT_AS, (limit, limit))
    save(folder / 'command.json', dict(native_command=cmd, wrapper=wrapper, timeout_s=timeout,
         binary_sha256=sha(cmd[0]), affinity=sorted(affinity), threads=16, memory_limit_mib=28672), exclusive=True)
    start = time.monotonic_ns()
    timed_out = waited = False
    with (folder / 'stdout.txt').open('xb') as stdout, (folder / 'stderr.txt').open('xb') as stderr:
        proc = subprocess.Popen(wrapper, cwd=folder, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, preexec_fn=setup)
        try:
            while True:
                pid, status, usage = os.wait4(proc.pid, os.WNOHANG)
                if pid:
                    waited = True
                    break
                if (time.monotonic_ns() - start) / 1e9 >= timeout:
                    timed_out = True
                    try:
                        os.killpg(proc.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    _, status, usage = os.wait4(proc.pid, 0)
                    waited = True
                    break
                time.sleep(0.025)
        finally:
            if not waited:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                    os.wait4(proc.pid, 0)
                except ProcessLookupError:
                    pass
        proc.returncode = os.waitstatus_to_exitcode(status)
    wall = (time.monotonic_ns() - start) / 1e9
    after = swap()
    err = (folder / 'stderr.txt').read_text(errors='replace').lower()
    oom = any(s in err for s in ('bad_alloc', 'cannot allocate memory', 'out of memory'))
    rss = None
    for line in (folder / 'child_rss.txt').read_text().splitlines():
        if line.startswith('max_rss_kb=') and line.split('=')[1].isdigit():
            rss = int(line.split('=')[1]) / 1024
    return dict(status='timeout' if timed_out else 'oom' if oom else 'ok' if proc.returncode == 0 else 'process_error',
                wall_s=wall, returncode=proc.returncode, peak_rss_mib=rss,
                swap_delta={k: after[k] - before[k] for k in before},
                rejected=bool(usage.ru_nswap or any(after[k] != before[k] for k in before)))


def collect(folder, baseline):
    path = folder / ('baseline_metrics.json' if baseline else 'metrics.json')
    if not path.exists():
        return dict(match_count=None, metric_status='MISSING_NATIVE_JSON')
    native = read(path)
    if baseline:
        phase = native.get('phase_ns', {})
        enum, planning, relation = (phase.get(k) for k in ('enumeration', 'planning', 'index_relation'))
        search = enum
    else:
        phase = native.get('experiment_contract', {}).get('phases_nanoseconds', {})
        enum, planning, relation = (phase.get(k) for k in ('enumeration', 'planning', 'td_construction'))
        load = (native.get('v6_wstore_loading') or {}).get('load_ns', 0)
        planning = planning - load if planning is not None else None
        search = native.get('enum_plus_build_nanoseconds')
    result = dict(match_count=native.get('match_count'), count_overflow=native.get('count_overflow'),
                  counters=native.get('counters') if baseline else {k: v for k, v in native.items() if k.startswith('ops_')})
    for key, value in [('enumeration_s', enum), ('search_s', search), ('planning_s', planning), ('index_relation_s', relation)]:
        result[key] = value / 1e9 if value is not None else None
    result['query_s'] = (search + planning + relation) / 1e9 if all(v is not None for v in (search, planning, relation)) else None
    return result


def run(spec, args):
    case = cases()[spec['case_id']]
    folder = args.output.resolve() / spec['slot_id']
    cmd, baseline, bound = command(case, spec, args, folder, verify_inputs=args.execute)
    if not args.execute:
        return dict(slot_id=spec['slot_id'], dry_run=True, command=cmd, cohort=bound['cohort'])
    folder.mkdir(parents=True, exist_ok=False)
    record = dict(spec, started_utc=dt.datetime.now(dt.timezone.utc).isoformat(), input_binding=bound)
    save(folder / 'START.json', record, exclusive=True)
    record.update(execute(cmd, folder, spec.get('timeout_s', 600), args))
    save(folder / 'PROCESS_RESULT.json', record, exclusive=True)
    if record['status'] == 'ok':
        record.update(collect(folder, baseline))
        expected = spec.get('expected_count', case.get('expected_count'))
        observed = record.get('match_count')
        valid = isinstance(observed, int) and not record.get('count_overflow')
        if expected is None:
            valid = valid and spec.get('count_check_policy') == 'screen_reference'
        elif spec.get('duration') == 'cardinality':
            valid = valid and observed >= expected
        else:
            valid = valid and observed == expected
        record['expected_con_count'] = expected
        record['answer_check'] = ('SCREEN_REFERENCE_OBSERVED' if expected is None else 'PASS') if valid else 'MISMATCH_OR_MISSING'
    save(folder / 'RECORD.json', record, exclusive=True)
    return record


def add_paths(parser):
    parser.add_argument('--data-root', type=Path, required=True)
    parser.add_argument('--build-root', type=Path, default=ROOT / 'build')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--execute', action='store_true')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    add_paths(parser)
    parser.add_argument('--case', choices=sorted(cases()), required=True)
    parser.add_argument('--arm', choices=ARMS, required=True)
    parser.add_argument('--snapshot', choices=['v7', 'v8', 'v9', 'baselines', 'profile_v9'])
    parser.add_argument('--boundary', choices=['prepared', 'raw', 'raw_f2'])
    parser.add_argument('--duration', choices=['consecutive', 'cardinality'], default='consecutive')
    parser.add_argument('--timeout', type=int, default=600)
    args = parser.parse_args()
    spec = dict(case_id=args.case, arm=args.arm, snapshot=args.snapshot, repeat=1,
                boundary=args.boundary or ('raw' if args.arm.endswith('_raw') else 'prepared'),
                duration=args.duration, timeout_s=args.timeout,
                slot_id=args.case + '__' + args.arm + '__r01')
    result = run(spec, args)
    print(json.dumps(result, indent=2))
    if result.get('answer_check') == 'MISMATCH_OR_MISSING':
        raise SystemExit('STOP: correctness mismatch; do not use this timing')


if __name__ == '__main__':
    main()
