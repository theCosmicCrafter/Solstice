"""Opt-in CivitAI model identification and search.

Identifies model files by their SHA-256 hash against the CivitAI database
and resolves human-readable metadata (name, base model, trigger words,
preview images, download URL). Pure lookup client — all behavior is gated
by the caller's opt-in setting; this module performs no I/O unless asked.

API reference: https://github.com/civitai/civitai/wiki/REST-API-Reference
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

from ..util import client_logger as log
from .network import NetworkError, RequestManager

API_BASE = "https://civitai.com/api/v1"
REQUEST_TIMEOUT = 15.0  # seconds


@dataclass
class CivitaiModelInfo:
    """Resolved metadata for one model version on CivitAI."""

    model_id: int
    version_id: int
    model_name: str
    version_name: str
    base_model: str  # e.g. "SDXL 1.0", "Flux.1 D", "Illustrious", "Pony"
    model_type: str  # e.g. "Checkpoint", "LORA", "TextualInversion"
    trigger_words: list[str] = field(default_factory=list)
    page_url: str = ""
    download_url: str | None = None
    preview_url: str | None = None
    nsfw_level: int = 0  # 0/1 = safe; higher = mature (caller decides)

    @property
    def display_name(self):
        if self.version_name and self.version_name != self.model_name:
            return f"{self.model_name} — {self.version_name}"
        return self.model_name


def _best_preview(images: list[dict[str, Any]]) -> tuple[str | None, int]:
    """Pick the highest-quality safe preview image; return (url, nsfw_level)."""
    best_url = None
    best_level = 1 << 30
    for img in images:
        if not isinstance(img, dict):
            continue
        level = img.get("nsfwLevel")
        if level is None:
            # Older payloads use a boolean
            level = 16 if img.get("nsfw") else 1
        if best_url is None or int(level) < best_level:
            best_url = img.get("url")
            best_level = int(level)
    return best_url, (best_level if best_url is not None else 0)


def parse_version_response(data: dict[str, Any]) -> CivitaiModelInfo | None:
    """Map a `/model-versions/by-hash` response to CivitaiModelInfo.

    Returns None when the payload does not describe a usable model version.
    Pure function — separated from I/O so it is unit-testable offline.
    """
    try:
        version_id = int(data["id"])
        model_id = int(data["modelId"])
    except (KeyError, TypeError, ValueError):
        return None

    model = data.get("model") or {}
    files = data.get("files") or []
    images = data.get("images") or []

    download_url = None
    for f in files:
        if isinstance(f, dict) and f.get("downloadUrl"):
            download_url = f["downloadUrl"]
            break
    if download_url is None and isinstance(data.get("downloadUrl"), str):
        download_url = data["downloadUrl"]

    preview_url, nsfw_level = _best_preview(images)

    trigger_words = [
        str(w) for w in (data.get("trainedWords") or []) if isinstance(w, str) and w.strip()
    ][:50]

    return CivitaiModelInfo(
        model_id=model_id,
        version_id=version_id,
        model_name=str(model.get("name") or data.get("name") or ""),
        version_name=str(data.get("name") or ""),
        base_model=str(data.get("baseModel") or ""),
        model_type=str(model.get("type") or ""),
        trigger_words=trigger_words,
        page_url=f"https://civitai.com/models/{model_id}?modelVersionId={version_id}",
        download_url=download_url,
        preview_url=preview_url,
        nsfw_level=nsfw_level,
    )


class CivitaiClient:
    """Thin async client over the shared RequestManager.

    All requests are explicit calls by the owner — the client performs no
    background work and holds no state beyond the request manager.
    """

    def __init__(self, requests: RequestManager):
        self._requests = requests

    async def lookup_by_hash(self, sha256: str) -> CivitaiModelInfo | None:
        """Resolve a model version by file hash. None when unknown/failed."""
        sha256 = sha256.strip().lower()
        if len(sha256) != 64 or any(c not in "0123456789abcdef" for c in sha256):
            return None
        try:
            data = await self._requests.get(
                f"{API_BASE}/model-versions/by-hash/{sha256}", timeout=REQUEST_TIMEOUT
            )
        except NetworkError as e:
            if e.status in (404, 410):
                return None  # simply unknown to CivitAI
            # Propagate real failures (offline, 5xx, timeout) so callers can
            # distinguish "no match" from "lookup failed".
            log.warning(f"CivitAI hash lookup failed: {e.message}")
            raise
        except Exception as e:
            log.warning(f"CivitAI hash lookup failed: {e}")
            raise
        if not isinstance(data, dict):
            return None
        return parse_version_response(data)

    async def search(
        self,
        query: str,
        model_type: str | None = None,
        limit: int = 20,
    ) -> list[dict[str, Any]]:
        """Search public models. Returns raw model entries (caller maps)."""
        from PyQt6.QtCore import QUrlQuery  # local import: keeps module head Qt-free

        params = QUrlQuery()
        params.addQueryItem("query", query)
        params.addQueryItem("limit", str(max(1, min(int(limit), 100))))
        if model_type:
            params.addQueryItem("types", model_type)
        try:
            data = await self._requests.get(
                f"{API_BASE}/models?{params.query()}", timeout=REQUEST_TIMEOUT
            )
        except Exception as e:
            log.warning(f"CivitAI search failed: {e}")
            return []
        items = data.get("items") if isinstance(data, dict) else None
        return [i for i in items if isinstance(i, dict)] if isinstance(items, list) else []
