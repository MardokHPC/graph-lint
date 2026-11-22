# The graph document

Schema 1. One JSON object per graph, written by `dump_graph` and read by `parse_graph`; it is the
input of `graphlint_report` and the `graph` block of a report. The document is an artifact: keys in
sorted order, no timestamp, no local path, and the same graph always gives the same bytes.

The writer is the reference. This page exists so that a document can also be written by hand, and so
that a parse error can be read without opening `json.hpp`.

## Start from a skeleton

```bat
graphlint_report --template graph.json
```

That writes the smallest document with a real shape: two kernels in a chain, `weight_kind` `levels`,
`source` `file`. It refuses to overwrite an existing file; edit it or pick another path.

If the graph already exists as a `cudaGraph_t`, do not write JSON at all: read it with
`from_cuda` and write the model with `dump_graph` (`docs/limits.md` says what the reader cannot
see). The template is for the case where there is no handle.

## The object

```json
{
  "children": [],
  "edges": [
    [0, 1]
  ],
  "nodes": [
    {"block": [128, 1, 1], "grid": [1, 1, 1], "id": 0, "name": "kernel_a", "type": "kernel"},
    {"block": [128, 1, 1], "grid": [1, 1, 1], "id": 1, "name": "kernel_b", "type": "kernel"}
  ],
  "schema_version": 1,
  "source": {"kind": "file"},
  "weight_kind": "levels"
}
```

| key | type | where | notes |
|---|---|---|---|
| `children` | array | every graph object | one nested object per `child_graph` node, in id order |
| `edges` | array of `[int, int]` | every graph object | `from < to`, both inside the node list |
| `nodes` | array | every graph object | the node at index `i` has `id` `i` |
| `schema_version` | int | root only | `1`; any other value is refused |
| `source` | object | root only | `{"kind": "capture" \| "built" \| "file"}` |
| `weight_kind` | string | root only | `"levels"` or `"ns"` |

Keys are in sorted order and the reader enforces it: an object whose keys are out of order is
refused, not reordered. That is what makes two documents comparable with `diff` and what keeps the
writer and the reader from disagreeing about a spelling.

A nested object carries `children`, `edges` and `nodes` only. `schema_version`, `source` and
`weight_kind` describe the whole document and live at the root; a nested copy is an unknown key.

## The node

`id` and `type` are required. `name` is optional and omitted when empty. Everything else depends on
`type`.

| type | required | forbidden |
|---|---|---|
| `kernel` | `grid`, `block` | — |
| `mem_alloc` | `mem_ptr` | — |
| `mem_free` | `mem_ptr` | — |
| `child_graph` | `child` | — |
| `memcpy`, `memset`, `host`, `empty`, `event_record`, `event_wait`, `ext_semaphore`, `unknown` | — | — |

| field | type | applies to | notes |
|---|---|---|---|
| `alloc_bytes` | uint | `mem_alloc` | bytes requested; `0` is written as absent |
| `block` | `[u32, u32, u32]` | `kernel` | every dimension ≥ 1 |
| `child` | int | `child_graph` | index into this graph's `children`, not a node id |
| `grid` | `[u32, u32, u32]` | `kernel` | every dimension ≥ 1 |
| `mem_ptr` | string | `mem_alloc`, `mem_free` | `"0x"` and lowercase hex, the address the driver reported |
| `name` | string | any | kernel name or a label; the reader cannot resolve a kernel symbol |
| `shared_bytes` | uint | `kernel` | dynamic shared memory; `0` is written as absent |
| `weight` | uint | any, only when `weight_kind` is `ns` | nanoseconds; absent in a `levels` document |

An empty `name`, `alloc_bytes: 0` and `shared_bytes: 0` are accepted by the reader and never written
back: the writer is the reference, so a document that spells them parses, and its next dump drops the
field.

The field/type pairs are not advisory. A `grid` on a `memcpy` is a `field "grid" is not valid for
type "memcpy"` error, and a zero dimension is refused even on a kernel: the document describes a
launched node, not a template placeholder.

## Invariants

- **Ids are dense and ordered.** Node `i` must have `id` `i`. A gap is refused where it appears; the
  writer never produces one.
- **Edges point forward.** Every edge is `[from, to]` with `from < to` and `to` inside the list. Two
  consequences: a document cannot encode a cycle, and the reader does not need to test for one.
- **Children follow their owners.** The `child_graph` nodes, in id order, own `children[0]`,
  `children[1]`, …; `children` must have exactly as many entries, and every entry must be owned.
  Reordering the nodes renumbers the children, which is why `canonicalize` keeps the two in step.
- **Nesting stops at eight.** The root is level 1, so the deepest legal document is the root plus
  seven nested children.
- **Weights match the kind.** `levels`: no node carries `weight`, and every node weighs one hop.
  `ns`: every node carries one, nested graphs included, because `weight_kind` is inherited from the
  root.

An empty graph is legal: `{"children": [], "edges": [], "nodes": [], ...}`. Its topology is depth 0,
and a report over it has no findings.

## Reading a parse error

The tools print `path:line:column: detail` and exit 2. The detail names the first fault only; fixing
it may reveal another one.

| what you wrote | what it says |
|---|---|
| keys out of order | `unsorted key "nodes"` |
| `"source": "file"` | `expected '{'` (the field is `{"kind": "file"}`) |
| node ids `0, 2` | `expected id 1, found 2` |
| edge `[3, 1]` | `edge [3, 1] is not from < to` |
| a kernel without `block` | `kernel node requires "block"` |
| `weight` with `weight_kind: "levels"` | `weight is not allowed when weight_kind is "levels"` |
| `"type": "memcpy"` with `grid` | `field "grid" is not valid for type "memcpy"` |

## What the document cannot say

The document is the model, not the graph. Kernel parameters and their values, the `cudaGraphExec_t`
built from it, the mutation of that exec, the edge ports of CUDA 12.3+, and any duration are not
representable: the API does not expose them to a reader. `docs/limits.md` lists each one and the
reason it is absent instead of pending.

A weight in `ns` is the one number in a document that did not come from the driver: it comes from
`weights.json`, bound to a graph by node count and a structural fingerprint
([`weights.md`](weights.md)). Without it, a document is topology and nothing more.
