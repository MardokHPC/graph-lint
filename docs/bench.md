# Benchmark — what a dependency costs

`tools/bench.cu` answers one question: what does a graph pay when the same nodes carry more
dependencies? Three shapes of the same N with `noop` kernels at 1x1x1 on one non-blocking stream:
`chain` (`i -> i+1`), `redundant` (the chain plus `i -> i+2`, every extra edge implied by a longer
path) and `forkjoin` (one source, N-2 independent middles, one sink).

```bat
build-device.bat bench
```

It configures with `GRAPHLINT_BUILD_BENCH=ON`, builds `bench` against `cuda-timer` v1.2.2 and
rewrites [`bench/bench.json`](bench/bench.json). Device: RTX 2060 SUPER (sm_75), driver reporting
CUDA API 13.3, runtime 12.6 compiled and loaded — the tool exits 1 when the loaded runtime is not
the one it was compiled against, so a document cannot claim a toolkit that did not produce it.

Protocol: one discarded sweep over the nine cells, then 3 recorded passes x 7 batches x 200
launches. Every cell collects 21 samples; the document carries `min`, `median` and `max`, and the
net columns subtract the empty graph measured by the same procedure. Each row is read back through
`from_cuda` and `topology` before it is measured: a builder and a linter that disagree stop the run.

| shape | N | nodes | edges | depth | redundant | instantiate net | launch net |
|---|---|---|---|---|---|---|---|
| chain | 8 | 8 | 7 | 8 | 0 | 8.9 [7.8-30.1] | 7.7 [7.4-12.2] |
| chain | 32 | 32 | 31 | 32 | 0 | 28.4 [27.9-55.8] | 27.0 [26.0-33.2] |
| chain | 128 | 128 | 127 | 128 | 0 | 117.8 [112.9-184.5] | 112.6 [103.8-118.6] |
| redundant | 8 | 8 | 13 | 8 | 6 | 19.5 [18.2-76.9] | 12.9 [12.6-13.7] |
| redundant | 32 | 32 | 61 | 32 | 30 | 82.2 [63.8-266.3] | 55.9 [53.3-57.3] |
| redundant | 128 | 128 | 253 | 128 | 126 | 265.4 [255.9-411.8] | 248.5 [243.6-270.9] |
| forkjoin | 8 | 8 | 12 | 3 | 0 | 23.8 [20.2-120.8] | 8.8 [8.2-16.9] |
| forkjoin | 32 | 32 | 60 | 3 | 0 | 106.2 [86.9-228.2] | 16.4 [15.8-23.7] |
| forkjoin | 128 | 128 | 252 | 3 | 0 | 505.4 [431.7-1318.6] | 61.7 [59.0-67.6] |

Microseconds, `median [min-max]`, from the run in [`bench/bench.json`](bench/bench.json); the
document holds nanoseconds, integers only.

## Read this before quoting a number

1. **Not byte-reproducible.** Three runs of the same binary move a median by a few percent, and
   `max` is a single worst sample, not a regime (30.1 us against a median of 8.9 on `chain` at 8
   nodes). The interval is the datum; a median alone is one draw. The discarded sweep is what keeps
   the `chain` baseline usable: without it the same cell has read 115 to 254 us across processes on
   this machine.
2. **`launch` is not submit latency.** It is the mean of 200 back-to-back launches of one exec
   between one event pair, so the execution of the nodes is inside the number: that is the cost of
   the dependencies, not of `cudaGraphLaunch`. An empty graph launches in ~0.28 us and the net
   column already removes that floor.
3. **`instantiate` is a host call**, timed with the host clock around `cudaGraphInstantiate`. An
   event pair cannot measure it: on an idle stream WDDM can batch both records and the gap collapses
   toward zero, which is how a 32-node graph produced sub-microsecond samples. An empty graph costs
   ~1.0 us and the net column removes it. None of this is driver compile time. And a subtracted
   floor is a measurement too, one that can overshoot: in the weights run below the all-disabled
   floor sits ~8 us above what a one-node launch pays, so a net column is a lower bound of what it
   nets, not an exact net.

One GPU, one driver, one toolkit: nothing here is a claim outside this machine. The four committed
graph documents, their environment and the demo that wrote them are in
[`graphs/REPORT.md`](../graphs/REPORT.md); what a level, a width and a reduction mean is in
[`topology.md`](topology.md).

## From topology to time

`tools/weights.cu` closes the loop the rest of this page opens: it measures one node at a time
and writes the two artifacts `graphlint_report --weights` consumes.

```bat
build-device.bat bench
```

It builds a chain of eight `work(iters)` kernels — a dependent integer loop whose length grows
with the position, 55 000 steps per position at ~0.7 ns each, because a noop is under the
~9 µs a launch of this exec costs with every node disabled. Then: read the chain back through
`from_cuda`, which returns the canonical model *and* the id → handle map; instantiate once; for
every canonical id disable all nodes but that one on the exec with `cudaGraphNodeSetEnabled`.
The nodes are created backwards, so the API order is the reverse of the chain: an unpermuted
map would hand id 0 the last node's load, and the tool refuses to write a table whose weights do
not grow with the id. The clock is brought to a regime by 600 full-chain launches first — a
per-node loop read about a third slower per iteration on the first ids than on the last, which
is why the passes are round-robin — then 3 passes × 7 batches × 20 launches, and
`ns = max(0, median − zero_point)`, the zero point being the same pass with every node disabled.
A node clamped to 0 is counted on stdout: the weights file has no field to say it, and `ns == 0`
stays legal.

`docs/bench/weights.json` is the table, `docs/bench/measured.json` the graph it binds to by
fingerprint:

```
$ graphlint_report docs/bench/measured.json --weights docs/bench/weights.json --format summary
source: built
weight_kind: ns
...
critical_path: 0 1 2 3 4 5 6 7 (1496588 ns, 1.496 ms)
```

The line is the sum of the eight weights over the longest chain: on this card one execution of
this graph cannot be shorter, and every node is on that chain, so no single kernel explains the
number (`weights.json` has the eight). It is not the frame time, and a weight is a lower bound of
its node: the zero point is the floor of an exec with every node disabled, and on this machine it
sits ~8 µs above the floor a one-node launch actually pays, so every weight is low by that much —
~19% of id 0, ~4% of the 1.5 ms sum, the error largest on the smallest node. Each node also runs
alone, with the whole card and nothing contending, so the sum is a lower bound of a serial
execution and never a measured duration. Re-measure on another driver and the table changes while
the fingerprint does not; the file cannot tell.
