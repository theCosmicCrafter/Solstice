#!/usr/bin/env python3
#
#  SPDX-License-Identifier: GPL-3.0-or-later
#
"""Replay the paint trace baseline stroke set with synthetic input.

Starts a fresh Solstice per run through test control (as run_scenarios.py
--launch does), with KRITA_PAINT_TRACE and the KRITA_GPU_* switches of the
requested mode, plays the 16 strokes of the controlled capture protocol
(docs/agent/paint-trace-baseline-runs.md): 64 px Build-up, 64 px Wash, 256 px
Build-up, 256 px Wash, each one warm-up and three measured strokes, on an
RGBA F32 2480x3508 document with "b) Basic-4 Flow Opacity". It quits Solstice
normally so the trace is written, summarizes the trace with
build-tools/paint-trace/summarize.py, checks it, and aggregates the runs the
way the baseline did: per process and condition, the median of the three
measured stroke medians; per mode and condition, the median and range of the
process values. The same for stroke p95 values.

The strokes are identical in every run, which hand drawing is not; they are a
separate series and must not be pooled with hand-drawn runs. Synthetic events
enter above the platform layer, so the times start at Qt's input receipt as
in the hand series but no tablet driver is involved.

Every run also times the strokes without the trace: test control records
when the canvas presents frames (canvas_frames) on the clock of the
injected press and release, and the process's CPU time while the strokes
play. With --overhead, each mode runs once with the trace and once without
it per round (off/on, on/off, off/on), and those trace-independent values
are compared: they show what the trace itself costs.

Example:
  python dev-tools/test-control/paint_trace_series.py --launch _install/bin/krita --out series-dir
  python dev-tools/test-control/paint_trace_series.py --launch _install/bin/krita --out overhead-dir --overhead
"""

import argparse
import hashlib
import json
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_scenarios  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
SUMMARIZE = REPO / "build-tools" / "paint-trace" / "summarize.py"
SERIES_FORMAT = "solstice-paint-trace-synthetic-series"

MODES = {
    "cpu": {"KRITA_GPU_PROJECTION": "0", "KRITA_GPU_BRUSH": "0"},
    "projection": {"KRITA_GPU_PROJECTION": "1", "KRITA_GPU_BRUSH": "0"},
    "brush": {"KRITA_GPU_PROJECTION": "1", "KRITA_GPU_BRUSH": "1"},
}
# The baseline's rotation: each round starts with the next mode.
ROUNDS = [["cpu", "projection", "brush"], ["projection", "brush", "cpu"], ["brush", "cpu", "projection"]]
CONDITIONS = [(64, "buildup"), (64, "wash"), (256, "buildup"), (256, "wash")]
PRESET = "b) Basic-4 Flow Opacity"
DOCUMENT = {"width": 2480, "height": 3508, "colorDepth": "F32"}
STROKE_POINTS = 25
POINT_INTERVAL_MS = 12
PRESSURE = 1.0
PAUSE_S = 0.5
# A stroke's frames are complete when the canvas has presented none for this
# long before they are read.
FRAME_QUIET_MS = 200
# The trace states of one mode in each --overhead round.
OVERHEAD_ORDER = [(False, True), (True, False), (False, True)]
# The binaries the baseline sheet hashes, plus the test control plugin, which
# plays the input.
ARTIFACTS = ["bin/krita", "lib/x86_64-linux-gnu/libkritaimage.so", "lib/x86_64-linux-gnu/libkritaui.so",
             "lib/x86_64-linux-gnu/libkritagpu.so", "lib/x86_64-linux-gnu/kritaplugins/kritadefaultpaintops.so",
             "lib/x86_64-linux-gnu/kritaplugins/kritatestcontrol.so",
             "bin/krita.exe", "bin/libkritaimage.dll", "bin/libkritaui.dll", "bin/libkritagpu.dll",
             "lib/kritaplugins/kritadefaultpaintops.dll", "lib/kritaplugins/kritatestcontrol.dll"]


class SeriesError(Exception):
    pass


