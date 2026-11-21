# M0 — real shape of a `cudaGraph_t`

Spike, not product. One `.cu` file, `cudart` only, no project structure.
It answers one question: **do the two flagship rules fire on real graphs, and on which ones?**

## Environment (read at runtime by the program, not declared by hand)

| | |
|---|---|
| GPU | NVIDIA GeForce RTX 2060 SUPER, sm_75 |
| Driver | 610.88 (CUDA driver API 13.3) |
| Toolkit | CUDA 12.6, runtime `CUDART_VERSION 12060` |
| Compiler | VS2022 BuildTools + `nvcc` 12.6, `-std=c++17 -O2 -arch=sm_75` |

## Command

```bat
cmd /c experiments\m0_graph\run.bat
```

`run.bat` calls `vcvars64.bat` (BuildTools 2022), puts `CUDA\v12.6\bin` on the `PATH`, and runs:

```bat
nvcc -std=c++17 -O2 -arch=sm_75 -o shape.exe shape.cu
shape.exe
```

For the full edge list: `set GL_DUMP_EDGES=1` before `shape.exe`.

## Run output (pasted)

```text
graph-lint M0 spike | NVIDIA GeForce RTX 2060 SUPER sm_75 | driver CUDA API 13.3 | runtime 12.6

=== A: hand-built transformer block ===
nodes 27  edges 30  kernel 27  empty 0  event 0
depth 17  levels width: 1 1 2 1 1 4 4 4 1 1 1 1 ...
parallelism 1.59
minimal DAG: 30 -> 30 (0 redundant edges)
RULE launch-shaped : silent (depth 17 / nodes 27 = 0.63)
RULE pointless-fork: FIRED  (1)
  u=1 j=4 succ: 2 3 branch sizes: 1 1

=== B: same shape captured from 4 streams ===
capture: 25 real kernels -> 25 nodes (0 Empty added, +0%)
nodes 25  edges 29  kernel 25  empty 0  event 0
depth 15  levels width: 1 2 1 4 4 4 1 1 1 1 1 1 ...
parallelism 1.67
minimal DAG: 29 -> 28 (1 redundant edges)
  redundant: 2->4
RULE launch-shaped : silent (depth 15 / nodes 25 = 0.60)
RULE pointless-fork: FIRED  (2)
  u=0 j=3 succ: 1 2 branch sizes: 1 1
  u=2 j=4 succ: 3 4 branch sizes: 1 0

=== C: hand-built chain of 1000 kernels ===
nodes 1000  edges 999  kernel 1000  empty 0  event 0
depth 1000  levels width: 1 1 1 1 1 1 1 1 1 1 1 1 ...
parallelism 1.00
minimal DAG: 999 -> 999 (0 redundant edges)
RULE launch-shaped : FIRED  (depth 1000 / nodes 1000 = 1.00)
RULE pointless-fork: silent

=== D: fake overlap (captured, 2 streams) ===
nodes 12  edges 21  kernel 12  empty 0  event 0
depth 12  levels width: 1 1 1 1 1 1 1 1 1 1 1 1
parallelism 1.00
minimal DAG: 21 -> 11 (10 redundant edges)
  redundant: 0->2 1->3 2->4 3->5 4->6 5->7 6->8 7->9 ...
RULE launch-shaped : silent (depth 12 / nodes 12 = 1.00)
RULE pointless-fork: FIRED  (10)
  u=0 j=2 succ: 1 2 branch sizes: 1 0
  u=1 j=3 succ: 2 3 branch sizes: 1 0
  u=2 j=4 succ: 3 4 branch sizes: 1 0
  u=3 j=5 succ: 4 5 branch sizes: 1 0
  u=4 j=6 succ: 5 6 branch sizes: 1 0
  u=5 j=7 succ: 6 7 branch sizes: 1 0

=== VERDICT ===
launch-shaped  : fires on 1/4 shapes  (C )
pointless-fork : fires on 3/4 shapes  (A B D )
```

## Verdict

- **`launch-shaped`: 1/4.** It fires only on the 1000-long chain (1.00 = 1.00). It does not fire on D
  *despite* `depth/nodes = 1.00`, because the `nodes >= 16` guard excludes it: 12 nodes.
- **`pointless-fork`: 3/4.** It fires on A, B and D, but for two different reasons:
  - **A and B**: the same 2-branch × 1-node fork, once built by hand (`u=1 j=4`) and once
    **captured** (`u=0 j=3`). This is the true positive: the rule crosses the capture.
  - **D (10 firings) and the second one in B**: `branch sizes: 1 0` means one of the "branches" is
    `j` itself, i.e. `u→j` already exists as a direct edge. It is not a fork: it is a **redundant
    edge** seen by another rule. In B it is exactly the `2->4` of the `redundant:` line.

