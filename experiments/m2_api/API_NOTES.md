# M2 — reading a `cudaGraph_t`: what the official docs actually promise

Sources, all fetched 2026-11-20. Runtime: [Graph Management, current (v13.5)](https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__GRAPH.html) ·
[12.6](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-runtime-api/group__CUDART__GRAPH.html) ·
[11.8](https://docs.nvidia.com/cuda/archive/11.8.0/cuda-runtime-api/group__CUDART__GRAPH.html) ·
[11.4](https://docs.nvidia.com/cuda/archive/11.4.0/cuda-runtime-api/group__CUDART__GRAPH.html) ·
[11.0 types](https://docs.nvidia.com/cuda/archive/11.0/cuda-runtime-api/group__CUDART__TYPES.html) ·
[thread safety](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-runtime-api/graphs-thread-safety.html) ·
[Programming Guide 4.2](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/cuda-graphs.html).
Driver: [Graph Management](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__GRAPH.html) ·
[Execution Control](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__EXEC.html).
`since` = earliest release whose docs I opened that list the function (11.0/11.4/11.8/12.6/13.5), so "11.4" is a checked fact, not a guess; "11.8" = safe at the CI floor.
Signatures are as printed on the 12.6 page, with the `__host__ cudaError_t` prefix dropped; rows marked 13.x come from the v13.5 page.

## Topology (links above, Graph Management)

| function | signature | since | notes |
|---|---|---|---|
| `cudaGraphGetNodes` | `cudaGraphGetNodes(cudaGraph_t graph, cudaGraphNode_t *nodes, size_t *numNodes)` | 11.4 | `nodes == NULL` → count only in `numNodes`. Oversized buffer → tail set to NULL and `numNodes` rewritten to the real count. No filter argument: every node of that graph object (a child graph's interior nodes belong to the child, not the parent). **No statement about order.** |
| `cudaGraphGetEdges` | `cudaGraphGetEdges(cudaGraph_t graph, cudaGraphNode_t *from, cudaGraphNode_t *to, size_t *numEdges)` | 11.4 | `from` and `to` may both be NULL → count only; same tail-NULL rule. Counts are `size_t` in 11.8 and 12.6, so nothing to fix for big graphs; no large-graph behaviour, cap or chunked variant is documented. The v13.5 page shows this same name with an extra `cudaGraphEdgeData *edgeData` argument. |
| `cudaGraphGetEdges_v2` | `cudaGraphGetEdges_v2(graph, from, to, cudaGraphEdgeData *edgeData, size_t *numEdges)` | 12.3+ | marked "(12.3+)" in the 12.6 page. `edgeData` may alone be NULL, but only if all edges carry default data, else `cudaErrorLossyQuery`. Not present in 11.8; not listed at all in 13.5. |
| `cudaGraphGetRootNodes` | `cudaGraphGetRootNodes(graph, cudaGraphNode_t *pRootNodes, size_t *pNumRootNodes)` | 11.4 | same NULL/tail semantics. |
| `cudaGraphNodeGetType` | `cudaGraphNodeGetType(cudaGraphNode_t node, cudaGraphNodeType *pType)` | 11.4 | 11.4 and 11.8 render it as `cudaGraphNodeType * *pType` — doc typo; M0 compiles `&type` on 12.6. |
| `cudaGraphNodeGetDependencies` | `cudaGraphNodeGetDependencies(cudaGraphNode_t node, cudaGraphNode_t *pDependencies, size_t *pNumDependencies)` | 11.4 | per-node in-edges, NULL → count. Cheaper than a whole-graph `cudaGraphGetEdges` when you only need one node. |
| `cudaGraphNodeGetDependencies_v2` | `(node, pDependencies, cudaGraphEdgeData *edgeData, size_t *pNumDependencies)` | 12.3+ | as above plus edge data. |
| `cudaGraphNodeGetDependentNodes` | `cudaGraphNodeGetDependentNodes(cudaGraphNode_t node, cudaGraphNode_t *pDependentNodes, size_t *pNumDependentNodes)` | 11.4 | per-node out-edges, NULL → count. |
| `cudaGraphNodeGetDependentNodes_v2` | `(node, pDependentNodes, cudaGraphEdgeData *edgeData, size_t *pNumDependentNodes)` | 12.3+ | as above plus edge data. |

`cudaGraphNodeType` values by release: **11.0** = Kernel 0x00, Memcpy 0x01, Memset 0x02, Host 0x03, Graph 0x04, Empty 0x05 ·
**11.4** adds WaitEvent 0x06, EventRecord 0x07, ExtSemaphoreSignal 0x08, ExtSemaphoreWait 0x09, MemAlloc 0x0a, MemFree 0x0b ·
**11.8** unchanged (12 values, no Conditional) · **12.6** adds Conditional 0x0d · **13.5** adds MemcpyBatch 0x0e, Reserved16 0x10, ExternalEngine 0x11.
`Memcpy`/`Memset` are dimension-less since 11.0. `cudaGraphNodeTypeMemcpy1D` / `2D` / `3D`: **not found in the docs**, in any version — one `Memcpy` type, and dimensionality lives inside `cudaMemcpy3DParms` (the 1D add/set calls are variants of the same node, not a distinct type).

## Node parameters

| function | signature | since | notes |
|---|---|---|---|
| `cudaGraphKernelNodeGetParams` | `cudaGraphKernelNodeGetParams(cudaGraphNode_t node, cudaKernelNodeParams *pNodeParams)` | 11.4 | struct identical in 11.8 and 12.6: `{void *func; dim3 gridDim; dim3 blockDim; unsigned int sharedMemBytes; void **kernelParams; void **extra;}`. `func` is `void*`, not a typed handle. `kernelParams` is a `void**` with **no length anywhere in the struct**. Returned `kernelParams`/`extra` and the values they point to are node-owned, valid until the node is destroyed or its params change, must not be modified; you get back *either* one, whichever was set last. `extra` exists already in 11.8. |
| `cudaGraphMemcpyNodeGetParams` | `(cudaGraphNode_t node, cudaMemcpy3DParms *pNodeParams)` | 11.4 | one node type for every dimensionality; 1D copies are expressed inside `cudaMemcpy3DParms`. |
| `cudaGraphMemsetNodeGetParams` | `(cudaGraphNode_t node, cudaMemsetParams *pNodeParams)` | 11.4 | `{void *dst; unsigned int value; unsigned int elementSize; size_t width; size_t height; size_t pitch}`. |
| `cudaGraphHostNodeGetParams` | `(cudaGraphNode_t node, cudaHostNodeParams *pNodeParams)` | 11.4 | `{cudaHostFn_t fn; void *userData}` — a raw host pointer, nothing nameable. |
| `cudaGraphEventRecordNodeGetEvent` | `(cudaGraphNode_t node, cudaEvent_t *event_out)` | 11.4 | exact name is `...EventRecordNodeGetEvent`; returns the event handle only. |
| `cudaGraphEventWaitNodeGetEvent` | `(cudaGraphNode_t node, cudaEvent_t *event_out)` | 11.4 | exact name is `...EventWaitNodeGetEvent`. |
| `cudaGraphChildGraphNodeGetGraph` | `(cudaGraphNode_t node, cudaGraph_t *pGraph)` | 11.4 | does **not** clone: changes to the returned graph are reflected in the node, node keeps ownership; alloc/free nodes cannot be added to it. |
| `cudaGraphMemAllocNodeGetParams` | `(cudaGraphNode_t node, cudaMemAllocNodeParams *params_out)` | 11.4 | fields (11.8 = 12.6): `size_t accessDescCount; cudaMemAccessDesc *accessDescs; size_t bytesize; void *dptr; cudaMemPoolProps poolProps`. `poolProps`/`accessDescs` are node-owned, valid until destroy, must not be modified. `dptr` is the allocation, fixed across instantiations and launches. |
| `cudaGraphMemFreeNodeGetParams` | `(cudaGraphNode_t node, void *dptr_out)` | 11.4 | the freed address only; nothing says which alloc node it pairs with. |
| `cudaGraphExternalSemaphoresSignalNodeGetParams` | `(cudaGraphNode_t hNode, cudaExternalSemaphoreSignalNodeParams *params_out)` | 11.4 | note the `hNode` spelling (only these and their Set/Exec variants use it). |
| `cudaGraphExternalSemaphoresWaitNodeGetParams` | `(cudaGraphNode_t hNode, cudaExternalSemaphoreWaitNodeParams *params_out)` | 11.4 | `numExtSems` is a field of that struct (the exec setter refuses to change it). |
| `cudaGraphNodeGetEnabled` | `(cudaGraphExec_t hGraphExec, cudaGraphNode_t hNode, unsigned int *isEnabled)` | 11.8 | exec, not template. "Currently only kernel, memset and memcpy nodes are supported." Not in the 11.4 page. |
| `cudaGraphNodeSetEnabled` | `(cudaGraphExec_t hGraphExec, cudaGraphNode_t hNode, unsigned int isEnabled)` | 11.8 | disabled node behaves as an empty node; params untouched; future launches only; template node not modified. |
| `cudaGraphNodeGetParams` | `(cudaGraphNode_t node, cudaGraphNodeParams *nodeParams)` | 13.x | not in 12.6 or 11.8. The generic reader; big caveat quoted in the last section. |
| `cudaGraphNodeGetLocalId` | `(cudaGraphNode_t hNode, unsigned int *nodeId)` | 13.x | "The local nodeId and graphId together can uniquely identify the node." |
| `cudaGraphNodeFindInClone` | `(cudaGraphNode_t *pNode, cudaGraphNode_t originalNode, cudaGraph_t clonedGraph)` | 11.4 | the documented way to map a node from the original into a clone; a clone is a separate graph object. |
| kernel name from `func` | — | — | **not found in the docs** (runtime API). Driver candidate only, see below. |
| kernel parameter count/sizes | — | — | **not found in the docs** (runtime API). Driver candidate only, see below. |
| capture vs hand-built graph | — | — | **not found in the docs**. |

## Driver API (12.6 pages linked above)

| function | signature | since | notes |
|---|---|---|---|
| `cuGraphKernelNodeGetParams` | `cuGraphKernelNodeGetParams(CUgraphNode hNode, CUDA_KERNEL_NODE_PARAMS *nodeParams)` | 12.6 docs | the only place where the kernel handle is *typed*: `CUfunction func` (plus `kernelParams`, `extra`, and in 12.6 `CUkernel kern`, `CUcontext ctx`). |
| `cuFuncGetName` | `cuFuncGetName(const char **name, CUfunction hfunc)` | 12.6 docs | generic "function name for a CUfunction handle"; mangled unless C linkage. The description never mentions graphs, and no page says a handle recovered from a graph node is valid input → **not guaranteed**. Not found on the 11.8 Execution Control or Module Management pages I checked. |
| `cuFuncGetParamInfo` | `cuFuncGetParamInfo(CUfunction func, size_t paramIndex, size_t *paramOffset, size_t *paramSize)` | 12.6 docs | per-parameter offset and size in the device-side layout; documented for device-side updates (`cudaGraphKernelNodeSetParam`), needs a `CUfunction`. `cuFuncGetParamCount` is absent from the 12.6 driver docs (it appears in 13.x). |

## Ordering and ids

**Nothing about ordering is guaranteed.** `cudaGraphGetNodes` is documented with one sentence — "Returns a list of graph's nodes" — identical in 11.4, 11.8, 12.6 and 13.5. Nothing states that it is insertion order, that it is topological order, or that it is stable across calls; the only way to correlate nodes with edges is the handle, inside one serialized snapshot. Treat every index you compute as valid for that snapshot only.

Handle lifetime is the only documented property: `cudaGraphDestroyNode` / `cudaGraphExecDestroy` say "Use of the handle after this call is undefined behavior". A clone is a separate graph object, and `cudaGraphNodeFindInClone` is the documented way to map a node from the original into it. The only documented identifiers are 13.x-only: `cudaGraphGetId` + `cudaGraphNodeGetLocalId` ("together can uniquely identify the node", and they match `cudaGraphDebugDotPrint`), plus a separate `cudaGraphNodeGetToolsId`.

Snapshot coherence: per [Graph object thread safety](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-runtime-api/graphs-thread-safety.html), "No API or pair of APIs is guaranteed to be safe to call on the same graph object from two different threads without serialization" — so the count-then-fill pattern is only safe if you own the graph exclusively.

## Version drift 11.8 → 12.x

- **`cudaGraphInstantiate` changed shape**: 11.8 = `(pGraphExec, graph, pErrorNode, pLogBuffer, bufferSize)`; 12.x = `(pGraphExec, graph, flags = 0)`. Use `cudaGraphInstantiateWithFlags` (11.4+) to compile on both.
- **12.3**: `cudaGraphEdgeData` plus `_v2` variants (`GetEdges`, `AddDependencies`, `RemoveDependencies`, `NodeGetDependencies`, `NodeGetDependentNodes`) and `cudaGraphAddNode`. Guard with `CUDART_VERSION >= 12030` only if you read edge data; a reader that ignores it works on every version, since zeroed edge data is the full dependency.
- **12.x**: Conditional node type (0x0d, in the 12.6 enum), `cudaGraphInstantiateWithParams`, `cudaGraphExecGetFlags`, `cudaGraphNodeSetParams`, `cudaGraphExecNodeSetParams`. The 12.6 flags page lists AutoFreeOnLaunch (1), Upload (2), DeviceLaunch (4), UseNodePriority (8) — the 11.8 page lists only 1 and 8, so do not depend on 2/4 at the CI floor from the docs alone.
- **13.x only**: `cudaGraphNodeGetParams`, `cudaGraphGetId`, `cudaGraphExecGetId`, `cudaGraphNodeGetLocalId`, `cudaGraphNodeGetToolsId`, `cudaGraphNodeGetContainingGraph`, node types MemcpyBatch/ExternalEngine; and the `_v2` names disappear (the base name takes `edgeData`).
- **M0 cross-check**: no conflict. `cudaGraphNodeTypeWaitEvent` is exactly the documented name in 11.4/11.8/12.6/13.5 (0x06); `cudaGraphNodeTypeEventWait` exists in none of them. `cudaDriverGetVersion` is outside the pages read here, so it is not re-verified.

## What is not observable

- The kernel behind `func`: no runtime API at all, and `cuFuncGetName` is not documented as accepting a handle taken from a graph node — a cast may work, but nothing promises it.
- Kernel parameter count and sizes: the struct has no count, and the doc says that information "is retrieved directly from the kernel's image" — it is consumed, not returned.
- Whether a graph came from stream capture or from hand-built calls: no flag, field or query anywhere in the API.
- The struct you originally passed: 13.x `cudaGraphNodeGetParams` states the returned params "may not be identical to the struct provided at creation and may not be suitable for direct creation of identical nodes".
- Limits: no documented maximum node or edge count, so any "this graph is too big" verdict is your own constant, not a CUDA limit.
- Template-side node enablement: `cudaGraphNodeGetEnabled` only exists for an exec.
- Which of `kernelParams` / `extra` a node uses, and its length: only "whichever was set last", with no size.
