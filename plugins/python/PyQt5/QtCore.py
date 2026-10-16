# SPDX-License-Identifier: GPL-3.0-or-later
#
# PyQt5.QtCore -> PyQt6.QtCore

import enum as _enum

import PyQt6.QtCore as _QtCoreMod
from PyQt6.QtCore import *  # noqa: F401,F403
from PyQt6.QtCore import (  # noqa: F401
    PYQT_VERSION_STR,
    QT_VERSION_STR,
    QCoreApplication,
    QEvent,
    QEventLoop,
    QObject,
    Qt,
    QThread,
    pyqtBoundSignal,
    pyqtProperty,
    pyqtSignal,
    pyqtSlot,
)


def _expose_enum_members(cls):
    """Install Qt5-style unscoped enum names on a class.

    PyQt5 allowed Qt.AlignLeft in addition to the scoped
    Qt.AlignmentFlag.AlignLeft; PyQt6 only ships the scoped forms.
    This hoists every enum member of every enum type found on ``cls``
    back up onto ``cls`` itself, without clobbering existing names.
    """
    for name in dir(cls):
        try:
            sub = getattr(cls, name)
        except AttributeError:
            continue
        if isinstance(sub, type) and issubclass(sub, _enum.Enum):
            # __members__ (not iteration) so zero-valued flags like
            # KeyboardModifier.NoModifier and mask aliases are kept.
            for member in sub.__members__.values():
                if not hasattr(cls, member.name):
                    setattr(cls, member.name, member)
    return cls


def _add_exec_alias(cls):
    """Qt5 used exec_(); Qt6 renamed it to exec(). Provide both."""
    if hasattr(cls, "exec") and not hasattr(cls, "exec_"):
        cls.exec_ = cls.exec
    return cls


# Re-export PyQt6's own module attributes that the star import skipped
# (anything PyQt6 keeps module-level, e.g. qDebug lives in * anyway).
for _name in dir(_QtCoreMod):
    if _name not in globals() and not _name.startswith("__"):
        globals()[_name] = getattr(_QtCoreMod, _name)

# Unscoped enums: Qt.* itself plus every other class in this module that
# carries enum types (QEvent.Type, QProcess.ProcessState, QUrl.*, ...).
_expose_enum_members(Qt)
for _cls_name in dir():
    _cls = globals().get(_cls_name)
    if isinstance(_cls, type) and getattr(_cls, "__module__", "").startswith("PyQt6"):
        _expose_enum_members(_cls)

# exec_ aliases for every class in this module that gained exec() in Qt6
for _cls_name in dir():
    _cls = globals().get(_cls_name)
    if isinstance(_cls, type) and getattr(_cls, "__module__", "").startswith("PyQt6"):
        _add_exec_alias(_cls)

del _cls_name, _cls

if hasattr(Qt, "MouseButton") and hasattr(Qt.MouseButton, "MiddleButton"):
    Qt.MouseButton.MidButton = Qt.MouseButton.MiddleButton
    Qt.MidButton = Qt.MouseButton.MiddleButton

if hasattr(_QtCoreMod, "QStandardPaths") and hasattr(_QtCoreMod.QStandardPaths, "StandardLocation"):
    if hasattr(_QtCoreMod.QStandardPaths.StandardLocation, "AppDataLocation") and not hasattr(_QtCoreMod.QStandardPaths, "DataLocation"):
        _QtCoreMod.QStandardPaths.DataLocation = _QtCoreMod.QStandardPaths.StandardLocation.AppDataLocation

# Qt6 removed QSignalMapper.mapped[...] in favour of typed signals
# (mappedInt, mappedString, mappedObject); the QWidget overload has no
# Qt6 equivalent. Restore the subscriptable Qt5 surface.


class _SignalMapperMappedProxy:
    def __init__(self, mapper):
        self._mapper = mapper

    def __getitem__(self, key):
        if key is int:
            return self._mapper.mappedInt
        if key is str:
            return self._mapper.mappedString
        if key is _QtCoreMod.QObject:
            return self._mapper.mappedObject
        raise KeyError(key)


if "mapped" not in dir(_QtCoreMod.QSignalMapper):
    _QtCoreMod.QSignalMapper.mapped = property(_SignalMapperMappedProxy)

try:
    from PyQt6.QtCore import QRegularExpression

    class QRegExp(QRegularExpression):
        """Minimal Qt5 QRegExp surface over QRegularExpression.

        Covers the common plugin usage: construction with a pattern,
        case sensitivity, exactMatch(), indexIn()/cap() style access.
        Syntax enums that no longer exist map onto regular expressions.
        """
        CaseInsensitive = Qt.CaseSensitivity.CaseInsensitive
        CaseSensitive = Qt.CaseSensitivity.CaseSensitive

        def __init__(self, pattern="", cs=None, syntax=None):
            super().__init__(pattern)
            if cs == self.CaseInsensitive:
                self.setPatternOptions(
                    self.patternOptions()
                    | QRegularExpression.PatternOption.CaseInsensitiveOption)

        def match(self, *args, **kwargs):  # noqa: N802 - Qt5 API name
            self._last_match = super().match(*args, **kwargs)
            return self._last_match

        def exactMatch(self, text):
            m = self.match(text)
            return m.hasMatch() and m.capturedLength() == len(text)

        def indexIn(self, text, offset=0):
            m = self.match(text, offset)
            return m.capturedStart() if m.hasMatch() else -1

        def cap(self, nth=0):
            return self._last_match.captured(nth) if self._last_match else ""

        def matchedLength(self):
            return self._last_match.capturedLength() if self._last_match else -1

        _last_match = None

    if hasattr(_QtCoreMod, "QSortFilterProxyModel"):
        _QSortFilterProxyModel = _QtCoreMod.QSortFilterProxyModel
        if not hasattr(_QSortFilterProxyModel, "setFilterRegExp"):
            def _proxy_set_filter_regexp(self, pattern):
                if isinstance(pattern, str):
                    self.setFilterRegularExpression(pattern)
                elif isinstance(pattern, QRegularExpression):
                    self.setFilterRegularExpression(pattern)
                elif hasattr(pattern, "pattern"):
                    self.setFilterRegularExpression(pattern.pattern())
                else:
                    self.setFilterRegularExpression(str(pattern))

            _QSortFilterProxyModel.setFilterRegExp = _proxy_set_filter_regexp

        if not hasattr(_QSortFilterProxyModel, "filterRegExp"):
            def _proxy_filter_regexp(self):
                regex = self.filterRegularExpression()
                try:
                    return QRegExp(regex)
                except Exception:
                    return regex

            _QSortFilterProxyModel.filterRegExp = _proxy_filter_regexp

except ImportError:  # pragma: no cover - PyQt6 always has it
    pass
