"""Tests for backend/modelspec.py (safetensors header / modelspec parsing).

modelspec.py is pure stdlib and is loaded directly by path so these tests run
without Krita, ComfyUI or PyQt6.
"""

import importlib.util
import json
import struct
from pathlib import Path

import pytest

_module_path = Path(__file__).parent.parent / "backend" / "modelspec.py"
_spec = importlib.util.spec_from_file_location("modelspec", _module_path)
modelspec = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(modelspec)


def make_safetensors(metadata=None, extra_header=None) -> bytes:
    header = {"weight": {"dtype": "F32", "shape": [1], "data_offsets": [0, 4]}}
    if extra_header:
        header.update(extra_header)
    if metadata is not None:
        header["__metadata__"] = metadata
    header_bytes = json.dumps(header).encode("utf-8")
    return struct.pack("<Q", len(header_bytes)) + header_bytes + b"\x00" * 4


def write_blob(tmp_path: Path, data: bytes, name="model.safetensors") -> Path:
    path = tmp_path / name
    path.write_bytes(data)
    return path


def test_reads_metadata_dict(tmp_path):
    meta = {"modelspec.architecture": "stable-diffusion-xl", "other": "1"}
    path = write_blob(tmp_path, make_safetensors(meta))
    assert modelspec.read_safetensors_metadata(path) == meta


def test_accepts_str_path(tmp_path):
    meta = {"modelspec.title": "test"}
    path = write_blob(tmp_path, make_safetensors(meta))
    assert modelspec.read_safetensors_metadata(str(path)) == meta


def test_missing_metadata_key(tmp_path):
    path = write_blob(tmp_path, make_safetensors(metadata=None))
    assert modelspec.read_safetensors_metadata(path) == {}


def test_non_dict_metadata(tmp_path):
    blob = make_safetensors(extra_header={"__metadata__": "not a dict"})
    path = write_blob(tmp_path, blob)
    assert modelspec.read_safetensors_metadata(path) == {}


def test_missing_file(tmp_path):
    assert modelspec.read_safetensors_metadata(tmp_path / "nope.safetensors") == {}


def test_truncated_file(tmp_path):
    path = write_blob(tmp_path, b"\x05\x00\x00")
    assert modelspec.read_safetensors_metadata(path) == {}


def test_truncated_header(tmp_path):
    blob = make_safetensors({"a": "b"})
    path = write_blob(tmp_path, blob[:12])  # header length says more than present
    assert modelspec.read_safetensors_metadata(path) == {}


def test_huge_header_rejected(tmp_path):
    blob = struct.pack("<Q", 10 * 1024**3) + b"{}"
    path = write_blob(tmp_path, blob)
    assert modelspec.read_safetensors_metadata(path) == {}


def test_invalid_json(tmp_path):
    payload = b"this is not json at all"
    blob = struct.pack("<Q", len(payload)) + payload
    path = write_blob(tmp_path, blob)
    assert modelspec.read_safetensors_metadata(path) == {}


def test_header_not_a_dict(tmp_path):
    payload = json.dumps([1, 2, 3]).encode("utf-8")
    blob = struct.pack("<Q", len(payload)) + payload
    path = write_blob(tmp_path, blob)
    assert modelspec.read_safetensors_metadata(path) == {}


def test_not_safetensors_file(tmp_path):
    path = write_blob(tmp_path, b"PK\x03\x04" + b"\x00" * 64)
    assert modelspec.read_safetensors_metadata(path) == {}


def test_arch_precedence():
    meta = {
        "modelspec.architecture": "flux",
        "modelspec.base_model": "sdxl",
        "modelspec.title": "Something",
        "ss_sd_model_name": "foo.safetensors",
        "ss_base_model_version": "sd_v1-5",
    }
    assert modelspec.arch_from_metadata(meta) == "flux"


def test_arch_fallback_order():
    assert modelspec.arch_from_metadata({"modelspec.base_model": "sdxl"}) == "sdxl"
    assert modelspec.arch_from_metadata({"modelspec.title": "T"}) == "T"
    meta = {"ss_sd_model_name": "m.safetensors", "ss_base_model_version": "sd_v1-5"}
    assert modelspec.arch_from_metadata(meta) == "m.safetensors"
    assert modelspec.arch_from_metadata({"ss_base_model_version": "sd_v1-5"}) == "sd_v1-5"


def test_arch_no_match():
    assert modelspec.arch_from_metadata({}) is None
    assert modelspec.arch_from_metadata({"modelspec.architecture": "  "}) is None
    assert modelspec.arch_from_metadata({"modelspec.architecture": 5}) is None


def test_arch_strings_all_candidates():
    meta = {"modelspec.title": "T", "ss_base_model_version": "sdxl", "other": "x"}
    assert modelspec.arch_strings_from_metadata(meta) == ["T", "sdxl"]


def test_trigger_phrase():
    meta = {"modelspec.trigger_phrase": "magic_word, other style"}
    assert modelspec.trigger_words_from_metadata(meta) == ["magic_word", "other style"]


def test_trigger_tag_frequency_dict():
    meta = {"ss_tag_frequency": {"ds": {"cat": 5, "dog": 10, "bird": 1}}}
    assert modelspec.trigger_words_from_metadata(meta) == ["dog", "cat", "bird"]


def test_trigger_tag_frequency_json_string():
    inner = json.dumps({"ds1": {"a": 1}, "ds2": {"b": 4, "a": 2}})
    meta = {"ss_tag_frequency": inner}
    assert modelspec.trigger_words_from_metadata(meta) == ["b", "a"]


def test_trigger_words_dedup_and_order():
    meta = {
        "modelspec.trigger_phrase": "hero, x",
        "ss_tag_frequency": {"ds": {"hero": 9, "villain": 3}},
    }
    assert modelspec.trigger_words_from_metadata(meta) == ["hero", "x", "villain"]


def test_trigger_words_capped():
    tags = {f"tag{i}": i for i in range(200)}
    meta = {"ss_tag_frequency": {"ds": tags}}
    result = modelspec.trigger_words_from_metadata(meta)
    assert len(result) == modelspec.max_trigger_words
    assert result[0] == "tag199"


def test_trigger_words_garbage():
    assert modelspec.trigger_words_from_metadata({"ss_tag_frequency": "{bad json"}) == []
    assert modelspec.trigger_words_from_metadata({"ss_tag_frequency": 42}) == []
    assert modelspec.trigger_words_from_metadata({}) == []


def test_extract_metadata_fields():
    meta = {
        "modelspec.architecture": "flux",
        "modelspec.title": "  My Model  ",
        "ss_tag_frequency": {"ds": {"hi": 2}},
        "unrelated": "ignored",
    }
    fields = modelspec.extract_metadata_fields(meta)
    assert fields["modelspec.architecture"] == "flux"
    assert fields["modelspec.title"] == "My Model"
    assert fields["trigger_words"] == ["hi"]
    assert "unrelated" not in fields
    assert modelspec.arch_from_metadata(fields) == "flux"


def test_extract_metadata_fields_empty():
    assert modelspec.extract_metadata_fields({}) == {}
    assert modelspec.extract_metadata_fields({"x": 1}) == {}


@pytest.mark.parametrize("size", [0, 4, 7])
def test_tiny_files(tmp_path, size):
    path = write_blob(tmp_path, b"\x00" * size)
    assert modelspec.read_safetensors_metadata(path) == {}
