#!/usr/bin/env python3
"""Replay a published interleaved panel serially; no experiment starts by default."""
import argparse
from collections import Counter
import fcntl
import json
import os
from pathlib import Path
import platform
from repro_common import ROOT, read, save, sha
from run_case import add_paths, run


def cell(spec):
    return tuple(spec.get(k, '') for k in ('case_id', 'arm', 'boundary', 'stage', 'duration'))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    add_paths(p)
    p.add_argument('--panel', required=True, help='E1/E6/E3/E4A/E4B_SCREEN/E5/V8_MAIN etc., or manifest JSON path')
    args = p.parse_args()
    path = Path(args.panel)
    if not path.is_file():
        path = ROOT / 'configs/panels' / (args.panel + '.json')
    manifest = read(path)
    summary = dict(experiment=manifest['experiment'], slots=len(manifest['slots']),
                   arms=dict(Counter(s['arm'] for s in manifest['slots'])),
                   cases=len({s['case_id'] for s in manifest['slots']}), dry_run=not args.execute)
    print(json.dumps(summary, indent=2), flush=True)
    if not args.execute:
        if manifest['slots']:
            print(json.dumps(run(manifest['slots'][0], args), indent=2))
        return
    args.output.mkdir(parents=True, exist_ok=True)
    # One matcher across panel invocations sharing a data root. No parallel
    # native runs: concurrency would corrupt comparable 16T measurements.
    args.data_root.mkdir(parents=True, exist_ok=True)
    with (args.data_root / 'matcher.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        receipt = args.output / 'PANEL.json'
        identity = dict(experiment=manifest['experiment'], manifest_sha256=sha(path),
                        data_root=str(args.data_root.resolve()), build_root=str(args.build_root.resolve()))
        if receipt.exists():
            if read(receipt) != identity:
                raise RuntimeError('Output belongs to a different panel/input root/build root')
        else:
            save(receipt, identity, exclusive=True)
            cpu = Path('/proc/cpuinfo').read_text()
            save(args.output / 'ENVIRONMENT.json', dict(os=platform.platform(), python=platform.python_version(),
                cpu_model=next((s.split(':', 1)[1].strip() for s in cpu.splitlines() if s.startswith('model name')), ''),
                logical_cpu_count=os.cpu_count(), allowed_affinity=sorted(os.sched_getaffinity(0)),
                smt_active=Path('/sys/devices/system/cpu/smt/active').read_text().strip()
                    if Path('/sys/devices/system/cpu/smt/active').exists() else 'unavailable'), exclusive=True)
        journal = args.output / 'raw_runs.jsonl'
        rows = [json.loads(s) for s in journal.read_text().splitlines()] if journal.exists() else []
        known = {r['slot_id']: r for r in rows}
        first = {cell(r): r for r in rows if r['repeat'] == 1}
        stop = args.output / 'STOP.json'
        if stop.exists():
            raise RuntimeError('STOP exists; inspect correctness failure before further work')
        for spec in manifest['slots']:
            if spec['slot_id'] in known:
                continue
            initial = first.get(cell(spec))
            reason = spec.get('fixed_skip')
            if initial and initial.get('status') == 'timeout':
                reason = reason or 'NOT_ATTEMPTED_AFTER_FIRST_TO'
            if initial and spec['repeat'] > 6 and initial.get('wall_s', 0) > 60 and spec.get('reduce_60s'):
                reason = reason or 'NOT_SCHEDULED_60S_RULE'
            row = dict(spec, status=reason, attempted=False) if reason else run(spec, args)
            row['experiment'] = manifest['experiment']
            with journal.open('a') as f:
                f.write(json.dumps(row, ensure_ascii=False, allow_nan=False) + '\n')
                f.flush()
            if spec['repeat'] == 1:
                first[cell(spec)] = row
            print(json.dumps({k: row.get(k) for k in ('slot_id', 'status', 'wall_s', 'search_s', 'query_s', 'match_count')}), flush=True)
            if row.get('answer_check') == 'MISMATCH_OR_MISSING':
                save(stop, dict(reason='ANSWER_MISMATCH_OR_MISSING', slot_id=spec['slot_id']), exclusive=True)
                raise SystemExit('STOP: answer mismatch; timings are not valid evidence')
            if (args.output / 'STOP_AFTER_CURRENT').exists():
                return
        print('COMPLETE: ' + manifest['experiment'])


if __name__ == '__main__':
    main()
