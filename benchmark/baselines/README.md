# Frozen pre-parallel-v2 baselines

These executables preserve the implementation state before the adaptive
parallel-structure experiment. They allow the new schedulers to be measured
against the exact old binaries even after the normal build outputs are
replaced.

| executable | structure | SHA-256 |
| --- | --- | --- |
| `td_tree_gopher_root_tasks` | CPU count-only search with one dynamically claimed task per TD-tree root | `7272818890bad847d96aa4ae89b7bbde577d9cd5b441ffe9bebadc081bc08de9` |
| `td_tree_gopher_gpu_level_frontier` | GPU two-pass count/scan/materialize execution with one block per frontier state | `9bce06c4966671ef7f63c16e863aa27985c85e4da70c413daca0c12e77e3efed` |

Both files were copied before the parallel-v2 source changes. Benchmark
manifests must still record their hashes at run time; this table is not a
substitute for runner provenance.
