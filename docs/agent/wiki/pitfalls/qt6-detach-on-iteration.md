---
type: pitfall
updated: 2026-10-14
sources:
  - libs/image/kis_image.cc (projectionUpdatesFilters, projectionUpdatesFiltersLock, projectionUpdatesFiltersSnapshot())
  - libs/image/kis_processing_applicator.cpp (UpdateCommand::partA/partB)
  - libs/libkis/Node.cpp (Node::save)
  - libs/image/tests/KisImageProjectionUpdatesFilterTest.cpp
  - Qt 6 qarraydatapointer.h (needsDetach(): `!d || d->needsDetach()`), qlist.h (non-const begin()/end() call detach())
related:
  - weak-pointer-map-keys.md
  - sip-return-types.md
---

# Iterating a Qt 6 container is a write

A non-const `begin()`, `end()`, `rbegin()` or `rend()` of a Qt 6 `QList`/
`QVector` calls `detach()`. `needsDetach()` is true not only for a shared
container but also for one that was never allocated (`d == nullptr`, a new,
empty list). `detach()` then builds a new block and swaps it in: it
**writes** the container. Reading a container from one thread while
another thread changes it is a data race anyway; with this detach, even a
"read-only" loop in a non-const member function can overwrite another
thread's `append()`.

## Where it happened

`KisImage` kept its projection updates filters in an unsynchronized
`QVector`. Stroke workers change it (`UpdateCommand` of
`KisProcessingApplicator` calls `disableDirtyRequests()` in `partA()` and
`enableDirtyRequests()` in `partB()`), while `refreshGraphAsync()` and
`requestProjectionUpdate()` iterate it from any thread, including the GUI
thread.

libkis `Node::save()` creates a new image, starts an asynchronous
`cropImage()` stroke and immediately calls `initialRefreshGraph()` on the
GUI thread. When the worker's `partA()` appended the drop-all filter while
the GUI thread's loop detached the still-unallocated vector, the filter was
lost but its cookie stayed in `disabledUpdatesCookies`. `partB()` then ran
`last()` on the empty vector: access violation reading `FFFFFFFFFFFFFFF0` in
`KisImage::removeProjectionUpdatesFilter`. Solstice 0.2.0-beta.3 crashed
this way after a handful of `Node.save()` calls (about 7 in one report; the
`krita-spine-export` plugin saves one PNG per layer). It is not
GPU-specific; it was seen on RGBA float documents converted for the GPU
engine, where timing made it likelier. Fixed 2026-10-14: the vector is
guarded by `projectionUpdatesFiltersLock`, and readers iterate a snapshot
copied under the lock (filter callbacks run outside it). The regression test
`KisImageProjectionUpdatesFilterTest` crashed in `refreshGraphAsync()`
within a second before the fix.

## Rule

- A container shared between threads needs a lock for readers too. Copy it
  under the lock and iterate the copy, or hold the lock while iterating.
- In single-threaded code, prefer `std::as_const()`, `cbegin()`/`crbegin()`
  or a range-for over a `const` reference when the loop does not modify the
  container; it avoids an unexpected detach (and a copy of shared data).
- A crash reading `FFFFFFFFFFFFFFF0` (or `-8`, `-4`) is `last()` on an empty,
  unallocated Qt 6 container: pointer `nullptr`, size 0, minus one element.
