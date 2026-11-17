#!/usr/bin/env python3
"""Docs integrity check for the Solstice repo.

Two checks, both report-only then a single exit code:

1. Every ``docs/...md`` path referenced by AGENTS.md (backtick-quoted or
   Markdown link) exists on disk. Placeholder paths containing ``<...>``
   are skipped.
2. Every relative Markdown link inside docs/**/*.md resolves to an
   existing file. External (http/https/mailto/file), anchor-only and
   placeholder links are skipped.

Exit 0 when clean, 1 when any broken reference is found. Run from the
repository root or pass the root as argv[1]. Used by ci-checks.yml and
runnable locally before committing docs.
"""

import os
import re
import sys
from urllib.parse import unquote

RE_DOC_PATH = re.compile(r"docs/(?:agent/)?[A-Za-z0-9_\-/]+\.md")
RE_MD_LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")
SKIP_SCHEMES = ("http://", "https://", "mailto:", "file:///", "file://")


def is_placeholder(path: str) -> bool:
    return "<" in path or ">" in path


def check_agents_refs(root: str) -> list[str]:
    agents = os.path.join(root, "AGENTS.md")
    broken = []
    if not os.path.isfile(agents):
        return ["AGENTS.md not found at repository root"]
    with open(agents, encoding="utf-8") as fh:
        text = fh.read()
    seen = set()
    for m in RE_DOC_PATH.finditer(text):
        rel = m.group(0)
        if is_placeholder(rel) or rel in seen:
            continue
        seen.add(rel)
        if not os.path.isfile(os.path.join(root, rel)):
            broken.append(f"AGENTS.md references missing file: {rel}")
    return broken


def check_markdown_links(root: str) -> list[str]:
    docs_root = os.path.join(root, "docs")
    broken = []
    for dirpath, _dirs, files in os.walk(docs_root):
        for name in files:
            if not name.endswith(".md"):
                continue
            path = os.path.join(dirpath, name)
            with open(path, encoding="utf-8") as fh:
                text = fh.read()
            rel_src = os.path.relpath(path, root)
            for m in RE_MD_LINK.finditer(text):
                target = unquote(m.group(1))
                if target.startswith(SKIP_SCHEMES) or target.startswith("#"):
                    continue
                target = target.split("#", 1)[0]
                if not target or is_placeholder(target):
                    continue
                resolved = os.path.normpath(os.path.join(dirpath, target))
                if not os.path.exists(resolved):
                    broken.append(f"{rel_src}: broken link -> {m.group(1)}")
    return broken


def main() -> int:
    root = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
    broken = check_agents_refs(root) + check_markdown_links(root)
    if broken:
        print(f"docs check FAILED: {len(broken)} broken reference(s)")
        for line in broken:
            print(f"  {line}")
        return 1
    print("docs check passed: AGENTS.md references and docs/ links resolve")
    return 0


if __name__ == "__main__":
    sys.exit(main())
