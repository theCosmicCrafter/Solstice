"""Tests for backend/sweep.py (prompt-embedded parameter-sweep specs).

sweep.py is pure stdlib and is loaded directly by path so these tests run
without Krita, ComfyUI or PyQt6.
"""

import importlib.util
from pathlib import Path

_module_path = Path(__file__).parent.parent / "backend" / "sweep.py"
_spec = importlib.util.spec_from_file_location("sweep", _module_path)
sweep = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sweep)


def test_no_spec_passes_prompt_through():
    assert sweep.parse_sweep_spec("a cat on a hill") == (None, [], "a cat on a hill")


def test_steps_spec_parses_and_strips():
    axis, values, prompt = sweep.parse_sweep_spec("a cat {{steps: 4, 6, 8}}")
    assert axis == "steps"
    assert values == [4, 6, 8]
    assert prompt == "a cat"


def test_cfg_spec_keeps_floats():
    axis, values, prompt = sweep.parse_sweep_spec("{{cfg:1.0, 2.5}} a cat")
    assert axis == "cfg"
    assert values == [1.0, 2.5]
    assert prompt == "a cat"


def test_case_insensitive_axis():
    axis, values, _ = sweep.parse_sweep_spec("x {{STEPS:4,8}}")
    assert axis == "steps"
    assert values == [4, 8]


def test_single_value_is_not_a_sweep():
    axis, values, prompt = sweep.parse_sweep_spec("x {{steps: 8}}")
    assert axis is None and values == [] and prompt == "x {{steps: 8}}"


def test_values_are_clamped():
    axis, values, _ = sweep.parse_sweep_spec("{{steps: 0, -3, 4.6}}")
    assert values == [1, 1, 5]
    axis, values, _ = sweep.parse_sweep_spec("{{cfg: -1, 0.5}}")
    assert values == [0.0, 0.5]


def test_values_capped_at_max():
    axis, values, _ = sweep.parse_sweep_spec("{{steps:1,2,3,4,5,6,7,8,9,10}}")
    assert values == [1, 2, 3, 4, 5, 6, 7, 8]


def test_invalid_tokens_are_skipped():
    axis, values, _ = sweep.parse_sweep_spec("{{steps: 4, nope, 8}}")
    assert values == [4, 8]


def test_non_sweep_double_braces_untouched():
    axis, values, prompt = sweep.parse_sweep_spec("{{other:1,2}}")
    assert axis is None and prompt == "{{other:1,2}}"
