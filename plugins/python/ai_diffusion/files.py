import asyncio
import hashlib
import json
import os
import time
from base64 import b64decode, b64encode
from collections.abc import Sequence
from dataclasses import dataclass, fields
from enum import Enum, Flag
from pathlib import Path
from typing import Any, Callable, NamedTuple, cast

from PyQt6.QtCore import QAbstractListModel, QModelIndex, QSortFilterProxyModel, Qt
from PyQt6.QtGui import QIcon

from .backend.modelspec import extract_metadata_fields, read_safetensors_metadata
from .util import client_logger as log
from .util import encode_json, read_json_with_comments, user_data_dir


class FileSource(Flag):
    unavailable = 0
    local = 1
    remote = 2


class FileFormat(Enum):
    unknown = 0
    checkpoint = 1  # TE + VAE + Diffusion model
    diffusion = 2  # Diffusion model only
    lora = 3


@dataclass
class File:
    id: str
    name: str
    source: FileSource = FileSource.unavailable
    format: FileFormat = FileFormat.unknown
    icon: QIcon | None = None
    hash: str | None = None
    path: Path | None = None
    size: int | None = None
    metadata: dict[str, Any] | None = None

    @staticmethod
    def remote(id: str, format=FileFormat.unknown):
        name = id.replace("\\", "/")
        dot = name.rfind(".")
        name = name if dot == -1 else name[:dot]
        return File(id, name, FileSource.remote, format)

    @staticmethod
    def local(path: Path, format=FileFormat.unknown, compute_hash=False):
        file = File(path.name, path.stem, FileSource.local, format, path=path)
        file.size = path.stat().st_size
        if compute_hash:
            file.compute_hash()
        return file

    @staticmethod
    def from_dict(data: dict):
        data["source"] = FileSource(data["source"])
        data["path"] = Path(data["path"]) if data.get("path") else None
        return File(**data)

    def to_dict(self):
        return {
            f.name: getattr(self, f.name)
            for f in fields(self)
            if f.name != "icon" and getattr(self, f.name) is not None
        }

    def update(self, other: "File"):
        assert self.id == other.id, "Cannot update file with different id"
        has_changes = self != other
        if has_changes:
            self.source = self.source | other.source
            self.icon = other.icon or self.icon
            self.hash = other.hash or self.hash
            self.path = other.path or self.path
            self.size = other.size or self.size
            # preserve user-edited keys (lora_triggers, lora_strength)
            self.metadata = {**(other.metadata or {}), **(self.metadata or {})} or None
        return has_changes

    async def compute_hash_async(self, progress_callback: Callable[[int, int], None] | None = None) -> str:
        """Compute hash asynchronously without blocking the main thread.

        Args:
            progress_callback: Optional callback(current_bytes, total_bytes) for progress updates

        Returns:
            Base64-encoded SHA256 hash of the file
        """
        if self.hash:
            return self.hash

        assert self.path is not None, "Local filepath must be set to compute hash"
        assert self.size is not None, "File size must be known"

        loop = asyncio.get_event_loop()

        def _compute_hash():
            sha = hashlib.sha256()
            processed = 0
            chunk_size = 65536  # 64KB chunks for better performance

            with open(self.path, "rb") as f:
                while chunk := f.read(chunk_size):
                    sha.update(chunk)
                    processed += len(chunk)
                    if progress_callback and processed % (1024 * 1024) == 0:  # Every MB
                        # Schedule callback on main thread
                        asyncio.run_coroutine_threadsafe(
                            self._invoke_progress(progress_callback, processed, self.size),
                            loop
                        )

            return b64encode(sha.digest()).decode()

        self.hash = await loop.run_in_executor(None, _compute_hash)
        self.read_model_metadata()
        return self.hash

    async def _invoke_progress(self, callback: Callable[[int, int], None], current: int, total: int):
        """Helper to invoke progress callback from executor thread."""
        try:
            callback(current, total)
        except Exception:
            pass  # Ignore callback errors

    def compute_partial_hash(self, max_mb: int = 100) -> str:
        """Compute hash of first and last N MB for quick comparison.

        This is much faster for large model files and suitable for caching.

        Args:
            max_mb: Number of MB to hash from start and end of file

        Returns:
            Base64-encoded SHA256 hash of partial content
        """
        if self.hash:
            return self.hash

        assert self.path is not None, "Local filepath must be set to compute hash"

        sha = hashlib.sha256()
        max_bytes = max_mb * 1024 * 1024

        with open(self.path, "rb") as f:
            # Hash first N MB
            sha.update(f.read(max_bytes))

            # Hash last N MB if file is larger
            if self.size and self.size > max_bytes:
                f.seek(-max_bytes, 2)
                sha.update(f.read(max_bytes))

        self.hash = b64encode(sha.digest()).decode()
        self.read_model_metadata()
        return self.hash

    def compute_hash(self):
        if self.hash:
            return self.hash
        assert self.path is not None, "Local filepath must be set to compute hash"
        sha = hashlib.sha256()
        with open(self.path, "rb") as f:
            while chunk := f.read(4096):
                sha.update(chunk)
        self.hash = b64encode(sha.digest()).decode()
        self.read_model_metadata()
        return self.hash

    def read_model_metadata(self):
        """Read modelspec metadata from the safetensors header into `metadata`.

        Reads at most ~32 MiB from the start of the file (typically a few KB).
        Results are stored in ModelMetadataCache so repeated scans stay cheap.
        Existing metadata keys are never overwritten."""
        if self.path is None or self.path.suffix.lower() != ".safetensors":
            return
        fields = ModelMetadataCache.get(self.path)
        if fields is None:
            fields = extract_metadata_fields(read_safetensors_metadata(self.path))
            if fields:
                ModelMetadataCache.set(self.path, fields)
        if not fields:
            return
        if self.metadata is None:
            self.metadata = {}
        for key, value in fields.items():
            self.metadata.setdefault(key, value)
        if triggers := fields.get("trigger_words"):
            self.metadata.setdefault("lora_triggers", " ".join(triggers))

    def meta(self, key: str, default=None) -> Any:
        return self.metadata.get(key, default) if self.metadata else default


