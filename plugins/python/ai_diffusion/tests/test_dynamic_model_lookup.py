# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for dynamic model lookup, hash matching, and LoRA syntax parsing."""

from base64 import b64encode
import hashlib
from pathlib import Path
import pytest

from ai_diffusion.files import File, FileCollection, FileSource, FileFormat, _normalize_model_query


def _sha256_b64(text: str) -> str:
    digest = hashlib.sha256(text.encode("utf-8")).digest()
    return b64encode(digest).decode()


def _sha256_hex(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


@pytest.fixture
def collection():
    col = FileCollection()
    
    # 1. Checkpoint in subfolder
    f1 = File(
        id=r"Illustrious\illustriousRealismBy_v10VAE.safetensors",
        name="illustriousRealismBy_v10VAE",
        source=FileSource.local | FileSource.remote,
        format=FileFormat.checkpoint,
        hash=_sha256_b64("illustrious-realism-content"),
        metadata={
            "modelspec.architecture": "stable-diffusion-xl-v1-base",
            "modelspec.title": "Illustrious Realism V10",
        }
    )
    
    # 2. LoRA with Kohya metadata and AutoV2 hash
    lora_content = "retro-anime-flux-lora-weights"
    lora_hex = _sha256_hex(lora_content)
    f2 = File(
        id=r"Flux.1 D\xeRetroAnimeFlux_01.safetensors",
        name="xeRetroAnimeFlux_01",
        source=FileSource.local,
        format=FileFormat.lora,
        hash=_sha256_b64(lora_content),
        metadata={
            "modelspec.architecture": "flux-1-dev/lora",
            "modelspec.title": "XeRetroFluxV1",
            "ss_sd_model_name": "xeRetroAnimeFlux_01.safetensors",
            "autov2": lora_hex[:10],
        }
    )

    # 3. Anima Turbo model
    f3 = File(
        id="anima-pencil-xl.safetensors",
        name="anima-pencil-xl",
        source=FileSource.local,
        format=FileFormat.checkpoint,
        hash=_sha256_b64("anima-pencil-weights"),
        metadata={"modelspec.title": "Anima Pencil XL"}
    )

    col.extend([f1, f2, f3])
    return col


def test_normalize_model_query():
    assert _normalize_model_query("my_model") == "my_model"
    assert _normalize_model_query("my_model:0.8") == "my_model"
    assert _normalize_model_query("my_model:0.8:0.8") == "my_model"
    assert _normalize_model_query("<lora:retro_style:0.75>") == "retro_style"
    assert _normalize_model_query("  <lora:sub/folder/model:1.0>  ") == "sub/folder/model"


def test_exact_id_match(collection):
    f = collection.find(r"Illustrious\illustriousRealismBy_v10VAE.safetensors")
    assert f is not None
    assert f.name == "illustriousRealismBy_v10VAE"


def test_normalized_path_match(collection):
    # Forward slash vs backslash match
    f = collection.find("Illustrious/illustriousRealismBy_v10VAE.safetensors")
    assert f is not None
    assert f.name == "illustriousRealismBy_v10VAE"


def test_filename_match_without_subfolder(collection):
    # Requesting without the "Illustrious" folder
    f = collection.find("illustriousRealismBy_v10VAE.safetensors")
    assert f is not None
    assert f.name == "illustriousRealismBy_v10VAE"


def test_stem_match_without_extension(collection):
    # Requesting pure stem
    f = collection.find("illustriousRealismBy_v10VAE")
    assert f is not None
    assert f.name == "illustriousRealismBy_v10VAE"


def test_lora_tag_syntax_lookup(collection):
    # Searching using prompt syntax: <lora:xeRetroAnimeFlux_01:0.8>
    f = collection.find("<lora:xeRetroAnimeFlux_01:0.8>")
    assert f is not None
    assert f.id == r"Flux.1 D\xeRetroAnimeFlux_01.safetensors"

    # Searching with colon weight: xeRetroAnimeFlux_01:0.65
    f2 = collection.find("xeRetroAnimeFlux_01:0.65")
    assert f2 is not None
    assert f2.id == r"Flux.1 D\xeRetroAnimeFlux_01.safetensors"


def test_hash_lookup_sha256_hex(collection):
    lora_hex = _sha256_hex("retro-anime-flux-lora-weights")
    f = collection.find(lora_hex)
    assert f is not None
    assert f.name == "xeRetroAnimeFlux_01"


def test_hash_lookup_autov2_short_prefix(collection):
    lora_hex = _sha256_hex("retro-anime-flux-lora-weights")
    short_hash = lora_hex[:10]  # 10 chars AutoV2
    f = collection.find(short_hash)
    assert f is not None
    assert f.name == "xeRetroAnimeFlux_01"


def test_metadata_title_lookup(collection):
    # Renamed file or title from modelspec
    f = collection.find("XeRetroFluxV1")
    assert f is not None
    assert f.name == "xeRetroAnimeFlux_01"

    f2 = collection.find("Illustrious Realism V10")
    assert f2 is not None
    assert f2.name == "illustriousRealismBy_v10VAE"


def test_find_local_only(collection):
    # f1 is local | remote
    assert collection.find_local("illustriousRealismBy_v10VAE") is not None
    # non-existent
    assert collection.find_local("does_not_exist") is None
