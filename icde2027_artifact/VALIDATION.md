# Publication checks

This document describes checks of the release packaging, not new performance
measurements or a new full correctness campaign.

On 2026-11-06, all six released source snapshots (`v6`, `v7`, `v8`, `v9`,
`baselines`, `profile_v9`) built successfully in Release mode with the public
build script. The small public smoke test then checked ten matcher
configurations on one eight-vertex directed fixture against an independent
Python mapping oracle. All ten produced the expected six answers. Two small
stores were built for this fixture. No real-data benchmark was run as part of
these packaging checks.

The source manifest distinguishes byte-preserved source files from the four
packaging changes to baseline/profile build definitions and a legacy path
constant. Source portability checks do not imply that a rebuilt executable
has the same binary hash as the original experimental executable.

The public result export retains recorded numeric observations and failures.
The results guide documents its source campaigns, exclusions, and the
calculation utilities. The old campaign observations are not relabeled as
measurements of the newly packaged build.

Publication excludes manuscript drafts, external reference papers, private
machine paths, credential files, generated executables, and raw graph/store
payloads. `ARTIFACT_FILES.json` inventories the released contents and hashes;
the inventory excludes itself. These are scoped packaging checks, not a
claim of formal verification, a security certification, or an artifact badge.
