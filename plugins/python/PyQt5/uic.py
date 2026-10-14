# SPDX-License-Identifier: GPL-3.0-or-later
#
# PyQt5.uic -> PyQt6.uic (loadUi / loadUiType exist in both)

from PyQt6.uic import *  # noqa: F401,F403
from PyQt6.uic import compileUi, loadUi, loadUiType  # noqa: F401
