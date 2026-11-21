# Weights — the contract M7a must satisfy

## 1. The weights file
`weights.json`, written by `tools/weights.cu` (§5.6) and never by hand: one row per node, `id → ns`,
sorted keys, rows by id, LF — the grammar of `graph.json`. Complete file for `diamond.json`:

```json
{
  "graph": "a184e191b8b5ac47", "node_count": 4, "schema_version": 1, "unit": "ns",
  "weights": [[0, 200], [1, 700], [2, 300], [3, 100]]
}
```

| field | type | rule |
|---|---|---|
| `schema_version` | int | `1`; versions this table, not `graph.schema_version`; `2` refused |
| `unit` | string | exactly `"ns"`; missing or `"us"` refused, never rescaled |
| `node_count` | int | equals `graph.nodes.size()`; the readable half of the binding |
| `graph` | string | 16 lowercase hex: FNV-1a over one refinement round of each node's structural key and its neighbours' keys, then over the distinct edges, so a relabelled copy of the same graph hashes the same; the half that catches a same-size different graph |
| `weights` | `[id,ns][]` | dense `0..node_count-1`, ascending, one row per id, `[0, 1e12]` |

**No default, no subset.** A missing, repeated, out-of-range, negative or unsorted id is a `GraphError`
(`WeightRule`) naming it: a silent 1 on a number that lands in a report is the worst way to be wrong. `ns == 0`
is legal and not a disabled node — it fell under the timer's resolution after the zero-point, and a disabled
node has no measurement to write; `1.5` is `BadNumber`, not a rounded `1`, and scaling is the producer's job.
`graph` and `node_count` must both match: the count names the mistake in a sentence, the fingerprint refuses
a table from another graph of the same size, which the count cannot see.

## 2. Applying it
`include/graphlint/weights.hpp`, `[[nodiscard]] inline`, no I/O: `bool parse_weights(std::string_view,
WeightsTable&, GraphError&)`, `bool apply_weights(Graph&, const WeightsTable&, GraphError&)`, `WeightsTable` a
`std::vector<std::uint64_t>` by node id plus the `graph` fingerprint it carries, checked against the graph;
`graphlint_report --weights <path>` reads the bytes, and a refused table is exit 2.

`apply_weights` writes `Node::weight` on every node and flips `graph.weight_kind` to `ns`. It refuses a graph
with `children` — the file covers the root, and a child is one opaque node on the exec — and a graph already in
`ns`, where two files claim one number with no tie-break. `graph.json` on disk is untouched; the report's `graph`
block then carries `weight_kind: "ns"` and one `weight` per node: a transformation, not a second model.

## 3. The critical path
`dist(n) = weight(n) + max(dist(p))` over `p → n`, `dist(n) = weight(n)` at a source, nodes in id order
(ids are a topological order); `path_ns = max dist(n)`. Tie-break, total: the smallest id reaching
`path_ns`, then the smallest predecessor `p` with `dist(p) + weight(n) == dist(n)`, until a source; `path`
ascends because `from < to`.

`critical_path(const Graph&, const Topology&, CriticalPath&, GraphError&)` in `analysis.hpp`, separate from
`Topology`, which is verified (30 cases, three mutants) and must not see `level`, `depth` or `width` move.
`CriticalPath` holds `nodes` and `weight_sum` — not `ns`, so the unit comes from `weight_kind`.

- Empty graph `[]`/`0`; a non-empty graph always has a path, so all-zero weights give `[0]`/`0`; an isolated
  node is a path of one. Every consecutive pair is in `graph.edges`, hence in `edges_minimal`: a real chain.
- **Uniform 1 links M3 and M7a**: `dist(n) = level(n) + 1`, the maximisers are level `depth - 1`, the
  back-step takes the smallest predecessor one level down, and `weight_sum == nodes.size()` — the weighted
  path *is* the hop path M5 draws. That equality is a test.

## 4. The report: schema v2
`analysis` gains `"path": [0, 1, 3]` and `"path_ns": 1000`, **only when `graph.weight_kind` is `ns`**,
between `edges_redundant` and `width`; a levels report keeps five fields and only the version moves. Chosen
over (b) summary-only and (c) an optional block: the gate must compare `path_ns`, the reader refuses an unknown
key, and v1.0 is unreleased, so v2 costs six fixtures now and every committed report later.

