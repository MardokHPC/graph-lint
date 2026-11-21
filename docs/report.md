# Report and gate — the contract M6 must satisfy

## 1. `report.json`, schema v1

A two-node `built` graph, `kernel → host`, as `graphlint_report --format json` writes it: 2-space
indent, keys alphabetical in every object, one node object and one edge pair per line, LF.

```json
{
  "analysis": {
    "depth": 2,
    "distinct_edges": 1,
    "edges_minimal": [
      [0, 1]
    ],
    "edges_redundant": [],
    "width": [1, 1]
  },
  "applicability": {
    "applicable": [
      {"reason": "graph is a DAG", "rule": "dag-check"},
      {"reason": "depth and node count are known", "rule": "launch-shaped"},
      {"reason": "the reduction is exact on a DAG", "rule": "redundant-edge"},
      {"reason": "node types are readable", "rule": "sync-node"},
      {"reason": "successors and reachability are readable", "rule": "pointless-fork"}
    ],
    "not_applicable": [
      {"reason": "no kernel name", "rule": "duplicate-launch"},
      {"reason": "no mem_alloc/mem_free node", "rule": "mem-lifecycle"}
    ]
  },
  "environment": {"cuda_runtime": 0, "device": ""},
  "findings": [
    {"message": "host node 1 runs a CPU callback inside the graph and serialises its stream", "nodes": [1], "rule": "sync-node", "severity": "severe"}
  ],
  "graph": {
    "children": [],
    "edges": [
      [0, 1]
    ],
    "nodes": [
      {"block": [1, 1, 1], "grid": [2, 2, 1], "id": 0, "type": "kernel"},
      {"id": 1, "type": "host"}
    ],
    "schema_version": 1,
    "source": {"kind": "built"},
    "weight_kind": "levels"
  },
  "schema_version": 2
}
```

| field | type | required | why |
|---|---|---|---|
| `analysis.depth` | int | always | levels, never a time; `0` for an empty graph |
| `analysis.width` | int[] | always | nodes per level; length is `depth` |
| `analysis.distinct_edges` | int | always | `graph.edges` deduplicated; the reduction's denominator |
| `analysis.edges_minimal` | `[int,int][]` | always | the reduction, sorted; 999 pairs (~10 KB) on `chain.json` |
| `analysis.edges_redundant` | `[int,int][]` | always | implied edges, sorted; `[]` is an answer |
| `applicability` | object | always | `applicable`/`not_applicable`, each `{reason, rule}` in RuleId order |
| `environment` | object | always | `{cuda_runtime, device}`; always present, `0`/`""` when unknown |
| `findings` | object[] | always | `{message, nodes, rule, severity}`, severity in `error\|severe\|warn\|info`, ordered by severity, nodes, rule, message |
| `graph` | object | always | the model document, same grammar as `graph.json`, nested one level |
| `schema_version` | int | always | `2`; a `1` or a `3` is exit 2 for the gate |

- `max_width` and `parallelism` are not fields: `max(width)`, `nodes / depth`, and v1 has no float;
  `weight_kind` lives once, in `graph.weight_kind` (PLAN_M2 §4), severities and rule tokens once, in
  `docs/rules.md`. `analysis` and `findings` cover the root, the scope `lint` has.
- `analysis.path` and `analysis.path_ns` exist only in a `ns` report (`docs/weights.md` §4): the
  critical path as node ids and its total. A levels report has neither key.
- A refused graph still reports: `analysis` at `0`/empty, one `dag-check` error, six `not_applicable`, and the gate makes that error exit 2.
- No byte round trip: `analysis` and `findings` are derived and `environment` is the machine, so the
  contract stays on `graph.json` (PLAN_M2 §1). The document is a function of `(graph, environment)`:
  sorted keys, LF, no timestamp, path or float, and two processes with different `LC_ALL`/`TZ` agree.

## 2. The gate

**Suspect, exit 2** — no comparison is possible: an input that does not parse (missing, truncated,
unknown key, a `graph.json` where a report belongs); `schema_version != 2`; `source.kind` different
(`built` vs `capture`); `weight_kind` different; an `error` finding, since a refused graph has no
verdict; `device` known on both and different; a threshold in the wrong unit — `--max-path-increase-ns`
on a levels pair, or `--max-depth-increase` given on an `ns` pair (`docs/weights.md` §5). An unknown
device on one side, or a differing `cuda_runtime`, is a note: the analysis is toolkit-independent.

**Regression, exit 1:** `current - baseline > threshold` on `depth`, `edges_redundant`,
`findings_severe` on a levels pair, or `path_ns` on an `ns` pair when `--max-path-increase-ns` was
given. Thresholds are absolute integers, default `0`, strict `>`: a level is a dependency at 100 nodes
and at 10 000, while a percentage hides it on the large graph and rounds on the small one. `delta ==
threshold` is inside. **Improvement, exit 0:** the same metric falls beyond its threshold.
**Pass, exit 0:** all inside; the same document on both sides passes, noting `baseline and current carry
the same graph` on the `graph` blocks, not the paths. A regression decides even if another metric
improved; order is suspect, regression, improvement, pass. **Not decisive:** `warn`/`info` counts,
`nodes`, `edges_distinct` (size is not shape), `width_max` (a deeper graph can be narrower) and a
`path_ns` without its threshold, which is a measurement and gets no default: no defect either way, and
no v1 flag names a `warn` rule.

