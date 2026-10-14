---
type: pitfall
updated: 2026-10-14
sources:
  - plugins/extensions/pykrita/sip/krita/Node.sip (save)
  - plugins/extensions/pykrita/sip/krita/Filter.sip (apply)
  - plugins/extensions/pykrita/sip/krita/Scratchpad.sip (setScale)
  - libs/libkis/Node.h (Node::save), libs/libkis/Filter.h, libs/libkis/Scratchpad.h
related:
  - build-format-test.md
---

# Python bindings declare their own return types

The Python API is generated from the `.sip` files in
`plugins/extensions/pykrita/sip/krita/`, not from the libkis headers. SIP
trusts the `.sip` declaration: when it says `void`, the generated wrapper
drops the C++ return value and Python gets `None`. Nothing fails at build
time, because a `bool` function can be called as if it were `void`.

## Where it happened

`Node::save()` returns `bool` (`libs/libkis/Node.h`), but `Node.sip`
declared it `void` (an upstream Krita bug). The external
`krita-spine-export` plugin checked `if not node.save(...)` and reported
"Could not write PNG" after every successful export (seen on 32-bit float
documents). A scan of all `.sip` files against `libs/libkis/*.h` found two
more: `Filter.apply()` and `Scratchpad.setScale()` (the latter is also
`const` in C++). All three were fixed on 2026-10-14.

## Rule

When adding or changing a libkis method, copy its full signature
(return type, `const`, default arguments) into the matching `.sip` file.
Check with kritarunner that Python receives the value (see the
[feature inventory](../../feature-inventory.md), "Undocumented custom
changes"). Rebuild target `python_module_PyKrita_krita` and install
`plugins\extensions\pykrita\sip`.

## Editing a .sip file does not trigger a rebuild

With SIP 5+, the generation step (`cmake/modules/sip-generate.py`) depends
only on the generated `<build>\plugins\extensions\pykrita\sip\pyproject.toml`
and `extra.pyi`, not on the `.sip` files. After a `.sip` edit, ninja reports
"no work to do" and `_tmp\krita\sipkritapart0.cpp` stays stale. Touch that
`pyproject.toml` before building the target, then check the generated
wrapper (for example `sipRes = sipCpp->save(...)`).

## Checking with kritarunner

- On Windows, pykrita replaces `PYTHONPATH`, so a script in a scratch
  folder is not found. Put it temporarily in `<install>\share\krita\pykrita`
  and remove it afterwards. kritarunner calls the function with one
  positional list (`def main(args)`); Python stdout is not captured, so
  write results to a file.
- Do not wrap the `cmd.exe /d /s /c "call env.bat && kritarunner ..."` call
  in Git Bash `timeout`: it then produces no output and hangs until killed.
- kritarunner has no file-format filters: every `Node.save()`,
  `Document.exportImage()` and `Document.saveAs()` returns `False`, and a
  `.kra` save even opens a modal "does not support this file format" dialog
  on the desktop. Check the `False` cases there and a successful save
  (`True`) in Solstice's Scripter. Verified 2026-10-14: `Node.save()`,
  `Filter.apply()` return `bool` (`Filter.apply()` gave `True`, and `False`
  on a locked node); `Scratchpad.setScale` is `-> bool`.

Known remaining differences that are not bugs: `ManagedColor.components()`
is `QVector<float>` in C++ and `QList<float>` in SIP (the same type in
Qt 6).
