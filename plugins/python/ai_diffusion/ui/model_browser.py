"""Model browser for checkpoint and LoRA files.

Presents the `FileLibrary` collections as a searchable card grid with
ComfyUI-style preview thumbnails (``<name>.png`` / ``<name>.preview.png``
beside the model file) and an opt-in CivitAI identify/download workflow.

The widget is self-contained: embed `ModelBrowserWidget` into any layout, or
call `show_model_browser()` to open it in a modeless dialog. All disk and
network work happens off the UI thread via the plugin's asyncio event loop
and executor — nothing here performs blocking I/O on the GUI thread.
"""

from __future__ import annotations

import asyncio
import html
import re
from base64 import b64decode
from pathlib import Path

from krita import Krita
from PyQt6.QtCore import (
    QAbstractListModel,
    QModelIndex,
    QRect,
    QSize,
    QSortFilterProxyModel,
    Qt,
    QUrl,
    pyqtSignal,
)
from PyQt6.QtGui import (
    QColor,
    QDesktopServices,
    QGuiApplication,
    QIcon,
    QImage,
    QPainter,
    QPalette,
    QPixmap,
)
from PyQt6.QtNetwork import QNetworkAccessManager
from PyQt6.QtWidgets import (
    QAbstractItemView,
    QCheckBox,
    QComboBox,
    QDialog,
    QFileDialog,
    QFrame,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListView,
    QMenu,
    QMessageBox,
    QProgressDialog,
    QPushButton,
    QScrollArea,
    QSplitter,
    QStyle,
    QStyledItemDelegate,
    QStyleOptionViewItem,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from .. import eventloop
from ..backend import network
from ..backend.civitai import CivitaiClient, CivitaiModelInfo
from ..backend.modelspec import (
    arch_keys,
    extract_metadata_fields,
    read_safetensors_metadata,
)
from ..backend.network import RequestManager
from ..backend.resources import Arch
from ..files import File, FileCollection, FileFormat, FileSource, ModelMetadataCache
from ..localization import translate as _
from ..model.root import root
from ..settings import settings
from ..util import client_logger as log
from ..util import find_unused_path, trim_text
from . import theme

thumb_size = QSize(184, 104)
card_size = QSize(200, 180)
preview_candidates = (".preview.png", ".png", ".webp", ".jpg", ".jpeg")


def find_preview_image(path: Path) -> Path | None:
    """Resolve the ComfyUI-convention preview image beside a model file."""
    for suffix in preview_candidates:
        if suffix == ".preview.png":
            candidate = path.with_name(path.stem + ".preview.png")
        else:
            candidate = path.with_suffix(suffix)
        if candidate.is_file():
            return candidate
    return None


def _load_preview_image(path: Path) -> QImage | None:
    """Worker: find + load + scale the preview image. Runs in the executor,
    so it must return QImage (QPixmap is not thread-safe)."""
    try:
        preview = find_preview_image(path)
        if preview is None:
            return None
        image = QImage(str(preview))
        if image.isNull():
            return None
        return image.scaled(
            thumb_size,
            Qt.AspectRatioMode.KeepAspectRatio,
            Qt.TransformationMode.SmoothTransformation,
        )
    except Exception as e:
        log.warning(f"Failed to load preview image for {path}: {e}")
        return None


def _scan_files_metadata(files: list[tuple[str, Path]]) -> dict[str, tuple[float, dict]]:
    """Worker: read safetensors model headers + file mtimes. Runs in the
    executor so large model folders never block the UI. Returns
    ``{str(path): (mtime, extracted_fields)}`` — all merging into `File`
    objects and the metadata cache happens on the UI thread."""
    results: dict[str, tuple[float, dict]] = {}
    for file_id, path in files:
        try:
            mtime = path.stat().st_mtime
        except OSError:
            mtime = 0.0
        fields: dict = {}
        if path.suffix.lower() == ".safetensors":
            try:
                fields = extract_metadata_fields(read_safetensors_metadata(path))
            except Exception:
                pass
        results[file_id] = (mtime, fields or {})
    return results


def arch_text(file: File) -> str:
    """Best human-readable architecture string for a file (modelspec metadata)."""
    meta = file.metadata or {}
    for key in arch_keys:
        value = meta.get(key)
        if isinstance(value, str) and value.strip():
            if arch := Arch.from_metadata_string(value, filename=file.id):
                return arch.value
            return value.strip()
    return ""


def trigger_text(file: File) -> str:
    if triggers := file.meta("lora_triggers"):
        return str(triggers)
    words = file.meta("trigger_words")
    if isinstance(words, list):
        return ", ".join(str(w) for w in words[:10])
    return ""


def source_text(file: File) -> str:
    local = FileSource.local in file.source
    remote = FileSource.remote in file.source
    if local and remote:
        return _("Local + remote")
    elif local:
        return _("Local")
    elif remote:
        return _("Remote")
    return _("Unavailable")


def _suggest_filename(info: CivitaiModelInfo) -> str:
    name = Path(QUrl(info.download_url).path()).name if info.download_url else ""
    # CivitAI download URLs are /api/download/models/<id> — the basename has no
    # extension, so only use it when it already looks like a model file.
    if Path(name).suffix.lower() not in {".safetensors", ".ckpt", ".pt", ".pth"}:
        name = re.sub(r'[\\/:*?"<>|]', "_", info.display_name).strip() or "model"
        name += ".safetensors"
    return name


# --- Shared network resources -------------------------------------------------
# Created lazily and kept for the lifetime of the plugin. CivitAI access is
# strictly opt-in (settings.civitai_lookup); nothing here sends requests
# unprompted.

_requests: RequestManager | None = None
_civitai: CivitaiClient | None = None
_download_net: QNetworkAccessManager | None = None


def _request_manager() -> RequestManager:
    global _requests
    if _requests is None:
        _requests = RequestManager()
    return _requests


def _civitai_client() -> CivitaiClient:
    global _civitai
    if _civitai is None:
        _civitai = CivitaiClient(_request_manager())
    return _civitai


def _download_manager() -> QNetworkAccessManager:
    global _download_net
    if _download_net is None:
        _download_net = QNetworkAccessManager()
    return _download_net


# --- Combined model over checkpoints + loras -----------------------------------


class ModelLibraryModel(QAbstractListModel):
    """Aggregates `FileLibrary.checkpoints` and `FileLibrary.loras` into one
    list model and lazily loads ComfyUI-convention preview thumbnails."""

    file_role = Qt.ItemDataRole.UserRole + 1
    kind_role = Qt.ItemDataRole.UserRole + 2
    arch_role = Qt.ItemDataRole.UserRole + 3
    mtime_role = Qt.ItemDataRole.UserRole + 4
    source_role = Qt.ItemDataRole.UserRole + 5
    collection_role = Qt.ItemDataRole.UserRole + 6

    architectures_changed = pyqtSignal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self._entries: list[tuple[File, FileCollection]] = []
        self._preview_cache: dict[str, QIcon] = {}
        self._pending: set[str] = set()
        self._missing: set[str] = set()  # paths known to have no preview image
        self._mtimes: dict[str, float] = {}
        self._scanned_ids: set[str] = set()
        self._closed = False

        self.rebuild()
        for collection in self._collections():
            collection.rowsInserted.connect(self.rebuild)
            collection.rowsRemoved.connect(self.rebuild)
            collection.modelReset.connect(self.rebuild)
            collection.dataChanged.connect(self._on_collection_changed)

    @staticmethod
    def _collections():
        return (root.files.checkpoints, root.files.loras)

    def rebuild(self, *args):
        self.beginResetModel()
        self._entries = [(f, root.files.checkpoints) for f in root.files.checkpoints]
        self._entries += [(f, root.files.loras) for f in root.files.loras]
        self.endResetModel()
        self.architectures_changed.emit()
        self._schedule_metadata_scan()

    def refresh(self):
        """Hard reload: drop preview/metadata caches and rebuild the entries."""
        self._preview_cache.clear()
        self._missing.clear()
        self._scanned_ids.clear()
        self.rebuild()

    def _on_collection_changed(self, top_left: QModelIndex, bottom_right: QModelIndex):
        # a file's data (source flags, icon, metadata) changed: refresh all rows,
        # mapping collection rows to entries is not worth the bookkeeping
        if self.rowCount() > 0:
            self.dataChanged.emit(self.index(0), self.index(self.rowCount() - 1))
        self.architectures_changed.emit()

    def _schedule_metadata_scan(self):
        files = [
            f
            for f, _ in self._entries
            if f.path is not None and str(f.path) not in self._scanned_ids
        ]
        if not files:
            return
        self._scanned_ids.update(str(f.path) for f in files)
        eventloop.run(self._scan_metadata(files))

    async def _scan_metadata(self, files: list[File]):
        try:
            loop = asyncio.get_running_loop()
            jobs = [(str(f.path), f.path) for f in files if f.path is not None]
            results = await loop.run_in_executor(None, _scan_files_metadata, jobs)
        except Exception as e:
            log.warning(f"Model metadata scan failed: {e}")
            return
        # Merge on the UI thread: warming the cache makes
        # File.read_model_metadata cheap (no further header reads).
        if self._closed:
            return
        for f in files:
            if f.path is None:
                continue
            mtime, fields = results.get(str(f.path), (0.0, {}))
            self._mtimes[str(f.path)] = mtime
            if fields:
                ModelMetadataCache.set(f.path, fields)
                f.read_model_metadata()
        if self.rowCount() > 0:
            self.dataChanged.emit(self.index(0), self.index(self.rowCount() - 1))
            self.architectures_changed.emit()

    def _row_of(self, file: File) -> int:
        return next((i for i, e in enumerate(self._entries) if e[0] is file), -1)

    def entry(self, row: int) -> tuple[File, FileCollection] | None:
        if 0 <= row < len(self._entries):
            return self._entries[row]
        return None

    @property
    def entries(self):
        return self._entries

    def rowCount(self, parent: QModelIndex | None = None):
        if parent is not None and parent.isValid():
            return 0
        return len(self._entries)

    def data(self, index: QModelIndex, role: int = Qt.ItemDataRole.DisplayRole):
        entry = self.entry(index.row()) if index.isValid() else None
        if entry is None:
            return None
        file, collection = entry

        match role:
            case Qt.ItemDataRole.DisplayRole | Qt.ItemDataRole.EditRole:
                return file.name
            case Qt.ItemDataRole.DecorationRole:
                return self._preview(file)
            case Qt.ItemDataRole.UserRole:
                return file.id
            case self.file_role:
                return file
            case self.kind_role:
                return "lora" if collection is root.files.loras else "checkpoint"
            case self.arch_role:
                return arch_text(file)
            case self.mtime_role:
                return self._mtimes.get(str(file.path), 0.0) if file.path else 0.0
            case self.source_role:
                return file.source.value
            case self.collection_role:
                return collection
            case _:
                return None

    def _preview(self, file: File):
        if file.path is None:
            return file.icon
        key = str(file.path)
        if key in self._preview_cache:
            return self._preview_cache[key]
        if key not in self._pending and key not in self._missing:
            self._pending.add(key)
            eventloop.run(self._load_preview(file))
        return file.icon

    async def _load_preview(self, file: File):
        assert file.path is not None
        key = str(file.path)
        try:
            loop = asyncio.get_running_loop()
            image = await loop.run_in_executor(None, _load_preview_image, file.path)
        except Exception as e:
            log.warning(f"Preview loading failed for {file.path}: {e}")
            image = None
        self._pending.discard(key)
        if image is None:
            self._missing.add(key)
        elif not self._closed:
            self._preview_cache[key] = QIcon(QPixmap.fromImage(image))
            if (row := self._row_of(file)) >= 0:
                idx = self.index(row)
                self.dataChanged.emit(idx, idx, [Qt.ItemDataRole.DecorationRole])


class ModelBrowserFilter(QSortFilterProxyModel):
    """Substring name filter + kind and architecture dropdown filters."""

    def __init__(self, source: QAbstractListModel, parent=None):
        super().__init__(parent)
        self._text = ""
        self._kind = "all"
        self._arch = ""
        self._sort_recent = False
        self.setSourceModel(source)
        self.setSortCaseSensitivity(Qt.CaseSensitivity.CaseInsensitive)
        self.sort(0)

    @property
    def text(self):
        return self._text

    @text.setter
    def text(self, value):
        self._text = value.lower()
        self.invalidateFilter()

    @property
    def kind(self):
        return self._kind

    @kind.setter
    def kind(self, value):
        self._kind = value
        self.invalidateFilter()

    @property
    def arch(self):
        return self._arch

    @arch.setter
    def arch(self, value):
        self._arch = value
        self.invalidateFilter()

    @property
    def sort_recent(self):
        return self._sort_recent

    @sort_recent.setter
    def sort_recent(self, value):
        self._sort_recent = value
        self.sort(0)

    def filterAcceptsRow(self, source_row: int, source_parent: QModelIndex):
        index = self.sourceModel().index(source_row, 0, source_parent)
        if self._kind != "all" and index.data(ModelLibraryModel.kind_role) != self._kind:
            return False
        if self._arch and index.data(ModelLibraryModel.arch_role) != self._arch:
            return False
        if self._text:
            name = (index.data(Qt.ItemDataRole.DisplayRole) or "").lower()
            file_id = (index.data(Qt.ItemDataRole.UserRole) or "").lower()
            if self._text not in name and self._text not in file_id:
                return False
        return True

    def lessThan(self, left: QModelIndex, right: QModelIndex):
        if self._sort_recent:
            lm = left.data(ModelLibraryModel.mtime_role) or 0.0
            rm = right.data(ModelLibraryModel.mtime_role) or 0.0
            if lm != rm:
                return lm > rm  # most recently modified first
        left_name = (left.data(Qt.ItemDataRole.DisplayRole) or "").lower()
        right_name = (right.data(Qt.ItemDataRole.DisplayRole) or "").lower()
        return left_name < right_name


class ModelCardDelegate(QStyledItemDelegate):
    """Draws a model card: preview thumbnail, name, arch badge, status and
    a trigger-word snippet."""

    def __init__(self, parent=None):
        super().__init__(parent)

    def sizeHint(self, option: QStyleOptionViewItem, index: QModelIndex):
        return card_size

    def paint(self, painter: QPainter, option: QStyleOptionViewItem, index: QModelIndex):
        self.initStyleOption(option, index)
        file: File | None = index.data(ModelLibraryModel.file_role)
        if file is None:
            return

        painter.save()
        palette = option.palette
        selected = bool(option.state & QStyle.StateFlag.State_Selected)
        hovered = bool(option.state & QStyle.StateFlag.State_MouseOver)
        rect = option.rect.adjusted(4, 4, -4, -4)

        bg = palette.color(QPalette.ColorRole.Base)
        if selected:
            bg = palette.color(QPalette.ColorRole.Highlight).lighter(160)
        elif hovered:
            bg = bg.lighter(105)
        border = palette.color(QPalette.ColorRole.Highlight) if selected else QColor(theme.line)
        painter.setBrush(bg)
        painter.setPen(border)
        painter.drawRoundedRect(rect, 6, 6)

        text_color = palette.color(QPalette.ColorRole.Text)
        muted = QColor(theme.grey)

        # Preview thumbnail
        thumb_rect = QRect(rect.x() + 4, rect.y() + 4, rect.width() - 8, thumb_size.height())
        icon = index.data(Qt.ItemDataRole.DecorationRole)
        pixmap = icon.pixmap(thumb_rect.size()) if isinstance(icon, QIcon) else None
        if pixmap and not pixmap.isNull():
            x = thumb_rect.x() + (thumb_rect.width() - pixmap.width()) // 2
            y = thumb_rect.y() + (thumb_rect.height() - pixmap.height()) // 2
            painter.drawPixmap(x, y, pixmap)
        else:
            painter.setPen(muted)
            painter.drawText(thumb_rect, Qt.AlignmentFlag.AlignCenter, _("No preview"))

        y = thumb_rect.bottom() + 6
        inner = rect.adjusted(6, 0, -6, 0)

        # File name
        bold = painter.font()
        bold.setBold(True)
        painter.setFont(bold)
        painter.setPen(text_color)
        metrics = option.fontMetrics
        name_rect = QRect(inner.x(), y, inner.width(), metrics.height())
        painter.drawText(
            name_rect,
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
            metrics.elidedText(file.name, Qt.TextElideMode.ElideRight, name_rect.width()),
        )
        y += metrics.height() + 4
        painter.setFont(option.font)

        # Arch badge + source status
        arch = index.data(ModelLibraryModel.arch_role) or ""
        status_row_h = metrics.height() + 4
        status_x = inner.x()
        if arch:
            small = painter.font()
            small.setPointSizeF(max(6.0, small.pointSizeF() - 1.5))
            painter.setFont(small)
            badge_metrics = painter.fontMetrics()
            badge_text = trim_text(arch, 24)
            badge_w = badge_metrics.horizontalAdvance(badge_text) + 10
            badge_rect = QRect(inner.x(), y, badge_w, badge_metrics.height() + 4)
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor(theme.highlight))
            painter.drawRoundedRect(badge_rect, 3, 3)
            painter.setPen(palette.color(QPalette.ColorRole.Base))
            painter.drawText(badge_rect, Qt.AlignmentFlag.AlignCenter, badge_text)
            painter.setFont(option.font)
            status_row_h = badge_rect.height()
            status_x += badge_w + 8
        painter.setPen(muted)
        status_rect = QRect(status_x, y, inner.right() - status_x, status_row_h)
        kind = _("LoRA") if index.data(ModelLibraryModel.kind_role) == "lora" else _("Checkpoint")
        painter.drawText(
            status_rect,
            Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter,
            metrics.elidedText(
                f"{kind} · {source_text(file)}", Qt.TextElideMode.ElideRight, status_rect.width()
            ),
        )
        y += status_row_h + 4

        # Trigger words snippet
        triggers = trigger_text(file)
        painter.setPen(muted)
        trig_rect = QRect(inner.x(), y, inner.width(), metrics.height())
        snippet = (
            metrics.elidedText(triggers, Qt.TextElideMode.ElideRight, trig_rect.width())
            if triggers
            else ""
        )
        painter.drawText(trig_rect, Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter, snippet)
        painter.restore()


