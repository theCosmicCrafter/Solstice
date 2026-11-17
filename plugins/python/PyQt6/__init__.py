# SPDX-License-Identifier: GPL-3.0-or-later
#
# PyQt6 -> PyQt5 compatibility shim for vendored plugins.
#
# Solstice's embedded Python on Qt5 builds ships PyQt5 only. Plugins written
# against Krita 6.x (e.g. vendored ai_diffusion 1.56) import the PyQt6 module
# names; this package re-exports the PyQt5 equivalents so those plugins can
# be vendored without rewriting every import.
#
# Coverage and limits are tracked in docs/agent/plugin-intake.md.
#
# Installed only on Qt5 builds (see plugins/python/CMakeLists.txt): on a
# Qt6 host the real PyQt6 is present and this package must not shadow it.
