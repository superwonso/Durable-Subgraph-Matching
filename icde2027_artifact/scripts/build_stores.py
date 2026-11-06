#!/usr/bin/env python3
"""Build one W-store per actual input SHA; never merge Bitcoin input variants."""
import argparse
import json
from pathlib import Path
import subprocess
from repro_common import ROOT, bindings, cases, check_file, graph_binding, save, sha


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--case', required=True, help='Frozen case ID; cases with the same input SHA reuse a store')
    p.add_argument('--data-root', type=Path, required=True)
    p.add_argument('--build-root', type=Path, default=ROOT / 'build')
    p.add_argument('--kind', choices=['window', 'raw_bitset', 'both'], default='both')
    p.add_argument('--execute', action='store_true')
    args = p.parse_args()
    case = cases()[args.case]
    graph, bound = graph_binding(args.data_root, case)
    kinds = ['window', 'raw_bitset'] if args.kind == 'both' else [args.kind]
    for kind in kinds:
        binary = args.build_root.resolve() / ('v7/build_wstore' if kind == 'window' else 'v8/build_raw_bitset_store_v8')
        directory = args.data_root.resolve() / 'stores' / bound['sha256']
        store = directory / (kind + '.wstore')
        metrics = directory / (kind + '.json')
        command = [str(binary), 'build', '--graph', str(graph), '--dataset', case['dataset_id'],
                   '--ks', '2,3,4,5' if kind == 'window' else '1', '--store', str(store), '--metrics', str(metrics)]
        print(json.dumps(dict(graph_sha256=bound['sha256'], kind=kind, command=command), indent=2))
        if not args.execute:
            continue
        check_file(graph, bound['sha256'])
        doc = bindings(args.data_root)
        key = bound['sha256'] + ':' + kind
        if key in doc['stores']:
            check_file(Path(doc['stores'][key]['path']), doc['stores'][key]['sha256'])
            print('Already bound; not rebuilding ' + key)
            continue
        if store.exists() or metrics.exists():
            raise RuntimeError('Unbound output already exists; do not overwrite: ' + str(directory))
        directory.mkdir(parents=True, exist_ok=True)
        subprocess.run(command, check=True, cwd=directory)
        doc['stores'][key] = dict(path=str(store), sha256=sha(store), input_sha256=bound['sha256'],
                                 metrics=str(metrics), kind=kind, builder_sha256=sha(binary))
        save(args.data_root / 'binding.json', doc)


if __name__ == '__main__':
    main()
