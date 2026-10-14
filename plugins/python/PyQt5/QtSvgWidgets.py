# SPDX-License-Identifier: GPL-3.0-or-later

try:
    from PyQt6.QtSvgWidgets import *  # noqa: F401,F403
except ImportError:
    # Qt5 put QSvgWidget in QtSvg; fall back to that shape.
    from PyQt6.QtSvg import *  # noqa: F401,F403
