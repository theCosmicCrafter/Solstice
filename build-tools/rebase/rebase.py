#!/usr/bin/env python3
"""Per-file 3-way merge helper for the Solstice-1.1.1 selective rebase.

No git merge base exists for the 1.1.1 snapshot, so we derive one per file:
  base     = `git show HEAD:<path>`      (fork's last committed state, LF)
  current  = working-tree file           (fork changes incl. uncommitted)
  other    = snapshot file               (upstream 1.1.1 evolution)

`git merge-file current base other` applies upstream deltas onto the
working file; hunks where both sides changed the same lines get conflict
markers. All three inputs are EOL-normalized to LF before merging (the
snapshot is LF, git blobs are LF, the checkout is CRLF via autocrlf) and
the merged result is written back as LF -- git's clean filter keeps
`git diff` clean of EOL noise.

Usage:
  python build-tools/rebase/rebase.py copy  --lane a   # copy lane manifests
  python rebase.py merge --lane b                      # merge a whole lane
  python rebase.py merge libs/image/kis_layer.cc       # merge one file
  python rebase.py scan  --lane b                      # files with markers left
  python rebase.py scan                                # scan the whole tree set
"""
import json
import os
import subprocess
import sys
import tempfile

REPO = os.environ.get("REPO_ROOT") or os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", ".."))
SNAP = os.environ.get(
    "SNAPSHOT_ROOT",
    r"C:\Users\richk\Downloads\Solstice-1.1.1\Solstice-1.1.1")
LANES = os.path.join(REPO, "build-tools", "rebase", "lanes")
REPORTS = os.path.join(REPO, "build-tools", "rebase", "reports")


def norm(b: bytes) -> bytes:
    return b.replace(b"\r\n", b"\n").replace(b"\r", b"\n")


def load_lane(lane: str) -> dict:
    return json.load(open(os.path.join(LANES, f"lane-{lane}.json"),
                         encoding="utf-8"))


def all_files():
    out = {"copy": [], "merge": []}
    for name in sorted(os.listdir(LANES)):
        if name.startswith("lane-"):
            d = json.load(open(os.path.join(LANES, name), encoding="utf-8"))
            out["copy"] += d["copy"]
            out["merge"] += [p for p, _, _ in d["merge"]]
    return out


def do_copy(rel: str) -> dict:
    src = os.path.join(SNAP, rel)
    dst = os.path.join(REPO, rel)
    if not os.path.exists(src):
        return {"file": rel, "error": "snapshot file missing"}
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    data = open(src, "rb").read()
    open(dst, "wb").write(data)
    return {"file": rel, "copied": len(data)}


def do_merge(rel: str) -> dict:
    wt = os.path.join(REPO, rel)
    up = os.path.join(SNAP, rel)
    if not os.path.exists(wt):
        return {"file": rel, "error": "worktree file missing"}
    if not os.path.exists(up):
        return {"file": rel, "error": "snapshot file missing"}

    tracked = subprocess.run(
        ["git", "ls-files", "--error-unmatch", "--", rel],
        cwd=REPO, capture_output=True).returncode == 0
    base = subprocess.run(
        ["git", "show", f"HEAD:{rel}"], cwd=REPO,
        capture_output=True).stdout if tracked else b""

    with tempfile.TemporaryDirectory() as td:
        for name, data in (("cur", norm(open(wt, "rb").read())),
                           ("base", base), ("oth", norm(open(up, "rb").read()))):
            open(os.path.join(td, name), "wb").write(data)
        cur = os.path.join(td, "cur")
        proc = subprocess.run(
            ["git", "merge-file", "-p", cur,
             os.path.join(td, "base"), os.path.join(td, "oth")],
            capture_output=True)
    if proc.returncode < 0:
        return {"file": rel, "error": f"merge-file failed rc={proc.returncode}",
                "stderr": proc.stderr.decode("utf-8", "replace")[:300]}

    merged = proc.stdout
    conflicts = merged.count(b"<<<<<<<")
    open(wt, "wb").write(merged)
    return {"file": rel, "conflicts": conflicts, "tracked": tracked,
            "bytes": len(merged)}


def has_markers(rel: str) -> bool:
    p = os.path.join(REPO, rel)
    try:
        return b"<<<<<<<" in open(p, "rb").read()
    except OSError:
        return False


def main():
    op, lane, targets = None, None, []
    args = sys.argv[1:]
    op = args[0] if args else ""
    if "--lane" in args:
        lane = args[args.index("--lane") + 1]
    targets = [a for a in args[1:] if a != "--lane" and a != lane]

    if op == "copy":
        files = targets or (load_lane(lane)["copy"] if lane else
                            all_files()["copy"])
        results = [do_copy(rel) for rel in files]
    elif op == "merge":
        files = targets or ([p for p, _, _ in load_lane(lane)["merge"]]
                            if lane else all_files()["merge"])
        results = [do_merge(rel) for rel in files]
    elif op == "scan":
        files = targets or ([p for p, _, _ in load_lane(lane)["merge"]]
                            if lane else all_files()["merge"])
        open_m = [f for f in files if has_markers(f)]
        results = [{"files_with_markers": open_m, "count": len(open_m)}]
    else:
        print(__doc__)
        return 2

    out = {"op": op, "lane": lane, "results": results}
    if lane and op in ("copy", "merge"):
        os.makedirs(REPORTS, exist_ok=True)
        rp = os.path.join(REPORTS, f"lane-{lane}-{op}.json")
        json.dump(out, open(rp, "w", encoding="utf-8"), indent=1)
        out["report"] = rp
    n_conf = sum(r.get("conflicts", 0) for r in results)
    n_err = sum(1 for r in results if "error" in r)
    print(json.dumps({"files": len(results), "conflict_markers": n_conf,
                      "errors": n_err,
                      "report": out.get("report")}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
