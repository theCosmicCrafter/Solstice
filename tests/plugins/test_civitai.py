"""Unit tests for backend.civitai — offline; no network calls."""

import sys
import types
from pathlib import Path

import pytest

PLUGIN_ROOT = Path(__file__).resolve().parents[2] / "plugins" / "python" / "ai_diffusion"


def _import_civitai():
    """Import backend.civitai with a synthetic package so relative imports resolve.

    Seeds a minimal `krita` mock (util.py honors IS_MOCK) and a synthetic
    `ai_diffusion` package so the real __init__.py (Krita registration)
    is never executed.
    """
    import importlib

    krita = types.ModuleType("krita")
    krita.IS_MOCK = True
    sys.modules.setdefault("krita", krita)

    pkg = types.ModuleType("ai_diffusion")
    pkg.__path__ = [str(PLUGIN_ROOT)]
    sys.modules.setdefault("ai_diffusion", pkg)

    backend = types.ModuleType("ai_diffusion.backend")
    backend.__path__ = [str(PLUGIN_ROOT / "backend")]
    sys.modules.setdefault("ai_diffusion.backend", backend)

    return importlib.import_module("ai_diffusion.backend.civitai")


@pytest.fixture(scope="module")
def civitai():
    pytest.importorskip("PyQt6")
    return _import_civitai()


def _version_payload(**overrides):
    data = {
        "id": 12345,
        "modelId": 678,
        "name": "v2.0",
        "baseModel": "Flux.1 D",
        "trainedWords": ["masterpiece", "cinematic"],
        "downloadUrl": "https://civitai.com/api/download/models/12345",
        "model": {"name": "CoolModel", "type": "Checkpoint", "nsfw": False},
        "files": [{"name": "cool_v2.safetensors", "downloadUrl": "https://dl/x.safetensors"}],
        "images": [
            {"url": "https://img/prev1.jpg", "nsfwLevel": 8},
            {"url": "https://img/prev2.jpg", "nsfwLevel": 1},
        ],
    }
    data.update(overrides)
    return data


def test_parse_full_payload(civitai):
    info = civitai.parse_version_response(_version_payload())
    assert info is not None
    assert info.model_id == 678
    assert info.version_id == 12345
    assert info.model_name == "CoolModel"
    assert info.version_name == "v2.0"
    assert info.base_model == "Flux.1 D"
    assert info.model_type == "Checkpoint"
    assert info.trigger_words == ["masterpiece", "cinematic"]
    assert info.download_url == "https://dl/x.safetensors"
    # picks the safest preview
    assert info.preview_url == "https://img/prev2.jpg"
    assert info.nsfw_level == 1
    assert "civitai.com/models/678" in info.page_url
    assert info.display_name == "CoolModel — v2.0"


def test_parse_missing_ids(civitai):
    assert civitai.parse_version_response({}) is None
    assert civitai.parse_version_response({"id": "x", "modelId": 1}) is None
    assert civitai.parse_version_response({"id": 1}) is None


def test_parse_no_images(civitai):
    info = civitai.parse_version_response(_version_payload(images=[]))
    assert info is not None
    assert info.preview_url is None
    assert info.nsfw_level == 0


def test_parse_legacy_bool_nsfw(civitai):
    info = civitai.parse_version_response(
        _version_payload(images=[{"url": "https://img/a.jpg", "nsfw": True}])
    )
    assert info is not None
    assert info.nsfw_level == 16


def test_trigger_words_cap_and_filter(civitai):
    words = [f"w{i}" for i in range(80)] + ["", 42, None]
    info = civitai.parse_version_response(_version_payload(trainedWords=words))
    assert info is not None
    assert len(info.trigger_words) == 50
    assert all(isinstance(w, str) for w in info.trigger_words)


def test_top_level_download_fallback(civitai):
    info = civitai.parse_version_response(_version_payload(files=[]))
    assert info is not None
    assert info.download_url == "https://civitai.com/api/download/models/12345"


def test_hash_validation(civitai):
    # validation happens in lookup_by_hash; exercise it without network
    client = civitai.CivitaiClient(requests=None)
    import asyncio

    assert asyncio.run(client.lookup_by_hash("not-a-hash")) is None
    assert asyncio.run(client.lookup_by_hash("zz" * 32)) is None
    assert asyncio.run(client.lookup_by_hash("")) is None