**What it reads:** `report.json` only — `graph.json` has neither `source.kind` nor `environment`. It
parses `report.graph`, recomputes `topology` and `lint`, and decides on those numbers; a declared
`analysis` or `findings` that disagrees yields a note, so a stale block never decides a verdict.

The document depends only on `(baseline, current, thresholds)`: `verdict`; `reason`, one sentence — `depth
grew 1 > 0`, `path_ns grew 120 > 100`, `all metrics inside their thresholds`, or the failed comparison;
`metrics`, each `{baseline, current, delta, name, status}`, in the order depth, edges_redundant,
findings_severe, path_ns (ns pairs only), nodes, edges_distinct, width_max, findings_warn, findings_info,
with `status` in `regression|improvement|none|context`; `thresholds`, the four values with `-1` for
"not asked"; both `environment`s; `notes`; `schema_version`.

stdout carries the document unless `--quiet`, `--json <path>` writes the same bytes, stderr one line
`graphlint_gate: <verdict> — <reason>` always; unreadable input emits the document, `suspect`, exit 2.

## 3. The two CLIs

`graphlint_report <graph.json> [--format summary|json|dot|mermaid] [--weights <path>] [--out <path>]
[--detail-params] [--max-nodes N]` writes the chosen document to stdout, default `summary`: source,
`weight_kind`, counts, depth in levels, max width, minimal DAG, one line per finding and per
`not_applicable` rule. `--weights` applies a `docs/weights.md` table before analysing, so `json`, `dot`,
`mermaid` and `summary` all see `ns` weights; a refused table is exit 2. `--out` writes the file and leaves
stdout empty. Exit `0` whenever a document is produced, `error` findings included — a report reports, it
never judges; `2` on usage, an unreadable file, or `--format dot|mermaid` on a graph `topology` refused,
where the emitters' precondition fails and `summary`/`json` still answer; never `1`. stderr is silent on
success, one `<path>:<line>:<column>: <detail>` line on failure. `--detail-params` and `--max-nodes` serve
DOT and Mermaid, `--max-nodes < 1` is a usage error, and a `report.json` handed here fails at `unknown key
"analysis"`, exit 2.

`graphlint_gate --baseline <a.json> --current <b.json> [--json <path>] [--max-depth-increase N]
[--max-redundant-increase N] [--max-severe-increase N] [--max-path-increase-ns N] [--quiet]` exits with the
verdict; a negative threshold is a usage error, because `0 > -1` turns a no-op comparison into a regression.
A unit that does not match the pair is exit 2 (`docs/weights.md` §5). Neither CLI touches CUDA in the core:
`environment` comes from the CLI under `GRAPHLINT_WITH_CUDA`, else `0`/`""`.

## 4. Exit-code cases, as ctest entries

| case | inputs | exit | asserted |
|---|---|---|---|
| pass, same document | `baseline.json` twice | 0 | stderr `pass`; the document notes the same graph |
| threshold exceeded | `baseline.json` → `depth_plus_one.json` | 1 | stderr `depth grew 1 > 0` |
| real regression | `baseline.json` → `host_node.json`, same depth | 1 | stderr `findings_severe grew 1 > 0` |
| other source | `baseline.json` → `other_source.json` | 2 | stderr `different source kind: built vs capture` |
| mixed weight kinds | `baseline.json` → `ns_weights.json` | 2 | stderr `different weight_kind: levels vs ns` |
| ns threshold on levels | `baseline.json` twice, `--max-path-increase-ns 0` | 2 | stderr names the unit mismatch |
| depth threshold on ns | `ns_baseline.json` twice, `--max-depth-increase 0` | 2 | stderr names the unit mismatch |
| ns regression | `ns_baseline.json` → `ns_path_plus_one.json` | 1 | stderr `path_ns grew … > …` |
| unreadable input | `baseline.json` → `truncated.json` | 2 | stderr carries `line:column`, `--json` written |

Fixtures live in `tests/data/reports/`; each row calls the copy of `poolkit/tests/tools/run_expect.cmake`
in `tests/tools/` (`-DTOOL`, `-DEXPECTED`, `-DARGS`, `-DEXPECT_STDERR`), the exit code being the contract.

## Needs a decision

1. **`edges_minimal` in every report.** Recommended: yes, the full list — 999 pairs, ~10 KB, on
   `chain.json`; a count plus a flag would need a second run to answer what the document already says.
2. **Derived scalars as fields.** Recommended: no — `max_width` is `max(width)` and `parallelism` is
   `nodes / depth`; a `{den, num}` alternative is a second copy of two integers that can disagree.
