# Conventions — `graph-lint`

Normative: when one of your habits contradicts this file, the file wins.
It describes this repository — code, tests, docs, history — not a general style guide, not a diary.

## 1. Language

Everything that lands in a commit is English: code, comments, `docs/`, README, CHANGELOG, commit
messages, CI steps. It is the shape of `cuda-timer`, `poolkit` and `ulpgrad`, and the first thing a
reviewer checks. Decisions are taken in conversation and arrive here already written in English.

## 2. Comments

A comment stays only if it says what the code cannot: why that choice, which constraint forces it,
which bug it avoids, which behaviour is not obvious from the line below. If the function name says
it, the comment is not needed.

Target in `include/`: ≤ 10% of non-blank lines are comments, no block runs past 3
consecutive lines. Measured: `poolkit/include` 8.0% and zero blocks over 3 lines (5.1% repo-wide),
`cuda-timer/include` 20.4% with blocks up to 12 lines. The first is the shape to copy; the second is
the Doxygen we do not want here. Past 3 lines it is prose: it goes to `docs/` or it goes away. Only
generated licence headers are exempt.

Good — the why survives a rewrite of the implementation:

    // BFS per edge, not the bitset closure: 5 000 x 625 B ≈ 3 MB for a question about one edge.
    // weight_kind "levels", not ns: without --weights there are no timings; a ns threshold exits 2.

Bad — the code below already says it:

    // This function computes the transitive reduction: iterate the edges, check for a path.
    // @param g the graph

## 3. Prose blacklist

| Don't write | Why it shows, and what to write instead |
|---|---|
| A paragraph announcing what you are about to do instead of why | it takes the first line of a section without a fact; the first line is already the fact |
| `@param g the graph`, `@return the count` | restates the signature; document the contract (ownership, valid ids, cycles) or nothing |
| List items that need a sub-explanation | if an item needs a paragraph it is not an item; write prose or cut it |
| `robust`, `complete`, `effective`, `seamlessly`, `comprehensive` | not verifiable; write the number, the command or the limit |
| "this approach guarantees…" | it grades itself; the reader decides, you bring the test that proves it |
| Tables with more than 3 columns | nobody reads them, and the third column is prose; two columns or a list |
| Emoji in technical docs | they change the tone and age badly; none |
| The same fact in README, CHANGELOG and a comment | three copies, two will diverge; one home, the others link |
| `Why this matters`, `Key takeaways`, `In summary` | they summarise a text already read; none |
| Bold on half the sentences | if everything matters nothing does; only on the term you are defining |

## 4. Docs (`docs/`)

- `topology.md` — what levels, depth, width, parallelism are; that `depth` in levels is not a time (two
  graphs with depth 10 can take 10 µs and 10 ms); the transitive reduction is unique on a DAG, cost
  `O(E(V+E))`.
- `rules.md` — per rule: what it measures, the known false positive, what to do. A rule without its
  false-positive sentence is not written; `applicable` / `not_applicable` per source live here too.
- `report.md`, `emitters.md`, `include/graphlint/json.hpp` — the JSON contracts (sorted keys, no
  timestamp or paths, `schema_version`), byte-for-byte determinism, why the DOT is ours (ids shared
  with the JSON, readable diff) and why `cudaGraphDebugDotPrint` is not the backend; Mermaid only
  compressed and under `--max-nodes`.
- `limits.md` — the table of what cannot be known: kernel name and parameters, mutations on the exec,
  the effect of instantiate, durations, host-side `cudaMalloc`. Read it before trusting a verdict.

One fact lives in one place. The README links, `docs/` explains, the CHANGELOG says what changes for
whoever upgrades. The same six-word sentence in two files is already a duplicate.

## 5. README

Mandatory sections, in this order: What it is (the links to `docs/`); What it is not (the table
against the incumbents, `cudaGraphDebugDotPrint` first); When to use it (three audiences and the two
commands); Limits (links `docs/limits.md`, does not copy it); Quality (test count and CI matrix,
numbers reproducible with `ctest`); Repository structure (full tree).

A number is written only if it is measured, if the command that reproduces it sits next to it and if
the raw file is committed in `docs/bench/`. A number that cannot be reproduced is a bug in the README.
Algorithms are not explained here: link `docs/topology.md`.

## 6. Commit