def layout(view):
    """Stroke positions in image pixels inside the visible part of the image.

    One row per condition, four strokes per row, far enough apart that a
    256 px dab of one stroke does not touch another stroke.
    """
    image = view["imageRectInWidget"]
    canvas_width, canvas_height = view["canvasWidth"], view["canvasHeight"]
    left = max(0, -image["x"])
    top = max(0, -image["y"])
    right = min(image["width"], canvas_width - image["x"])
    bottom = min(image["height"], canvas_height - image["y"])
    width, height = right - left, bottom - top
    margin, gap = 20, 40
    length = min(180, (width - 2 * margin - 4 * 256 - 3 * gap) // 4)
    if length < 60 or height < 2 * margin + 64 + 64 + 256 + 256 + 3 * gap:
        raise SeriesError(f"The visible image area {width}x{height} is too small for the stroke set")
    rows, y = [], top + margin
    for size, _ in CONDITIONS:
        rows.append(y + size // 2)
        y += size + gap
    strokes = []
    for (size, mode), row in zip(CONDITIONS, rows):
        for index in range(4):
            x0 = left + margin + 128 + index * (length + 256 + gap)
            points = [{"x": x0 + length * i / (STROKE_POINTS - 1), "y": row,
                       "pressure": PRESSURE, "t": i * POINT_INTERVAL_MS} for i in range(STROKE_POINTS)]
            strokes.append({"size": size, "paintMode": mode, "warmUp": index == 0, "points": points})
    return strokes


def artifact_hashes(install):
    hashes = {}
    for relative in ARTIFACTS:
        path = install / relative
        if path.exists():
            hashes[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
    return hashes


def source_state():
    def git(*arguments):
        return subprocess.run(["git", "-C", str(REPO), *arguments], capture_output=True, text=True).stdout.strip()
    return {"head": git("rev-parse", "HEAD"), "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
            "dirtyFiles": len(git("status", "--porcelain").splitlines())}


def frame_metrics(frames, pressed, released, now):
    """Trace-independent timing of one stroke from the canvas frame times.

    releaseToLastFrameMs: from the release to the last frame presented after
    it (the stroke's final pixels); frames and frameIntervalMs: the frames
    presented while the pen was down and the median time between them.
    """
    during = [frame for frame in frames if pressed <= frame <= released]
    after = [frame for frame in frames if frame > released]
    intervals = [b - a for a, b in zip(during, during[1:])]
    last = (after or during or [None])[-1]
    return {
        "releaseToLastFrameMs": round(after[-1] - released, 3) if after else None,
        "frames": len(during),
        "frameIntervalMs": round(statistics.median(intervals), 3) if intervals else None,
        "settled": last is not None and now - last >= FRAME_QUIET_MS,
    }


def cpu_ms(process):
    return process["cpuUserMs"] + process["cpuSystemMs"]


def play(client, strokes):
    client.call("document_close", {"all": True})
    client.call("document_new", DOCUMENT)
    client.call("tool_activate", {"id": "KritaShape/KisToolBrush"})
    client.call("brush_set", {"preset": PRESET, "opacity": 1, "color": "#000000"})
    _, recording, _ = client.call("canvas_frames", {"action": "start"})
    _, before, _ = client.call("session_info", {})
    played = []
    for stroke in strokes:
        client.call("brush_set", {"size": stroke["size"], "paintMode": stroke["paintMode"]})
        _, result, _ = client.call("input_stroke", {"points": stroke["points"], "device": "tablet",
                                                    "coordinates": "image", "realtime": True})
        if result.get("pointsOutsideCanvas"):
            raise SeriesError("A stroke left the canvas widget")
        client.call("wait_idle", {"timeoutMs": 30000})
        time.sleep(PAUSE_S)
        pressed, released = result.get("pressedAtMs"), result.get("releasedAtMs")
        if pressed is None or released is None:
            raise SeriesError("The stroke reported no press or release time")
        _, frames, _ = client.call("canvas_frames", {"sinceMs": pressed})
        if frames["overflowed"] or not frames["recording"]:
            raise SeriesError("The canvas frame record overflowed or stopped")
        played.append({"eventsSent": result.get("eventsSent"), "durationMs": result.get("durationMs"),
                       "heldMs": round(released - pressed, 3),
                       **frame_metrics(frames["frames"], pressed, released, frames["nowMs"])})
    _, after, _ = client.call("session_info", {})
    client.call("canvas_frames", {"action": "stop"})
    _, state, _ = client.call("query_state", {})
    if state["undo"]["count"] != len(strokes):
        raise SeriesError(f"Expected {len(strokes)} undo steps, found {state['undo']['count']}")
    process = {"frameKind": recording["kind"],
               "cpuMs": round(cpu_ms(after["process"]) - cpu_ms(before["process"]), 1),
               "wallMs": round(after["process"]["clockMs"] - before["process"]["clockMs"], 1)}
    return played, process


def measure_view(client):
    client.call("document_close", {"all": True})
    client.call("document_new", DOCUMENT)
    _, view, _ = client.call("view_reset", {"width": 1920, "height": 1080})
    _, session, _ = client.call("session_info", {})
    view["canvasWidth"] = session["canvas"]["width"]
    view["canvasHeight"] = session["canvas"]["height"]
    return view, session


def summarize(trace):
    result = subprocess.run([sys.executable, "-B", str(SUMMARIZE), str(trace)], capture_output=True, text=True)
    if result.returncode != 0:
        raise SeriesError(f"summarize.py failed: {result.stderr.strip()}")
    return json.loads(result.stdout)[0]


def check_and_select(summary, strokes):
    """Checks the trace against the played strokes; returns per-condition measured strokes."""
    problems = []
    if summary["metadata"].get("dropped_events", 0):
        problems.append("dropped events")
    timed = summary["command_presentation_timing"]["strokes"]
    if len(timed) != len(strokes):
        problems.append(f"{len(timed)} strokes in the trace, {len(strokes)} played")
    selected = {}
    for played, record in zip(strokes, timed):
        conditions = record["conditions"] or {}
        expected = (played["size"], played["paintMode"] == "buildup")
        actual = (round(conditions.get("nominal_size_px", -1)), conditions.get("incremental"))
        if actual != expected or conditions.get("preset_name") != PRESET:
            problems.append(f"stroke {record['stroke']}: conditions {conditions} do not match {expected}")
        if not played["warmUp"]:
            key = condition_key(played["size"], played["paintMode"])
            selected.setdefault(key, []).append(record)
    return selected, problems


def paths_summary(summary):
    """Execution path records, summed over verified inputs (shared work counts once per input)."""
    totals = {}
    evidence = summary.get("execution_path_evidence") or {}
    for row in evidence.get("inputs", []):
        for stage in row.get("stages", {}).values():
            for name, count in stage.get("events", {}).items():
                totals[name] = totals.get(name, 0) + count
    return dict(sorted(totals.items()))


def run_once(args, mode, index, out, trace=True):
    run_directory = out / f"run-{index:02d}-{mode}{'' if trace else '-notrace'}"
    run_directory.mkdir(parents=True, exist_ok=False)
    environment = dict(MODES[mode], KRITA_GPU_VALIDATION="0")
    if trace:
        environment["KRITA_PAINT_TRACE"] = str(run_directory / "trace")
    launched = run_scenarios.Launched(args.launch, args.port, "en_US", environment)
    client = run_scenarios.Client(f"http://127.0.0.1:{args.port}/mcp", launched.token)
    record = {"run": index, "mode": mode, "traced": trace, "environment": environment}
    try:
        launched.wait_ready(client, args.launch_timeout)
        view, session = measure_view(client)
        strokes = layout(view)
        record["view"] = view
        record["session"] = {key: session.get(key) for key in ("solsticeVersion", "qtVersion", "platform",
                                                                 "environment")}
        record["played"], record["process"] = play(client, strokes)
        # The device the GPU engine actually started on (null when it did not).
        _, after, _ = client.call("session_info", {})
        record["gpuEngine"] = after.get("gpuEngine")
    finally:
        log = launched.directory / "solstice.log"
        if log.exists():
            (run_directory / "solstice.log").write_bytes(log.read_bytes())
        launched.quit(run_scenarios.Client(f"http://127.0.0.1:{args.port}/mcp", launched.token, timeout=120))
    record["exitCode"] = launched.process.returncode
    (run_directory / "strokes.json").write_text(json.dumps(strokes, indent=1) + "\n", encoding="utf-8")
    record["problems"] = frame_problems(record["played"])
    record["frameConditions"] = select_frames(record["played"], strokes)
    if not trace:
        return record
    traces = sorted(run_directory.glob("trace.*.json"))
    if len(traces) != 1:
        raise SeriesError(f"Expected one trace in {run_directory}, found {len(traces)}")
    trace_file = traces[0]
    record["trace"] = trace_file.name
    record["traceSha256"] = hashlib.sha256(trace_file.read_bytes()).hexdigest()
    summary = summarize(trace_file)
    (run_directory / "summary.json").write_text(json.dumps(summary, indent=1) + "\n", encoding="utf-8")
    selected, problems = check_and_select(summary, strokes)
    record["problems"] += problems
    record["paths"] = paths_summary(summary)
    timing = summary["command_presentation_timing"]
    record["timedInputs"] = timing["samples"]
    record["excludedInputs"] = timing["excluded_inputs"]
    record["conditions"] = {}
    for key, measured in selected.items():
        medians = [stroke["median_ms"] for stroke in measured]
        p95s = [stroke["p95_ms"] for stroke in measured]
        record["conditions"][key] = {
            "strokes": [stroke["stroke"] for stroke in measured],
            "inputs": [stroke["samples"] for stroke in measured],
            "medianMs": medians, "p95Ms": p95s,
            "processMedianMs": statistics.median(medians) if None not in medians else None,
            "processP95Ms": statistics.median(p95s) if None not in p95s else None,
        }
    return record


def frame_problems(played):
    problems = []
    for index, stroke in enumerate(played):
        if not stroke["settled"]:
            problems.append(f"stroke {index + 1}: the canvas still presented frames when they were read")
        if stroke["releaseToLastFrameMs"] is None:
            problems.append(f"stroke {index + 1}: no frame after the release")
    return problems


def condition_key(size, paint_mode):
    return f"{size}px {'Buildup' if paint_mode == 'buildup' else 'Wash'}"


def select_frames(played, strokes):
    """Per condition, the measured strokes' frame timing and the process values (medians)."""
    selected = {}
    for stroke, timing in zip(strokes, played):
        if not stroke["warmUp"]:
            selected.setdefault(condition_key(stroke["size"], stroke["paintMode"]), []).append(timing)
    result = {}
    for key, measured in selected.items():
        latencies = [t["releaseToLastFrameMs"] for t in measured]
        intervals = [t["frameIntervalMs"] for t in measured]
        result[key] = {
            "releaseToLastFrameMs": latencies, "frameIntervalMs": intervals,
            "processReleaseToLastFrameMs": statistics.median(latencies) if None not in latencies else None,
            "processFrameIntervalMs": statistics.median(intervals) if None not in intervals else None,
        }
    return result


def spread(values):
    return {"processes": len(values), "medianMs": statistics.median(values), "minMs": min(values),
            "maxMs": max(values)}


def aggregate_overhead(runs):
    """Per condition, mode and trace state: the median and range of the process values."""
    table = {"conditions": {}, "cpu": {}}
    for mode in MODES:
        for trace in (False, True):
            state = "on" if trace else "off"
            group = [run for run in runs if run["mode"] == mode and run["traced"] == trace]
            if not group:
                continue
            table["cpu"].setdefault(mode, {})[state] = spread([run["process"]["cpuMs"] for run in group])
            for size, paint_mode in CONDITIONS:
                key = condition_key(size, paint_mode)
                values = [run["frameConditions"].get(key, {}) for run in group]
                latencies = [v["processReleaseToLastFrameMs"] for v in values
                             if v.get("processReleaseToLastFrameMs") is not None]
                intervals = [v["processFrameIntervalMs"] for v in values
                             if v.get("processFrameIntervalMs") is not None]
                if latencies:
                    cell = spread(latencies)
                    cell["frameIntervalMs"] = statistics.median(intervals) if intervals else None
                    table["conditions"].setdefault(key, {}).setdefault(mode, {})[state] = cell
    return table


def print_overhead(table):
    def difference(cell):
        if "off" not in cell or "on" not in cell:
            return ""
        off, on = cell["off"]["medianMs"], cell["on"]["medianMs"]
        return f" ({(on - off) / off * 100:+.1f} %)" if off else ""

    modes = [mode for mode in MODES if mode in table["cpu"]]
    print("Release to last frame, median (range) of process values, ms: trace off / on")
    print("| Condition | " + " | ".join(modes) + " |")
    print("| --- |" + " --- |" * len(modes))
    for key, row in table["conditions"].items():
        cells = []
        for mode in modes:
            cell = row.get(mode, {})
            cells.append(" / ".join(f"{cell[s]['medianMs']:.2f} ({cell[s]['minMs']:.2f}-{cell[s]['maxMs']:.2f})"
                                    for s in ("off", "on") if s in cell) + difference(cell))
        print(f"| {key} | " + " | ".join(cells) + " |")
    print("CPU time while the 16 strokes play, median (range), ms: trace off / on")
    for mode in modes:
        cell = table["cpu"][mode]
        print(f"| {mode} | " + " / ".join(f"{cell[s]['medianMs']:.0f} ({cell[s]['minMs']:.0f}-{cell[s]['maxMs']:.0f})"
                                         for s in ("off", "on") if s in cell) + difference(cell) + " |")


def aggregate(runs):
    table = {}
    for mode in MODES:
        for size, paint_mode in CONDITIONS:
            key = condition_key(size, paint_mode)
            processes = [run["conditions"].get(key) for run in runs if run["mode"] == mode and run.get("traced", True)]
            processes = [p for p in processes if p and p["processMedianMs"] is not None]
            if not processes:
                continue
            medians = [p["processMedianMs"] for p in processes]
            p95s = [p["processP95Ms"] for p in processes]
            table.setdefault(key, {})[mode] = {
                "processes": len(processes),
                "medianMs": statistics.median(medians), "minMs": min(medians), "maxMs": max(medians),
                "p95MedianMs": statistics.median(p95s),
                "inputs": sum(sum(p["inputs"]) for p in processes),
            }
    return table


def print_table(table):
    modes = [mode for mode in MODES if any(mode in row for row in table.values())]
    print("| Condition | " + " | ".join(f"{mode} median (range), ms / p95 / inputs" for mode in modes) + " |")
    print("| --- |" + " --- |" * len(modes))
    for key, row in table.items():
        cells = []
        for mode in modes:
            value = row.get(mode)
            cells.append("-" if not value else
                         f"{value['medianMs']:.2f} ({value['minMs']:.2f}-{value['maxMs']:.2f}) / "
                         f"{value['p95MedianMs']:.2f} / {value['inputs']}")
        print(f"| {key} | " + " | ".join(cells) + " |")


def run_plan(order, overhead):
    """(mode, trace) per process: with overhead, each mode off and on, the order alternating per round."""
    plan = []
    for round_index, round_modes in enumerate(order):
        states = OVERHEAD_ORDER[round_index % len(OVERHEAD_ORDER)] if overhead else (True,)
        plan += [(mode, trace) for mode in round_modes for trace in states]
    return plan


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--launch", required=True, metavar="EXECUTABLE", help="Solstice executable (from _install)")
    parser.add_argument("--out", required=True, type=Path, help="New directory for traces, summaries and series.json")
    parser.add_argument("--rounds", type=int, default=3, help="Rounds of the three modes (default 3)")
    parser.add_argument("--modes", nargs="+", choices=list(MODES),
                        help="Run only these modes, once each in this order (a feasibility pass)")
    parser.add_argument("--overhead", action="store_true",
                        help="Run each mode with and without the trace in every round, to measure its cost")
    parser.add_argument("--port", type=int, default=8767)
    parser.add_argument("--launch-timeout", type=int, default=180)
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_arguments(argv)
    if args.out.exists() and any(args.out.iterdir()):
        print(f"error: {args.out} is not empty; a series is never appended to", file=sys.stderr)
        return 2
    args.out.mkdir(parents=True, exist_ok=True)
    install = Path(args.launch).resolve().parents[1]
    order = [args.modes] if args.modes else ROUNDS[:args.rounds] if args.rounds <= 3 else \
        [ROUNDS[i % 3] for i in range(args.rounds)]
    plan = run_plan(order, args.overhead)
    series = {"format": SERIES_FORMAT, "version": 1, "startedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
              "source": source_state(), "artifacts": artifact_hashes(install),
              "protocol": {"document": DOCUMENT, "preset": PRESET, "conditions": CONDITIONS,
                           "strokePoints": STROKE_POINTS, "pointIntervalMs": POINT_INTERVAL_MS,
                           "pressure": PRESSURE, "pauseSeconds": PAUSE_S, "device": "tablet",
                           "warmUp": "first stroke of each condition", "frameQuietMs": FRAME_QUIET_MS,
                           "overhead": args.overhead},
              "runs": []}
    status = 0
    index = 0
    try:
        for mode, trace in plan:
            index += 1
            print(f"run {index}: {mode}{'' if trace else ', trace off'}", flush=True)
            record = run_once(args, mode, index, args.out, trace)
            series["runs"].append(record)
            for problem in record["problems"]:
                print(f"  problem: {problem}")
                status = 1
            if trace:
                print(f"  {record['timedInputs']} timed inputs, {record['excludedInputs']} excluded, "
                      f"paths {record['paths']}", flush=True)
            print(f"  GPU engine {record['gpuEngine']}, CPU {record['process']['cpuMs']:.0f} ms, "
                  f"frames: {record['process']['frameKind']}", flush=True)
            if artifact_hashes(install) != series["artifacts"]:
                raise SeriesError("Installed binaries changed during the series")
    except (SeriesError, run_scenarios.RunnerError) as error:
        print(f"error: {error}", file=sys.stderr)
        status = 2
    series["finishedAt"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    accepted = [run for run in series["runs"] if not run["problems"]]
    series["aggregate"] = aggregate(accepted)
    series["overhead"] = aggregate_overhead(accepted)
    (args.out / "series.json").write_text(json.dumps(series, indent=1) + "\n", encoding="utf-8")
    if series["aggregate"]:
        print_table(series["aggregate"])
    print_overhead(series["overhead"])
    return status


if __name__ == "__main__":
    sys.exit(main())