So: **yes, `pointless-fork` fires on the 4-stream capture (B), not only on A** — but the same
capture shows that the rule, as written, duplicates `redundant-edge`.

## What changes in the plan (measured, not assumed)

1. **The capture does not introduce `Empty` nodes.** 0 out of 25 nodes in B, 0 out of 12 in D. Joins
   become multiple in-edges on the consumer node (in B the 4-branch join is a single node with 4
   predecessors). The original assumption — "nested joins produce `Empty` join nodes" — does not
   hold on this machine. Consequence for M2/M3: `from_cuda` must work without counting on Empty
   nodes, and `pointless-fork` cannot use them as a signal.
2. **`redundant-edge` has material on captured graphs too**, so it is not a "builder-only" rule:
   1 edge out of 29 in B (3.4%), 10 out of 21 in D (48%). Case D is the most interesting: two streams
   exchanging events produce a chain with one extra edge every two nodes, all implied by the two-hop
   path (`0->2` is implied by `0->1->2`). The minimal DAG of D is 11 edges, i.e. the pure chain.
3. **`pointless-fork` must be fixed before M4**: exclude `j ∈ succ(u)` (or require ≥ 2 non-empty
   branches). With that condition it stays on for A and B (`u=0`), and goes quiet on D. I did not
   change it here because the spike spec fixes the rule; the change is one line in the candidate loop.
4. **`launch-shaped` and the `nodes >= 16` threshold**: the "fake overlap" pattern, which is the
   declared target of the rule, does not fire at 12 kernels. Either lower the guard to 12, or
   document that below 16 nodes the rule is silent on purpose (launch latency is the problem, not the
   topology).

## Numbers useful for the rest of the project

| shape | nodes | edges | depth | parallelism | minimal DAG | redundant |
|---|---|---|---|---|---|---|
| A built | 27 | 30 | 17 | 1.59 | 30 | 0 |
| B captured | 25 | 29 | 15 | 1.67 | 28 | 1 (3.4%) |
| C chain | 1000 | 999 | 1000 | 1.00 | 999 | 0 |
| D ping-pong | 12 | 21 | 12 | 1.00 | 11 | 10 (48%) |

A and B have the same topology except for the two explicit fork nodes of A, which do not exist in
the capture: the fork starts from the previous node. 27 − 2 = 25 nodes, and depth 17 − 2 = 15.
Consistent.

## What is NOT measured (and why)

- **`event`, `mem`, `Empty` nodes: never observed.** Types 1, 2, 3 of the model remain unexercised.
  In a capture `cudaEventRecord`/`cudaStreamWaitEvent` do not become nodes: they become edges. Mem
  nodes need `cudaMallocAsync` inside the capture, which this spike does not use.
- **No graph from a real workload** (PyTorch, diffusion, serving): the four shapes are synthetic,
  built to make the rules fire or not. A real graph can have widths that do not appear here.
- **One driver and one toolkit.** The "0 Empty nodes" is verified on driver 610.88 + CUDA 12.6; it is
  not a promise about other versions.
- **The transitive reduction is O(E·(V+E))**: on 1000 nodes and 999 edges it is instant, but it has
  not been tried on a wide graph (5000 nodes, 20000 edges) — that is M3.

## What did not work

- **First run of D**: `cudaStreamEndCapture -> capturing stream has unjoined work`. It was my bug:
  the last kernel ran on stream 2 and nobody brought it back to the originating stream. Added
  `cudaStreamWaitEvent(s0, ev[11])` before `cudaStreamEndCapture`, and D measures. **No API error to
  report**: `cudaStreamWaitEvent` inside a capture works on this machine, and shape D really has
  `depth == nodes`.
- **Wrong API names in CUDA 12.6** (one compile round lost): `cudaGraphNodeTypeEventWait` does not
  exist, it is `cudaGraphNodeTypeWaitEvent`; `cudaDeviceProp::driverVersion` no longer exists, use
  `cudaDriverGetVersion`. To be noted in `docs/limits.md` as a portability note, it is not obvious.
- **The shell eats the quotes**: every command went through a `.bat` launched as a single token.
  `run.bat` is self-contained precisely for this.

## Files

- `shape.cu` — 348 lines, C++17, `cudart` only.
- `run.bat` — rebuilds and reruns on this machine (vcvars64 BuildTools + CUDA 12.6).
- `REPORT.md` — this file.