`<type>: <lowercase imperative>`, types `feat|fix|docs|ci|build|tests|chore|release`, no trailing
period, subject ≤ 75 characters. The subject describes the change for whoever reads the code, not the
process: no `hygiene`, `cleanup`, `cut noise`, percentages of removed lines, "refactor" alone. The
body, when the change is not obvious, goes symptom → cause → fix → how it was verified ("Verified:
host 12/12, device 3/3 skipped").

One commit per logical unit, never `wip`, never `git add -A` in a script. A commit that touches every
folder is repeated all over the file list on GitHub: if you have to rename something everywhere, fold
it into a commit that makes sense for those folders. No PRs on your own repos: commit to `main`.

A commit that removes thousands of comment lines is the signal that the text was generated, and
rewriting history to hide it is not a fix: it must not exist here, because comments are written right
in the first commit.

## 7. Tests

`TEST(Suite, ExpectedBehaviour)`, for example `TEST(Reduction, DropsEdgeImpliedByTwoHopPath)`. Never
`Test1`, `Works`, `Basic`. A test that cannot run exits 77, ctest reports it SKIPPED and not green.

The assertion carries the expected number computed by hand, with the computation in the comment:
chain `a→b→c` plus `a→c` gives 3 edges, 1 redundant, minimal DAG 2, depth 3, width `[1,1,1]`. Depth
and reduction also have a naive `O(V³)` reference on graphs ≤ 60 nodes; when the two diverge the naive
one is right until proven otherwise. The test that regenerates `graphs/` fails on divergence: the
README cannot age on its own.

## 8. C++

Header-only C++17, `#pragma once`, 4 spaces, ~100 columns. Includes in three groups separated by a
blank line: own header, system/CUDA, STL alphabetically; in tests GoogleTest after the library header.
Namespace `graphlint`, `detail` for internals, closed with its name. Files `snake_case`, types and
values `PascalCase`, functions and variables `snake_case`, private members with a trailing `_`, macros
`GRAPHLINT_`. No `using namespace` in headers, no ceremonial Doxygen.

`-Wall -Wextra -Wpedantic -Werror` on host and `/W4 /WX` on MSVC. On `.cu` `-Wall -Wextra -Werror`
through `-Xcompiler`, never `-Wpedantic`: cudafe++ emits `#line`, GCC flags it, and with `-Werror`
every `.cu` fails. `[[nodiscard]]` on every pure query, all of them, not half: inside `EXPECT_THROW`
GCC fails with `-Werror=unused-result`, so the `(void)` cast goes in the same commit.

## 9. What does not enter the repo

- No rewriter: `--fix` rebuilding a `cudaGraph_t` does not exist; the reduction is safe on order, not
  on events, visibility and buffer reuse.
- No PyTorch, Python or bindings in v1.0.
- Only `cuda_source.hpp` may name CUDA types, and it includes `<cuda_runtime_api.h>`, never
  `<cuda_runtime.h>`.
- No number without its artifact in `docs/bench/`.
- No absolute paths, usernames or local plans in tracked files: the handoff document stays out,
  decisions that matter go into `docs/`.
- No undeclared dependency: the library has none.
- A false positive gets documented, not turned into a threshold to make CI pass.

## 10. How to check a file is in order

From the repo root, in Git Bash. Every command reads tracked files only (`git grep`, `git ls-files`): a
build directory in the working tree must not change the answer.

1. Comment blocks over 3 lines: `git ls-files 'include/**' | xargs -r awk 'FNR==1{n=0} /^[[:space:]]*(\/\/|\/\*|\*)/{if(++n>3)print FILENAME":"FNR;next}{n=0}'` — zero.
2. Comment density: `git ls-files 'include/**' | xargs -r cat | awk 'NF{t++} /^[[:space:]]*(\/\/|\/\*|\*)/{c++} END{printf "%.1f%%\n", t ? 100*c/t : 0}'` — ≤ 10.0%.
3. Blacklist words: `git grep -niE 'robust|comprehensive|seamless|guarantee[sd]?|key takeaway|why this matters|in summary|effective' -- README.md docs include ':!docs/CONVENTIONS.md'` — zero.
4. Duplication: `git grep -c '<six-word phrase>'` — one file, count greater than zero.
5. Absolute paths and the account name: `git grep -nE '(^|[^A-Za-z])[A-Za-z]:[\\/]|/Users/|<account>' -- ':!LICENSE' ':!docs/CONVENTIONS.md'` — zero. The `[^A-Za-z]` boundary is what keeps `https://` out; without it the check never passes.
6. Lazy test names: `git grep -nE 'TEST\([^,]+, *(Test[0-9]+|Works|Basic)\)' -- tests` — zero.
7. `git grep -n 'Wpedantic' -- CMakeLists.txt`: the line does not reach the `.cu` targets, and the build stays green with `-Werror`.
8. `git show --stat HEAD`: if it lists every directory, it is not one logical unit and the commit gets split.
