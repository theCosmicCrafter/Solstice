---
type: decision
updated: 2026-10-14
sources:
  - AGENTS.md ("Branch policy")
  - docs/agent/upstream-intake.md
  - \\192.168.1.37\share\upstream-krita-check (README.md, hermes/SKILL.md, reports/2026-W42.md)
related:
  - decisions/versioning.md
---

# Upstream Krita: weekly review, selective cherry-picks

**Decision (user, 2026-10-14).** Upstream merges stay discontinued, but
upstream Krita is reviewed every week. Hermes Agent on the DGX Spark writes a
report with a recommendation per item; the user decides; Claude cherry-picks
the approved items and records each decision in the ledger of
`docs/agent/upstream-intake.md`.

**Why.** `main` has an independent history (no common commit with
upstream) and diverges heavily (GPU engine, brush option model, Android and
Popup Palette removal, branding), so merges are no longer practical. Upstream
still fixes crashes and file-format issues that Solstice shares.

**How to apply.**
- Never add a persistent `upstream` remote. Do not fetch from the share
  mirror either: it is shallow, and a fetch from a shallow repository makes
  the receiver shallow. Apply patches with the share's `scripts/intake.sh`
  (`git am -3 --keep-non-patch` after importing the preimage blobs; plain
  `git am` drops subject prefixes such as `[psd]`). First used for
  2026-W42.
- Dry runs must test a commit both alone and after the earlier candidates of
  its branch: in the first run (2026-W42) ten commits conflicted alone but
  applied cleanly in series.
- A dry-run 3-way apply writes merged blobs into the object store even with
  `--cached`; `collect.py` redirects writes with `GIT_OBJECT_DIRECTORY` and
  reads both repositories through alternates.
- The share is owned by uid 1000, so Git on Windows needs
  `safe.directory`; pass it per command, not in the global configuration.
