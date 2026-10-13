---
type: pitfall
updated: 2026-10-12
sources:
  - libs/global/kis_shared_ptr.h (KisWeakSharedPtr::operator const T*())
  - libs/ui/KisLayerThumbnailCache.cpp (ThumbnailMap, findRecord())
  - libs/ui/tests/KisLayerThumbnailCacheTest.cpp
related:
  - build-format-test.md
---

# Weak pointers as ordered map keys

`KisWeakSharedPtr` has no `operator<`. `QMap`/`std::map` compare it through
`operator const T*()`, which returns the address while the object lives and
`nullptr` once it is destroyed. A key therefore changes its sort position
while it sits in the map. The map's ordering breaks and lookups of other,
living keys miss their records; inserting again does not repair it. Only
erasing the dead entries does.

## Where it happened

`KisLayerThumbnailCache` (upstream code) kept `QMap<KisNodeWSP, ThumbnailRecord>`.
The layer model notifies a removal while the undo command still holds the
node, so `cleanupDeletedNodes()` finds nothing to erase. The node is
destroyed later (undo limit, a discarded redo branch) without a
notification. From then on, some layers in the Layers docker showed a 1x1
placeholder (a small dot) instead of their thumbnail, until a layer was
added or removed. Tooltips were correct because they create their
thumbnail directly. A probe with 40 layers, 13 removed and destroyed, left
16 of the 27 living layers without a thumbnail, also after another idle run.

## Rule

Key such maps by something that does not change: the raw address
(`const KisNode *`), with the `KisNodeWSP` stored in the value. A record
belongs to a node only while that weak pointer is valid and points to the
node, because a new node can reuse a destroyed node's address. Clean up by
erasing values whose weak pointer is no longer valid. Fixed 2026-10-12;
regression test `KisLayerThumbnailCacheTest`.

Unordered containers (`QHash`) hash the converted pointer too, so the same
problem applies there.
