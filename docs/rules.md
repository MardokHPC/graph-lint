# Rules — the contract M4 must satisfy

A rule without its false-positive sentence does not ship. Numbers come from `graphs/REPORT.md`,
`experiments/m0_graph/REPORT.md` or `HANDOFF.md`.

`lint` reads the root graph only: a `child_graph` node is opaque and the rules do not descend. It
does not call `validate` (field and type invariants belong to the parser and the builder) and takes
the `Topology` a caller already has — `level`, `depth`, `width`, `distinct_edges`, `edges_minimal`,
`edges_redundant` — without recomputing any of it.

## `dag-check` — `error`, `warn` for a duplicate pair and an isolated node
**Measures** a cycle, an edge whose endpoint is not a node id, a duplicated pair, and a node with no
in- and no out-edge in a graph that has edges.

**What is left after M0.** `parse_graph` refuses non-dense ids, `from >= to` and out-of-range
endpoints, and `from < to` is a topological order, so a document cannot express a cycle; `validate`
repeats those three in memory. Surviving both: duplicate edges (kept on purpose) and isolated nodes,
the two defects the rule still scans. Third job: when `topology()` refuses the graph, the refusal
becomes one `error` finding with `GraphErrorKind` and `detail`, and the other six rules are
`not_applicable` with `graph failed dag-check` — else a caller that skipped `validate` gets nothing.

**Why `error`** for a cycle or an unknown endpoint: the graph cannot be analysed at all, so that
finding is the report's last sentence. A duplicate pair and an isolated node are `warn`: the parser
keeps duplicates on purpose and the analysis folds them into `distinct_edges`, so the graph still
answers, and an isolated node is legal — it runs concurrently — while a forgotten dependency looks
exactly like a deliberate independent root. Measured: no duplicates and no isolated node in the four
artifacts.
**Reader**: fix the builder or the document; on `error` nothing else is trustworthy.
**Applies** to `capture`, `built`, `file`: it reads the model only — on `capture` a firing means
`from_cuda` is wrong, on `file` that `parse_graph` was bypassed.

## `launch-shaped` — `warn`
**Measures** `10 * depth >= 9 * nodes && nodes >= 16`: depth is 90% of the node count, so nothing
can overlap and the graph pays the launch path only. Integers, never a float. Message: "1000 levels
over 1000 nodes: no two nodes can overlap" — `levels`, never a duration.

**Never fires** below the guard: `pingpong.json` is 12 levels over 12 nodes (ratio 1.00) and stays
silent on purpose, the limit §5.1 declares. The ratio is the rule, not the floor: a bare
`depth >= 15` fires on `captured.json`, which is 15 levels over 25 nodes. M0's "15 is silent, 16
fires" is about chains, where `depth == nodes`.
**Reader**: measure whether the graph earns its keep (`cuda-timer`, §5.6); if not, run the kernels
without it.
**Applies** to `capture`, `built`, `file`: on a graph with no edges at all it is the only rule with
something to say.

## `redundant-edge` — `warn`
**Measures** nothing itself: `Topology::edges_redundant` is the answer (the reduction is exact and
unique on a DAG), reported against `distinct_edges` and `edges_minimal.size()`. One finding per
graph, nodes = the sorted endpoints. Message: "10 of 21 edges are implied by a longer path; the
minimal DAG has 11 edges".

**Never fires** without an alternative path, and the registered caveat is not the algorithm: an edge
carrying a port (12.3+) arrives from `cudaGraphGetEdges` as a plain dependency (`limits.md`), so an
implied edge can be the only carrier of a port. It reports, never removes (§10).

**0 is an answer**: `built.json` 0 of 30, `chain.json` 0 of 999. On a capture it is possible, not the
expectation: the clean 4-stream capture measures 1 of 29, the ping-pong 10 of 21, where the minimal
DAG is the 11-edge chain. `0` reads as the right answer, never as a broken rule.
**Reader**: look at the endpoints; deleting one is a decision about ports, timing, buffer reuse.
**Applies** to `capture`, `built`, `file`, and on `capture` it is the flagship (10 of 21) and the
reason the tool exists (§0.1).

## `sync-node` — `severe` / `info`
**Measures** `host` (`severe`, one finding per node: a CPU callback serialises its stream and waits
on anything), `ext_semaphore` (`severe`, one per node: the graph depends on another API and a
producer it does not contain), `event_record`/`event_wait` (`info`, one aggregated finding per graph:
this is how a fork is expressed, not a defect).

**Never fires** above `info` on an event and has no count threshold — ten events are not ten
defects. No `empty` node (a join marker; 0 in both captured artifacts), no edge made from an event,
no mem node, and no `warn` tier in v1.0: a node class is a defect or context, and a count threshold
belongs to M6's gate.
**Reader**: for `severe`, check whether the callback or the semaphore can leave the graph.
**Applies** to `capture`, `built`, `file`; on a capture the `info` part is usually absent (0 event
nodes in `captured.json` and `pingpong.json`) because record/wait become edges.

## `pointless-fork` — `info`
**Measures** a node `u` with at least two successors `t != j` where every `t` reaches `j` and the
branch through `t` holds fewer than two nodes — in practice `t→j` is the whole branch. One finding
per `(u, j)`, nodes `{u, j}`.