class ModelMetadataCache:
    """Persistent cache for model metadata with TTL and file modification tracking."""

    _cache_file: Path = user_data_dir / "model_metadata_cache.json"
    _cache: dict[str, dict] = {}
    _default_ttl: int = 300  # 5 minutes default

    @classmethod
    def load(cls):
        """Load cache from disk."""
        if cls._cache_file.exists():
            try:
                cls._cache = json.loads(cls._cache_file.read_text())
                log.info(f"Loaded model metadata cache with {len(cls._cache)} entries")
            except Exception as e:
                log.warning(f"Failed to load model metadata cache: {e}")
                cls._cache = {}

    @classmethod
    def get(cls, file_path: Path) -> dict | None:
        """Get cached metadata for a file if valid.

        Returns metadata if:
        - Cache entry exists
        - File hasn't been modified since cache
        - TTL hasn't expired
        """
        key = str(file_path)
        entry = cls._cache.get(key)
        if not entry:
            return None

        # Check TTL
        cached_at = entry.get("cached_at", 0)
        if time.time() - cached_at > cls._default_ttl:
            return None

        # Check if file modified
        try:
            current_mtime = file_path.stat().st_mtime
            if current_mtime > entry.get("mtime", 0):
                return None  # File changed, cache invalid
        except (OSError, FileNotFoundError):
            return None

        return entry.get("metadata")

    @classmethod
    def set(cls, file_path: Path, metadata: dict, save: bool = True):
        """Cache metadata for a file. Pass save=False in bulk loops, then flush() once."""
        try:
            mtime = file_path.stat().st_mtime
            cls._cache[str(file_path)] = {
                "mtime": mtime,
                "metadata": metadata,
                "cached_at": time.time(),
            }
            if save:
                cls._save()
        except (OSError, FileNotFoundError):
            pass

    @classmethod
    def flush(cls):
        """Persist pending cache entries after batched set(..., save=False) calls."""
        cls._save()

    @classmethod
    def invalidate(cls, file_path: Path | None = None):
        """Invalidate cache entry or entire cache."""
        if file_path:
            cls._cache.pop(str(file_path), None)
        else:
            cls._cache.clear()
        cls._save()

    @classmethod
    def _save(cls):
        """Save cache to disk atomically (temp file + rename, so a crash
        mid-write can't corrupt the cache)."""
        try:
            cls._cache_file.parent.mkdir(parents=True, exist_ok=True)
            tmp = cls._cache_file.with_suffix(".tmp")
            tmp.write_text(json.dumps(cls._cache, default=str))
            os.replace(tmp, cls._cache_file)
        except Exception as e:
            log.warning(f"Failed to save model metadata cache: {e}")


# Initialize cache on module load
ModelMetadataCache.load()


