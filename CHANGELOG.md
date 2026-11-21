# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
- Measured weights (`docs/weights.md`): a complete `ns` table per node, applied by
  `graphlint_report --weights`, bound to one graph by node count and a structural fingerprint that
  survives relabelling. `path` and `path_ns` appear in a report schema 2 only when the weights are
  nanoseconds; with uniform weights the weighted path is the hop path. `graphlint_gate` compares
  `path_ns` under `--max-path-increase-ns` and refuses a threshold whose unit does not match the
  pair.
- CI with a host-only job on Linux and Windows, plus a CUDA job that compiles the device path on
  three toolkit versions without executing it.
- `docs/limits.md`: what a `cudaGraph_t` cannot expose, and the canonical-id residue.

### Changed

- The CUDA reader is a separate static target behind `GRAPHLINT_WITH_CUDA`, not part of the
  header-only library. nvcc and a GPU enter only with `GRAPHLINT_BUILD_DEVICE_TESTS`.
- `demo_check` runs in the host-only test job, so the committed `graphs/*.json` are verified on a
  machine with no toolkit and no card, next to the model and JSON suites.

`0.1.0` will be the first tag; the API is unstable until then.
