# SPDX-License-Identifier: GPL-3.0-or-later
# PyQtPyQt5.Qt catch-all: QtCore + QtGui + QtWidgets (+ extras when present).
from PyQt5.QtCore import *  # noqa: F401,F403
from PyQt5.QtGui import *  # noqa: F401,F403
try:
    from PyQt5.QtWidgets import *  # noqa: F401,F403
except ImportError:
    pass
try:
    from PyQt5.QtSvg import *  # noqa: F401,F403
except ImportError:
    pass
try:
    from PyQt5.QtPrintSupport import *  # noqa: F401,F403
except ImportError:
    pass
