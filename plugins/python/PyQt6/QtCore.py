# SPDX-License-Identifier: GPL-3.0-or-later
# PyQt6.QtCore -> PyQt5.QtCore
import PyQt5.QtCore as _mod
from PyQt5.QtCore import *  # noqa: F401,F403
from PyQt5.QtCore import (  # noqa: F401
    PYQT_VERSION_STR,
    QT_VERSION_STR,
    QCoreApplication,
    QEvent,
    QEventLoop,
    QObject,
    QThread,
    Qt,
    pyqtBoundSignal,
    pyqtProperty,
    pyqtSignal,
    pyqtSlot,
)

for _name in dir(_mod):
    if _name not in globals() and not _name.startswith("__"):
        globals()[_name] = getattr(_mod, _name)

from ._qt6_compat import patch_module
patch_module(_mod)
