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
- CI with a host-only job on Linux and Windows, plus a CUDA job that compiles the device path on
  three toolkit versions without executing it.
- `docs/limits.md`: what a `cudaGraph_t` cannot expose, and the canonical-id residue.

### Changed

- The CUDA reader is a separate static target behind `GRAPHLINT_WITH_CUDA`, not part of the
  header-only library. nvcc and a GPU enter only with `GRAPHLINT_BUILD_DEVICE_TESTS`.
- `demo_check` runs in the host-only test job, so the committed `graphs/*.json` are verified on a
  machine with no toolkit and no card, next to the model and JSON suites.

`0.1.0` will be the first tag; the API is unstable until then.
