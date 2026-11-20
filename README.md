# graph-lint

A linter for the topology of a `cudaGraph_t`.

## Status

M0 to M2 of nine milestones are closed. The tree holds the graph model, the JSON contract (schema
v1), the `cudaGraph_t` reader, the demo that produced the four artifacts in `graphs/`, and CI. The
rules, the DOT and Mermaid emitters, the two command-line tools and the benchmark are not here:
nothing in this tree lints a graph today.

What works now, inside this tree: build and test the core without CUDA or a GPU, read a
`cudaGraph_t` into `graphlint::Graph` with `from_cuda`, and write or parse that model as JSON with
`dump_graph` and `parse_graph`.

## What it is

`graph-lint` will report the topology of a `cudaGraph_t`: depth in levels, width per level, the
edges a transitive reduction removes and the minimal DAG that remains, the synchronization nodes
(event record and wait, empty, external semaphore), and the peak device bytes held by the
`cudaMallocAsync` nodes. The intended contract is one document per graph, fixed key order, no
timestamp and no local path, so a report is a diffable artifact.

The model in `graph.hpp` and the reader in `src/cuda_source.cpp` are what that report will be
computed from. They exist today; the report does not.

## What it is not

| incumbent | what it gives |
|---|---|
| `cudaGraphDebugDotPrint` | the toolkit's DOT writer: a drawing, no report, and output that is not stable across toolkit versions |
| Nsight Systems, Nsight Compute | a timeline and per-kernel counters with durations, not a verdict on the topology |
| `cudaGraphGetNodes`, `cudaGraphGetEdges` | handle and edge arrays: any number built on them is twenty lines of your own code |

`cudaGraphDebugDotPrint` sits first because it is the closest thing the toolkit ships. The DOT
emitter planned here will differ on the three points that make a drawing diffable: ids shared with
the JSON, order by id, and identical bytes across runs on the same driver. That emitter does not
exist yet.

It is not a rewriter: the reduction is reported, never applied (`docs/limits.md`).

## When to use it

For the engineer holding a captured or hand-built `cudaGraph_t` who needs its depth and width
before deciding where to split a stream. For the team comparing two graph versions that wants the
difference as a file in a review. For the reviewer who has to explain what changed in a graph
without reading a list of node handles.

## Limits

A level is a dependency, not a time. The reader sees the template, not the instantiated graph:
kernel names and parameters, mutations applied to a `cudaGraphExec_t`, and durations never reach a
report. `docs/limits.md` lists every missing field and why it is a property of the CUDA API, not a
defect waiting for a fix.

## Quality

The host suite needs no CUDA toolkit and no GPU: 43 GoogleTest cases in 4 ctest entries (`config` 3,
`graph` 7, `json` 28, `demo_check` 5). `build-host.bat test` runs them; `ctest --test-dir build-host -V`
prints the per-binary `N tests from ... ran` lines that add up to 43.

The device suite is 5 cases in one ctest entry. They passed on the card named below; with no visible
card the binary exits 77 and ctest reports the entry SKIPPED:
`set CUDA_VISIBLE_DEVICES=-1 && ctest --test-dir build-dev --output-on-failure`.

`graphs/` holds four documents read from real handles by `tests/device/demo_graph.cu`:

- `built.json` — 27 nodes, 30 edges, depth 17, 0 redundant edges, 2493 bytes
- `captured.json` — 25 nodes, 29 edges, depth 15, 1 redundant edge, 2334 bytes
- `pingpong.json` — 12 nodes, 21 edges, depth 12, 10 redundant edges, 1261 bytes
- `chain.json` — 1000 nodes, 999 edges, depth 1000, 0 redundant edges, 89792 bytes

`demo_check`, a host ctest entry, re-asserts nodes and edges from the committed files and checks
that parsing then dumping each one returns the same bytes; `wc -c graphs/*.json` gives the byte
counts. Depth and the redundant count were measured on the same four shapes by the M0 spike
(`experiments/m0_graph/run.bat`, output pasted in its REPORT); no test in this tree computes them
yet, `analysis.hpp` arrives with M3.

