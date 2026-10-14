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
2. Do not `git fetch` from the share mirror: it is shallow, and fetching
   from a shallow repository makes this repository shallow too. Apply the
   commits as patches with the share helper, one upstream commit at a time,
   in the order the report gives (Git Bash, in the checkout):

   ```bash
   TMPDIR=<scratch-dir> bash //192.168.1.37/share/upstream-krita-check/scripts/intake.sh U-<YYYY>W<ww>-<nn> <full-sha>
   ```

   The helper copies the commit's preimage blobs into this repository (so
   `git am -3` can merge with local objects only), runs
   `git am -3 --keep-non-patch` (plain `git am` drops subject prefixes such
   as `[psd]`), and appends `(cherry picked from commit <sha>)`, the
   trailer `Upstream-Intake: <item>` and the attribution line. The share
   belongs to another user, so it passes `safe.directory` per command. A
   persistent `upstream` remote stays forbidden by `AGENTS.md`.
3. Keep one Solstice commit per upstream commit. When `git am` stops on a
   conflict, resolve it (next step), `git am --continue`, and add the
   message lines by hand.
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
| U-2026W42-01 | `653c3b6bb7f7bbb43821a886fb9d71de241e0d90` | krita/6.0 | adopted | `c822f0df82` | 2026-10-14 | Relax an assert to a safe assert in KisResourceThumbnailCache |
| U-2026W42-02 | `efabcb3781ee37b7d9e83276a9b280f1c6700e00` | krita/6.0 | adopted | `0c9a2cb628` | 2026-10-14 | Disable left-click scrolling on animation curves |
| U-2026W42-03 | `65ad64022b39bede8697e13b7be354d5b7adac66` | krita/6.0 | adopted | `1bf931f228` | 2026-10-14 | Fix invalid widget class name |
| U-2026W42-04 | `c21eee1b878b2c24c86e1685960ee94412141e08` | master | adopted | `bd85d92d6c` | 2026-10-14 | Fix conditions for 4-point transformation |
| U-2026W42-04 | `3ee2270d8ead36966a055ab72265b8d767d162b8` | master | adopted | `b6a32e1e33` | 2026-10-14 | Make perspective transform build under Qt5 again |
| U-2026W42-05 | `4bdaa2f4ac08e0cba011f2c6e561e005d1639f23` | master | adopted | `8639d50eac` | 2026-10-14 | Fix horizontal scrolling with the second mouse wheel |
| U-2026W42-06 | `fa7e98c931d646e7d7f52188c5b414cf6b5ddff2` | master | adopted | `d831de684b` | 2026-10-14 | Save and load the collapse state of layer groups to PSD files |
| U-2026W42-06 | `496f786bdb7cbbe6db0657f757c46e023c9f47e3` | master | adopted | `50efcbd662` | 2026-10-14 | Implement layer locking read/write in the PSD format |
| U-2026W42-06 | `4216dd347f843d59ca51c1e097a0dc8af7c753aa` | master | adopted | `d9e6bbff69` | 2026-10-14 | [psd] Add a unittest for user-lock and collapsed PSD fields |
| U-2026W42-07 | `8972b631af37631af33c480741d18deb7b6b1565` | krita/6.0 | rejected | - | 2026-10-14 | Add miscellaneous frame icons; user: not adopted (recommendation consider) |
| U-2026W42-07 | `b4960514a46f9dec6004e87ea8294debb50149cb` | krita/6.0 | rejected | - | 2026-10-14 | Add isolate group / layer icons; user: not adopted (recommendation consider) |
| U-2026W42-07 | `fa1e3484ff8f1b06e654e64747c1594bc0fd62c6` | krita/6.0 | rejected | - | 2026-10-14 | Add brush option slider 1-5 icons; user: not adopted (recommendation consider) |
| U-2026W42-07 | `6ef66b905a44bc7a63911488ffd099eed91bf882` | krita/6.0 | rejected | - | 2026-10-14 | Add and revamp increase/decrease property icons; user: not adopted (recommendation consider) |
| U-2026W42-07 | `eb9f1bcabb882e0e1e8ef5ed1427843607eb0187` | krita/6.0 | rejected | - | 2026-10-14 | Remove trailing spaces from svg-icons.qrc; user: not adopted (recommendation consider) |
| U-2026W42-07 | `89e8c5d17a169d456cc6d16dadad44f5e6348aab` | krita/6.0 | rejected | - | 2026-10-14 | Add 16px frame icons for frame icons; user: not adopted (recommendation consider) |
| U-2026W42-07 | `a7809adb610ab565fd05b2d0fe2fbf2d71129699` | krita/6.0 | rejected | - | 2026-10-14 | Add pin to timeline icon; user: not adopted (recommendation consider) |
| U-2026W42-07 | `7c5fe72b750beda115ab2e5c0cdee4e9f2d87c85` | krita/6.0 | rejected | - | 2026-10-14 | Add wrap direction icons; user: not adopted (recommendation consider) |
| U-2026W42-08 | `303407ff4f8b79a5893328c9daeed047e0aec283` | master | rejected | - | 2026-10-14 | Use an image job for capturing recorder frames; user: not adopted (recommendation consider) |
| U-2026W42-08 | `9f92f4cde35b226fcc9a8ceb2d7cab7fba6b33d3` | master | rejected | - | 2026-10-14 | Allow more tool during recording; user: not adopted (recommendation consider) |
| U-2026W42-08 | `0ece3d797416a4e5ae96ee3c8c6903b6c7789703` | master | rejected | - | 2026-10-14 | Prevent writers from quiting while a job is pending; user: not adopted (recommendation consider) |
| U-2026W42-09 | `c9c44bee771357b8339c6a12d3022bb136d9e3b7` | master | rejected | - | 2026-10-14 | Added Bayer dithering option to Gradient Tool; user: not adopted (recommendation consider) |
| U-2026W42-10 | `6d451f2e9d5a48541bb7e2484fd74522779d2bb2` | master | rejected | - | 2026-10-14 | Consolidate brushtip export widgets; user: not adopted (recommendation consider) |
| U-2026W42-10 | `4c2d9a51dcb044a34877d061f8c84a5c5adc146c` | master | rejected | - | 2026-10-14 | Remove unused parameter; user: not adopted (recommendation consider) |
| U-2026W42-11 | `c42f07284f23f96d2fcbb0400133acef2d69c4cf` | master | rejected | - | 2026-10-14 | ENHANCEMENT: Expose the live brush position to Python scripts; user: not adopted (recommendation consider) |
| U-2026W42-11 | `5d79890f9e340138bd6e8532db0b35f85dd15d34` | master | rejected | - | 2026-10-14 | Fix formatting.; user: not adopted (recommendation consider) |
| U-2026W42-11 | `961b654161ded8f6321e4bb3955321d35c0d3578` | master | rejected | - | 2026-10-14 | Add Python API for input-aware brush strokes; user: not adopted (recommendation consider) |
| U-2026W42-11 | `c7b05604b38f4cc038a89d4569ddbc5b5564397a` | master | rejected | - | 2026-10-14 | Python: Rename paintStroke to paintPolylineStroke.; user: not adopted (recommendation consider) |
| U-2026W42-12 | `a89867be92a62d36272ceca066bce84a629edfcc` | master | rejected | - | 2026-10-14 | Remove all uncertain plural forms from translatable strings; user: not adopted (recommendation consider) |
| U-2026W42-13 | `b16bd1a171a66d534df5c7cd8f8cb08d62ab1132` | master | rejected | - | 2026-10-14 | Use popup flag for color picker popups on all platforms; user: not adopted (recommendation reject) |
| U-2026W42-13 | `fea0d876f7a943a3f27404f693e53b525bd50040` | master | rejected | - | 2026-10-14 | Fix positioning of the color preview popup on Wayland; user: not adopted (recommendation reject) |
| U-2026W42-14 | `8d9ed135b4a066f3aed2416aa49c4ccca71048ed` | krita/6.0 | rejected | - | 2026-10-14 | Update AppData to point to 5.3.4.1; user: not adopted (recommendation reject) |
| U-2026W42-15 | `1cb2aa1a7cbc6471c7da7977b0ad956b8ea19aab` | master | rejected | - | 2026-10-14 | Drop obsolete FOUND_VAR args to find_package_handle_standard_args; user: not adopted (recommendation reject) |
| U-2026W42-16 | `c523e544bcbb3b69fcce12abf9257faeac952a95` | master | rejected | - | 2026-10-14 | tmp commit; user: not adopted (recommendation reject) |
| U-2026W42-16 | `b116b2e79327e9dee30b0baa1ac99ebc40ab134c` | master | rejected | - | 2026-10-14 | Add brush option slider 1-5 icons; user: not adopted (recommendation reject) |
| U-2026W42-16 | `7de61db1d64bc2b2faa0a087417df1bde5f0cf4b` | master | rejected | - | 2026-10-14 | clean up svg-icons.qrc; user: not adopted (recommendation reject) |
