# SPDX-License-Identifier: GPL-3.0-or-later
#
# PyQt5 -> PyQt6 compatibility shim for vendored Krita plugins.
#
# Solstice's embedded Python ships PyQt6 only. Plugins written for Krita 5.x
# import the PyQt5 module names; this package re-exports the PyQt6 equivalents
# so those plugins can be vendored without rewriting every import.
#
# Coverage and limits are tracked in docs/agent/plugin-intake.md.
#
# On a Krita 5.x host where real PyQt5 exists this package is shadowed by the
# genuine one only if that one comes first on sys.path; installed layouts put
# share/krita/pykrita early enough that this shim is what plugins get, so it
# must stay harmless (pure re-export) on top of whatever Qt version is real.
