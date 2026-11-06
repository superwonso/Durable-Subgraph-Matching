"""Portable input bindings; no dependency on the private experiment workspace."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return json.loads(Path(path).read_text())


def save(path, value, exclusive=False):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('x' if exclusive else 'w') as f:
        json.dump(value, f, ensure_ascii=False, indent=2, allow_nan=False)
        f.write('\n')


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def cases():
    return {c['case_id']: c for c in read(ROOT / 'configs/cases.json')['cases']}


def bindings(data_root):
    p = Path(data_root) / 'binding.json'
    return read(p) if p.exists() else {'schema_version': 1, 'graphs': {}, 'labels': {}, 'stores': {}}


def bound_path(data_root, entry):
    path = Path(entry['path'])
    return path if path.is_absolute() else Path(data_root) / path


def graph_binding(data_root, case, boundary='prepared'):
    role = 'raw_graph' if boundary in ('raw', 'raw_f2') else 'temporal_graph'
    reference = case['inputs'][role]
    bound = bindings(data_root)['graphs'].get(reference['sha256'])
    if bound is None:
        bound = {'path': reference['path'], 'sha256': reference['sha256'],
                 'reference_sha256': reference['sha256'], 'cohort': 'archive-exact'}
    return bound_path(data_root, bound).resolve(), bound


def check_file(path, digest):
    if not Path(path).is_file():
        raise RuntimeError('Missing input: ' + str(path))
    actual = sha(path)
    if actual != digest:
        raise RuntimeError(f'Input SHA mismatch: {path}; expected {digest}, got {actual}')


def label_paths(data_root, case, boundary):
    role = 'raw' if boundary in ('raw', 'raw_f2') else 'prepared'
    local = bindings(data_root)['labels'].get(case['case_id'] + ':' + role)
    if local:
        return [(bound_path(data_root, local[k]).resolve(), local[k]['sha256'])
                for k in ('global_label_map', 'query_label_projection')]
    refs = case.get('raw_label_reference', case['inputs']) if role == 'raw' else case['inputs']
    result = []
    for key in ('global_label_map', 'query_label_projection'):
        entry = refs[key]
        base = ROOT if entry['path'].startswith('queries/') else Path(data_root)
        result.append(((base / entry['path']).resolve(), entry['sha256']))
    return result


def store_path(data_root, graph_digest, kind):
    entry = bindings(data_root)['stores'].get(graph_digest + ':' + kind)
    if entry:
        return bound_path(data_root, entry).resolve()
    return (Path(data_root) / 'stores' / graph_digest / (kind + '.wstore')).resolve()
