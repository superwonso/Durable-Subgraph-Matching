#!/usr/bin/env python3
"""Prepare inputs/labels, default dry-run. Source datasets are not downloaded.

Use an independently downloaded, uncompressed three-column temporal edge list.
New serialization is a new cohort; its SHA must match its own labels/stores,
not necessarily the historical serialization SHA in configs/graphs.json.
"""
import argparse
from pathlib import Path
import subprocess
import json

from repro_common import ROOT, bindings, cases, graph_binding, read, save, sha, check_file


def bind(args):
    registry = read(ROOT / 'configs/graphs.json')['graphs']
    if args.reference_sha not in registry:
        raise RuntimeError('Unknown reference identity; choose configs/graphs.json entry')
    graph = args.graph.resolve()
    print(json.dumps(dict(action='bind', graph=str(graph), reference_sha256=args.reference_sha,
                         archive_exact=args.archive_exact)))
    if not args.execute:
        return
    actual = sha(graph)
    if args.archive_exact and actual != args.reference_sha:
        raise RuntimeError('Archive-exact mode requires the historical byte SHA')
    doc = bindings(args.data_root)
    entry = dict(path=str(graph), sha256=actual, reference_sha256=args.reference_sha,
                 cohort='archive-exact' if actual == args.reference_sha else 'regenerated', bytes=graph.stat().st_size)
    old = doc['graphs'].get(args.reference_sha)
    if old and old != entry:
        raise RuntimeError('Already bound differently; use a separate data root for another cohort')
    doc['graphs'][args.reference_sha] = entry
    save(args.data_root / 'binding.json', doc)


def convert(args):
    output = args.output.resolve()
    build = args.build_root.resolve()
    binary = build / ('weekly_converter' if args.mode == 'unix-week' else 'preserve_f2' if args.mode == 'preserve-f2' else 'activity_stats')
    source = ROOT / 'scripts' / (binary.name + '.cpp')
    if args.mode == 'preserve-f2':
        if args.retained is None:
            raise RuntimeError('preserve-f2 requires --retained pointing to canonical F2 graph')
        command = [str(binary), str(args.input.resolve()), str(args.retained.resolve()), str(output / 'p00.txt')]
    elif args.mode == 'unix-week':
        command = [str(binary), str(args.input.resolve()), str(output / 'with_duplicates.txt'),
                   str(output / 'raw.txt'), '10080']
    else:
        command = [str(binary), str(args.input.resolve()), str(output), args.dataset,
                   args.mode, 'emit' if not args.stats_only else 'stats']
    compile_cmd = ['c++', '-O3', '-std=c++17', str(source), '-o', str(binary)]
    print(json.dumps(dict(compile=compile_cmd, command=command), indent=2))
    if not args.execute:
        return
    if output.exists() and any(output.iterdir()):
        raise RuntimeError('Output must be new/empty; do not overwrite a previous preparation')
    output.mkdir(parents=True, exist_ok=True)
    build.mkdir(parents=True, exist_ok=True)
    subprocess.run(compile_cmd, check=True)
    subprocess.run(command, check=True)
    files = {p.name: dict(sha256=sha(p), bytes=p.stat().st_size)
             for p in output.iterdir() if p.suffix == '.txt' and p.name != 'with_duplicates.txt'}
    save(output / 'PREPARATION.json', dict(input_sha256=sha(args.input), mode=args.mode,
         dataset=args.dataset, command=command, files=files,
         note='New cohort. weekly accepts already bucketed signed snapshots; unix-week uses first-record origin.'))


def labels(args):
    chosen = list(cases().values()) if args.case == 'all' else [cases()[args.case]]
    chosen = [c for c in chosen if c['label_mode'] == 'explicit']
    print(json.dumps(dict(action='labels', cases=[c['case_id'] for c in chosen], boundary=args.boundary)))
    if not args.execute:
        return
    import label_metadata as meta
    doc = bindings(args.data_root)
    cache = {}
    for case in chosen:
        graph, bound = graph_binding(args.data_root, case, args.boundary)
        check_file(graph, bound['sha256'])
        count, seed = case['label_cardinality'], case['label_seed']
        key = (bound['sha256'], count, seed)
        map_path = args.data_root.resolve() / 'labels' / bound['sha256'] / f'c{count:03d}_seed{seed}.json'
        receipt_path = map_path.with_suffix('.receipt.json')
        if key not in cache:
            if map_path.exists() and receipt_path.exists():
                receipt = read(receipt_path)
                check_file(map_path, receipt['file_sha256'])
            else:
                if map_path.exists():
                    raise RuntimeError('Unfinished label map; do not overwrite: ' + str(map_path))
                vertices = set()
                with graph.open() as stream:
                    for line in stream:
                        if not line.strip() or line.lstrip().startswith('#'):
                            continue
                        row = line.split()
                        vertices.update((int(row[0]), int(row[1])))
                mapping, scheme = meta.stable_id_label_map(vertices, seed=seed, label_count=count)
                receipt = meta.write_explicit_label_map(map_path, mapping, scheme).as_json()
                save(receipt_path, receipt, exclusive=True)
                del vertices, mapping
            cache[key] = receipt
        receipt = cache[key]
        projection = read(ROOT / case['inputs']['query_label_projection']['path'])
        projection['global_label_map_file_sha256'] = receipt['file_sha256']
        projection['global_label_map_mapping_digest'] = receipt['mapping_digest']
        # Class IDs/order, query bytes and query SHA remain frozen.
        projection_path = args.data_root.resolve() / 'projections' / args.boundary / (case['case_id'] + '.json')
        if projection_path.exists() and read(projection_path) != projection:
            raise RuntimeError('Existing projection differs; use a new cohort directory')
        save(projection_path, projection)
        role = 'raw' if args.boundary in ('raw', 'raw_f2') else 'prepared'
        doc['labels'][case['case_id'] + ':' + role] = {
            'graph_sha256': bound['sha256'],
            'global_label_map': dict(path=str(map_path), sha256=receipt['file_sha256']),
            'query_label_projection': dict(path=str(projection_path), sha256=sha(projection_path))}
        save(args.data_root / 'binding.json', doc)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    p = sub.add_parser('convert')
    p.add_argument('--input', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--mode', choices=['unix-week', 'weekly', 'month', 'day', 'preserve-f2'], required=True)
    p.add_argument('--retained', type=Path, help='F2 edge membership input for preserve-f2 mode')
    p.add_argument('--dataset', required=True)
    p.add_argument('--stats-only', action='store_true')
    p.add_argument('--build-root', type=Path, default=ROOT / 'build/preparation')
    p.set_defaults(func=convert)
    p = sub.add_parser('bind')
    p.add_argument('--graph', type=Path, required=True)
    p.add_argument('--reference-sha', required=True)
    p.add_argument('--archive-exact', action='store_true')
    p.add_argument('--data-root', type=Path, required=True)
    p.set_defaults(func=bind)
    p = sub.add_parser('labels')
    p.add_argument('--case', required=True, help='Frozen case ID, or all (may need large memory)')
    p.add_argument('--data-root', type=Path, required=True)
    p.add_argument('--boundary', choices=['prepared', 'raw'], default='prepared')
    p.set_defaults(func=labels)
    for p in sub.choices.values():
        p.add_argument('--execute', action='store_true', help='Default only prints the operation')
    args = parser.parse_args()
    args.func(args)


if __name__ == '__main__':
    main()
