# SPDX-License-Identifier: GPL-3.0-or-later
# PyQt6.QtNetwork -> PyQt5.QtNetwork
import PyQt5.QtNetwork as _mod
from PyQt5.QtNetwork import *  # noqa: F401,F403

for _name in dir(_mod):
    if _name not in globals() and not _name.startswith("__"):
        globals()[_name] = getattr(_mod, _name)
