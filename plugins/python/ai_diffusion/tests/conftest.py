"""Prevent pytest from importing the real ai_diffusion package __init__.

When pytest collects test modules inside a package directory it imports the
package's __init__.py during setup (Package node). ai_diffusion/__init__.py
requires the Krita runtime (and a bundled websockets install), which is not
available in a plain pytest environment. Tests in this directory only load
pure-stdlib modules directly by file path, so a stub is registered here to
keep collection working.
"""

import sys
import types
from pathlib import Path

# Stub krita if not in runtime
if "krita" not in sys.modules:
    krita_mock = types.ModuleType("krita")
    krita_mock.IS_MOCK = True
    sys.modules["krita"] = krita_mock

# Stub PyQt6 if not installed in current test runner
if "PyQt6" not in sys.modules:
    try:
        import PyQt6
    except ImportError:
        pyqt6 = types.ModuleType("PyQt6")
        QtCore = types.ModuleType("PyQt6.QtCore")
        QtGui = types.ModuleType("PyQt6.QtGui")
        QtWidgets = types.ModuleType("PyQt6.QtWidgets")
        sip = types.ModuleType("PyQt6.sip")

        class QObject:
            def __init__(self, *args, **kwargs): pass

        class QAbstractListModel(QObject):
            def __init__(self, *args, **kwargs):
                super().__init__(*args, **kwargs)
                self._files = []
            def beginResetModel(self): pass
            def endResetModel(self): pass
            def beginInsertRows(self, *args): pass
            def endInsertRows(self): pass
            def beginRemoveRows(self, *args): pass
            def endRemoveRows(self): pass
            @property
            def dataChanged(self):
                class Signal:
                    def emit(self, *a): pass
                return Signal()
            def index(self, *args): return 0

        class QModelIndex: pass
        class QSortFilterProxyModel(QObject): pass
        class Qt:
            class ItemFlag:
                ItemIsDropEnabled = 1
                ItemIsDragEnabled = 2
            class ItemDataRole:
                UserRole = 256

        class QStandardPaths:
            class StandardLocation:
                AppDataLocation = 1
                GenericDataLocation = 2
            @staticmethod
            def writableLocation(*args):
                return str(Path.home() / ".krita_test")

        class QIcon:
            def isNull(self): return True

        QtCore.QObject = QObject
        QtCore.QAbstractListModel = QAbstractListModel
        QtCore.QModelIndex = QModelIndex
        QtCore.QSortFilterProxyModel = QSortFilterProxyModel
        QtCore.Qt = Qt
        QtCore.QStandardPaths = QStandardPaths
        QtGui.QIcon = QIcon

        pyqt6.QtCore = QtCore
        pyqt6.QtGui = QtGui
        pyqt6.QtWidgets = QtWidgets
        pyqt6.sip = sip
        sys.modules["PyQt6"] = pyqt6
        sys.modules["PyQt6.QtCore"] = QtCore
        sys.modules["PyQt6.QtGui"] = QtGui
        sys.modules["PyQt6.QtWidgets"] = QtWidgets
        sys.modules["PyQt6.sip"] = sip

pkg = types.ModuleType("ai_diffusion")
pkg.__path__ = [str(Path(__file__).parent.parent)]
sys.modules.setdefault("ai_diffusion", pkg)
