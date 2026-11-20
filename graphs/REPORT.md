# M2 device demo — what ran, what the artifacts prove

Generated on one machine, from real `cudaGraph_t` handles, by `tests/device/demo_graph.cu`
through `from_cuda` (`src/cuda_source.cpp`). Nothing in `graphs/` is written by hand.

## Environment

| item | value |
|---|---|
| GPU | NVIDIA GeForce RTX 2060 SUPER, sm_75 |
| driver | 610.88, reports CUDA API 13.3 |
| runtime the demo was compiled against | CUDA 12.6 (V12.6.20), `CUDART_VERSION` 12060 |
| host compiler | MSVC 19.44.35229 (VS 2022 BuildTools 17.14), Ninja + CMake 4.4.3 |
| OS | Windows 10 22H2 (10.0.19045.6466) |

## The four commands

```bat
build-host.bat clean && build-host.bat configure && build-host.bat build && build-host.bat test
build-device.bat clean && build-device.bat configure && build-device.bat build && build-device.bat demo
build-device.bat test
set CUDA_VISIBLE_DEVICES=-1 && ctest --test-dir build-dev --output-on-failure
```

1. Host build, `GRAPHLINT_WITH_CUDA=OFF`: 43 cases in 4 ctest entries (the three host suites
   plus `demo_check`), all green, no CUDA toolkit involved. The core needs nothing, and the
   committed artifacts are verified on every host build.
2. Device build, `GRAPHLINT_WITH_CUDA=ON` + `GRAPHLINT_BUILD_DEVICE_TESTS=ON`: zero warnings
   with `/W4 /WX` on the host TUs and `-Xcompiler=/W4 -Xcompiler=/WX` on the `.cu` files
   (no `-Wpedantic`, as agreed: nvcc's `#line` directives are not ours).
3. `ctest` on the GPU box: 5/5 green — the four host entries plus `graphlint_test_cuda`
   (5 cases: chain + geometry, `cudaMallocAsync` alloc/free nodes, child graph, the depth
   bound, live capture 25/29).
4. Same `ctest` with no visible card: `graphlint_test_cuda` exits 77, ctest prints
   `5/5 ... ***Skipped` and `(Skipped)`, exit code 0. The four host entries, `demo_check`
   included, still run: none of them opens a handle.

## What was read, per shape

| artifact | nodes | edges | source | types | bytes |
|---|---|---|---|---|---|
| `built.json` | 27 | 30 | `built` | 27 kernel | 2493 |
| `captured.json` | 25 | 29 | `capture` | 25 kernel, 0 empty | 2334 |
| `pingpong.json` | 12 | 21 | `capture` | 12 kernel | 1261 |
| `chain.json` | 1000 | 999 | `built` | 1000 kernel | 89792 |

`demo_check` (host, no CUDA) re-asserts these numbers from the parsed documents, plus: dense
ids, unit geometry, the single in-degree-4 join in `captured`, the exact edge set
`i->i+1` (11) ∪ `i->i+2` (10) in `pingpong`, `edges[k] == [k, k+1]` in `chain`, and a
byte-identical `dump(parse(file))` for all four. The expectation comes from the builder, not
from the reader, so a systematic error in `from_cuda` cannot cancel out.

## Two consecutive runs

`demo_graph` was run twice; the four files were copied out between runs and compared with
`fc /b` plus SHA256. **All four are byte-identical**, including `built` and `captured`, whose
four branches are structurally tied (F2). So the F2 residue — a permutation inside one tie
class when the API order moves — is **not visible on this driver today**. That is a
measurement on one driver, not a proof: the tie-break is still the API index, and a future
driver that reorders equal nodes would permute ids inside those branches for `built` and
`captured` only. `chain` and `pingpong` are unique by construction (only one node is ready at
any step), so their bytes do not depend on the enumeration at all.

```
built.json    8aa3903056cf8dcafcdea710386f8fb93d16ffb0d79b25425137fd75c884a2e0
captured.json 5a8aa8a88499dc9051c47fedb81fa6914d2450168d54ef9b1ae56f0993546d2d
pingpong.json 86575b04f256011992a74e352e77195c0d86b794ba05ff978b6f754126790ab0
chain.json    daadf3675fff3d6b4f7f6bf172812531d3572f7dfe98a0fa2c1f30ec7271d7c2
```

## What the demo does not cover

- No memcpy, memset, host or external-semaphore nodes in `graphs/`: all four shapes are
  kernel-only captures or kernel-only builds. The `mem_alloc`/`mem_free` getters are
  exercised only by `tests/device/test_cuda_source.cu`, and a real `cudaMallocAsync` capture
  is not in the artifacts.
- No child graph in `graphs/`: the recursion is covered by tests only — depth 1, and the
  8-level bound (8 levels convert and round-trip, 9 are refused by the reader).
- One driver, one GPU, one toolkit. The reader also compiles against CUDA 12.1 headers
  (`-DGRAPHLINT_WITH_CUDA=ON` without pinning finds the first toolkit on PATH), but no
  artifact was generated with it.
- Kernel names are absent by design: M2 does not read `func`, so every node has no `name`.
- `cudaGraphGetEdges`, not `_v2`: an edge carrying port/data (12.3+) would arrive as a plain
  dependency. None was exercised here.
- 1000 nodes is the largest input; nothing about tens of thousands is measured.
- Two runs on the same driver say nothing about stability across driver versions.

## One place where the SDK signature is wrong

`cudaGraphMemFreeNodeGetParams` is declared `(cudaGraphNode_t, void *dptr_out)` but documents
`dptr_out` as "pointer to return the device address" and writes through it: passing a null
value returns `cudaErrorInvalidValue` (checked on 12.6). `src/cuda_source.cpp` therefore hands
it the address of a `void*` with one commented `reinterpret_cast`; `void*` holds a `void**`,
so the round trip is well defined. The alternative — leaving `mem_ptr` at zero — would make
the alloc/free pair uncorrelatable in `report.json`.
