# SPDX-License-Identifier: GPL-3.0-or-later
#
# PyQt5.QtWidgets -> PyQt6.QtWidgets, plus the classes that moved to QtGui
# in Qt6 but were commonly imported from QtWidgets in PyQt5 code.

from PyQt6.QtGui import (  # noqa: F401
    QAction,
    QActionGroup,
    QFileSystemModel,
    QShortcut,
    QShortcutEvent,
    QStandardItem,
    QStandardItemModel,
    QUndoCommand,
    QUndoStack,
)
from PyQt6.QtWidgets import *  # noqa: F401,F403

from PyQt5.QtCore import _add_exec_alias, _expose_enum_members

# Unscoped enum members (QMessageBox.Ok, QAbstractItemView.*,
# QSizePolicy.Expanding, QStyle.*, ...) and exec_ aliases for every
# dialog/menu class that renamed exec_() -> exec().
for _cls_name in dir():
    _cls = globals().get(_cls_name)
    if isinstance(_cls, type) and getattr(_cls, "__module__", "").startswith("PyQt6"):
        _expose_enum_members(_cls)
        _add_exec_alias(_cls)

del _cls_name, _cls

# QDesktopWidget was removed in Qt6; PyQt5 plugins call
# QApplication.desktop().screenGeometry(). Provide a minimal stand-in
# backed by the primary screen.
if "QDesktopWidget" not in dir():
    from PyQt6.QtGui import QGuiApplication
    from PyQt6.QtWidgets import QWidget

    class QDesktopWidget(QWidget):
        """Qt5 desktop-widget stand-in over QGuiApplication screens."""

        def screen(self, screen=-1):
            app = QGuiApplication.instance()
            if app is None:
                return None
            screens = app.screens()
            if screen < 0 or screen >= len(screens):
                return app.primaryScreen()
            return screens[screen]

        def screenCount(self):
            app = QGuiApplication.instance()
            return len(app.screens()) if app else 0

        def screenGeometry(self, screen=-1):
            s = self.screen(screen)
            return s.geometry() if s else None

        def availableGeometry(self, screen=-1):
            s = self.screen(screen)
            return s.availableGeometry() if s else None

    _the_desktop_widget = QDesktopWidget.__new__(QDesktopWidget)

    def _app_desktop():
        return _the_desktop_widget

    from PyQt6.QtWidgets import QApplication
    if not hasattr(QApplication, "desktop"):
        QApplication.desktop = staticmethod(_app_desktop)
