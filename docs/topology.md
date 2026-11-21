# Topology — levels, width, reduction, path

## Levels and depth

`level(n) = 0` for a node with no predecessor, `1 + max(level(p))` over its predecessors otherwise.
`depth = max(level) + 1`: the number of levels, and the node count of the longest chain. In edges
that chain has `depth - 1` hops. `depth` is not a duration: two graphs that both report 10 levels
can run in 10 µs or in 10 ms, which is why the summary prints `10 levels` and the JSON carries
`weight_kind`.

The longest route, not the shortest distance: `level(n)` must sit past every predecessor, since a
node cannot start before its slowest predecessor finishes. `graphs/captured.json` has 25 nodes in
15 levels; `graphs/chain.json` has 1000 nodes in 1000 levels, one per node, and 999 edges.

`level` stays in hops when `weight_kind` is `ns`: weights move the critical path, never the levels.
An empty graph has `depth = 0`, `width = []`, `parallelism = 0`
(`include/graphlint/analysis.hpp`).

## Width and parallelism

`width[i]` counts the nodes at level `i`; `max(width)` is the widest level and
`parallelism = nodes / depth` the average nodes per level. `max(width)` and `parallelism` are not
fields of `report.json`: `docs/report.md` §1 keeps `width` and derives the other two, because a
stored `max_width` is a second copy of an integer that can disagree.

A wide level is where the graph allows work at once, nothing more. A graph of width 4 can still be
serialised by its own sync nodes, and width is not how many kernels run together on a GPU: neither
the scheduler nor the occupancy is visible from the template (`docs/limits.md`).

## The transitive reduction

An edge `u→v` is implied when a path `u→…→v` of two or more edges exists, the direct edge excluded.
`edges_redundant` lists the implied pairs, `edges_minimal` the rest. On a DAG the reduction is
unique, so "this edge is redundant" is a well-defined claim; a graph with a cycle gets no answer.
`validate` reports `Cycle` for a hand-built graph; `topology` refuses a `from >= to` edge with
`EdgeBackwards` before it walks, and a cycle needs at least one such edge.

The minimal DAG is the declared topology with the implied edges folded away, not the executed
graph. Nothing is deleted from the graph: from 12.3 an edge can carry port and data,
`cudaGraphGetEdges` returns it as a plain pair (`docs/limits.md`), so an implied edge may be the
only carrier of a port. The tool reports it and stops there; there is no `--fix`.

| graph | redundant of distinct | minimal |
|---|---|---|
| `pingpong.json` | 10 of 21 | 11 |
| `captured.json` | 1 of 29 | 28 |
| `built.json` | 0 of 30 | 30 |
| `chain.json` | 0 of 999 | 999 |

Cost `O(E · (V + E))`, one search per edge; the host suite keeps a 5 000-node, 20 060-edge graph
under one second. The same suite asserts the four rows above from the documents; `0` is an answer.

## Canonical ids

The model does not keep the order of `cudaGraphGetNodes`, which the documentation does not promise.
`canonicalize` sorts on a structural key — type, name, geometry, degrees — refines it with one round
of FNV-1a hashes of the neighbours' keys (written out by hand, so no `std::hash`), and takes nodes
in Kahn order from a min-heap on that key. `new_to_old` carries the inverse for the handle map.

With distinct keys the bytes are a function of the topology alone: the same graph relabelled by the
API produces the same document. A tie leaves a permutation inside one class, and the reported
numbers — depth, width, reduction, path — are invariant under it. `docs/limits.md` §Canonical ids
has the residue and the one driver it was measured on. The point is the diff: a CI that rewrites
`graph.json` because the driver reordered two equal nodes is noise, not a change.

## Hops and nanoseconds

Without `--weights` the report says `weight_kind: "levels"` and every node weighs 1:
`critical_path` maximises the sum, which is a hop count, and `path` is the hop path Mermaid draws
in red. With a table from `tools/weights.cu` (`docs/weights.md`) the same recurrence,
`dist(n) = weight(n) + max(dist(p))`, gives the heaviest chain, and an `ns` report adds `path` and
`path_ns` (schema 2). Uniform 1 is the same path: `dist(n) = level(n) + 1` and
`weight_sum == nodes.size()`, which is a test. The gate refuses a depth threshold on an `ns` pair
(`docs/weights.md` §5): a level is exact, a nanosecond is a measurement.

A weight is a lower bound of its node: measured alone, with a floor that sits ~8 µs above what a
one-node launch pays on the machine of `docs/bench.md`, it sees no overlap, no shared cache and no
launch latency. The sum is what an infinitely parallel machine would take, not the frame time
(`docs/weights.md` §7). The eight-node chain in `docs/bench.md`:
`critical_path: 0 1 2 3 4 5 6 7 (1496588 ns, 1.496 ms)`.

## What the numbers do not say

| the number | what it is not |
|---|---|
| `depth` | a latency: a level is a dependency, not a duration |
| `width`, `parallelism` | occupancy or concurrent kernels: nodes per level, not warps resident |
| `edges_minimal` | the executed graph: `cudaGraphInstantiate` compiles the template, the exec is another object |
| `weight`, `path_ns` | durations: one node at a time, floor subtracted, lower bounds |
| the template's shape | the run: `cudaGraphNodeSetEnabled` and `cudaGraphExec*NodeSetParams` mutate the exec, not this model |

`docs/limits.md` has each row with its API reason; this page stops at the arithmetic.

## A worked example

`graphs/captured.json`, a 4-stream capture, is 25 nodes, 29 edges, depth 15, widest level 4, one
edge implied by a longer path. `graphs/pingpong.json` is 12 nodes, 21 edges, depth 12, width 1 at
every level, and 10 implied edges; its minimal DAG is the 11-edge chain. The mechanism is
`EventWait`: an event between two streams adds an edge already implied by the stream order
(`experiments/m0_graph/REPORT.md`).

The comparison is the point. One redundant edge on a capture that looks parallel, ten on a graph
that is a chain wearing 21 edges: the ping-pong is the case the tool exists for, and
`redundant-edge` states it in a line a CI can gate on.
