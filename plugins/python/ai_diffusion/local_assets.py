from __future__ import annotations

import os
import random
import re
from pathlib import Path
from typing import TYPE_CHECKING, Any, Iterable, NamedTuple

from PyQt6.QtCore import QObject, pyqtSignal

from .settings import settings
from .util import client_logger as log

if TYPE_CHECKING:
    from .document import Document
    from .layer import Layer

# Optional local asset roots (Solstice feature — see docs/agent/local-assets.md).
# Files are only ever read from disk; nothing is downloaded. Roots default to
# machine-local library folders and can be changed in Settings > Interface or
# overridden per-machine via environment variables (env var wins over the
# settings value, so a shared settings.json can still be tweaked per machine).

wildcard_file_extensions = {".txt", ".yaml", ".yml"}
pose_file_extensions = {".png", ".jpg", ".jpeg", ".webp", ".bmp", ".tif", ".tiff"}
lut_file_extensions = {".cube", ".3dl", ".look"}

# __name__ tokens (Automatic1111 wildcard syntax). Names resolve relative to the
# wildcard root, e.g. __animals/cats__ -> <root>/animals/cats.txt|.yaml.
# Non-greedy so single _ inside names (adjective_base) works; ____ stays literal.
pattern_file_wildcard = re.compile(r"__(?!_)(.{1,200}?)__")


class AssetRoot(NamedTuple):
    """Where one kind of local assets is read from."""

    kind: str  # "wildcards" | "poses" | "luts"
    settings_key: str  # key in the plugin settings.json
    env_var: str  # environment variable that overrides the settings value
    extensions: set[str]


asset_roots = [
    AssetRoot(
        "wildcards",
        "wildcards_folder",
        "KRITA_AI_DIFFUSION_WILDCARDS_FOLDER",
        wildcard_file_extensions,
    ),
    AssetRoot(
        "poses",
        "poses_folder",
        "KRITA_AI_DIFFUSION_POSES_FOLDER",
        pose_file_extensions,
    ),
    AssetRoot(
        "luts",
        "luts_folder",
        "KRITA_AI_DIFFUSION_LUTS_FOLDER",
        lut_file_extensions,
    ),
]

_settings_keys = {root.settings_key: root.kind for root in asset_roots}


def _normalize_key(relative: Path) -> str:
    """Index key: extensionless relative path, forward slashes, lower case."""
    return relative.with_suffix("").as_posix().lower()


def _normalize_name(name: str) -> str:
    """Lookup key for a user-typed name (e.g. inside __...__ in a prompt)."""
    return name.strip().strip("/\\").replace("\\", "/").lower()


