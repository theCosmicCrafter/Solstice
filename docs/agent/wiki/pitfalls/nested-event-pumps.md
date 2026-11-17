---
type: pitfall
updated: 2026-11-16
sources:
  - libs/global/KisSynchronizedConnection.cpp (forceDeliverAllSynchronizedEvents())
  - libs/libkis/Document.cpp (activeNode(), setActiveNode())
  - libs/ui/KisDocument.cpp (lockAndCloneImpl())
  - plugins/python/ai_diffusion/util.py (exclusive_poll_tick)
  - plugins/python/kritamcp/__init__.py (_command_processing)
related:
  - qstringbuilder-deduced-types.md
---

# Calls that pump the event loop re-enter your timers

Several APIs run `qApp->processEvents()` internally. Any timer, queued
signal or autosave that is due fires **inside** that call:

- libkis `Document.activeNode()` / `setActiveNode()` call
  `KisSynchronizedConnectionBase::forceDeliverAllSynchronizedEvents()`.
- `KisDocument::lockAndCloneImpl()` pumps while it waits for the image
  lock.
- `busyWaitWithFeedback` and similar waits.

Qt does not deliver a timer to itself while its slot is still running. Each
*other* timer can still nest one level, though. When the number of timers
grows with the number of documents, nesting depth grows with it.

## Where it happened

- ai_diffusion keeps per-document 20 ms and 500 ms poll timers that call
  `activeNode()`. With about 23 documents open, the GUI thread hit
  0xC00000FD. Fixed with a process-wide guard shared by the timer ticks
  (2026-11-16, see `plugin-intake.md`).
- kritamcp `process_commands` nested once per queued command. It has a
  module-level guard.
- Autosave of one document started a second clone of another document
  inside the first clone's pump. Fixed with the `thread_local`
  `cloneInProgress` guard in `KisDocument::lockAndCloneImpl()`.

## Rule

A timer slot that calls a pumping API needs a reentrancy guard. Share the
guard across instances when there is one timer per document. Guard the
timer entry point only, not direct callers. Skip the nested tick instead of
queueing it, unless the event is one-shot, in which case defer it with
`QTimer::singleShot(0, ...)`. Don't suppress the pump globally: an earlier
attempt delayed node activation and broke `fill` right after `new_canvas`.
