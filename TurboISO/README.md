# TurboISO baseline

This directory contains a static TurboISO baseline. It now preserves directed
arc semantics by using TurboISO's undirected projection only for candidate
generation and verifying every complete mapping against the original directed
arcs.

## Build and test

```powershell
.\build.ps1
.\run_tests.ps1
```

The build produces `turbo.exe` and `turbo_convert.exe`.

## Convert the project inputs

The project datasets use `src dst timestamp` triples and have no vertex labels.
The converter collapses all timestamps for each **ordered** pair into one static
directed arc, compacts sparse vertex IDs, and assigns each data vertex a random
label from A-E. IDs are sorted before seeded assignment, so seed 42 is stable
under input-line permutations. Reciprocal arcs remain distinct.

Self-loops are not emitted as matchable arcs or Turbo vertices. Their raw
endpoint IDs are nevertheless included in the seed-ordered RNG universe before
labels for active non-self endpoints are selected. This matches the label
assignment policy used by `ours` and `original` without changing the static
baseline's active vertex set.

```powershell
.\turbo_convert.exe `
  ..\Dataset\sx-superuser-temporal-1w-unique_filtered.txt `
  ..\Dataset\Query3.txt `
  .\superuser.turbo `
  .\query3.turbo `
  42
```

Query input lines are directed label pairs such as `A B`. To represent two
different query vertices with the same label, use identity-qualified tokens,
for example `left:A center:B` and `center:B right:A`.

The conversion is intentionally a static baseline: timestamps are collapsed;
no durability threshold is evaluated by TurboISO.

## Run

```powershell
.\turbo.exe .\superuser.turbo .\query3.turbo .\answers.txt
```

The result file always contains one count/timing/status summary per data/query
pair. Define `PRINT_RESULT` at build time only when every compact-ID mapping is
also needed; on large result sets that can be much slower and use substantial
disk space.

Options:

- `--time-limit seconds`: cooperative per-query deadline; `0` disables it.
- `--max-results count`: optional result cap. The default is unlimited. When a
  cap stops enumeration, stdout reports `truncated=yes`.

The default timeout is 600 seconds. It is checked inside candidate-region and
backtracking loops; it does not delay execution before matching.

## Native TurboISO format

Converted data graphs contain:

```text
t # 0
vertex_count edge_count 5
v vertex_id label_id
e directed_source directed_destination
t # -1
```

Converted queries use the same structure, with an additional edge-label count
in the header and edge label `1` on every query arc.