class LocalAssets(QObject):
    """Lazy, cached indexes of optional local asset folders.

    All indexes are built on first access and return empty results when the
    configured root does not exist or cannot be read. Use reload() (or change
    the corresponding settings key) to rescan.
    """

    changed = pyqtSignal()

    _wildcard_paths: dict[str, Path] | None = None  # name -> file (fast scan)
    _wildcard_lines: dict[str, list[str]]  # name -> lines (read on demand)
    _poses: dict[str, Path] | None = None
    _luts: dict[str, Path] | None = None

    def __init__(self):
        super().__init__()
        self._wildcard_lines = {}
        settings.changed.connect(self._on_setting_changed)

    # --- root resolution -------------------------------------------------

    def root(self, kind: str) -> Path | None:
        """Effective root folder for an asset kind, or None when not usable."""
        spec = next(r for r in asset_roots if r.kind == kind)
        raw = os.environ.get(spec.env_var) or getattr(settings, spec.settings_key, "")
        raw = raw.strip()
        if not raw:
            return None
        path = Path(os.path.expandvars(raw)).expanduser()
        return path if path.is_dir() else None

    def is_available(self, kind: str) -> bool:
        return self.root(kind) is not None

    # --- indexes ---------------------------------------------------------

    @property
    def wildcards(self) -> dict[str, list[str]]:
        """Wildcard name -> list of prompt lines (normalized name, see _normalize_key).

        Reads every wildcard file on first access (~10 s for very large
        libraries). Callers that only need names or a single entry should use
        wildcard_names / wildcard_lines instead — both stay cheap.
        """
        return {name: self._wildcard_entry(name) for name in self._wildcard_path_index()}

    @property
    def wildcard_names(self) -> list[str]:
        """Sorted wildcard names (fast — does not read file contents)."""
        return sorted(self._wildcard_path_index())

    @property
    def poses(self) -> dict[str, Path]:
        """Pose image name -> file path (normalized name, see _normalize_key)."""
        if self._poses is None:
            self._poses = self._scan_files("poses")
        return self._poses

    @property
    def luts(self) -> dict[str, Path]:
        """LUT name -> file path (normalized name, see _normalize_key)."""
        if self._luts is None:
            self._luts = self._scan_files("luts")
        return self._luts

    def _scan_files(self, kind: str) -> dict[str, Path]:
        root = self.root(kind)
        index: dict[str, Path] = {}
        if root is None:
            return index
        spec = next(r for r in asset_roots if r.kind == kind)
        try:
            for path in root.rglob("*"):
                if path.is_file() and path.suffix.lower() in spec.extensions:
                    index.setdefault(_normalize_key(path.relative_to(root)), path)
        except OSError as e:
            log.warning(f"Failed to scan local {kind} folder {root}: {e}")
        else:
            log.info(f"Indexed {len(index)} local {kind} files from {root}")
        return index

    def _wildcard_path_index(self) -> dict[str, Path]:
        """Wildcard name -> file. Fast name-only scan; contents stay unread."""
        if self._wildcard_paths is None:
            self._wildcard_paths = self._scan_files("wildcards")
        return self._wildcard_paths

    def _wildcard_entry(self, normalized_name: str) -> list[str]:
        """Lines of one wildcard, read from disk on first use and memoized."""
        if normalized_name not in self._wildcard_lines:
            lines: list[str] = []
            if path := self._wildcard_path_index().get(normalized_name):
                try:
                    lines = read_wildcard_file(path)
                except OSError as e:
                    log.warning(f"Failed to read wildcard file {path}: {e}")
            self._wildcard_lines[normalized_name] = lines
        return self._wildcard_lines[normalized_name]

    def find(self, kind: str, name: str) -> Path | None:
        """Look up a pose/LUT by name (path relative to the root, no extension)."""
        index = self.poses if kind == "poses" else self.luts if kind == "luts" else {}
        return index.get(_normalize_name(name))

    def wildcard_lines(self, name: str) -> list[str] | None:
        """Lines of a single wildcard (cheap: only this file is read), or None."""
        if _normalize_name(name) not in self._wildcard_path_index():
            return None
        return self._wildcard_entry(_normalize_name(name))

    def counts(self) -> dict[str, int]:
        return {
            "wildcards": len(self._wildcard_path_index()),
            "poses": len(self.poses),
            "luts": len(self.luts),
        }

    def reload(self, kind: str | None = None):
        """Drop cached indexes so they are rebuilt on next access."""
        if kind in (None, "wildcards"):
            self._wildcard_paths = None
            self._wildcard_lines = {}
        if kind in (None, "poses"):
            self._poses = None
        if kind in (None, "luts"):
            self._luts = None
        self.changed.emit()

    def _on_setting_changed(self, name: str, value: Any):
        if kind := _settings_keys.get(name):
            self.reload(kind)

    # --- prompt wildcards --------------------------------------------------

    def expand_wildcards(self, text: str, rng: random.Random) -> str:
        """Replace __name__ tokens with a random line of the wildcard file.

        Unknown names are left untouched so literal __text__ survives. File
        content may itself contain {a|b} inline wildcards or nested __name__
        tokens — the caller iterates until the text is stable.
        """
        if "__" not in text:
            return text
        if not self._wildcard_path_index():
            return text

        def replace(match: re.Match[str]):
            if lines := self._wildcard_entry(_normalize_name(match[1])):
                return rng.choice(lines)
            return match[0]

        return pattern_file_wildcard.sub(replace, text)

    # --- poses -------------------------------------------------------------

    def create_pose_layer(
        self, document: Document, name: str, make_active=True
    ) -> Layer | None:
        """Import an indexed pose image as a paint layer named "[Pose] <name>",
        centered on the canvas. Returns the layer, or None when the name is
        unknown. Intended caller: a pose picker next to ControlNet pose layers.
        """
        from .image import Bounds, Image

        path = self.poses.get(_normalize_name(name))
        if path is None:
            return None
        image = Image.load(path)
        canvas = document.extent
        bounds = Bounds(
            max(0, (canvas.width - image.width) // 2),
            max(0, (canvas.height - image.height) // 2),
            image.width,
            image.height,
        )
        return document.layers.create(f"[Pose] {name}", image, bounds, make_active)


def read_wildcard_file(path: Path) -> list[str]:
    """Read one wildcard file into a list of candidate lines."""
    text = path.read_text(encoding="utf-8", errors="replace")
    if path.suffix.lower() in (".yaml", ".yml"):
        return _read_yaml_wildcard(text)
    return _read_txt_wildcard(text)


def _read_txt_wildcard(text: str) -> list[str]:
    return [
        line
        for raw in text.splitlines()
        if (line := raw.strip()) and not line.startswith("#")
    ]


def _read_yaml_wildcard(text: str) -> list[str]:
    """Extract candidate strings from a wildcard .yaml file.

    PyYAML is a server-side dependency and may not exist in Krita's Python, so
    fall back to a lenient line parser that understands the common shape of
    wildcard packs (nested `key:` mappings with `- item` lists).
    """
    try:
        import yaml  # type: ignore
    except ImportError:
        yaml = None

    if yaml is not None:
        try:
            data = yaml.safe_load(text)
        except Exception:
            data = None
        lines = [s for s in _flatten_yaml(data)]
        if lines:
            return lines
    return _read_yaml_wildcard_fallback(text)


def _flatten_yaml(data: Any) -> Iterable[str]:
    if isinstance(data, dict):
        for value in data.values():
            yield from _flatten_yaml(value)
    elif isinstance(data, (list, tuple)):
        for item in data:
            yield from _flatten_yaml(item)
    elif isinstance(data, str):
        if line := data.strip():
            yield line
    elif isinstance(data, (int, float)):
        yield str(data)


def _read_yaml_wildcard_fallback(text: str) -> list[str]:
    lines: list[str] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith(("#", "---")):
            continue
        if line.startswith("- "):
            line = line[2:].strip()
        elif ":" in line:
            # mapping line "key: value" — keep the value, skip bare keys
            _, _, value = line.partition(":")
            if not (line := value.strip()):
                continue
        if line := line.strip("'\""):
            lines.append(line)
    return lines


local_assets = LocalAssets()
