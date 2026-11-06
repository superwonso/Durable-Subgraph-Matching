# Data and labels

Raw datasets, derived large graph files, label-map JSONs and W-stores are not
redistributed. Download the full three-column `SRC DST UNIXTS` edge list from
the original provider, respecting its terms and citation instructions. Do not
substitute a signed-rating Bitcoin graph or an interaction-type subset.

| Dataset | Official source | Choose |
|---|---|---|
| Bitcoin | [Cornell temporal-bitcoin](https://www.cs.cornell.edu/~arb/data/temporal-bitcoin/) | `temporal-bitcoin.txt.zip`, transaction network |
| StackOverflow | [SNAP sx-stackoverflow](https://snap.stanford.edu/data/sx-stackoverflow.html) | union `sx-stackoverflow.txt.gz`, not a2q/c2q/c2a alone |
| SuperUser | [SNAP sx-superuser](https://snap.stanford.edu/data/sx-superuser.html) | union `sx-superuser.txt.gz` |
| WikiTalk | [SNAP wiki-talk-temporal](https://snap.stanford.edu/data/wiki-talk-temporal.html) | `wiki-talk-temporal.txt.gz`, not usernames |

These provider pages were checked on 2026-11-06; this release does not download
them automatically. The original providers remain the authorities for access
conditions and citations.

## Representations and SHA meanings

`configs/graphs.json` records 13 historical graph **byte identities** used by
the released queries: four weekly raw inputs, five weekly prepared inputs,
and raw/prepared pairs for Bitcoin-month and StackOverflow-month. These are
not hashes of the provider's compressed archive. `queries/manifest.json`
maps every query to its input, label contract and expected count.

The weekly bin converter uses 604800-second bins, with the first input record's
timestamp rounded down to a week as origin, then adds one. Negative snapshot
indices are valid. The historical C++ arithmetic truncates negative division
toward zero; replacing it with Python floor division changes the input.
`scripts/weekly_converter.cpp` preserves that code. `unix-week` emits the
deduplicated raw input in first-occurrence order, retaining directions and IDs.
No timezone-local calendar operation is used for weekly bins.

`activity_stats.cpp` implements the original E4/E7 preparation: month buckets
are UTC Gregorian `year*12 + month-1`, shifted so the first month is 1; day
buckets are UTC epoch days shifted similarly. Missing periods are not removed.
Its `weekly` mode takes already bucketed snapshots without rebucketing. It
sorts `(source,target,snapshot)` and removes duplicate occurrences. F2 retains
an edge's **entire** history iff that history contains at least one consecutive
pair. It does not retain only the consecutive portion. The same helper emits
Table-I-style counts, F2 edge/occurrence reduction, word counts and E7 activity
distributions. `--stats-only` avoids emitting graph copies.

Historical weekly F2 serialization used unordered-map iteration and some
files use CRLF. A current compiler/OS can produce different bytes with the
same graph. Therefore the default public workflow labels generated inputs as
a **new cohort**, records their actual hashes, and binds labels and stores to
those hashes. It does not pretend to recreate the historical byte SHA. Use
`bind --archive-exact` only when supplying the actual archived input bytes.
Neither workflow overwrites published measurements. Answer counts are checked
against the frozen reference; a mismatch stops the run.

## The two Bitcoin inputs are distinct files

The M4/system prepared Bitcoin file has reference SHA
`8cda283c93d62f5ba24b9a6623ada9ec967e19f50fc0cec7cb17b7893a4cf93c`.
The ten weekly `shape__bitcoin__...` cases use the original `bitcoin_p00.txt`
with SHA `ab3a84db0111c1c01253d76d910e1435ac0ca6e18e13909591b497a5ef748923`.
The historical path includes `synthetic`, but p00 adds **no** artificial edges
or vertices: it contains the F2-retained rows from the raw source in source
order. Existing source audit found both contain 1,654,443 directed edges and
8,027,213 occurrences with identical histories; their row ordering and LF/CRLF
serialization differ. This does not authorize merging or aliasing their stores.
Keep separate prepared files and bind each reference identity explicitly;
build one store per actual file SHA. Regeneration is a new cohort, not a claim
that historical file order was reproduced. `configs/graphs.json` lists all
query uses so both variants remain visible.

Generate the p00 variant using `convert --mode preserve-f2 --input RAW_WEEK
--retained CANONICAL_F2 --output NEW_P00_DIR --dataset bitcoin`. This retains
only F2 member edges while preserving the raw file's row order and line endings;
it emits `p00.txt`. Bind that file to the p00 reference identity separately.

## Labels

Stable-ID labels use seed 42. Explicit-label cases use the original
SplitMix64 function of `(uint32(seed)<<32)|uint32(external_vertex_id)`, reduced
modulo the frozen global label cardinality. The selected global classes and
their A–E slot order are frozen in each query projection. Never relabel vertex
IDs to a dense range, reorder selected classes, or choose new queries.

`prepare_data.py labels` regenerates maps from the vertices of the actual
bound input with the original deterministic encoder in `label_metadata.py`.
It updates only the projection's map SHA/mapping digest; query SHA and class
order remain unchanged. Raw-baseline maps cover raw vertices, while prepared
maps cover F2 vertices. They are different artifacts even though every shared
vertex has the same global class. `--boundary raw` must therefore be prepared
separately. Large raw Bitcoin maps require substantial RAM; map generation is
offline preparation, not a query timing. Map/store creation is never implicit
inside a measured process.
