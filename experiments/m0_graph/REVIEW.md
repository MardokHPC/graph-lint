# M0 spike — independent review

Evidence: `GL_DUMP_EDGES=1 cmd /c ...\run.bat` (dump in `%TEMP%\opencode\m0_dump.txt`);
reference `%TEMP%\opencode\naive.py` (levels by relaxation to fixpoint, reduction by per-edge
DFS); harness `%TEMP%\opencode\edgecases.cu` that `#include`s `shape.cu` unmodified and calls
`topology`/`redundant_edges`/`pointless_forks`/`report` on graphs I supply. The three source
files are untouched (`run.bat` regenerated `shape.exe`/`.lib`/`.exp` in place).

| shape | n | m | depth dev/naive | width 0..11 dev/naive | redundant dev/naive | lists |
|---|---|---|---|---|---|---|
| A | 27 | 30 | 17/17 | `1 1 2 1 1 4 4 4 1 1 1 1` = | 0/0 | = |
| B | 25 | 29 | 15/15 | `1 2 1 4 4 4 1 1 1 1 1 1` = | 1/1 `2->4` | = |
| C | 1000 | 999 | 1000/1000 | all 1 = | 0/0 | = |
| D | 12 | 21 | 12/12 | all 1 = | 10/10 `0->2 … 9->11` | = |

Every number in REPORT.md reproduces (3.4%, 48%, 1.59/1.67/1.00/1.00, minimal DAG
30/28/999/11). No mismatch in depth, width, counts, or edge identities.

## Claims

**(a) Confirmed on this data, not general.** 0 Empty in B (25 nodes) and D (12); joins are
multi-predecessor kernel nodes: B node 3 (in 2), 4 (2), 16 (**4**, the 4-way join). Caveats
the plan section flattens: in-degree ≥ 2 ≠ join — D's 10 double-pred nodes are event
skip-edges and D has no join; and only kernel-only captures in `Global` mode were exercised
(no memcpy, no `cudaMallocAsync`, no child graph, no wait on a pre-capture event), one
driver. "Capture never creates Empty" is a hypothesis about that subset.

**(b) Confirmed as stated; the fix is wrong.** All 10 D firings have `j ∈ succ(u)`, each the
redundant edge `u->u+2`; B `u=2 j=4` is the same defect (`2->4` is B's only redundant edge);
A `u=1 j=4` and B `u=0 j=3` have `j ∉ succ(u)` and survive. 11 of 13 firings overall are the
redundant-edge signal re-labelled. But "exclude `j ∈ succ(u)`" also silences a genuine fork:
`0->{1,2}->3` plus redundant `0->3` fires today (`sizes 1 1 0`) and goes silent under the fix.
Correct form: require ≥ 2 successors **other than j**.

**(c) Confirmed; the guard is arbitrary.** D: 12 nodes, depth 12, ratio 1.00, silent at
`shape.cu:184`. `n≥16 && ratio≥0.9` implies `depth≥15` (measured: 15-chain silent, 16-chain
fires), so the guard is really "≥ 15 sequential nodes": defensible as a noise filter, not as
topology — D is exactly §5.1's target and misses by three levels. Prevalence of a 12-kernel
ping-pong: **not measured**. D proves it is constructible in 16 lines and that any alternating
cross-stream chain yields `depth == nodes`, nothing about frequency. Document the effective
floor (depth 15) or write the guard as `depth >= N`.

## Defects (no number above depends on them)

- `shape.cu:176` — `nn/depth` on a 0-node graph prints `parallelism -nan(ind)`.
- `shape.cu:83`, `:107` — duplicate edges inflate out-degree: a 2-node graph whose only
  dependency is listed twice reports `pointless-fork u=0 j=1 sizes 0 0`.
- `shape.cu:63` — duplicates are never reduced: `0->1,0->1,1->2` prints `minimal DAG: 3 -> 3`
  where the minimal relation has 2 edges. 0 findings matches §5.2's wording; the number does not.
- `shape.cu:33`, `:54` — `out[e.first]` has no bounds check: an edge to a missing id is
  out-of-bounds. `dag-check`'s "edge su id inesistenti" exists nowhere. Not run: UB by inspection.
- `shape.cu:341,343` — denominator hardcoded `/4` even when `measured == false`: a failed capture
  prints `0/4`, indistinguishable from "rules are silent" (the §6.5 failure mode).
- OK: isolated nodes (depth 2, width `2 2`), one node (depth 1), duplicate edges in Kahn (levels
  still 0,1,2), cycles → message + exit 2 (self-loop, 2-cycle, cycle-with-source verified).

## Gate

**PASSED** on the letter: `pointless-fork` fires on B, a real 4-stream capture; `launch-shaped`
fires only on C, a 1000-chain built to satisfy it, so that firing carries no information. The
rule with material is `redundant-edge` (1/29 on B, 10/21 on D), which the gate never tested.

Consequences: (1) §0.1's mechanism is wrong, its conclusion is not — no Empty joins; redundant
edges come from event-wait edges duplicating stream order (in D every node depends on i−1 by
stream order and i−2 by the event). (2) §5.2's "grafi catturati, dove 0 è la risposta giusta"
must go: it makes D's 10 true findings look like a tool bug. The §9 prediction (HANDOFF:454)
inverts on captures — `redundant-edge` > `pointless-fork` > `launch-shaped` — and the planned
demotion of `redundant-edge` to info (HANDOFF:456) is cancelled. (3) After the fix,
`pointless-fork` keeps only the 1-kernel-per-branch diamond, the smallest honest fork, not a
demonstrated defect: re-specify or demote it before M4.
