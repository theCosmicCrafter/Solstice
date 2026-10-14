---
type: index
updated: 2026-10-13
---

# Agent wiki

A persistent knowledge base maintained by coding agents, following the LLM
wiki pattern (sources -> wiki pages -> schema). It holds what the feature
documents do not: cross-cutting technical knowledge, the reasons behind
decisions, pitfalls found while working, benchmarks and archived phase
records.

Read this index before starting work; open the pages whose one-line summary
matches the task.

## Conventions (schema)

- **Single source of truth.** Feature behavior, invariants, build/test
  commands and manual checks live in the feature documents under
  `docs/agent/` (index in `AGENTS.md`). Wiki pages link to them instead of
  copying them. A wiki page may summarize, but the feature document wins in
  a conflict; fix the wiki page.
- **Page types and folders:**
  - `concepts/`: technical knowledge that spans features (how Krita/Qt
    behave, techniques);
  - `decisions/`: one decision per page, with the reason and date;
  - `pitfalls/`: things that went wrong and how to avoid them;
  - `benchmarks/`: measured numbers with date, hardware, workload and source;
  - `history/`: archived records (phase logs), moved verbatim; not edited
    except to append to the newest page;
  - `queries/`: saved answers to questions that are likely to come up again.
- **Frontmatter** on every page: `type`, `updated` (YYYY-MM-DD), `sources`
  (commits, `path:line` or symbols, documents, external URLs) and `related`
  (other wiki pages). History pages carry `status` instead of `updated`,
  and no `related` (they are archives; the section index in
  `docs/agent/gpu-engine.md` links them).
- **Names:** lowercase-hyphenated file names; one topic per page; update a
  page instead of adding a near-duplicate.
- **Evidence:** name the symbol or file a claim depends on, so a lint pass can
  check it still exists. Mark unverified statements as such.
- **Language:** English, like the other agent documents.

## Workflows

- **Ingest** (after each committed task): add or update the pages the work
  taught something about: a new concept, a decision with its reason, a
  pitfall, a benchmark. Append an entry to [log.md](log.md). Phase records
  of the GPU engine go to the newest `history/gpu-phases-*.md` page.
- **Query** (before a task): read this index, then the relevant pages and
  the feature documents they link to. Save a reusable answer in `queries/`.
- **Lint** (periodically, or when asked): check that referenced files and
  symbols still exist, benchmarks are not stale, pages do not contradict
  `AGENTS.md` or the feature documents, and every page is listed here. Log
  the pass.

## Pages

### Concepts

- [CPU/GPU bit-identical parity](concepts/cpu-gpu-bit-parity.md): which
  parity rule applies (bit-identical transforms, tolerance for the FFT-based
  Gaussian blur); how the GPU transform paths reproduce CPU results bit for bit (planning with CPU
  classes, `precise` doubles, correctly rounded division and square root,
  rounding to float and half, ordering, fallback), and the test pattern.
- [Krita copy and sampling semantics](concepts/krita-copy-semantics.md):
  `copyAreaOptimized()`, `bitBlt()`'s `fastBitBlt()` path, the Copy
  composite op clearing transparent pixels and clamping HDR colors in SIMD
  batches,
  `KisRandomSubAccessor` weights.
- [Qt numeric and geometry semantics](concepts/qt-numeric-and-geometry.md):
  `qRound()`, `QPolygonF::containsPoint()`, `boundingRect()` /
  `toAlignedRect()`, fuzzy comparisons in Qt 6.8.

### Decisions

- [GPU engine phase numbering](decisions/gpu-phase-numbering.md): how phase
  numbers are assigned, and the next free number.
- [`krita5.xmlgui` stays unchanged](decisions/xmlgui-unchanged.md): why menu
  changes avoid the main XMLGUI file.
- [Solstice version vs. Krita compatibility version](decisions/versioning.md):
  why `0.1.0-alpha` is shown while `6.0.5-prealpha` stays internal.
- [Upstream: weekly review, selective cherry-picks](decisions/upstream-selective-intake.md):
  Hermes reports, the user decides, Claude cherry-picks; dry-run lessons.

### Pitfalls

- [Build, format and test pitfalls](pitfalls/build-format-test.md):
  clang-format include sorting, shells, shader and struct layout mistakes,
  test environment, installation (including plugins after a vtable change).
- [Weak pointers as ordered map keys](pitfalls/weak-pointer-map-keys.md):
  a `KisWeakSharedPtr` key turns into `nullptr` when its object dies and
  corrupts the map (missing layer docker thumbnails).
- [Python bindings declare their own return types](pitfalls/sip-return-types.md):
  a `.sip` file that says `void` gives Python `None` for a C++ `bool`
  (`Node.save()`); `.sip` edits do not trigger a rebuild.

### Benchmarks

- [Transform and filter costs](benchmarks/transform-and-filter-costs.md):
  CPU baseline and GPU results for affine transforms, Liquify, Puppet Warp,
  Gaussian blur and other filters on a 2480x3508 RGBA32F layer.

### History

GPU engine phase records moved from `docs/agent/gpu-engine.md` (section index
there, under "Phase history"):

- [Reviews and memory budgets](history/gpu-reviews-and-memory.md)
- [Pixel brush and Wash (phases 4.1-4.16)](history/gpu-phases-4.01-4.16.md)
- [Blend modes, RGBA16F and transfers (phases 4.17-4.57)](history/gpu-phases-4.17-4.57.md)
- [Paint trace and latency analysis (phases 4.58-4.81)](history/gpu-phases-4.58-4.81.md)
- [Generated dabs and brush latency (phases 4.82-4.92)](history/gpu-phases-4.82-4.92.md)
- [Filters and transforms (phases 4.93-)](history/gpu-phases-4.93-.md) (newest)
- [GPU Engine user guide, development notes](history/gpu-user-guide-archive.md):
  the status and measurement sections removed from `docs/gpu-engine.md`
  before the first public release (2026-10-13)

### Queries

None saved yet.
