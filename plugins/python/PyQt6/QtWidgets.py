# SPDX-License-Identifier: GPL-3.0-or-later
# PyQt6.QtWidgets -> PyQt5.QtWidgets
import PyQt5.QtWidgets as _mod
from PyQt5.QtWidgets import *  # noqa: F401,F403

for _name in dir(_mod):
    if _name not in globals() and not _name.startswith("__"):
        globals()[_name] = getattr(_mod, _name)

from ._qt6_compat import patch_module
patch_module(_mod)
