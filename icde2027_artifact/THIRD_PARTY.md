# Attribution and provenance

The top-level project license is preserved in this artifact. This release
does not relicense third-party datasets or academic papers.

## Evaluated baselines

The TD-Tree baseline is the repository's corrected, paper-derived reference
implementation. The `original/` name in its source snapshot is historical;
it does not identify a verified original-author artifact.

The TiNLA baseline is a clean-room C++17 reimplementation/adaptation. It is
not represented as the original authors' released source. Consecutive and
cardinality modes must be selected explicitly when comparing semantics.

Adapter checks compare the local pre-adapter and adapted implementations.
They do not establish equivalence to an unobserved upstream author binary.
The evaluated TD-Tree baseline enables leaf time compaction and block-union
checks; the public configuration retains that behavior.

## Data and dependencies

Dataset publisher links and acquisition/preparation instructions are in
[DATASETS.md](DATASETS.md). Raw data are not redistributed in this release.
Users must comply with the respective publishers' access and use conditions.

The source snapshots use C++17 and standard threading facilities; their build
requirements are documented in [SOURCES.md](SOURCES.md). Compilers, CMake,
the operating system, and optional analysis tools are external dependencies
and are not bundled or relicensed here. Retained source notices remain with
the respective source files.

Third-party paper PDFs, downloaded source archives, local tool caches, and
unrelated experimental GPU code are not part of this artifact addition.
