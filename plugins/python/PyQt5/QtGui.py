# SPDX-License-Identifier: GPL-3.0-or-later
#
# PyQt5.QtGui -> PyQt6.QtGui
#
# In Qt6 the widgets-facing helper classes stayed (or moved into) QtGui:
# QAction, QActionGroup, QShortcut, QFileSystemModel, the QUndo* classes and
# QStandardItem* are all here already, so plain re-export plus the shared
# compat passes are sufficient.

from PyQt6.QtGui import *  # noqa: F401,F403
from PyQt6.QtGui import (  # noqa: F401
    QFontMetrics,
    QFontMetricsF,
    QMouseEvent,
    QPalette,
    QWheelEvent,
)

from PyQt5.QtCore import _add_exec_alias, _expose_enum_members

# Unscoped enum members (QPalette.Window, QPixmap.*, QTextCharFormat.*, ...)
for _cls_name in dir():
    _cls = globals().get(_cls_name)
    if isinstance(_cls, type) and getattr(_cls, "__module__", "").startswith("PyQt6"):
        _expose_enum_members(_cls)
        _add_exec_alias(_cls)

del _cls_name, _cls

# Qt5 renamed/removals frequently hit by plugins:

if not hasattr(QFontMetrics, "width"):
    QFontMetrics.width = QFontMetrics.horizontalAdvance
if not hasattr(QFontMetricsF, "width"):
    QFontMetricsF.width = QFontMetricsF.horizontalAdvance


def _point_compat(name, source):
    """Return a QPoint-returning method wrapping a QPointF-returning one."""
    def method(self):
        return getattr(self, source)().toPoint()
    return method


# Qt6 renamed the QPoint accessors of pointer events to QPointF versions
# (position(), globalPosition(), scenePosition()). Qt5 code calls pos(),
# globalPos(), x(), y(), localPos(), windowPos(), screenPos().
for _evt in (QMouseEvent, QWheelEvent):
    if not hasattr(_evt, "pos"):
        _evt.pos = _point_compat("pos", "position")
    if not hasattr(_evt, "globalPos"):
        _evt.globalPos = _point_compat("globalPos", "globalPosition")
    if not hasattr(_evt, "x"):
        _evt.x = lambda self: int(self.position().x())
        _evt.y = lambda self: int(self.position().y())
    if not hasattr(_evt, "localPos"):
        _evt.localPos = lambda self: self.position()
    if not hasattr(_evt, "windowPos"):
        _evt.windowPos = lambda self: self.scenePosition()
    if not hasattr(_evt, "screenPos"):
        _evt.screenPos = lambda self: self.globalPosition()

del _evt

try:
    from PyQt6.QtGui import QRegularExpressionValidator

    QRegExpValidator = QRegularExpressionValidator  # noqa: F401
except ImportError:
    pass