# --- Browser widget ------------------------------------------------------------


class ModelBrowserWidget(QWidget):
    """Searchable card grid over the local+remote model library."""

    def __init__(self, parent=None):
        super().__init__(parent)

        self._model = ModelLibraryModel(self)
        self._filter = ModelBrowserFilter(self._model, self)
        self._lookup: CivitaiModelInfo | None = None
        self._lookup_file: File | None = None
        self._lookup_collection: FileCollection | None = None
        self._task: asyncio.Task | None = None
        self._progress: QProgressDialog | None = None
        self._busy = False
        self._closed = False
        self._suppress_setting_sync = False

        layout = QVBoxLayout(self)
        layout.setContentsMargins(4, 4, 4, 4)

        # Toolbar
        toolbar = QHBoxLayout()
        self._search = QLineEdit(self)
        self._search.setPlaceholderText(_("Search models"))
        self._search.setClearButtonEnabled(True)
        self._search.textChanged.connect(self._apply_text_filter)
        toolbar.addWidget(self._search, 3)

        self._kind_combo = QComboBox(self)
        self._kind_combo.addItem(_("All"), "all")
        self._kind_combo.addItem(_("Checkpoints"), "checkpoint")
        self._kind_combo.addItem(_("LoRAs"), "lora")
        self._kind_combo.currentIndexChanged.connect(self._apply_filters)
        toolbar.addWidget(self._kind_combo, 1)

        self._arch_combo = QComboBox(self)
        self._arch_combo.currentIndexChanged.connect(self._apply_filters)
        toolbar.addWidget(self._arch_combo, 1)

        self._sort_combo = QComboBox(self)
        self._sort_combo.addItem(_("Sort by name"), False)
        self._sort_combo.addItem(_("Recently modified"), True)
        self._sort_combo.currentIndexChanged.connect(self._apply_filters)
        toolbar.addWidget(self._sort_combo, 1)

        self._refresh_button = QToolButton(self)
        self._refresh_button.setIcon(Krita.instance().icon("reload-preset"))
        self._refresh_button.setToolTip(_("Reload model metadata and previews"))
        self._refresh_button.clicked.connect(self._model.refresh)
        toolbar.addWidget(self._refresh_button)
        layout.addLayout(toolbar)

        # Card grid + details
        splitter = QSplitter(Qt.Orientation.Horizontal, self)
        self._view = QListView(self)
        self._view.setModel(self._filter)
        self._view.setViewMode(QListView.ViewMode.IconMode)
        self._view.setResizeMode(QListView.ResizeMode.Adjust)
        self._view.setUniformItemSizes(True)
        self._view.setWrapping(True)
        self._view.setSpacing(6)
        self._view.setGridSize(QSize(card_size.width() + 8, card_size.height() + 8))
        self._view.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self._view.setItemDelegate(ModelCardDelegate(self._view))
        self._view.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self._view.customContextMenuRequested.connect(self._context_menu)
        splitter.addWidget(self._view)

        self._details = self._build_details(splitter)
        splitter.addWidget(self._details)
        splitter.setStretchFactor(0, 3)
        splitter.setStretchFactor(1, 1)
        splitter.setSizes([640, 260])
        layout.addWidget(splitter, 1)

        if selection := self._view.selectionModel():
            selection.currentChanged.connect(self._update_details)
        self._model.architectures_changed.connect(self._update_arch_filter)
        settings.changed.connect(self._on_setting_changed)
        self._update_arch_filter()
        self._update_details()

    # -- filters --

    def _apply_text_filter(self):
        self._filter.text = self._search.text()

    def _apply_filters(self):
        self._filter.kind = self._kind_combo.currentData()
        self._filter.arch = self._arch_combo.currentData() or ""
        self._filter.sort_recent = bool(self._sort_combo.currentData())

    def _update_arch_filter(self):
        current = self._arch_combo.currentData()
        self._arch_combo.blockSignals(True)
        self._arch_combo.clear()
        self._arch_combo.addItem(_("All architectures"), "")
        archs = sorted(
            {arch_text(f) for f, _ in self._model.entries} - {""}, key=str.lower
        )
        for arch in archs:
            self._arch_combo.addItem(arch, arch)
        index = self._arch_combo.findData(current)
        self._arch_combo.setCurrentIndex(max(0, index))
        self._arch_combo.blockSignals(False)
        self._apply_filters()

    # -- selection + details --

    def _selected_entry(self) -> tuple[File, FileCollection] | None:
        index = self._view.currentIndex()
        if not index.isValid():
            return None
        return self._model.entry(self._filter.mapToSource(index).row())

    def _build_details(self, parent: QWidget):
        panel = QScrollArea(parent)
        panel.setWidgetResizable(True)
        panel.setFrameShape(QFrame.Shape.NoFrame)
        panel.setMinimumWidth(230)
        inner = QWidget(panel)
        panel.setWidget(inner)
        layout = QVBoxLayout(inner)
        layout.setContentsMargins(6, 6, 6, 6)

        self._preview_label = QLabel(inner)
        self._preview_label.setFixedSize(216, 124)
        self._preview_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._preview_label.setFrameShape(QFrame.Shape.StyledPanel)
        layout.addWidget(self._preview_label, alignment=Qt.AlignmentFlag.AlignHCenter)

        self._name_label = QLabel(inner)
        self._name_label.setWordWrap(True)
        font = self._name_label.font()
        font.setBold(True)
        self._name_label.setFont(font)
        layout.addWidget(self._name_label)

        small = inner.font()
        small.setPointSizeF(max(6.0, small.pointSizeF() - 1.0))

        self._id_label = QLabel(inner)
        self._id_label.setFont(small)
        self._id_label.setStyleSheet(f"color: {theme.grey};")
        self._id_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self._id_label.setWordWrap(True)
        layout.addWidget(self._id_label)

        self._path_label = QLabel(inner)
        self._path_label.setFont(small)
        self._path_label.setStyleSheet(f"color: {theme.grey};")
        self._path_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self._path_label.setWordWrap(True)
        layout.addWidget(self._path_label)

        self._meta_label = QLabel(inner)
        self._meta_label.setFont(small)
        self._meta_label.setWordWrap(True)
        layout.addWidget(self._meta_label)

        self._triggers_label = QLabel(inner)
        self._triggers_label.setFont(small)
        self._triggers_label.setStyleSheet(f"color: {theme.grey};")
        self._triggers_label.setWordWrap(True)
        layout.addWidget(self._triggers_label)

        line = QFrame(inner)
        line.setFrameShape(QFrame.Shape.HLine)
        line.setStyleSheet(f"color: {theme.line};")
        layout.addWidget(line)

        self._civitai_check = QCheckBox(_("CivitAI lookup"), inner)
        self._civitai_check.setToolTip(
            _(
                "Allow identifying local model files on civitai.com by their file hash. "
                "Only the hash is sent; file content never leaves your machine."
            )
        )
        self._civitai_check.setChecked(settings.civitai_lookup)
        self._civitai_check.toggled.connect(self._toggle_civitai)
        layout.addWidget(self._civitai_check)

        self._identify_button = QPushButton(theme.icon("web-connection"), _("Identify"), inner)
        self._identify_button.setToolTip(
            _("Compute the file hash and look it up on civitai.com")
        )
        self._identify_button.clicked.connect(self._identify)
        layout.addWidget(self._identify_button)

        self._identify_status = QLabel(inner)
        self._identify_status.setFont(small)
        self._identify_status.setWordWrap(True)
        layout.addWidget(self._identify_status)

        self._result_group = QWidget(inner)
        result_layout = QVBoxLayout(self._result_group)
        result_layout.setContentsMargins(0, 0, 0, 0)

        self._result_preview = QLabel(self._result_group)
        self._result_preview.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._result_preview.setFixedHeight(96)
        result_layout.addWidget(self._result_preview)

        self._result_name = QLabel(self._result_group)
        self._result_name.setWordWrap(True)
        self._result_name.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        result_layout.addWidget(self._result_name)

        self._result_meta = QLabel(self._result_group)
        self._result_meta.setFont(small)
        self._result_meta.setWordWrap(True)
        result_layout.addWidget(self._result_meta)

        self._result_triggers = QLabel(self._result_group)
        self._result_triggers.setFont(small)
        self._result_triggers.setStyleSheet(f"color: {theme.grey};")
        self._result_triggers.setWordWrap(True)
        self._result_triggers.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse
        )
        result_layout.addWidget(self._result_triggers)

        self._apply_button = QPushButton(_("Apply trigger words"), self._result_group)
        self._apply_button.clicked.connect(self._apply_triggers)
        result_layout.addWidget(self._apply_button)

        self._page_button = QPushButton(_("Open on CivitAI"), self._result_group)
        self._page_button.clicked.connect(self._open_civitai_page)
        result_layout.addWidget(self._page_button)

        self._download_button = QPushButton(theme.icon("import"), _("Download"), self._result_group)
        self._download_button.clicked.connect(self._download)
        result_layout.addWidget(self._download_button)

        self._result_group.setVisible(False)
        layout.addWidget(self._result_group)
        layout.addStretch()
        return panel

    def _update_details(self, *args):
        entry = self._selected_entry()
        file = entry[0] if entry else None
        if file is not self._lookup_file:
            self._lookup = None
            self._lookup_file = None
            self._lookup_collection = None
            self._result_group.setVisible(False)
            self._identify_status.setText("")

        if file is None:
            self._preview_label.setPixmap(QPixmap())
            self._name_label.setText(_("No model selected"))
            self._id_label.setText("")
            self._path_label.setText("")
            self._meta_label.setText("")
            self._triggers_label.setText("")
        else:
            if file.path and (preview := find_preview_image(file.path)):
                self._preview_label.setPixmap(
                    QPixmap(str(preview)).scaled(
                        self._preview_label.size(),
                        Qt.AspectRatioMode.KeepAspectRatio,
                        Qt.TransformationMode.SmoothTransformation,
                    )
                )
            elif isinstance(file.icon, QIcon):
                self._preview_label.setPixmap(file.icon.pixmap(self._preview_label.size()))
            else:
                self._preview_label.setPixmap(QPixmap())
            self._name_label.setText(file.name)
            self._id_label.setText(file.id)
            self._path_label.setText(str(file.path) if file.path else "")
            arch = arch_text(file)
            meta = f"{_('Type')}: {_('LoRA') if entry[1] is root.files.loras else _('Checkpoint')}"
            meta += f" · {source_text(file)}"
            if arch:
                meta += f" · {arch}"
            self._meta_label.setText(meta)
            self._triggers_label.setText(
                (_("Trigger words") + ": " + trigger_text(file)) if trigger_text(file) else ""
            )
        self._update_actions()

    def _update_actions(self):
        entry = self._selected_entry()
        file = entry[0] if entry else None
        self._identify_button.setEnabled(
            file is not None
            and file.path is not None
            and settings.civitai_lookup
            and not self._busy
        )
        if file is not None and file.path is None:
            self._identify_button.setToolTip(
                _("Identification requires a local file")
            )
        elif not settings.civitai_lookup:
            self._identify_button.setToolTip(
                _("Enable CivitAI lookup above to identify this file")
            )
        else:
            self._identify_button.setToolTip(
                _("Compute the file hash and look it up on civitai.com")
            )

    def _toggle_civitai(self, checked: bool):
        self._suppress_setting_sync = True
        settings.civitai_lookup = checked
        settings.save()
        self._suppress_setting_sync = False
        self._update_actions()

    def _on_setting_changed(self, name: str, value):
        if name == "civitai_lookup" and not self._suppress_setting_sync:
            self._civitai_check.setChecked(bool(value))
            self._update_actions()

    # -- context menu --

    def _context_menu(self, pos):
        index = self._view.indexAt(pos)
        if not index.isValid():
            return
        self._view.setCurrentIndex(index)
        entry = self._model.entry(self._filter.mapToSource(index).row())
        if entry is None:
            return
        file, _collection = entry

        menu = QMenu(self)
        identify = menu.addAction(theme.icon("web-connection"), _("Identify on CivitAI"))
        identify.setEnabled(settings.civitai_lookup and file.path is not None)
        identify.triggered.connect(self._identify)

        menu.addSeparator()
        show_folder = menu.addAction(
            Krita.instance().icon("document-open"), _("Show in Folder")
        )
        show_folder.setEnabled(file.path is not None)
        show_folder.triggered.connect(lambda: self._show_in_folder(file))
        copy_name = menu.addAction(
            Krita.instance().icon("edit-copy"), _("Copy Filename")
        )
        copy_name.triggered.connect(
            lambda: QGuiApplication.clipboard().setText(file.id)
        )
        triggers = trigger_text(file)
        copy_triggers = menu.addAction(
            Krita.instance().icon("edit-copy"), _("Copy Trigger Words")
        )
        copy_triggers.setEnabled(bool(triggers))
        copy_triggers.triggered.connect(
            lambda: QGuiApplication.clipboard().setText(triggers)
        )
        menu.exec(self._view.viewport().mapToGlobal(pos))

    def _show_in_folder(self, file: File):
        if file.path is not None:
            QDesktopServices.openUrl(QUrl.fromLocalFile(str(file.path.parent)))

    # -- CivitAI identify (opt-in) --

    def _identify(self):
        entry = self._selected_entry()
        if entry is None or not settings.civitai_lookup or self._busy:
            return
        file, collection = entry
        if file.path is None:
            return
        self._set_busy(True)
        self._set_status(_("Hashing file..."))
        self._task = eventloop.run(self._identify_async(file, collection))

    async def _identify_async(self, file: File, collection: FileCollection):
        try:
            if not file.hash:

                def progress(done: int, total: int):
                    if total:
                        pct = int(100 * done / total)
                        self._set_status(_("Hashing file... {pct}%", pct=pct))

                await file.compute_hash_async(progress)
            assert file.hash is not None
            sha256_hex = b64decode(file.hash).hex()
            self._set_status(_("Looking up on CivitAI..."))
            info = await _civitai_client().lookup_by_hash(sha256_hex)
            self._show_lookup(file, collection, info)
            if info is not None and info.preview_url:
                await self._fetch_result_preview(info)
        except asyncio.CancelledError:
            self._set_status(_("Cancelled"))
        except Exception as e:
            log.warning(f"CivitAI identification failed: {e}")
            self._set_status(_("Lookup failed") + f": {e}")
        finally:
            self._set_busy(False)

    def _show_lookup(
        self, file: File, collection: FileCollection, info: CivitaiModelInfo | None
    ):
        if self._closed:
            return
        self._lookup = info
        self._lookup_file = file
        self._lookup_collection = collection
        self._result_preview.setPixmap(QPixmap())
        if info is None:
            self._result_group.setVisible(False)
            self._identify_status.setText(_("No match found on CivitAI."))
            return
        self._result_name.setText(
            f"<b>{html.escape(info.display_name)}</b>"
            + (f" ({html.escape(info.model_type)})" if info.model_type else "")
        )
        meta = []
        if info.base_model:
            meta.append(_("Base model") + f": {info.base_model}")
        self._result_meta.setText(" · ".join(meta))
        self._result_triggers.setText(
            ", ".join(info.trigger_words) if info.trigger_words else ""
        )
        self._apply_button.setVisible(bool(info.trigger_words))
        self._page_button.setVisible(bool(info.page_url))
        self._download_button.setVisible(bool(info.download_url))
        self._result_group.setVisible(True)
        self._identify_status.setText(_("Identified on CivitAI."))

    async def _fetch_result_preview(self, info: CivitaiModelInfo):
        try:
            data = await _request_manager().download(info.preview_url, timeout=15)
            pixmap = QPixmap()
            if pixmap.loadFromData(data):
                self._result_preview.setPixmap(
                    pixmap.scaledToHeight(96, Qt.TransformationMode.SmoothTransformation)
                )
        except Exception as e:
            log.info(f"Failed to fetch CivitAI preview image: {e}")

    def _apply_triggers(self):
        if self._lookup is None or self._lookup_file is None or self._lookup_collection is None:
            return
        if not self._lookup.trigger_words:
            return
        self._lookup_collection.set_meta(
            self._lookup_file, "lora_triggers", ", ".join(self._lookup.trigger_words)
        )
        self._update_details()
        self._identify_status.setText(_("Trigger words saved to model metadata."))

    def _open_civitai_page(self):
        if self._lookup is not None and self._lookup.page_url:
            QDesktopServices.openUrl(QUrl(self._lookup.page_url))

    # -- download --

    def _download(self):
        info = self._lookup
        if info is None or not info.download_url or self._busy:
            return
        directory = QFileDialog.getExistingDirectory(self, _("Choose download folder"))
        if not directory:
            return
        path = find_unused_path(Path(directory) / _suggest_filename(info))
        # note: network.download appends ".part" while downloading and resumes
        # an existing partial file automatically
        self._progress = QProgressDialog(
            _("Downloading {name}", name=path.name), _("Cancel"), 0, 100, self
        )
        self._progress.setWindowTitle(_("Download Model"))
        self._progress.setWindowModality(Qt.WindowModality.WindowModal)
        self._progress.setMinimumDuration(0)
        self._progress.canceled.connect(self._cancel_task)
        self._set_busy(True)
        self._task = eventloop.run(self._download_async(info, path))

    def _cancel_task(self):
        if self._task is not None and not self._task.done():
            self._task.cancel()

    async def _download_async(self, info: CivitaiModelInfo, path: Path):
        assert self._progress is not None
        try:
            async for progress in network.download(
                _download_manager(), info.download_url, path
            ):
                if self._closed or self._progress is None:
                    return
                if progress.value >= 0:
                    self._progress.setMaximum(100)
                    self._progress.setValue(int(progress.value * 100))
                else:
                    self._progress.setMaximum(0)  # indeterminate
                label = _("Downloading {name}", name=path.name)
                if progress.total > 0:
                    label += f"\n{progress.received:.0f} / {progress.total:.0f} MB"
                else:
                    label += f"\n{progress.received:.0f} MB"
                if progress.speed > 0:
                    label += f" ({progress.speed:.1f} MB/s)"
                self._progress.setLabelText(label)
            if self._closed:
                return
            self._progress.setValue(self._progress.maximum())
            self._register_download(path, info)
            self._set_status(
                _("Downloaded to {path}", path=str(path))
            )
        except asyncio.CancelledError:
            self._set_status(
                _("Download cancelled (partial file kept for resume)")
            )
        except Exception as e:
            log.warning(f"Model download failed: {e}")
            if not self._closed:
                QMessageBox.warning(
                    self, _("Download failed"), _("Failed to download model") + f":\n{e}"
                )
        finally:
            self._set_busy(False)

    def _register_download(self, path: Path, info: CivitaiModelInfo):
        """Add the freshly downloaded file to the matching file collection."""
        try:
            # CivitAI uses several LoRA-family type names; all belong in the
            # LoRA collection (they load through LoraLoader/LoraLoaderModelOnly).
            is_lora = info.model_type.lower() in {
                "lora",
                "locon",
                "lycoris",
                "dora",
                "loha",
                "lokr",
                "hypernetwork",
            }
            fmt = FileFormat.lora if is_lora else FileFormat.checkpoint
            collection = root.files.loras if is_lora else root.files.checkpoints
            file = File.local(path, fmt)
            collection.add(file)
            if info.trigger_words:
                collection.set_meta(file, "lora_triggers", ", ".join(info.trigger_words))
        except Exception as e:
            log.warning(f"Failed to register downloaded model {path}: {e}")

    def _on_closed(self):
        """Dialog closing: cancel in-flight work and stop all further UI
        updates — the widgets get deleted while coroutines may still be
        scheduled on the event loop."""
        self._closed = True
        self._model._closed = True
        self._cancel_task()

    def _set_status(self, text: str):
        if not self._closed:
            self._identify_status.setText(text)

    def _set_busy(self, busy: bool):
        if self._closed:
            return
        self._busy = busy
        if not busy:
            self._task = None
            if self._progress is not None:
                self._progress.close()
                self._progress = None
        self._update_actions()


# --- Dialog host + entry point ------------------------------------------------

_browser_dialog: QDialog | None = None


def _clear_dialog(*args):
    global _browser_dialog
    _browser_dialog = None


def show_model_browser(parent: QWidget | None = None) -> QDialog:
    """Open (or raise) the modeless model browser dialog."""
    global _browser_dialog
    if _browser_dialog is None:
        _browser_dialog = QDialog(parent)
        _browser_dialog.setWindowTitle(_("Model Browser"))
        _browser_dialog.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose)
        _browser_dialog.destroyed.connect(_clear_dialog)
        layout = QVBoxLayout(_browser_dialog)
        layout.setContentsMargins(4, 4, 4, 4)
        widget = ModelBrowserWidget(_browser_dialog)
        _browser_dialog.finished.connect(widget._on_closed)
        layout.addWidget(widget)
        _browser_dialog.resize(920, 560)
    _browser_dialog.show()
    _browser_dialog.raise_()
    _browser_dialog.activateWindow()
    return _browser_dialog
