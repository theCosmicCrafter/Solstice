# Solstice interface

Solstice is gradually refining Krita's interface: calmer colors, clearer
docker headers, document tabs and toolbars, without moving any controls or
changing how they work. New profiles start with these changes on; each one can
be turned off to return to Krita's look.

## UI font

Solstice uses **Cantarell**, bundled with the application, as its default
interface font, at the size of the system's interface font. Text that
Cantarell has no characters for, such as Japanese, is shown in the system's
fonts.

To use another font, open **Settings → Configure Solstice → General →
Window**, enable the custom font and choose it; Cantarell is listed there as
well. A custom font chosen earlier stays in use until it is turned off.

Cantarell is licensed under the SIL Open Font License 1.1; the license is
installed as `share/krita/fonts/Cantarell-OFL.txt`.

## Interface scale

**Settings → Configure Solstice → General → Window → Interface scale** makes
the whole interface larger or smaller, from 75% to 200% in steps of 5%. It
multiplies the system's display scaling, so 110% on a 100% display shows the
interface at 1.1 times its size. The canvas follows the new scale too.

The scale applies after Solstice is restarted. 100%, 150% and 200% give the
sharpest result; other values can make thin lines and icons slightly blurry,
as with fractional display scaling in the system. A `QT_SCALE_FACTOR`
environment variable, if set, takes precedence.

## Solstice Dark theme

**Settings → Themes → Solstice Dark** selects a dark color theme with neutral
graphite surfaces:

- The window, input fields and buttons use three distinct shades, with
  buttons slightly lighter so clickable controls stand out.
- Focus and hover use a muted blue that is kept apart from the selection
  color.

Switch back with **Settings → Themes → Krita darker** (or any other theme).

## Solstice interface option

**Settings → Configure Solstice → General → Window → Solstice interface
(docker titles, document tabs, toolbars)** changes four parts of the window:

- **Docker titles** get a header band with a separator line and a little more
  padding, so each docker's title reads as its header. The lock, float and
  close buttons stay where they are.
- **Document tabs** are sized to their titles, so the close button sits next
  to the title. The current tab is lighter and underlined in the selection
  color; other tabs use dimmer text.
- **Toolbars** are separated from the area below by a line, with slightly more
  padding.
- **Settings dialog** pages are listed in a compact sidebar, with a small
  icon and the name in one row, instead of large icons above bold names.
  This applies when the dialog is opened again after the option is
  changed.

The colors follow the current theme, so the option works with light and dark
themes. Clear the check box to return to the original look; the change applies
when the dialog is closed with **OK**.

## Solstice style

**Settings → Styles → Solstice** draws the common controls flat and rounded in
the theme's colors, on top of the Fusion style:

- Buttons are rounded; toggled buttons and the active tool are filled with
  the selection color.
- Check boxes and radio buttons are filled with the selection color when
  checked.
- Input fields and spin boxes (including the slider spin boxes) get a
  rounded frame that turns to the selection color when focused; the step
  buttons are flat inside the frame.
- Menus show the highlighted item as a rounded block; checked items show a
  plain check mark (or a dot for choices), highlighted or not.
- Scroll bars have a flat track and a rounded handle.
- Tabs in dialogs and docker groups are flat, with a line under the current
  tab.

Sizes and layouts are the same as with Fusion. Choose **Settings → Styles →
Fusion** to return to the original look.

## Limitations

- The Solstice interface option changes only the main window's docker
  titles, document tabs and toolbars, and the Settings dialog's page list; the
  Solstice style changes the common controls.
- After switching styles, a few lists and menus that were already open (for
  example the layer list) keep the previous style until Solstice is
  restarted.
- Without the Solstice interface option, the Settings dialog keeps its large
  page icons.
- Panels built with QML, such as Text Properties, follow the theme's colors
  but keep their own control shapes; the Solstice style does not apply to
  them.
- The color of the area around the canvas comes from **Configure Solstice →
  Display → Canvas Border Color**, not from the theme.
- The theme, the Solstice interface option and the Solstice style have been
  checked on Windows at 125% display scaling only.
