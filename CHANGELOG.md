# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `write_graph_json`: include `<graphlint/cuda_source.hpp>` and one call turn a `cudaGraph_t` into the
  document the CLI reads, with the path and the reason in `error` when it fails. The reader is
  header-only, so a program that holds a handle needs no graphlint library and no CMake; cudart is
  the only library behind it.
- The release archives carry `include/graphlint/*.hpp` from the checkout next to `bin/`, at the same
  paths on Windows and Linux. A download is the two tools plus the reader.

### Changed

- `graphlint::cuda` is an INTERFACE target that attaches the include path and `CUDA::cudart`; the
  implementation of the reader moved into `cuda_source.hpp` and `src/cuda_source.cpp` is gone. The
  installed package carries the header and no library, and the version file is `ARCH_INDEPENDENT`
  again because nothing in the package is compiled.

## [0.2.0] - 2026-11-22

### Added

- `graphlint_report --template <out.json>`: the starting document for a hand-written graph, two
  kernels in a chain, built in memory and written through `dump_graph`. It exits 2 if the file
  exists, and a graph or a report option next to `--template` is a usage error: dropping either in
  silence is worse than refusing. `tests/data/graphs/template.json` is the committed source of
  truth, compared byte for byte by a process-level test, and the template is a graph the linter
  reads clean.
- `docs/graph.md`: the grammar of the schema 1 document. Keys per node type, the invariants that
  make a cycle unrepresentable, and the parse errors a hand-written file can hit, each with the
  message it produces.
- The host-only package is guarded: `tests/tools/check_host_package.cmake` asserts it carries
  neither `cuda_source.hpp` nor a reader library, and the `host-only` job runs it after the install.
  Without it, a package config that exports `graphlint::cuda` on a machine with no toolkit would be
  green until the first consumer.

### Changed

- The reader ships with the package. With `GRAPHLINT_WITH_CUDA=ON`, `cuda_source.hpp` and the static
  library are installed and exported as `graphlint::cuda`, the same name the build alias uses; the
  generated package config calls `find_dependency(CUDAToolkit)` only in that case, and a host-only
  package does not look for a toolkit. The version file drops `ARCH_INDEPENDENT` when a static
  library is part of the package.
- The `cuda` CI job installs the package into a scratch prefix and builds `tests/consumer_cuda`
  against `graphlint::cuda`, on all three toolkit versions.
- ctest grows from 37 to 42 entries: the template pair, three refusal cases, and the host-only
  guard. The 225 GoogleTest cases are unchanged.

### Fixed

- `--template` used the check-then-act pattern: a file created between the existence probe and the
  truncating write was clobbered, while the README promised a template is never overwritten. The
  refusal is the exclusive open now.
- The capture snippet in the README used `SourceKind::Captured`; the enum is `Capture`, `Built`,
  `File`.

## [0.1.0] - 2026-11-21

### Added

- The graph model: `graphlint::Graph` with `Node`, `NodeType`, `SourceKind` and canonical ids, so
  the bytes do not depend on input order. No CUDA type appears in `graph.hpp`.
- The JSON contract, schema v1: `dump_graph` and `parse_graph`, sorted keys, no timestamp and no
  local path, parse errors with line and column, and a byte-identical round trip on every fixture.
- `from_cuda`: reads a `cudaGraph_t` into the model (nodes, edges, mem alloc/free nodes, child
  graphs up to 8 levels). Declared in `cuda_source.hpp`, defined in `src/cuda_source.cpp`. The
  header is not installed yet, so `find_package(graphlint CONFIG)` sees the model and JSON headers
  only.
- `tests/device/demo_graph.cu`: four shapes read from real handles and written to `graphs/`.
- `topology`: levels, depth, width per level and the transitive reduction, with the reduction
  checked against an independent naive reference on small graphs. Pure host code.
- `lint`: the seven rules of `docs/rules.md`, each with the reason it did not apply. Findings are
  ordered by severity, node list, rule and message, so a report is a function of the graph. The
  kernel-name rule stays `not_applicable` while the reader cannot resolve a name.
- `to_dot` and `to_mermaid`: the same graph as a diffable drawing that carries the ids of the JSON.
  Mermaid compresses a run of kernels into one `kernel#<head>×<count>` box, marks the critical path
  and the `sync-node` class, and emits a diagram over the node cap whole behind a banner instead of
  truncating it; DOT dashes the edges the transitive reduction removes. Both are pure in
  `(graph, topology, options)`, LF only, no timestamp and no local path.
- `tools/emit_artifacts.cpp`, built with the tests and run by `build-host.bat artifacts`: it rewrites
  `graphs/{built,captured,pingpong,chain}.{dot,mmd}` from the four committed JSON, and the suite and
  the README figure are compared with those files byte for byte.
- `report.json`, schema v2 (`docs/report.md`): the graph, the analysis, the findings, the
  applicability list and the environment, written by `graphlint_report`. `graphlint_gate` reads it
  back, recomputes the analysis from the embedded graph and exits 0, 1 or 2: pass, regression,
  suspect. Thresholds on depth, implied edges and severe findings are absolute integers; the gate
  never trusts a declared analysis block to decide.
- `examples/three_lines.cu`, built with `GRAPHLINT_BUILD_EXAMPLES=ON`: capture four launches on two
  streams, read the capture back with `from_cuda`, print the topology and write the JSON the report
  binary reads next. The binary exits 77 without a card.
- Measured weights (`docs/weights.md`): a complete `ns` table per node, applied by
  `graphlint_report --weights`, bound to one graph by node count and a structural fingerprint that
  survives relabelling. `path` and `path_ns` appear in a report schema 2 only when the weights are
  nanoseconds; with uniform weights the weighted path is the hop path. `graphlint_gate` compares
  `path_ns` under `--max-path-increase-ns` and refuses a threshold whose unit does not match the
  pair.
- `tools/bench.cu`, built only with `GRAPHLINT_BUILD_BENCH=ON` against `cuda-timer` v1.2.2: the same
  `noop` workload as a chain, a chain with implied skip edges, and a fork-join, swept over N and
  timed for instantiate and launch with zero points and a discarded warm-up sweep. The measured
  table, the command and the caveats are `docs/bench.md`; the raw document is
  `docs/bench/bench.json`.
- `tools/weights.cu`, same opt-in target: it measures one node at a time with
  `cudaGraphNodeSetEnabled`, subtracts the all-disabled floor, and writes `docs/bench/weights.json`
  with the graph fingerprint, plus `docs/bench/measured.json`. `graphlint_report --weights` on the
  pair turns the critical path into nanoseconds. `canonicalize` can now report its permutation and
  `from_cuda` the handle behind each canonical id.
- CI with a host-only job on Linux and Windows, plus a CUDA job that compiles the device path on
  three toolkit versions without executing it.
- `docs/limits.md`: what a `cudaGraph_t` cannot expose, and the canonical-id residue.
- `docs/topology.md`: what a level, the depth, the width per level, the transitive reduction and the
  weighted path mean, with the worked example over `graphs/captured.json` and
  `graphs/pingpong.json`.

### Changed

- The CUDA reader is a separate static target behind `GRAPHLINT_WITH_CUDA`, not part of the
  header-only library. nvcc and a GPU enter only with `GRAPHLINT_BUILD_DEVICE_TESTS`.
- `demo_check` runs in the host-only test job, so the committed `graphs/*.json` are verified on a
  machine with no toolkit and no card, next to the model and JSON suites.
