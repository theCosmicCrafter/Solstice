# SPDX-License-Identifier: GPL-3.0-or-later
# PyQt6.QtGui -> PyQt5.QtGui, plus the classes Qt6 moved out of QtWidgets.
import PyQt5.QtGui as _mod
from PyQt5.QtGui import *  # noqa: F401,F403

from PyQt5.QtWidgets import (  # noqa: F401  (QtWidgets in Qt5, QtGui in Qt6)
    QAction,
    QActionGroup,
    QFileSystemModel,
    QShortcut,
    QUndoCommand,
    QUndoGroup,
    QUndoStack,
)

for _name in dir(_mod):
    if _name not in globals() and not _name.startswith("__"):
        globals()[_name] = getattr(_mod, _name)

from ._qt6_compat import patch_module
patch_module(_mod)