`build-device.bat demo` regenerates the four files. Two consecutive runs produced the same SHA256
for all four (`sha256sum graphs/*.json` before and after the second run). They were generated on one
machine: RTX 2060 SUPER (sm_75), driver 610.88 reporting CUDA API 13.3, runtime 12.6. Environment
and full evidence: `graphs/REPORT.md`. The M0 spike measured the same numbers with a separate
implementation (`experiments/m0_graph/REPORT.md`).

CI runs two jobs:

| job | environment | what it does |
|---|---|---|
| `host-only` | ubuntu-22.04, windows-2022, no toolkit | builds the core and runs the host suite |
| `cuda` | ubuntu-22.04 with CUDA 11.8 and 12.1, windows-2022 with 12.9 | compiles the reader, the demo and the device suite on sm_75 |

The `cuda` job has no GPU: it proves the CUDA path compiles, not that it reads a handle. The
host-only job also installs the package into a scratch prefix and builds `tests/consumer` against
`find_package(graphlint CONFIG)`. The 5 device cases are the only ones that have touched a card.
The matrix is in `.github/workflows/ci.yml`.

## Build

CMake 3.20 or newer, Ninja, a C++17 compiler. The host build fetches GoogleTest v1.15.2 at configure
time and needs no CUDA toolkit, no nvcc and no GPU:

    build-host.bat configure
    build-host.bat build
    build-host.bat test

The reader is compiled by `GRAPHLINT_WITH_CUDA=ON` and links `cudart` only, no nvcc. It needs a
toolkit of 11.4 or newer, where the mem-node and child-graph getters arrive. nvcc enters only with
`GRAPHLINT_BUILD_DEVICE_TESTS=ON`, which adds the demo and the device suite:

    build-device.bat configure
    build-device.bat build
    build-device.bat demo
    build-device.bat test

On Windows the two scripts wire two toolchains that normally fight over PATH: Visual Studio 2022
BuildTools for the host compiler, CUDA 12.6 for nvcc. `build-device.bat` sets
`NVCC_PREPEND_FLAGS=--allow-unsupported-compiler`, because CUDA 12.6 does not know MSVC 14.44.

## Repository structure

Tracked files only; `build*/` trees are ignored.

```text
graph-lint/
├── include/graphlint/
│   ├── config.hpp
│   ├── cuda_source.hpp
│   ├── graph.hpp
│   └── json.hpp
├── src/
│   └── cuda_source.cpp
├── tests/
│   ├── consumer/
│   │   ├── CMakeLists.txt
│   │   └── main.cpp
│   ├── data/graphs/
│   │   ├── chain8.json
│   │   ├── child_graph.json
│   │   ├── diamond.json
│   │   ├── empty.json
│   │   ├── mixed.json
│   │   └── ns.json
│   ├── device/
│   │   ├── demo_check.cpp
│   │   ├── demo_graph.cu
│   │   └── test_cuda_source.cu
│   ├── test_config.cpp
│   ├── test_graph.cpp
│   ├── test_json.cpp
│   ├── test_main.cpp
│   └── test_main_device.cpp
├── graphs/
│   ├── built.json
│   ├── captured.json
│   ├── chain.json
│   ├── pingpong.json
│   └── REPORT.md
├── docs/
│   ├── CONVENTIONS.md
│   └── limits.md
├── experiments/
│   ├── m0_graph/
│   │   ├── REPORT.md
│   │   ├── REVIEW.md
│   │   ├── run.bat
│   │   └── shape.cu
│   └── m2_api/API_NOTES.md
├── cmake/graphlint-config.cmake.in
├── .github/workflows/ci.yml
├── .gitattributes
├── .gitignore
├── CMakeLists.txt
├── build-device.bat
├── build-host.bat
├── CHANGELOG.md
├── LICENSE
└── README.md
```

## License

MIT, Copyright (c) 2026 MardokHPC. The full text is in `LICENSE`.
