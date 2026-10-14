# SPDX-License-Identifier: GPL-3.0-or-later
try:
    from PyQt5.QtSvgWidgets import *  # noqa: F401,F403
except ImportError:
    from PyQt5.QtSvg import *  # noqa: F401,F403