**Never fires** when the extra successor is `j`: before `t != j`, 11 of 13 firings carried `branch
sizes: 1 0`, i.e. `u→j` existed and the rule was `redundant-edge` renamed; `pingpong.json` goes from
10 firings to 0. Silent too when a branch holds two or more nodes.

**The true positive that must not disappear**: the minimal diamond `u→{a,b}→j` — `built.json` `u=1
j=4`, `captured.json` `u=0 j=3`. A real two-way split, hence `info` and an aggressive name.
**Reader**: check whether the branches are independent work; if they are, ignore it.
**Applies** to `capture`, `built`, `file`: successors and reachability, not the source.

## `duplicate-launch` — `info`
**Measures** two or more `kernel` nodes with equal non-empty `name` and equal
`(grid, block, shared_bytes)`. One finding per group, nodes = the group, and the message says
"candidate": parameters are unreadable (`limits.md`), so equal geometry is not equal work.

**`not_applicable`, not 0, when the names are empty.** The four artifacts hold 27, 25, 12 and 1000
kernel nodes, every one `1×1×1` and unnamed (`graphs/REPORT.md`); on geometry alone the rule would
report 27 candidates on `built.json` and 1000 on `chain.json`, a 1000-line report carrying nothing.
So equal non-empty names only, and fewer than two named kernels is `not_applicable` with `no kernel
name`. Cutting it is §7's first option; as written it documents its own blindness.
**Reader**: check the call site; the arguments are the difference and the tool cannot see them.
**Applies** to `capture`, `built`, `file`, conditionally on names: dark until the reader resolves
`func`, or until a document carries a name.

## `mem-lifecycle` — `warn`
**Measures** `mem_alloc`/`mem_free` correlated by `mem_ptr`, swept in canonical id order (ids are a
topological order: every edge has `from < to`). An allocation lives from its node to its free, and to
the last node when no free exists. Peak = the largest sum of `alloc_bytes` live at one position;
churn = the number of alloc/free pairs. It is not the allocator's peak but the peak of the
allocations the graph declares on one linearisation: per-stream concurrency is invisible and the
allocator may reuse a block the graph does not name.

Two findings: one per alloc node with no free ("4096 bytes at node 7 are never freed: live to the
end and counted in the peak"); one per graph when the peak exceeds the largest single allocation,
i.e. two are live together ("peak 8192 bytes at node 9, 2 live, largest request 4096, 2 alloc/free
pairs").

**Never fires** on a `mem_free` whose pointer was allocated outside the graph (legal:
`cudaGraphAddMemFreeNode` accepts it) and never matches `mem_ptr == 0` — the getter can fail and a
zero correlates with nothing. `not_applicable` with `no mem_alloc/mem_free node` when the graph
declares none: a `cudaMalloc` before the capture leaves no node (`limits.md`), all four artifacts
have 0 mem nodes, and only `tests/device/test_cuda_source.cu` exercises the rule.
**Reader**: free earlier or shrink the overlap; the tool cannot tell whether the allocator did.
**Applies** to `capture` with `cudaMallocAsync`, to `built` and `file` when the mem nodes are there.

## `applicable` / `not_applicable`
The report carries, per rule, one of the two with a one-line reason; the reason is schema text and
part of the byte-identical contract. A `not_applicable` rule never changes the exit code, and `0`
findings is legitimate only for a rule in `applicable` — without the table a capture reads as "0
findings, the tool is broken". `source` is declared (`limits.md`); `cond` is decided on the graph.

| rule | capture | built | file |
|---|---|---|---|
| `dag-check` | yes | yes | yes |
| `launch-shaped` | yes | yes | yes |
| `redundant-edge` | yes | yes | yes |
| `sync-node` | cond (1) | yes | yes |
| `pointless-fork` | yes | yes | yes |
| `duplicate-launch` | cond (2) | cond (2) | cond (2) |
| `mem-lifecycle` | cond (3) | cond (3) | cond (3) |

1. A capture turns `cudaEventRecord`/`cudaStreamWaitEvent` into edges: measured 0 event nodes in
   `captured.json` and `pingpong.json`, so only `ext_semaphore` reaches the rule.
2. No kernel node in the four artifacts has a name: `not_applicable` with `no kernel name`.
3. No mem node in the four artifacts: `not_applicable` with `no mem_alloc/mem_free node`.

## Finding shape
Four fields, no others: `rule`, `severity`, `nodes` (sorted ids of the graph, empty for a
graph-level finding), `message` (one sentence, carries the numbers, no duration, no path).

Order, total: severity rank (`error` 0, `severe` 1, `warn` 2, `info` 3), then `nodes` element by
element with the shorter list first, then `rule`, then `message`. A rule never emits two findings
with the same node list, so the key is unique and two runs are byte-identical.

## Severities
| severity | meaning | M6 exit |
|---|---|---|
| `error` | the graph breaks the model: no other rule can answer | 2 |
| `severe` | a real defect of the shape; the gate should block | 1 |
| `warn` | a finding a human must weigh; blocks when the gate counts it | 0 |
| `info` | context, never a defect | 0 |

Four values, no fifth. `error` is 2: "verdict not sustainable" is exit 2 in the suite (§3.6), and a
graph failing `dag-check` has no verdict. `warn` is 0 by default — a clean capture legitimately
reports `redundant-edge` on 1 of 29 — and `graphlint_gate` makes it 1 only when a threshold names it.

