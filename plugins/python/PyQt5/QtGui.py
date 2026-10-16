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

def _qfontmetrics_width(self, *args, **kwargs):
    return self.horizontalAdvance(*args, **kwargs)

if not hasattr(QFontMetrics, "width"):
    QFontMetrics.width = _qfontmetrics_width
if not hasattr(QFontMetricsF, "width"):
    QFontMetricsF.width = _qfontmetrics_width


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

if not hasattr(QWheelEvent, "delta"):
    QWheelEvent.delta = lambda self: self.angleDelta().y() or self.angleDelta().x()

_orig_palette_color = QPalette.color
def _palette_color(self, *args):
    new_args = []
    if len(args) == 1:
        role = args[0]
        if isinstance(role, int) and not isinstance(role, QPalette.ColorRole):
            try:
                role = QPalette.ColorRole(role)
            except ValueError:
                pass
        new_args.append(role)
    elif len(args) == 2:
        group, role = args
        if isinstance(group, int) and not isinstance(group, QPalette.ColorGroup):
            try:
                group = QPalette.ColorGroup(group)
            except ValueError:
                pass
        if isinstance(role, int) and not isinstance(role, QPalette.ColorRole):
            try:
                role = QPalette.ColorRole(role)
            except ValueError:
                pass
        new_args.extend([group, role])
    else:
        new_args = args
    return _orig_palette_color(self, *new_args)

QPalette.color = _palette_color

_orig_palette_setColor = QPalette.setColor
def _palette_setColor(self, *args):
    new_args = list(args)
    if len(args) == 2:
        role, color = args
        if isinstance(role, int) and not isinstance(role, QPalette.ColorRole):
            try:
                new_args[0] = QPalette.ColorRole(role)
            except ValueError:
                pass
    elif len(args) == 3:
        group, role, color = args
        if isinstance(group, int) and not isinstance(group, QPalette.ColorGroup):
            try:
                new_args[0] = QPalette.ColorGroup(group)
            except ValueError:
                pass
        if isinstance(role, int) and not isinstance(role, QPalette.ColorRole):
            try:
                new_args[1] = QPalette.ColorRole(role)
            except ValueError:
                pass
    return _orig_palette_setColor(self, *new_args)

QPalette.setColor = _palette_setColor

try:
    from PyQt6.QtGui import QRegularExpressionValidator

    QRegExpValidator = QRegularExpressionValidator  # noqa: F401
except ImportError:
    pass
