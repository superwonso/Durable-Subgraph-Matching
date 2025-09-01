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

# Build Command

g++ -Wall -Wextra -g3 -O3 -std=c++17 main.cpp query_decomposition.cpp TDTree.cpp Utils.cpp -o td_tree.o

# Temporal Graph File Format

The helper that loads temporal graphs expects each line of the data file to
contain three integers: `src dst time`. During loading, labels for vertices and
edges are synthesised from the set `{A, B, C, D, E}` using a fixed random seed
to make results reproducible across runs.

# Method

td_tree.o {data_graph} {query_graph} {threshold(integer)}

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
