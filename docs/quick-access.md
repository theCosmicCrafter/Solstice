# Quick Access Manager

Quick Access Manager is a set of dockers and popup tools. It
provides configurable action, docker, and brush grids; editable profiles;
gesture menus; Quick Brush Adjustments; a compact HueSVC selector; and
hold-based temporary brush shortcuts.

Solstice has no right-click Popup Palette or On-Canvas Brush Editor; the
Quick Access palette, HueSVC popup and Quick Brush Adjustments take their
place. Right-click with a brush tool does nothing; other tools keep their
right-click menus.

HueSVC places small overlapping foreground and background color swatches at
the selector's top-left, like the Wide Gamut Color Selector's. Clicking either
swatch swaps the two colors. The button at the top right chooses the selector
shape: a vertical hue bar left of the saturation/value square (the default),
or the square inside a hue ring. The choice applies to the HueSVC docker and
its popup and does not follow the Advanced Color Selector's shape.
Its popup uses a fixed-width vertical adjustment panel so each brush and layer
slider spans the panel, as in the Quick Access plugin. Docker-only status
buttons and their separators are omitted from the popup. Opening either brush
or layer blending-mode list keeps the HueSVC popup open while the list is in
use.

The popup and the Quick Brush Adjustments docker use the same controls as Tool
Options: value bars that show their name and value (double-click to type a
value, Shift + drag for fine steps) for size, opacity, flow and layer opacity,
and in the popup the angle dial and value of Tool Options for brush rotation,
with its flip options in one menu button so the row fits the panel. The size bar covers the full brush size range and moves
in finer steps at small sizes, like the brush tip's Diameter.

## Arranging items

- **Ctrl + drag** an item to move it to another cell of the palette without
  opening Grid Edit. A frame shows where it will land; items in the way are
  moved aside. A plain click still uses the item, and any other mouse button
  cancels the drag. Resizing, multi-selection and moving items between tabs
  stay in Grid Edit.
- Items added from the **Resources** dialog fill the palette's last empty row
  from the left and continue on the next row when the row is full. Items
  added from the header menu start a new row each time.

## Brushes

Brushes show stroke previews, like the Brush Presets docker:

- In the **Resources** dialog, the **Brushes** tab works like the Brush
  Presets docker: stroke previews with names, tags, search, the **Engines**
  and **Bundles** filters and the grouping dropdown (shared with the
  docker). Select several presets with Ctrl or Shift and click **Add
  Selected Brushes**, or double-click a preset.
- A brush added to the palette (from the Resources dialog or with **Add
  Current Brush**) shows its stroke preview over two cells, without the name.
  The name appears in the tooltip.
- Right-click a brush and choose **Property** to choose its **Display**:
  **Stroke Preview** (two cells) or **Icon** (the preset icon, one cell).
  Changing it resizes the item and moves the following items as needed.
- **Grid Edit** shows each brush the same way as the palette.

Brushes added before stroke previews existed keep their icon until their
display is changed.

Profiles, aliases, custom labels, colors and icons saved by the Quick Access
plugin still load.

Native implementation: [`../plugins/dockers/quickaccess`](../plugins/dockers/quickaccess/)
