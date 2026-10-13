# Brush Stroke Previews

The **Brush Presets** docker shows each saved brush as a landscape stroke with
its name underneath. Previews use a fixed light color on a dark background,
so they remain readable in light and dark themes. Smudge, deform and filter
brushes use gray stripes to show their effect.

![Brush Presets grouped by bundle, with the Engines filter open](images/brush-presets.png)

The number of columns follows the docker's width, with additional presets
wrapping onto subsequent rows. Tags, search and display controls stay below
the grid. In a narrow docker, the controls wrap into multiple rows to keep them accessible.

The **Engines** and **Bundles** dropdowns contain checkboxes for multiple
selection. Checked entries within one dropdown are combined as alternatives;
engine, bundle, tag and search conditions are combined together. The dropdown
stays open while checking entries (mouse click, Space or Enter); Escape closes
it. **Select All** removes that restriction, and **Clear All** shows no matches
until an entry is checked again. The button shows the number selected.

Bundle filtering uses the preset's registered storage memberships. Identical
copies shared by multiple bundles match any checked member bundle and remain
a single item in the list. Photoshop brush libraries (`.abr`) count as
bundles. **Not in a bundle** covers
local/imported presets and other non-bundle storage; it does not infer which
bundle a copied preset originally came from. Filtering never changes bundle
activation. These filters are specific to each Brush Presets docker. New
docker instances start with all entries selected.

The Brush Presets docker always shows stroke previews; the icon view and its
Thumbnails/Details settings are not available there. The docker's display
menu (the ≡ button) has **Preview Size**, which changes the stroke cells, and
**Scroll to Selected Preset** (see below).

The other preset lists show stroke previews too, from the same cache, so a
generated preview appears everywhere at once:

- the **toolbar brush popup** (the brush button in the toolbar): the same
  view as the docker, with tags, search, the Engines and Bundles filters and
  grouping;
- the **Brush Editor**'s preset list: stroke previews, with its existing
  engine filter; its display menu also has only **Preview Size**;
- the [Quick Access](quick-access.md) palette and its Resources dialog.

The lists share the preview size and the grouping choice; a change made in
one list applies to the others after Solstice is restarted. The F5
live preview keeps its existing appearance and live brush-setting behavior.

## Grouping

The **grouping** dropdown in the docker's filter bar shows the presets in
groups, each under a header:

- **No Grouping** (default): one grid, as before.
- **Group by Engine**: one group per brush engine, in name order.
- **Group by Bundle**: one group per bundle, in name order, with presets that
  are not in a bundle last under **Not in a bundle**. A preset that belongs to
  several bundles is shown once, under the bundle whose name comes first.

Within a group the presets keep their usual order. The tag, search, engine
and bundle filters still apply, and groups without visible presets are not
shown. The choice is remembered.

## Selected preset

When you switch brushes elsewhere (a shortcut, the toolbar, Quick Access),
the Brush Presets docker highlights the new preset. By default it does not
scroll: the list stays where you left it, and resizing the docker keeps the
position too. Turn on **Scroll to Selected Preset** in the docker's display
menu to scroll the list to the selected preset, as Krita does. The setting is
remembered and applies to all Brush Presets dockers. Clicking presets and
moving with the keyboard work as before, and the toolbar brush popup always
shows the selected preset.

## Generating previews

On first use, visible previews appear progressively. Generated images are
cached locally and reused after restarting Solstice. Scrolling and hiding
the docker stop work on previews that are no longer needed. Painting takes
priority: generation pauses while a document is busy and requests cancellation
when a new stroke starts. A paint engine already executing a job must reach
its cancellation boundary before it can stop.

Previews reflect **saved presets**. Unsaved brush edits do not replace the
preview; the existing modified-preset indicator remains visible. Saving the
brush requests a new preview. No preset file or stored icon is overwritten.

Round Marker, Experiment and Clone engines show an empty preview area with
the brush name. Failed or excessively slow previews also remain empty;
the stored icon is never substituted. A five-second timeout requests
cancellation, but an individual paint-engine job cannot be forcibly stopped.
Cache files are disposable, limited to 256 MB, and unused entries are removed
after 60 days. The feature uses the CPU and does not require the GPU engine.

## Brush tips in the Brush Editor

The list of predefined brush tips in the Brush Editor has the same
**Bundles** dropdown and a grouping choice (**No Grouping** or **Group by
Bundle**), so the tips of one bundle or Photoshop brush library can be shown
on their own. The tips' grouping is remembered separately from the presets'.
The tip list widens with the Brush Editor and shows more columns.
