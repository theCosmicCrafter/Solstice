---
type: pitfall
updated: 2026-11-16
sources:
  - CMakeLists.txt (-DQT_USE_QSTRINGBUILDER, -DQT_USE_FAST_OPERATOR_PLUS)
  - libs/ui/widgets/KisBrushStrokePreviewCache.cpp (process())
  - plugins/dockers/quickaccess/QuickColorSelectorDock.cpp (updateChannelGradients())
related:
  - build-format-test.md
---

# `auto` and deduced lambda returns capture a dangling QStringBuilder

Solstice compiles with `QT_USE_QSTRINGBUILDER`. With it, `a + b` on strings
is a lazy `QStringBuilder<A, B>` that holds **references** to its operands
and only builds a `QString` when converted. Anything whose type is deduced
keeps the builder instead of a string:

```cpp
const auto path = dir + '/' + key;          // builder; `dir + '/'` temp dies at ;
auto style = [&](const QString &g) {        // deduced return: builder whose
    return QStringLiteral("%1").arg(g) + h; // .arg() temp dies at return
};
```

Converting it later reads a dead stack object. The symptom is not a clean
crash at that line. It shows up as a garbage length in `QString::append`,
heap corruption (0xC0000374), or a crash in some later, unrelated
allocation. The location moves with stack layout, so the crash looks flaky.

## Where it happened

- `KisBrushStrokePreviewCache::process()` `const auto path = ...` (fixed
  2026-11-15, found by ASan as `stack-use-after-scope`).
- `QuickColorSelectorWidget::updateChannelGradients()` `style` lambda
  (fixed 2026-11-16). It had been there since the initial snapshot. It
  became deterministic only after `setCanvas()` started calling it during
  view activation, which crashed the first `new_canvas`.

## Rule

Spell out `QString` for any variable or lambda return that is built with
`+`: `const QString path = ...;`, `[&](...) -> QString { ... }`. When
sweeping, check both forms. A grep for `const auto x = ... + ...` misses
the lambda case.
