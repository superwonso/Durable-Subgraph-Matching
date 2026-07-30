# Stable-ID witness queries

These directed path-3 queries are concrete nonzero witnesses for the
protocol-corrected raw-baseline versus prefiltered-proposed comparison.
The source datasets contain no native vertex labels, so every engine is run
with label seed `42` and `--label-mode stable-id`.  A label is consequently a
function of the external vertex ID and seed, and removing transient vertices
from a supplied `_filtered` file cannot shift the labels of retained vertices.

The queries were sampled from each supplied `_filtered` graph with:

```bash
benchmark/query_sampler <filtered-data> <query> path3 3 42 \
  --label-mode stable-id
```

The sampled witness supports were `[11-13]` for Superuser, `[70-72]` for
Wiki-Talk, `[21-23]` for Stack Overflow, and `[331-333]` for Bitcoin.  The
paired raw and filtered engines must return the same complete match count for
each query because the consecutive-pair filter is lossless for `k >= 2`.

| Query | SHA-256 |
|---|---|
| `sx_superuser_path3_k3.qry` | `332328c23e8c98987ebfc3fb28c2d214d6dc2639e5e2373922fd8b6ade321a87` |
| `wiki_talk_path3_k3.qry` | `ebf8c11c98d728f250232981998e928babb9b19e6a0761d9d679b23ff24a518e` |
| `sx_stackoverflow_path3_k3.qry` | `20ed2f92903c6eca2cd9e18806ef91285f3df9757409d9bf827f1b64e7318232` |
| `bitcoin_path3_k3.qry` | `94ad2c9511f13e14a3f2d9e8fc1dfb73a64ca21c9420764bceaaa3fff7254197` |
