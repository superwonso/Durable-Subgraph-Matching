#!/usr/bin/env python3
"""Recompute published cell statistics from the public observations (no matcher).

Run from any working directory:
  python3 scripts/summarize_results.py --check
  python3 scripts/summarize_results.py --output-dir /tmp/icde2027-recomputed

Reference CSVs are never overwritten. Missing/TO/OOM/rejected observations are
not replaced, and campaigns, input boundaries and panel prefixes stay separate.
Times are seconds; peak RSS is MiB; quantiles use R7; CV uses sample stdev/mean.
"""
import argparse
from collections import defaultdict
import csv
import json
import math
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parents[1]


def csv_rows(path):
    with path.open(newline='') as stream:
        return list(csv.DictReader(stream))


def load_observations(campaign):
    index = json.loads((ROOT / f'results/{campaign}/observations/index.json').read_text())
    result = []
    for part in index['parts']:
        with (ROOT / part['path']).open() as stream:
            result.extend(json.loads(line) for line in stream)
    assert len(result) == index['records']
    return result


def accepted(row):
    return (row.get('status') == 'ok' and row.get('rejected', False) is False
            and row.get('answer_check') not in ('MISMATCH_OR_MISSING', 'FAIL'))


def r7(values, q):
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * q
    lo = math.floor(position)
    hi = min(lo + 1, len(ordered) - 1)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (position - lo)


def describe(values):
    result = dict(n=len(values), mean=None, median=None, min=None, max=None,
                  sample_cv=None, q1=None, q3=None, iqr=None)
    if values:
        mean = math.fsum(values) / len(values)
        q1, q3 = r7(values, .25), r7(values, .75)
        result.update(mean=mean, median=r7(values, .5), min=min(values), max=max(values),
                      sample_cv=statistics.stdev(values) / mean if len(values) > 1 and mean else None,
                      q1=q1, q3=q3, iqr=q3-q1)
    return result


def assert_value(observed, expected, label):
    if expected in ('', None):
        if observed is not None:
            raise AssertionError(f'{label}: reference missing but recomputed {observed}')
    elif observed is None or not math.isclose(observed, float(expected), rel_tol=1e-10, abs_tol=1e-12):
        raise AssertionError(f'{label}: recomputed={observed}, reference={expected}')


def verify_stats(actual, reference, prefix, label, n_key=None, cv_key=None):
    checked = 0
    for name, value in actual.items():
        key = n_key if name == 'n' and n_key else cv_key if name == 'sample_cv' and cv_key else prefix + name
        if key in reference:
            assert_value(value, reference[key], label + ':' + key)
            checked += 1
    return checked


def metric_v8(row, metric):
    if metric == 'e2e_wall_s':
        return row.get('wall_s')
    if metric == 'e2e_excl_read_filter_s':
        values = [row.get(k) for k in ('wall_s', 'read_parse_s', 'filter_s')]
        return max(0., values[0] - values[1] - values[2]) if all(x is not None for x in values) else None
    ns_key = {'online_s': 'online_ns', 'enum_plus_build_s': 'enum_plus_build_ns'}.get(metric)
    ns = row.get(ns_key) if ns_key else None
    if metric == 'online_s' and ns is None:
        ns = row.get('primary_wall_ns')
    return ns / 1e9 if ns is not None else row.get(metric)


def recompute_v8():
    groups = defaultdict(list)
    for r in load_observations('v8'):
        if r['stage'] == 'performance':
            groups[r['case_id'], r['arm'], r['boundary']].append(r)
    output, checked = [], 0
    for ref in csv_rows(ROOT / 'results/v8/summary.csv'):
        chosen = [r for r in groups[ref['case_id'], ref['arm'], ref['boundary']]
                  if r['repeat'] <= int(ref['expected_n']) and accepted(r)]
        values = [metric_v8(r, ref['metric']) for r in chosen]
        stats = describe([x for x in values if x is not None])
        label = '/'.join(ref[k] for k in ('panel', 'case_id', 'arm', 'boundary', 'metric'))
        checked += verify_stats(stats, ref, '', label, n_key='value_n')
        output.append({k: ref[k] for k in ('panel', 'case_id', 'arm', 'boundary', 'metric')} | stats)
    return output, checked


