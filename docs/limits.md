# Limits — what a `cudaGraph_t` cannot say

Read this before trusting a verdict. Every row is a property of the CUDA API or a
choice of this tool, not a defect waiting for a fix.

## Not observable through the runtime API

| what | why it is out of reach |
|---|---|
| kernel name | `cudaKernelNodeParams::func` is a `void*`. No runtime call names it, and the driver's `cuFuncGetName` does not document a handle taken from a graph node. |
| kernel parameters | `kernelParams` is a `void**` with no length anywhere in the struct: node-owned, valid while the node lives. The reader never dereferences it. |
| capture or hand-built | no flag, field or query says how the handle was made, so `source.kind` is the caller's declaration (the `SourceKind` argument), never a deduction. |
| mutations on the exec | `cudaGraphExec*NodeSetParams` and `cudaGraphNodeSetEnabled` change the instantiated graph; the template keeps its own shape, and the template is what we read. |
| the effect of instantiate | `cudaGraphInstantiate` compiles the template into an executable graph: flags, node priorities and the device-launch form live there, not on the template. |
| durations | a level is a dependency, not a time: two graphs of depth 10 can take 10 µs and 10 ms. A weight in ns exists only in a document that carries one, and a measured one is a lower bound of its node: `tools/weights.cu` subtracts the all-disabled exec's floor, which on this machine sits ~8 µs above what a one-node launch pays, so the smallest node carries the largest error. |
| host-side allocations | a plain `cudaMalloc` before capture leaves no node. Only the `cudaMallocAsync` / `cudaGraphAddMemAllocNode` pair appears as `mem_alloc`/`mem_free`. |

## Dropped by choice

| what | what survives |
|---|---|
| edge port/data (12.3+) | `cudaGraphGetEdges` returns the pair, so an edge with a port arrives as a plain dependency. A port-aware rule needs schema v2. |
| `ExtSemaphoreSignal` vs `ExtSemaphoreWait` | both decode as `ext_semaphore`; no rule needs the direction. |
| `Conditional` (12.6, 0x0d) and any newer node type | `unknown`; the node keeps its id and no rule reads it. |
| nesting deeper than `k_max_graph_depth` (8 levels) | `from_cuda` returns false naming `cudaGraphChildGraphNodeGetGraph`, `parse_graph` returns `TooDeep`; the two bounds are one constant. |

## Canonical ids

Two nodes are tied when their `key2` is equal. A tie is not an automorphism: a rigid graph
can tie two nodes whose neighbours differ. The tie-break is then the API enumeration index,
which the documentation neither orders nor promises to keep stable.

The residue is a permutation inside one tie class. Node count, edge count, depth, width, the
minimal DAG and the per-type and per-degree histograms are invariant under relabelling, so
`report.json` and the gate do not move, while `graph.json` of the same capture can differ
inside those classes. On the one driver measured (`graphs/REPORT.md`) the bytes did not move
across runs; that is a measurement, not a promise.

## What the tool does not do

It does not rewrite. There is no `--fix` and no rebuilt `cudaGraph_t`: the transitive
reduction is reported, never applied, because node order, events, visibility and buffer reuse
make a mechanical rewrite unsound.
