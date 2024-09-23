# Enhanced Durable Subgraph Matching



This repository is the implementation code about DongHyeon Cha’s Master Degree Thesis

# Experiments Spec

AMD Ryzen Threadripper PRO 5955WX

DDR4 ECC 128GB @ 3200Mhz, Octa Channal

Samsung SSD 990 Pro

Ubuntu 23.04 LTS

g++ 14.2.0, x64

# Requirements

g++ (c++17)

# Build Command

g++ -Wall -Wextra -g3 -O3 main.cpp utils.cpp grow_td_tree.cpp match_td_tree.cpp trim_td_tree.cpp decompose_query.cpp bloom_filter.cpp -o main.exe

# Compared algorithms



| Algorithm | Paper |
| --- | --- |
| DSM | https://ieeexplore.ieee.org/document/9706278 |
|  |  |

# Datasets



| Datasets | Link |
| --- | --- |
| bitcoin-temporal | https://www.cs.cornell.edu/~arb/data/temporal-bitcoin/ |
|  |  |

# References

| Elements | Link |
| --- | --- |
| TurboISO | https://github.com/bookug/siep |
|  |  |
