# Witness-backed Bitcoin path queries

These queries were sampled deterministically from
`Dataset/bitcoin-temporal-timeinstance-unique-1w_filtered.txt` with label seed
42. They provide controlled inputs for the query-size, duration, and combined
paper-axis experiments; they are not the unreleased queries used for the
paper's published bars.

## Generation

The sampler was built and invoked from the repository root:

```bash
g++ -std=c++17 -O3 -DNDEBUG -pthread \
  benchmark/query_sampler.cpp ours/Utils.cpp -o /tmp/dsm_query_sampler

/tmp/dsm_query_sampler Dataset/bitcoin-temporal-timeinstance-unique-1w_filtered.txt \
  benchmark/queries/bitcoin/path3_k5.qry path3 5 42
/tmp/dsm_query_sampler Dataset/bitcoin-temporal-timeinstance-unique-1w_filtered.txt \
  benchmark/queries/bitcoin/path4_k4.qry path4 4 42
/tmp/dsm_query_sampler Dataset/bitcoin-temporal-timeinstance-unique-1w_filtered.txt \
  benchmark/queries/bitcoin/path5_k5.qry path5 5 42
```

All three sampler runs reported the same concrete witness support interval,
`[332-336]` (five consecutive snapshots). Thus `path3_k5.qry` is reused at
`k=3,4,5`; `path4_k4.qry` at `k=3,4`; and `path5_k5.qry` at `k=3,5`.

SHA-256 provenance:

```text
dataset       8cda283c93d62f5ba24b9a6623ada9ec967e19f50fc0cec7cb17b7893a4cf93c
query_sampler f1a6140d09dfe16caf890530717fbd60c03ea7b79b6c0d270556f09dbe85381b
path3_k5      92209f3095d82d94d62ea5c484c8ef81597cdc8f1e25c7753d8d2d997c0d3d14
path4_k4      f229c4c88217c6b0255a6f9d36d661b05f3b21aba63e35b52c3119f261e76842
path5_k5      0814a3aa7df5b273050b67c2a99a61eeba35137b066cd8cc534a743f2e75efdc
```

## Consecutive-duration parity check

Each row was run once with seed 42 and count-only output. The compared commands
used `original --duration-mode consecutive`, `ours --selectivity paper`, and
`ours_gopher --selectivity paper --threads 1 --statistics off`. All commands
completed without a timeout and returned the exact same count:

| Query | k | Matches | original (s) | ours (s) | ours_gopher 1T (s) |
|---|---:|---:|---:|---:|---:|
| path3_k5 | 3 | 1,124,896 | 6.19 | 5.60 | 5.88 |
| path3_k5 | 4 | 398,369 | 4.03 | 3.66 | 3.92 |
| path3_k5 | 5 | 186,048 | 3.73 | 3.69 | 3.43 |
| path4_k4 | 3 | 2,037,228 | 16.45 | 5.44 | 6.62 |
| path4_k4 | 4 | 712,828 | 9.64 | 3.65 | 4.04 |
| path5_k5 | 3 | 9,314,743 | 25.59 | 20.80 | 26.38 |
| path5_k5 | 5 | 1,078,629 | 5.92 | 4.39 | 4.19 |

The times are single GNU `time` wall-clock observations including input
loading and preprocessing. They are correctness-validation evidence, not
headline benchmark measurements; the benchmark manifest supplies repetition,
interleaving, and median reporting.
