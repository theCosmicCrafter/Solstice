"""Tests for the license / min_vram_gb metadata on backend/resources.py
ModelResource and its presence in presets/models.json.

resources.py is pure stdlib and is loaded directly by path so these tests
run without Krita, ComfyUI or PyQt6 (same convention as test_modelspec.py).
"""

import importlib.util
import json
from pathlib import Path

_module_path = Path(__file__).parent.parent / "backend" / "resources.py"
_spec = importlib.util.spec_from_file_location("resources", _module_path)
resources = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(resources)

PRESETS = Path(__file__).parent.parent / "presets" / "models.json"


def _all_models():
    data = json.loads(PRESETS.read_text(encoding="utf-8"))
    return {section: list(resources.ModelResource.from_list(entries))
            for section, entries in data.items()}


def test_models_json_parses():
    parsed = _all_models()
    assert parsed["checkpoints"], "models.json checkpoints failed to parse"
    assert parsed["required"], "models.json required failed to parse"


def test_checkpoints_carry_metadata():
    checkpoints = _all_models()["checkpoints"]
    licensed = [m for m in checkpoints if m.license]
    floored = [m for m in checkpoints if m.min_vram_gb is not None]
    assert len(licensed) == len(checkpoints), "not all checkpoints carry a license field"
    assert len(floored) == len(checkpoints), "not all checkpoints carry a min_vram_gb field"
    for m in floored:
        assert m.min_vram_gb > 0, f"{m.id.string} has a non-positive vram floor"


CATALOG_SECTIONS = [
    "checkpoints",
    "legacy_checkpoints",
    "required",
    "optional",
    "prefetch",
]


def test_all_catalog_entries_have_license_and_sha256():
    """Assert 100% of entries across checkpoints, legacy_checkpoints, required,
    optional, and prefetch carry a non-empty license string, positive min_vram_gb,
    and a valid 64-hex sha256 checksum on every file."""
    data = json.loads(PRESETS.read_text(encoding="utf-8"))
    for section in CATALOG_SECTIONS:
        assert section in data, f"missing section {section} in models.json"
        models = list(resources.ModelResource.from_list(data[section]))
        assert models, f"section {section} has no parsed models"
        for m in models:
            assert isinstance(m.license, str) and m.license.strip(), (
                f"{section} model {m.id.string} is missing a non-empty license"
            )
            assert isinstance(m.min_vram_gb, (int, float)) and m.min_vram_gb > 0, (
                f"{section} model {m.id.string} is missing positive min_vram_gb"
            )
            assert m.files, f"{section} model {m.id.string} has no files"
            for f in m.files:
                assert isinstance(f.sha256, str) and len(f.sha256) == 64 and all(
                    c in "0123456789abcdefABCDEF" for c in f.sha256
                ), f"{section} model {m.id.string} file {f.path} has invalid sha256: {f.sha256}"



def test_restrictive_licenses_are_flagged():
    """The models a user could get into trouble redistributing must not
    silently install — FLUX dev/Kontext assets and MAT are non-commercial."""
    all_models = [m for models in _all_models().values() for m in models]
    by_id = {m.id.string: m for m in all_models}
    assert by_id["checkpoint-flux_dev-flux"].license == "non-commercial"
    assert by_id["checkpoint-flux_kontext-flux"].license == "non-commercial"
    assert by_id["inpaint-default-all"].license == "non-commercial"  # MAT


def test_metadata_round_trips_through_as_dict():
    m = resources.ModelResource(
        "Test", resources.ResourceId.parse("checkpoint-x-sd15"), [],
        license="apache-2.0", min_vram_gb=8)
    d = m.as_dict()
    assert d["license"] == "apache-2.0"
    assert d["min_vram_gb"] == 8
    back = list(resources.ModelResource.from_dict(d))
    assert back[0].license == "apache-2.0"
    assert back[0].min_vram_gb == 8


def test_metadata_defaults_when_absent():
    m = resources.ModelResource(
        "Test", resources.ResourceId.parse("checkpoint-x-sd15"), [])
    assert m.license is None
    assert m.min_vram_gb is None
    assert "license" not in m.as_dict()
    assert "min_vram_gb" not in m.as_dict()
