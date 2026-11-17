#!/usr/bin/env python3
"""Post-merge fork-drop audit for the Solstice-1.1.1 selective rebase.

Hazard (found in lane F): `git merge-file current base=HEAD other` treats
lines that exist in HEAD but not in the snapshot as "upstream deleted
them" and removes them from the merged file WITHOUT a conflict marker.
Those are actually committed fork-only additions upstream never had.

For every file in the merge manifests this computes hunks where HEAD had
content the merged file lost, and classifies each dropped hunk:
  LEGIT     - at least one dropped line exists in the snapshot file
              (upstream really changed that region; drop is intentional)
  SUSPECT   - none of the dropped lines exist upstream (upstream never
              had them) -> fork-only work silently eaten; restore needed.
  MARKERS   - file still contains <<<<<<< conflict markers (unresolved).

Usage:
  python build-tools/rebase/audit.py            # all lanes
  python build-tools/rebase/audit.py --lane c   # one lane
Writes build-tools/rebase/reports/audit[-lane].json + prints a summary.
"""
import difflib
import json
import os
import subprocess
import sys

REPO = os.environ.get("REPO_ROOT") or os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", ".."))
SNAP = os.environ.get(
    "SNAPSHOT_ROOT",
    r"C:\Users\richk\Downloads\Solstice-1.1.1\Solstice-1.1.1")
LANES = os.path.join(REPO, "build-tools", "rebase", "lanes")
REPORTS = os.path.join(REPO, "build-tools", "rebase", "reports")


def norm(t: str) -> list:
    return t.replace("\r\n", "\n").replace("\r", "\n").splitlines()


def read_rel(root: str, rel: str) -> list:
    p = os.path.join(root, rel)
    try:
        return norm(open(p, encoding="utf-8", errors="replace").read())
    except OSError:
        return []


def head_lines(rel: str) -> list:
    out = subprocess.run(["git", "show", f"HEAD:{rel}"], cwd=REPO,
                         capture_output=True).stdout
    return norm(out.decode("utf-8", "replace"))


def dropped_hunks(base: list, merged: list):
    """HEAD line-ranges absent from merged (via difflib opcodes)."""
    sm = difflib.SequenceMatcher(None, base, merged, autojunk=False)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag in ("delete", "replace") and i2 > i1:
            yield base[i1:i2]


def audit(rel: str) -> dict:
    wt = read_rel(REPO, rel)
    if not wt:
        return {"file": rel, "error": "worktree file missing"}
    if any(l.startswith("<<<<<<<") for l in wt):
        return {"file": rel, "status": "MARKERS"}
    base, merged, snap = head_lines(rel), wt, read_rel(SNAP, rel)
    snap_set = set(snap)
    suspects = []
    for hunk in dropped_hunks(base, merged):
        if not any(l.strip() in snap_set for l in hunk if l.strip()):
            suspects.append(hunk)
    return {"file": rel,
            "status": "SUSPECT" if suspects else "OK",
            "suspect_hunks": suspects,
            "suspect_lines": sum(len(h) for h in suspects)}


def main():
    lane = None
    if "--lane" in sys.argv:
        lane = sys.argv[sys.argv.index("--lane") + 1]
    files = []
    names = [f"lane-{lane}.json"] if lane else sorted(os.listdir(LANES))
    for n in names:
        d = json.load(open(os.path.join(LANES, n), encoding="utf-8"))
        files += [p for p, _, _ in d["merge"]]

    results = [audit(rel) for rel in files]
    sus = [r for r in results if r.get("status") == "SUSPECT"]
    mark = [r for r in results if r.get("status") == "MARKERS"]
    err = [r for r in results if "error" in r]
    os.makedirs(REPORTS, exist_ok=True)
    rp = os.path.join(REPORTS, f"audit-{lane or 'all'}.json")
    json.dump(results, open(rp, "w", encoding="utf-8"), indent=1)
    print(json.dumps({
        "audited": len(results), "suspect_files": len(sus),
        "files_with_markers": len(mark), "errors": len(err),
        "report": rp,
        "suspects": [{"file": r["file"],
                      "suspect_lines": r["suspect_lines"]} for r in sus],
        "markers": [r["file"] for r in mark]}, indent=1))


if __name__ == "__main__":
    main()
