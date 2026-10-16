"""Prompt-embedded parameter-sweep specs ({{axis:v1,v2,...}}).

A sweep spec in the text prompt expands one enqueue into one job per value,
overriding a single sampling field — the lightweight counterpart of an XY
plot for preset/schedule QA. This module is pure stdlib so it is unit-testable
without Krita/ComfyUI/PyQt6.
"""

from __future__ import annotations

import re

pattern_sweep = re.compile(r"\{\{\s*(steps|cfg)\s*:\s*([^}]*)\}\}", re.IGNORECASE)

# Upper bound on jobs a single spec can enqueue.
max_sweep_values = 8


def parse_sweep_spec(prompt: str):
    """Extract an optional {{axis:v1,v2,...}} sweep spec from a prompt.

    axis is "steps" or "cfg"; values are numbers. Returns
    (axis, values, prompt_without_spec), or (None, [], prompt) when no usable
    spec is present (no match, or fewer than two valid values). Values are
    clamped to sane ranges (steps >= 1, cfg >= 0) and capped at
    max_sweep_values.
    """
    match = pattern_sweep.search(prompt)
    if not match:
        return None, [], prompt
    axis = match[1].lower()
    values = []
    for token in match[2].split(","):
        token = token.strip()
        if not token:
            continue
        try:
            value = float(token)
        except ValueError:
            continue
        if axis == "steps":
            values.append(max(1, int(round(value))))
        else:
            values.append(max(0.0, value))
    values = values[:max_sweep_values]
    if len(values) < 2:
        return None, [], prompt
    cleaned = (prompt[: match.start()] + prompt[match.end() :]).strip()
    return axis, values, cleaned
