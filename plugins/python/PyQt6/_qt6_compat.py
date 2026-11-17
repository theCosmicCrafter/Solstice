# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared helpers for the PyQt6 -> PyQt5 shim modules."""

import inspect as _inspect


def patch_module(mod):
    """Apply PyQt6-on-PyQt5 compatibility fixes to a re-exported module.

    - Qt6 renamed exec_() -> exec() on event-loop-style classes. Qt5 kept
      exec_() because exec is a keyword in Python 2; add exec() wherever
      exec_() exists and exec() does not.
    - Qt5.15 already provides the scoped enum spellings Qt6 requires
      (Qt.AlignmentFlag.AlignLeft, QMessageBox.StandardButton, ...), so no
      enum work is needed here.
    """
    for _name in dir(mod):
        try:
            cls = getattr(mod, _name)
        except AttributeError:
            continue
        if not _inspect.isclass(cls):
            continue
        try:
            if hasattr(cls, "exec_") and not hasattr(cls, "exec"):
                cls.exec = cls.exec_
        except (AttributeError, TypeError):
            # sip-wrapped classes can refuse attribute writes; skip them.
            continue
