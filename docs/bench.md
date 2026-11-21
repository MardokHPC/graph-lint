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
| chain | 8 | 8 | 7 | 8 | 0 | 9.4 [8.7-33.9] | 7.5 [7.5-11.3] |
| chain | 32 | 32 | 31 | 32 | 0 | 27.5 [28.3-49.9] | 27.8 [26.3-33.5] |
| chain | 128 | 128 | 127 | 128 | 0 | 117.8 [112.9-247.6] | 116.1 [108.5-121.0] |
| redundant | 8 | 8 | 13 | 8 | 6 | 20.3 [19.4-84.6] | 13.1 [12.9-16.7] |
| redundant | 32 | 32 | 61 | 32 | 30 | 65.9 [63.8-212.7] | 55.8 [53.1-56.9] |
| redundant | 128 | 128 | 253 | 128 | 126 | 260.8 [253.6-399.7] | 248.1 [247.0-252.7] |
| forkjoin | 8 | 8 | 12 | 3 | 0 | 22.2 [21.7-77.5] | 8.9 [8.8-14.6] |
| forkjoin | 32 | 32 | 60 | 3 | 0 | 103.1 [86.2-149.0] | 16.2 [15.9-19.5] |
| forkjoin | 128 | 128 | 252 | 3 | 0 | 424.8 [417.9-729.2] | 61.1 [59.1-63.1] |

Microseconds, `median [min-max]`, from the run in [`bench/bench.json`](bench/bench.json); the
document holds nanoseconds, integers only. The median is the net column (`*_net_ns`, the empty
floor subtracted); the interval is the raw `*_min_ns`–`*_max_ns` of the same 21 samples. The table
restates the committed `bench.json`; regenerating the JSON rewrites this table or the doc lies.

## Read this before quoting a number

1. **Not byte-reproducible.** Three runs of the same binary move a median by a few percent, and
   `max` is a single worst sample, not a regime (33.9 us against a median of 9.4 on `chain` at 8
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
~22% of id 0 (36507 ns), ~4% of the 1.5 ms sum, the error largest on the smallest node. Each node
also runs alone, with the whole card and nothing contending, so the sum is a lower bound of a serial
execution and never a measured duration. Re-measure on another driver and the table changes while
the fingerprint does not; the file cannot tell.
