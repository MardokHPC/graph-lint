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
   ~1.0 us and the net column removes it. None of this is driver compile time.

One GPU, one driver, one toolkit: nothing here is a claim outside this machine. The four committed
graph documents, their environment and the demo that wrote them are in
[`graphs/REPORT.md`](../graphs/REPORT.md); what a level, a width and a reduction mean is in
[`topology.md`](topology.md).