def _normalize_model_query(query: str) -> str:
    """Strip LoRA tag wrapper <lora:...> and weight suffixes :1.0."""
    q = query.strip()
    if q.startswith("<lora:") and q.endswith(">"):
        q = q[6:-1].strip()
    if ":" in q:
        q = q.split(":", 1)[0].strip()
    return q


class FileCollection(QAbstractListModel):
    source_role = Qt.ItemDataRole.UserRole + 1

    def __init__(self, database: Path | None = None, parent=None):
        super().__init__(parent)

        self._database = database
        self._files: list[File] = []
        self.load()

    def rowCount(self, parent: QModelIndex | None = None):
        return len(self._files)

    def data(self, index: QModelIndex, role: int = 0):
        if 0 <= index.row() < len(self._files):
            item = self._files[index.row()]

            match role:
                case Qt.ItemDataRole.DisplayRole | Qt.ItemDataRole.EditRole:
                    return item.name
                case Qt.ItemDataRole.DecorationRole:
                    return item.icon
                case Qt.ItemDataRole.UserRole:
                    return item.id
                case FileCollection.source_role:
                    return item.source.value
                case _:
                    return None

    def extend(self, input_files: Sequence[File]):
        if len(input_files) == 0:
            return

        existing = {f.id: (i, f) for i, f in enumerate(self._files)}
        new_files = []
        for f in input_files:
            if lookup := existing.get(f.id):
                idx, existing_file = lookup
                if existing_file.update(f):
                    self.dataChanged.emit(self.index(idx), self.index(idx))
            else:
                new_files.append(f)

        if len(new_files) > 0:
            end = len(self._files)
            self.beginInsertRows(QModelIndex(), end, end + len(new_files) - 1)
            self._files.extend(new_files)
            self.endInsertRows()

        self.save()

    def remove(self, index: int):
        self.beginRemoveRows(QModelIndex(), index, index)
        del self._files[index]
        self.endRemoveRows()
        self.save()

    def update(self, new_files: Sequence[File], source: FileSource):
        new_ids = {f.id for f in new_files}
        for i, f in enumerate(self._files):
            if source in f.source and f.id not in new_ids:
                f.source = f.source & (~source)
                self.dataChanged.emit(self.index(i), self.index(i))

        self.extend(new_files)

    def add(self, file: File):
        self.extend([file])
        return file

    def find(self, query: str) -> File | None:
        """Find a model file by exact ID, path, filename, stem, hash, or title."""
        return self._find_matching(query, local_only=False)

    def find_local(self, query: str) -> File | None:
        """Find a locally available model file by exact ID, path, filename, stem, hash, or title."""
        return self._find_matching(query, local_only=True)

    def _find_matching(self, query: str, local_only: bool = False) -> File | None:
        if not query:
            return None
        candidates = [f for f in self._files if (not local_only or FileSource.local in f.source)]
        if not candidates:
            return None

        # 1. Exact match
        for f in candidates:
            if f.id == query:
                return f

        # Clean query (strip <lora:...>, weights)
        clean_q = _normalize_model_query(query)
        if clean_q != query:
            for f in candidates:
                if f.id == clean_q:
                    return f

        clean_norm = clean_q.replace("\\", "/").lower()
        clean_name = Path(clean_norm).name.lower()
        clean_stem = Path(clean_norm).stem.lower()

        # 2. Normalized path match (case-insensitive, slash-insensitive)
        for f in candidates:
            if f.id.replace("\\", "/").lower() == clean_norm:
                return f

        # 3. Filename match (ignoring parent subfolder)
        for f in candidates:
            if Path(f.id.replace("\\", "/")).name.lower() == clean_name:
                return f

        # 4. Stem match (ignoring extension like .safetensors, .ckpt, .pt)
        for f in candidates:
            f_stem = Path(f.id.replace("\\", "/")).stem.lower()
            if f_stem == clean_stem or (f.name and f.name.lower() == clean_stem):
                return f

        # 5. Hash lookup (exact b64, hex sha256, autov2/autov3 prefix)
        q_hex = clean_q.lower()
        is_hex = len(q_hex) >= 8 and all(c in "0123456789abcdef" for c in q_hex)
        for f in candidates:
            if f.hash and f.hash == clean_q:
                return f
            if is_hex and f.hash:
                try:
                    f_hex = b64decode(f.hash).hex().lower()
                    if f_hex == q_hex or f_hex.startswith(q_hex) or q_hex in f_hex:
                        return f
                except Exception:
                    pass
            if f.metadata:
                for h_key in ("hash", "sha256", "autov2", "autov1"):
                    val = str(f.metadata.get(h_key, "")).lower()
                    if val and (val == q_hex or val.startswith(q_hex)):
                        return f

        # 6. Metadata Title / ss_sd_model_name match
        for f in candidates:
            if f.metadata:
                title = str(f.metadata.get("modelspec.title", "")).strip().lower()
                if title and (title == clean_stem or title == clean_norm):
                    return f
                sd_name = str(f.metadata.get("ss_sd_model_name", "")).strip()
                if sd_name and Path(sd_name).stem.lower() == clean_stem:
                    return f

        return None

    def find_index(self, id: str):
        idx = next((i for i, f in enumerate(self._files) if f.id == id), -1)
        if idx >= 0:
            return idx
        match = self.find(id)
        if match:
            return next((i for i, f in enumerate(self._files) if f is match), -1)
        return -1

    def set_meta(self, file: File, key: str, value: Any):
        if not file.metadata:
            file.metadata = {}
        file.metadata[key] = value
        if (i := self.find_index(file.id)) >= 0:
            self.dataChanged.emit(self.index(i), self.index(i))
        self.save()

    def load(self):
        if not self._database or not self._database.exists():
            return
        try:
            data = read_json_with_comments(self._database)
            self.extend([File.from_dict(f) for f in data])
            log.info(f"Loaded {len(self)} model files from {self._database}")
            self._remove_missing_files()
        except Exception as e:
            log.error(f"Failed to read {self._database}: {e}")

    def save(self):
        if self._database:
            db = [f.to_dict() for f in self._files]
            self._database.parent.mkdir(parents=True, exist_ok=True)
            self._database.write_text(json.dumps(db, indent=2, default=encode_json))

    def flags(self, index: QModelIndex):
        if not index.isValid():
            return super().flags(index) | Qt.ItemFlag.ItemIsDropEnabled
        return super().flags(index) | Qt.ItemFlag.ItemIsDragEnabled | Qt.ItemFlag.ItemIsDropEnabled

    def _remove_missing_files(self):
        i = 0
        while i < len(self._files):
            f = self._files[i]
            is_valid = True
            if f.source is FileSource.local and f.path:
                try:
                    is_valid = f.path.exists()
                except OSError:
                    is_valid = False
            if not is_valid:
                log.warning(f"Local file {f.path} inaccessible, removing from collection")
                self.remove(i)
            else:
                i += 1

    def __iter__(self):
        return iter(self._files)

    def __len__(self):
        return len(self._files)

    def __getitem__(self, index: int):
        return self._files[index]