- `dump_report` writes `"schema_version": 2`; `ReportReader` accepts `2` and refuses the rest with
  `SchemaVersion`; `ReportAnalysis` gains both fields and a presence flag; `same_analysis` compares them.
- `tests/data/reports/*.json` (6) get `"schema_version": 2`; `ns_weights.json` gains `[0, 1, 2, 3]`/`900`;
  `graphs/*` do not move; byte-pinned tests change on the version line, and `report.md` §1 moves to 2.

## 5. The gate in ns
`GateThresholds` gains `int max_path_increase_ns = -1`; `-1` is "not asked", below `-1` a usage error.

| threshold given | pair `levels` | pair `ns` |
|---|---|---|
| `--max-path-increase-ns N` | suspect, exit 2 | decisive |
| `--max-depth-increase N` | decisive | suspect, exit 2 |
| `--max-redundant-increase`, `--max-severe-increase` | decisive | decisive |

`path_ns` compares only when **both** sides are `ns`; `different weight_kind` is already suspect, and `path`
differing while `path_ns` matches is a **note**. No default for the ns threshold, against the `0` of the
others: a level is exact, a nanosecond a measurement, so absent means `context`. Depth is refused on an `ns`
pair because it only stood in for time (§5.1); it keeps its delta as `context`, and the other two still decide.
The wrong-unit refusals are two ctest cases: two `ns` reports with `--max-depth-increase 0`, and a
levels report with `--max-path-increase-ns`, both exit 2. The mixed pair (levels vs `ns`) is a third case,
suspect with no flags at all.

## 6. Mermaid and summary
`to_mermaid` calls `critical_path` and keeps three arguments: red is the weighted path in `ns`, the hop path
in `levels`; with weights `1` the two agree (§3), so the four `.mmd` artifacts stay byte-identical. Chains are
untouched: a middle member's only in-edge comes from a member with one out-edge, so no path enters a chain in
the middle and a chain is wholly on or off; labels stay `kernel#<head>×<count>`, no weight on them, a chain sum
being a path total only when the chain is on the path. JSON carries `path_ns` as an integer, never `ms`; text
prints `4200000 ns (4.200 ms)` by integer division; summary `critical_path: 0 1 3 (1000 ns)`, or `(3 hops)`.

## 7. What it is not
- **Not a measurement.** The linter sums integers; `weights.cu` measures under `GRAPHLINT_BUILD_BENCH`, core C++17.
- **Not portable.** A weight is a (node, graph, GPU, driver, cache) tuple the file does not record: the
  fingerprint refuses a table from another graph, but the same graph re-measured on another card is a
  different table with the same `graph`, and the format cannot tell.
- **Not the frame time.** What an infinitely parallel machine would take — no launch latency, no overlap, no
  stream concurrency: a lower bound, never a measured duration. Nothing is written back.

## 8. Tests
Loader: missing, repeated, out-of-range, negative and unsorted ids, `ns > 1e12`, `unit` missing, `"us"`,
`node_count` mismatch, a `graph` fingerprint from another graph of the same size, `1.5`, `schema_version: 2`
— each refused naming the id or position; `0` accepted. `apply_weights`: diamond `200/700/300/100` →
`[0, 1, 3]`/`1000`; `children` refused; already `ns` refused; tie-break with two ends at equal `dist`, and a
tie only in the middle. Uniform 1: `weight_sum == size()` and `path` equal to M5's hop path on all six
`tests/data/graphs/*.json`, every pair in `edges_minimal`. Report: dump→parse→dump byte-identical, a v1
document refused, levels without `path`, ns with `path_ns` as the sum. Gate: ns on levels → 2, depth on ns →
2, both ns with the threshold → 1 growth / 0 within / 0 improvement, both ns without it → pass, `-2` → usage
error. Mermaid: `chain8.json` weighted marks the whole `kernel#0×8`, a path skipping a level draws every
node. Determinism: `LC_ALL`/`TZ` differ, same bytes.
