# Lazy Tools

Solstice includes native versions of the core productivity features from the
Lazy Tools Python plugin. No separate Lazy Tools docker or Python installation
is required.

## Layers docker

Two compact color controls appear before the blending-mode control:

- The first assigns a color label only to the explicitly selected layers.
  Selecting a group does not change any of its child layers.
- The second toggles visibility for every layer with the chosen color label.

The existing layer-list filter button remains available for filtering the
docker by name or color without changing layer visibility.

## Shortcut actions

The following actions can be assigned under **Settings > Configure Solstice >
Keyboard Shortcuts**:

- **Create Selection Mask Alternative** saves the current selection in a
  hidden `Selection_Mask_Group` at the bottom of the layer stack.
- **Create Selection Mask Popup** opens a thumbnail grid for creating,
  refreshing, and restoring saved selections.
- **Set Foreground Color 1** through **Set Foreground Color 9** select the
  corresponding configurable color slot.
- **Fast Image Export** opens an export dialog for PNG or JPEG, the active
  document or all open documents, and the source, configured, or newly chosen
  output folder.
- **Rename Alternative** opens a preset-based rename dialog at the mouse
  cursor. A preset changes the active layer's name and optional color label.
  Manual names can be saved as new presets.

Documents must already have a file name for Fast Image Export. PNG compression
and alpha handling, JPEG quality, and the configured output folder are retained
between uses.

## Custom settings

Open **Settings > Configure Solstice > General > Custom** to configure the nine
foreground-color slots and these default-enabled options:

- **Color Pick from Anywhere** registers the fixed Windows system-wide shortcut
  `Win+Shift+C`. It samples the pixel under the cursor, including from another
  application, and makes it the foreground color.
- **Disable top menu shortcuts** removes mnemonic activation from the top
  menu labels from startup, including menus added or rebuilt later. This frees
  combinations such as `Alt+E` for your own shortcuts without
  first opening Settings. Menus remain accessible by mouse; shortcuts inside
  an open menu are unchanged. Disable the option to restore menu mnemonics.

Color Pick from Anywhere is available only on Windows.

Rename presets remain compatible with the Python plugin and are stored in
`lazy_tools/config/name_color_list.txt` inside the Solstice resource folder
(`%APPDATA%\Solstice\resources` by default).
Each line uses `layer name` or `layer name, Color`; supported color names are
Blue, Green, Yellow, Orange, Brown, Red, Purple, and Grey.