_instance = None


class FileLibrary(NamedTuple):
    checkpoints: FileCollection
    loras: FileCollection

    @staticmethod
    def load(database_dir: Path | None = None):
        global _instance
        database_dir = database_dir or user_data_dir / "database"
        _instance = FileLibrary(
            checkpoints=FileCollection(database_dir / "checkpoints.json"),
            loras=FileCollection(database_dir / "loras.json"),
        )
        return _instance

    @staticmethod
    def instance():
        if not _instance:
            return FileLibrary.load()
        return _instance


class FileFilter(QSortFilterProxyModel):
    def __init__(self, source: FileCollection, parent=None):
        super().__init__(parent)
        self._available_only = False
        self._name_prefix = ""
        self.setSourceModel(source)
        self.setSortCaseSensitivity(Qt.CaseSensitivity.CaseInsensitive)
        self.sort(0)

    @property
    def available_only(self):
        return self._available_only

    @available_only.setter
    def available_only(self, value):
        self._available_only = value
        self.invalidateFilter()

    @property
    def name_prefix(self):
        return self._name_prefix

    @name_prefix.setter
    def name_prefix(self, value):
        self._name_prefix = value
        self.invalidateFilter()

    def filterAcceptsRow(self, source_row: int, source_parent: QModelIndex):
        if src := self.sourceModel():
            index = src.index(source_row, 0, source_parent)
            if self._available_only:
                source = FileSource(src.data(index, FileCollection.source_role))
                if source is FileSource.unavailable:
                    return False
            if self._name_prefix:
                name = src.data(index)
                if not name.startswith(self._name_prefix):
                    return False
        return True

    def __getitem__(self, index: int):
        src = cast(FileCollection, self.sourceModel())
        idx = self.mapToSource(self.index(index, 0)).row()
        return src[idx]
