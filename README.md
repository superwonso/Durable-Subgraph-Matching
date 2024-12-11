# Enhanced Durable Subgraph Matching

This repository is the implementation code about DongHyeon Cha’s Master Degree Thesis

# Evaluation Environments

Intel Core i7-9700KF

DDR4 64GB @ 3200Mhz

Windows 11 23H2

g++ 14.2.0, x64

# Requirements

g++ (c++17)

# Build Command

g++ -Wall -Wextra -g3 -O3 -std=c++17 main.cpp query_decomposition.cpp TDTree.cpp Utils.cpp -o td_tree.o

# Method

td_tree.o {data_graph} {query_graph} {threshold(integer)}

# Papers 

동적 그래프에서 지속 가능한 서브 그래프 매칭 방법(Durable Subgraph Matching Method in Dynamic Graph), Cha et al., 한국콘텐츠학회 2024 종합학술대회, (2024) 
An Efficient Durable Subgraph Matching Scheme on Temporal Graphs, Cha et al., ICCC 2024 (KoCon.a), Vietnam, Danang, (2024)
An Efficient Durable Subgraph Matching Scheme on Temporal Graphs, D.H. Cha, M.S. dissertation, Dept. of Info. & Comm. Eng., Chungbuk National Univ., Cheongju, Chungbuk, Korea, 2025(to be published)

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