def recompute_v9():
    groups = defaultdict(list)
    for r in load_observations('v9'):
        groups[r['experiment'], r['case_id'], r['arm']].append(r)
    output, checked = [], 0
    for experiment, filename in (('E1','e1_width.csv'), ('E6','e6_union.csv'),
                                 ('E4A','e4a_bitcoin_monthly.csv'), ('E4B','e4b_so_monthly.csv')):
        for ref in csv_rows(ROOT / 'results/v9' / filename):
            prefixes = [k.removesuffix('_expected_n') for k in ref if k.endswith('_expected_n')]
            for prefix in prefixes:
                arm = prefix.removesuffix('_query')
                metric = 'query_s' if prefix.endswith('_query') else 'search_s'
                selected = [r for r in groups[experiment, ref['case_id'], arm]
                            if r['repeat'] <= int(ref[prefix + '_expected_n']) and accepted(r)]
                values = [r[metric] for r in selected if r.get(metric) is not None]
                stats = describe(values)
                label = experiment + '/' + ref['case_id'] + '/' + prefix
                checked += verify_stats(stats, ref, prefix + '_', label)
                output.append(dict(experiment=experiment, panel=ref.get('panel',''), case_id=ref['case_id'],
                                   arm=arm, metric=metric, **stats))
                for field, suffix in (('wall_s','wall'), ('peak_rss_mib','rss')):
                    vals = [r[field] for r in selected if r.get(metric) is not None and r.get(field) is not None]
                    checked += verify_stats(describe(vals), ref, prefix + '_' + suffix + '_', label)
    return output, checked


def recompute_v6():
    groups = defaultdict(list)
    for row in csv_rows(ROOT / 'results/v6/table_viii_runs.csv'):
        if row['status'] == 'ok' and row['rejected'] == 'False':
            groups[row['case_id'], row['arm']].append(row)
    output, checked = [], 0
    medians = {}
    for ref in csv_rows(ROOT / 'results/v6/table_viii_cells.csv'):
        vals = [float(r['enum_plus_build_s']) for r in groups[ref['case_id'], ref['arm']]]
        stats = describe(vals)
        checked += verify_stats(stats, ref, 'enum_plus_build_s_', ref['case_id'] + '/' + ref['arm'],
                                cv_key='enum_plus_build_s_cv')
        output.append(dict(case_id=ref['case_id'], arm=ref['arm'], metric='enum_plus_build_s', **stats))
        medians[ref['case_id'], ref['arm']] = stats['median']
    for ref in csv_rows(ROOT / 'results/v6/table_viii_summary.csv'):
        cases = sorted(cid for cid, arm in medians if arm == ref['arm'])
        ratios = [medians[cid, ref['arm']] / medians[cid, 'win_s'] for cid in cases]
        actual = dict(total_of_query_medians_s=math.fsum(medians[cid, ref['arm']] for cid in cases),
                      geomean_median_ratio_to_win_s=math.exp(math.fsum(math.log(v) for v in ratios)/len(ratios)),
                      slower_median_count=sum(v > 1 for v in ratios))
        for key, value in actual.items():
            assert_value(value, ref[key], 'TableVIII/' + ref['arm'] + '/' + key)
            checked += 1
    return output, checked


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='Verify archived summary values without writing outputs (default).')
    parser.add_argument('--output-dir', type=Path, help='Optional new directory for recomputed cell CSVs.')
    args = parser.parse_args()
    receipt = {}
    for name, function in (('v6_table_viii',recompute_v6), ('v8',recompute_v8), ('v9',recompute_v9)):
        rows, checks = function()
        receipt[name] = dict(cells=len(rows), scalar_checks=checks, status='PASS')
        if args.output_dir:
            args.output_dir.mkdir(parents=True, exist_ok=True)
            target = args.output_dir / (name + '_recomputed.csv')
            with target.open('x', newline='') as stream:
                writer = csv.DictWriter(stream, fieldnames=list(dict.fromkeys(k for row in rows for k in row)))
                writer.writeheader()
                writer.writerows(rows)
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()
