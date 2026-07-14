# Abstract

Durable subgraph matching schemes have been proposed to discover subgraph patterns that occur consecutively
across multiple snapshots of a temporal graph. However, during candidate selection, the cost of isomorphism test
increases because the entire temporal range must be scanned. In addition, decomposing the query without considering
the characteristics of temporal graphs generates many unnecessary candidates. In this paper, we propose an efficient
durable subgraph matching scheme in temporal graph. The proposed scheme reduces the search space in the candidate
selection phase by applying a filter during initial snapshot merging to remove edges that can never form durable
subgraphs. It also accelerates overall processing by decomposing the query with a selectivity formula that reflects
temporal graph characteristics so that fewer candidates are produced. 

# Evaluation Environments

Windows 11 23H2

g++(c++17) 14.2.0, x64

# Implementations

| Directory | Semantics | Build and usage |
| --- | --- | --- |
| `ours` | Algorithm 3.1 prefilter + temporal root selectivity + exact durable matching | [`ours/README.md`](ours/README.md) |
| `original` | No prefilter, original topological root selectivity + exact durable matching | [`original/README.md`](original/README.md) |
| `TurboISO` | Timestamp-collapsed static directed baseline | [`TurboISO/README.md`](TurboISO/README.md) |

On Windows, each directory provides `build.ps1` and a regression-test script.
For example:

```powershell
powershell -ExecutionPolicy Bypass -File .\ours\build.ps1
powershell -ExecutionPolicy Bypass -File .\original\run_tests.ps1
powershell -ExecutionPolicy Bypass -File .\TurboISO\run_tests.ps1
```

# Temporal Graph File Format

Each data row contains three integers, `src dst time`, and represents the
directed arc `src -> dst` at one snapshot. The datasets do not contain vertex
labels, so sorted raw vertex IDs receive reproducible random labels from
`{A, B, C, D, E}` using `std::mt19937` and seed 42 by default. Reciprocal arcs
remain distinct.

# Method

The durable implementations use:

```text
td_tree.exe <data_graph> <query_graph> <minimum_duration_k> [label_seed]
```

`Original` additionally supports `--count-only` for large result sets. The
TurboISO converter collapses timestamps by ordered pair because TurboISO is a
static, not durable, comparison baseline; see its README for the two-step
convert/run command.

# Papers 

An Efficient Durable Subgraph Matching Scheme on Temporal Graphs, D.H. Cha, M.S. dissertation, Dept. of Info. & Comm. Eng., Chungbuk National Univ., Cheongju, Chungbuk, Korea, 2025

# Compared algorithms

| Algorithm | Paper |
| --- | --- |
| DSM | Durable Subgraph Matching on Temporal Graphs |
| TiNLA | TurboISO: Towards Ultrafast and Robust Subgraph Isomorphism Search in Large Graph Databases |
| CECI | CECI: Compact Embedding Cluster Index for Scalable Subgraph Matching |
|  |  |

# Datasets



| Datasets | Link |
| --- | --- |
| bitcoin-temporal | https://www.cs.cornell.edu/~arb/data/temporal-bitcoin/ |
| sx-stackoverflow | https://snap.stanford.edu/data/sx-stackoverflow.html |
| wiki-talk-temporal | https://snap.stanford.edu/data/wiki-talk-temporal.html |
| sx-superuser | https://snap.stanford.edu/data/sx-superuser.html |
|  |  |

# References

| Elements | Link |
| --- | --- |
| TiNLA | https://github.com/bookug/siep |
|  |  |
