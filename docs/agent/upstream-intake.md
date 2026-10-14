# Upstream intake — agent reference

Solstice does not merge upstream Krita. Selected upstream commits are
cherry-picked one item at a time after the user approves them. This document
is the procedure for that work and the ledger of every decision.

## Roles

| Who | Does |
| --- | --- |
| Hermes Agent (DGX Spark) | Weekly research: collects new upstream commits, dry-runs them against `main`, writes a report with a recommendation per item. Never changes this repository. |
| User | Decides per item: adopt, reject or hold. |
| Claude | Adopts the approved items on `main` and records every decision in the ledger below. |

## Locations

The review lives outside this repository, on the DGX Spark share
`\\192.168.1.37\share\upstream-krita-check`:

- `reports/<YYYY-Www>.md` (Japanese, for the user) and
  `reports/<YYYY-Www>.json` (items, commits, dry-run results, notes for
  Claude);
- `krita.git`: shallow bare mirror of
  `https://invent.kde.org/graphics/krita.git` (`krita/6.0`, `master`);
- `scripts/collect.py`: the deterministic collector;
  `hermes/SKILL.md`: Hermes's instructions; `README.md`: layout.

Items are named `U-<YYYY>W<ww>-<nn>`, for example `U-2026W42-01`.

The upstream snapshot this repository was created from is `krita/6.0`
`e88c22892c80853dc1517fbe6ea2e292b1a3b30f` (2026-09-30). `main` has no
common commit with upstream (independent history), so upstream work can only
arrive as cherry-picks or patches.

## Adopting an item

1. Read the item in `reports/<week>.json` and its section in the `.md`
   report, then read the agent documents of every Solstice feature the item
   touches (`solstice_files`, and the features in the report).
2. Fetch the commits from the share mirror without adding a remote (a
   persistent `upstream` remote stays forbidden by `AGENTS.md`). The share
   belongs to another user, so pass `safe.directory`:

   ```bash
   git -c safe.directory='*' fetch //192.168.1.37/share/upstream-krita-check/krita.git <sha>...
   ```

3. Apply in the order the report gives, with `git cherry-pick -x <sha>`, so
   each commit message keeps `(cherry picked from commit <sha>)`. Add the
   trailer `Upstream-Intake: U-<YYYY>W<ww>-<nn>` above the attribution
   lines. One item can be several commits; keep one Solstice commit per
   upstream commit unless a conflict resolution needs them combined.
4. Resolve conflicts in favor of Solstice behavior: removed Android
   branches stay removed, application strings keep "Solstice", and the
   touch points in `feature-inventory.md` keep working. When the conflict
   is more than mechanical, stop and ask the user.
5. Format, build, test and install as the touched area requires
   (`development-workflow.md`, the feature documents), then run the item's
   `claude_notes` checks and list the manual checks for the user.
6. Record the decision in the ledger below in the same change. Update
   `feature-inventory.md` only when the adoption changes a Solstice touch
   point.

## Rejecting or holding an item

When the user rejects or holds an item, add its commits to the ledger with
`rejected` or `held` and the user's reason in the note. Nothing else
changes. A held item that is later adopted or rejected gets its row updated.

## Ledger

One row per upstream commit, full 40-character SHA. `collect.py` reads this
table from `main`: commits marked `adopted` or `rejected` (and upstream
backports whose `cherry picked from` line names them) are not reported
again. `held` rows stay candidates, so the next report re-checks them.

Decision values: `adopted`, `rejected`, `held`. The Solstice commit column is
the adopting commit (`-` otherwise).

| Item | Upstream commit | Branch | Decision | Solstice commit | Date | Note |
| --- | --- | --- | --- | --- | --- | --- |
