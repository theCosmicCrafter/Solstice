"""Read safetensors model headers (modelspec metadata) without loading weights.

Safetensors files start with an 8-byte little-endian uint64 header length N,
followed by N bytes of UTF-8 JSON describing the tensors, followed by the tensor
data. Only the header is read here, which keeps this cheap even for multi-GB
checkpoints. This module is intentionally free of plugin imports so it can be
unit-tested without Krita/ComfyUI.
"""

from __future__ import annotations

import json
import struct
from pathlib import Path
from typing import Any

# Largest safetensors header we are willing to read. Real headers are a few KB;
# anything larger is almost certainly a corrupt or malicious file.
max_header_size = 32 * 1024 * 1024  # 32 MiB

# Upper bound for extracted trigger words (ss_tag_frequency can list hundreds).
max_trigger_words = 50

# Metadata keys that can hold an architecture identifier, in priority order.
# The modelspec.* keys follow the CivitAI modelspec convention, ss_* keys are
# written by Kohya-style trainers (used for most CivitAI-trained LoRAs).
arch_keys = (
    "modelspec.architecture",
    "modelspec.base_model",
    "modelspec.title",
    "ss_sd_model_name",
    "ss_base_model_version",
)


def read_safetensors_metadata(path: Path | str) -> dict:
    """Return the ``__metadata__`` dict of a safetensors file.

    Reads only the 8-byte length prefix and the JSON header, never the tensor
    data. Never raises: missing/unreadable files, truncated headers, absurd
    header sizes and malformed JSON all result in an empty dict.
    """
    try:
        with open(path, "rb") as f:
            length_bytes = f.read(8)
            if len(length_bytes) != 8:
                return {}
            (header_size,) = struct.unpack("<Q", length_bytes)
            if header_size <= 0 or header_size > max_header_size:
                return {}
            header_bytes = f.read(header_size)
            if len(header_bytes) != header_size:
                return {}
        header = json.loads(header_bytes)
        if not isinstance(header, dict):
            return {}
        metadata = header.get("__metadata__")
        return metadata if isinstance(metadata, dict) else {}
    except Exception:
        return {}


def arch_strings_from_metadata(meta: dict) -> list[str]:
    """Return all architecture identifier candidates in priority order."""
    result = []
    for key in arch_keys:
        value = meta.get(key)
        if isinstance(value, str) and value.strip():
            result.append(value.strip())
    return result


def arch_from_metadata(meta: dict) -> str | None:
    """Return the best architecture identifier found in the metadata."""
    return next(iter(arch_strings_from_metadata(meta)), None)


def trigger_words_from_metadata(meta: dict) -> list[str]:
    """Extract trigger words (modelspec.trigger_phrase or ss_tag_frequency).

    ``ss_tag_frequency`` is a JSON-encoded ``{dataset: {tag: count}}`` mapping
    written by Kohya-style trainers; tags are sorted by frequency so the most
    relevant words come first. The result is deduplicated and capped at
    ``max_trigger_words``.
    """
    words: list[str] = []

    phrase = meta.get("modelspec.trigger_phrase")
    if isinstance(phrase, str):
        words.extend(w.strip() for w in phrase.split(",") if w.strip())
    elif isinstance(phrase, list):
        words.extend(w.strip() for w in phrase if isinstance(w, str) and w.strip())

    tag_frequency = meta.get("ss_tag_frequency")
    if isinstance(tag_frequency, str):
        try:
            tag_frequency = json.loads(tag_frequency)
        except ValueError:
            tag_frequency = None
    if isinstance(tag_frequency, dict):
        counts: dict[str, float] = {}
        for dataset_tags in tag_frequency.values():
            if isinstance(dataset_tags, dict):
                for tag, count in dataset_tags.items():
                    if isinstance(tag, str) and tag.strip():
                        counts[tag] = max(counts.get(tag, 0.0), _as_number(count))
        ranked = sorted(counts.items(), key=lambda item: item[1], reverse=True)
        words.extend(tag.strip() for tag, _ in ranked)

    result: list[str] = []
    seen: set[str] = set()
    for word in words:
        if word not in seen:
            seen.add(word)
            result.append(word)
            if len(result) >= max_trigger_words:
                break
    return result


def extract_metadata_fields(meta: dict) -> dict[str, Any]:
    """Extract the interesting subset of a ``__metadata__`` dict for caching.

    Returns architecture-related fields under their original key names (so the
    result can be fed back into ``arch_strings_from_metadata``) plus any trigger
    words under the ``trigger_words`` key.
    """
    result: dict[str, Any] = {}
    for key in arch_keys:
        value = meta.get(key)
        if isinstance(value, str) and value.strip():
            result[key] = value.strip()
    if triggers := trigger_words_from_metadata(meta):
        result["trigger_words"] = triggers
    return result


def _as_number(value: Any) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return 0.0
