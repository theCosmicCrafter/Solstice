# Solstice visual branding TODO

This checklist covers visual assets and visual-facing integration only. Internal
Krita compatibility identifiers, plugin APIs, MIME types, resource paths, and
configuration filenames are intentionally outside this checklist.

## Temporary gray placeholders (2026-10-03)

The user requested solid gray replacements while the final brand is undecided.
169 nonempty image assets now use opaque `#808080`, with their original pixel
dimensions or SVG width/height/viewBox preserved:

- All images in `krita/pics/branding/`, including default, Beta, Next, Plus,
  the legacy root ICO, and every Apple `.icon/Assets` source.
- All 30 images in `krita/pics/mimetypes/`, including the `krz` subdirectory.
- The 3840 x 1920 PNG splash, 1920 x 960 JPEG source, and 131 x 80 SVG banner.
- The 24 x 24 `krita/pics/svg/support-krita.svg` resource. Its upstream link
  and text remain unchanged.
- All MSIX images in the actual source directory
  `packaging/windows/msix/pkg/Assets/`.
- `packaging/macos/KritaIcon.icns` and `packaging/macos/krita_dmgBG.png`.

ICO files preserve all seven sizes (16, 32, 48, 64, 96, 128, 256). ICNS images
preserve the individual pixel sizes and Retina representations; the legacy
16/32-pixel ARGB entries use equivalent PNG entries (`icp4`/`icp5`). The Apple
layer composition configuration is unchanged, so macOS effects and final
rendering still need platform verification. No packaging identity was renamed.

`libs/ui/kis_splash_screen.cpp` leaves the splash artwork credit empty while
the solid fill is used. Restore the correct artist credit with final artwork.
Original artwork remains available in Git history. The empty seasonal splash
file has no dimensions and is unchanged; its code path remains disabled.
Third-party sponsor artwork, upstream links, and contributor attributions were
unchanged by that pass (the About dialog was trimmed later, on 2026-10-07; see
"In-application branded visuals"). Template/store-screenshot audits remain future work, not completed
by this mechanical image replacement.

The Windows build embeds Next's SVGZ and splash QRC in `krita.dll` and
generates application/file ICO files from the PNG sources. Rebuild `krita`,
`krita_windows_stub_exe`, `krita_windows_stub_com`, and `krafile_dummy_obj`;
install `libs/ui`, `krita`, and `krita/pics/branding/Next` locally with the
application closed. Generated `krita.ico`/`kritafile.ico` are installed under
`installer/` for packaging. NSIS uses those generated icons for shortcuts;
there is no repository-specific NSIS header bitmap to replace.

The Windows source lists in `krita/CMakeLists.txt` exclude 512/1024-pixel PNGs
from ICO generation only. Otherwise ECM's icon conversion embedded a 1024-pixel
PNG in a directory entry declaring 256 pixels. The high-resolution source and
installed PNGs are retained; generated Windows ICO frames must match their
declared dimensions as well as being gray.

Verification: compare source dimensions with the pre-change Git assets, decode
every ICO/ICNS representation, and check all raster pixels and SVG rectangles
are opaque gray. Also inspect generated installer icons after rebuilding.
The 169 source assets passed comparison with pre-change `HEAD`, including every
ICO/ICNS representation. The application and Windows stubs compiled successfully.
Both generated Windows ICOs passed pixel and declared-dimension checks at all
seven sizes (16, 24, 32, 48, 64, 128, 256). Installation completed after the user
closed `krita.com`; `krita.dll`, `libkritaui.dll`, both Windows stubs and both
installer ICOs match their build SHA-256 hashes. When checking for a running
application on Windows, check both process names `krita` and `krita.com`
(`solstice` since 2026-10-11).
Interactive splash/About/taskbar appearance needs a restarted application;
macOS, Linux, MSIX packaging, icon-cache and scaling checks remain open.

User-facing status: [Temporary visual branding](../visual-branding.md).
The unchecked items below refer to final artwork and release acceptance;
gray placeholders do not complete them.

## Application icon artwork (2026-10-10)

The user's icon artwork replaced the gray application icon:

- Master: `krita/pics/branding/source/solstice-icon-1024.png` (1024 x 1024,
  16-bit RGBA, transparent corners; the rounded square spans 806 x 862 px,
  centered). The editable `.kra` stays outside the repository (64.5 MiB).
- Generated from the master into `Next/` (the development build's variant)
  and `default/`: `16/22/24/32/48/64/128/256/512/1024-apps-krita.png`
  (smooth halving, then one smooth step, in RGBA64), `krita.ico` (PNG frames
  16-256), and `sc-apps-krita.svgz` (an SVG embedding the 512 px PNG, which
  Qt's SVG renderer draws; there is no vector source). The legacy
  `krita/pics/branding/krita.ico` is a copy of `Next/krita.ico`. The
  generator was a small Qt program in the session scratchpad (not in the
  repository); regenerate the same set when the master changes.
- The build's own `krita.ico` (ECM, installed to `installer/`) decodes at all
  seven sizes with matching declared dimensions. `krita.exe`, `krita.dll`,
  the hicolor PNGs and the scalable SVGZ were reinstalled; `krita.com` has no
  icon resource.
- Still gray: `Beta/`, `Plus/`, the Apple `krita.icon`, the `.icns` document
  icons, MSIX and macOS package art.

Document icons (2026-10-10, the user chose a page derived from the
application icon): `krita/pics/mimetypes/*-mimetypes-application-x-krita.png`
and `krz/*-mimetypes-application-x-krz.png` (16-1024) and their SVG copies
(`application-x-krita[-16|-22|-24].svg`, which embed the PNG of their size;
the main SVG embeds 256 px). Drawn from `branding/Next/1024-apps-krita.png`
(already sRGB): a page with a folded corner and a shadow, the application
icon at half the size, centered for `.kra`; `.krz` adds a bold "KRZ" label
above it. Below 32 px the page is drawn directly at its size with a larger
emblem and no label. The build's `kritafile.ico` (installed to
`installer/`) decodes at all seven sizes. Regenerate them when the
application icon changes.

Splash artwork (2026-10-10): master `krita/pics/branding/source/solstice-splash.png`
(3146 x 1920, 16-bit RGBA, 13 MB). The resource is an 8-bit opaque copy
composited over white, `krita/data/splash/solstice-splash.png` (1.9 MB), as
`:/splash/0.png` in `splash.qrc` (compiled into `krita.dll`). The gray
`electrichearts_20250824A_kiki_4K.png` and the unused HD JPEG were removed.
`KisSplashScreen` keeps the image's aspect ratio (480 px high when loading,
about 786 x 480; 320 px in About) and draws the version and loading text
right-aligned at the top right in white with a drop shadow; the artwork's
top-right corner is dark for that reason. No artwork credit is shown.

**Color profile of the masters.** Both masters are exported from RGBA
float documents and embed the linear-gamma profile `sRGB-elle-V2-g10.icc`.
Their 16-bit values are linear light: read as plain sRGB they come out too
dark and lose the pale blue-gray tones (the first generated splash and icons
did this). Generated assets are converted from the embedded profile to sRGB
first (`QImage::convertToColorSpace(QColorSpace::SRgb)` on an RGBA64 copy),
then scaled or composited; the generated PNGs are plain 8-bit sRGB. Do the
same whenever the masters are regenerated.

## Brand source package

- [ ] Create the canonical Solstice logo as an editable vector source.
- [ ] Create light, dark, monochrome, and high-contrast logo variants.
- [ ] Define minimum clear space, approved background colors, and small-size behavior.
- [ ] Record the logo font, colors, and asset license in this document or a neighboring
      brand guide.
- [ ] Keep source artwork separate from generated PNG, ICO, ICNS, and Apple icon assets.

## Application icon

- [x] Replace the icon sources under `krita/pics/branding/default/` (and `Next/`,
      2026-10-10, from `source/solstice-icon-1024.png`).
- [ ] Replace or retire the `Beta`, `Next`, and `Plus` branding variants; the current
      development build is configured with the `Next` variant, so changing only
      `default` will not change its icon.
- [ ] Generate and visually inspect the installed icon at 16, 22, 32, 48, 64, 128,
      256, 512, and 1024 pixels where the platform supports those sizes.
- [x] Replace `krita/pics/branding/default/krita.ico` while retaining the filename until
      the executable and packaging rename is completed.
- [x] Rename the Windows launchers to `solstice.exe` and `solstice.com`
      (2026-10-11, `docs/agent/executable-name.md`); `krita.dll`, the icon file
      names and the application name `krita` stay.
- [ ] Replace the Apple `krita.icon` asset package and verify its generated ICNS output.
- [ ] Update `branding.qrc` and the `krita-branding` resource alias only if a coordinated
      code migration is made; the alias may safely remain internal.
- [ ] Check the icon in the Windows taskbar, Alt+Tab, Task Manager, Start menu, desktop
      shortcut, file picker, and executable properties.
- [ ] Check the icon on light and dark Windows themes at 100%, 150%, and 200% scaling.
- [ ] Check Linux launcher, task switcher, and desktop-file icon rendering.
- [ ] Check macOS Dock, Finder, About panel, and application bundle rendering.

## Application name in interface strings (2026-10-07)

User-visible strings that call the running application "Krita" now say
"Solstice". This covers 199 strings in 88 files:

- `i18n()`/`i18nc()` calls;
- `.ui` `<string>` values;
- `.action` text, tool tips and "What's This";
- Python plugin `i18n()` calls.

Examples are message box titles (`i18nc("@title:window", "Krita")`, "Krita:
Warning", "Krita - Edit Text"), "restart Krita", "Krita will not use more
memory...", "Show Krita log for bug reports", the bug-information captions,
the command-line option help and the Preferences window title.

The scratchpad scripts `kritastrings.py` (inventory) and `rebrand.py`
(replacement with a keep-list) did the work. They are not in the
repository; rerun an inventory with a similar script after larger merges.

**Kept on purpose:**

- file-format names (the MIME descriptions "Krita Brush Preset", "Krita
  Archival Image Format" and others; "a Krita file", "Not a valid Krita file",
  "Krita Palette (KPL)", "Krita files" in the comics manager);
- references to Krita versions ("Krita 4.2+", "before Krita 4.3", "Krita 4.x",
  "this version of Krita (%1)" with the compatibility version);
- "Upstream Krita" links, news and handbook entries;
- the About and License texts;
- the copyright credit in `KAboutData`;
- Elle Stone's quoted profile notes;
- the Krita Script Starter and Krita scripting school texts (the Python API is
  Krita's);
- the database explorer's stored "Krita version";
- translator contexts only;
- Qt Designer plugins;
- Microsoft Store, AppImage and X11 messages, which do not apply to the
  Windows desktop build.

**Translations.** Changing an English source string detaches its existing
translation, so these strings show in English in other languages until they
are translated again. This is accepted: new profiles default to English, and
the Solstice-specific strings are untranslated anyway.

## Update checks (2026-10-07)

`ENABLE_UPDATERS` (top-level `CMakeLists.txt`) defaults to **OFF**:

- `KisManualUpdater` compares with Krita's release feed on krita.org, which
  would offer Solstice users Krita releases;
- the welcome page creates no updater and keeps its version notice hidden;
- its news option reads "Enable upstream Krita news".

Existing build trees keep their cached value: reconfigure with
`-DENABLE_UPDATERS=OFF`. An update check against Solstice's GitHub releases
can replace it once releases exist.

## Splash and About artwork

- [ ] Create new Solstice splash artwork with an explicit redistribution license.
- [ ] Replace `krita/data/splash/electrichearts_20250824A_kiki_4K.png`.
- [ ] Provide a correctly sized lower-resolution source if the desktop build later needs
      one; Android-specific splash work is not in scope for this distribution.
- [x] Banner and logo overlays: removed instead of replaced (2026-10-07, user
      request). `KisSplashScreen` shows a single splash image with the version and
      loading text at the top right; it no longer creates the `banner.svg` and
      `krita-branding.svgz` overlays. The files stay in their QRCs; the
      branding SVG is still the window icon source.
- [ ] Update the splash artist credit in `libs/ui/kis_splash_screen.cpp`.
- [ ] Confirm loading text remains readable against the brightest and darkest parts of
      the new artwork.
- [ ] Verify the compact About-dialog form of the splash does not crop the links
      or artwork credit.
- [ ] Decide whether seasonal splash support will use Solstice artwork, then replace or
      remove the empty `splash_holidays_dummy.png` placeholder.

## Document and file-type artwork

- [ ] Inspect every image under `krita/pics/mimetypes/` for the Krita logo or other
      upstream brand marks.
- [ ] Preserve `.kra`, `.krz`, `.kpp`, and other compatible format identities even when
      replacing branded artwork.
- [ ] If document icons are changed, generate the Windows `kritafile.ico` and verify
      Explorer thumbnails and fallback icons separately.
- [ ] Verify that a Solstice application icon is not confused with the `.kra` document
      icon at small sizes.
- [ ] Review brush preset, resource bundle, workspace, session, and shortcut-scheme
      icons shown in native file dialogs.

## Installer and package visuals

- [ ] Create a Solstice installer header/banner image if the Windows NSIS theme uses one.
- [ ] Replace installer, uninstaller, Start menu, and desktop-shortcut icons.
- [ ] Replace MSIX tile assets under `packaging/windows/msix/pkg/Assets` if MSIX packaging is
      retained.
- [ ] Test Windows pinned shortcuts before and after an upgrade; Windows may cache the
      old Krita icon.
- [ ] Replace macOS DMG background and volume icon if macOS packaging is retained.
- [ ] Replace AppImage, Flatpak, and Snap store imagery if those packages are retained.
- [ ] Capture new store screenshots only after the full visible-name pass is complete.

## In-application branded visuals

- [ ] Audit `support-krita` and other donation/community icons on the Welcome page.
- [ ] Keep upstream Krita links visually identified as upstream rather than presenting
      them as Solstice services.
- [x] Review the About dialog sponsor artwork and decide whether to retain it as an
      explicitly labelled upstream Krita section. Decided 2026-10-07 (user): the
      About dialog hides Krita-related items.
      - `KisAboutApplication` deletes the Authors (upstream developers),
        Translators (generic KDE text), Sponsors (upstream fund) and Also Thanks To
        (upstream credits) tabs; their resources (`developers.txt`, `credits.txt`,
        sponsor images) are unchanged.
      - About, License and Third-party libraries remain. The License tab keeps
        the attribution to Krita and its contributors, and the GPL text.
      - `KisSplashScreen::displayLinks()` (About tab and splash with links) shows
        Solstice Website, Documentation (`docs/` on GitHub) and Source Code; the
        krita.org support, manual, community and scripting links are gone.
- [ ] Search all embedded QRC resources for Krita wordmarks, logos, mascot artwork, and
      screenshots containing the old interface name.
- [ ] Review welcome-page banners under `share/krita/donation/`; the internal path may
      remain `krita`, but displayed artwork must be labelled accurately.
- [ ] Review example templates and bundled resources for preview images containing a
      visible Krita logo.

## Visual QA and release acceptance

- [ ] Test a clean install with no icon cache from an earlier Krita or Solstice build.
- [ ] Test an upgrade from the last custom Krita-branded build.
- [ ] Test side-by-side installation with official Krita.
- [ ] Confirm no Solstice shortcut, installer page, splash, About page, or OS application
      surface displays the Krita application logo as the current product logo.
- [ ] Confirm every retained occurrence of the Krita name is clearly a format,
      compatibility, historical, documentation, upstream-project, or attribution
      reference.
- [ ] Capture final reference screenshots of splash, About, Welcome, taskbar, Start menu,
      installer, and `.kra` file association states.

## Pre-release review (2026-10-13)

Three read-only audits (public docs, interface text, visual assets) before the
first public release; the user asked for the review after phase 4.109.

Done:

- Branding: `Beta` and `Plus` (gray) now resolve to `Next` in the top-level
  `CMakeLists.txt`; Krita's switch of Qt6 "default" builds to `Beta` is gone,
  so a stable configuration no longer turns every icon gray.
- The embedded document icons `pics/{16,22,32,64}_{dark,light}_application-x-
  {krita,krz}.svg` (shown in File > Document Information; they still had
  Krita's pink gradient) are regenerated from the `krita/pics/mimetypes`
  PNGs as SVGs embedding the PNG of their size, like the mimetype SVGs.
- `pics/krita.png` (the Krita logo, in `icons.qrc`, apparently unused) is a
  copy of `branding/Next/128-apps-krita.png`.
- `splash.qrc` no longer embeds the gray `banner.svg` or the empty seasonal
  `splash_holidays_dummy.png` (the files stay; the seasonal code is `#if 0`).
- The Windows ZIP no longer contains Krita's `krita*.lnk` shortcuts, which
  started the absent `bin\krita.exe` (`package-complete.py`,
  `package-complete-msvc.py`).
- Help > Report Bug opens https://github.com/udtren/Solstice/issues instead
  of Krita's guide or bugs.kde.org; the welcome page's Source Code link goes
  to the Solstice repository.
- Interface text: remaining "Krita" meaning the running application, and
  feature wording (Asset Library, Rest Note, Quick Access, Vision ML tool
  names, Lazy Tools); Quick Access's Properties menu item gets its icon from
  `KisIconUtils` (`QIcon::fromTheme()` is empty on Windows).
- Public docs: `docs/gpu-engine.md` rewritten as a user guide (old sections
  archived in `wiki/history/gpu-user-guide-archive.md`), stale and
  development-diary statements removed from README and the feature guides.

User decisions (2026-10-13) and what was done:

- **Installer and MSIX: rebranded.** `installer_krita.nsi` and its
  translations are replaced by `installer_solstice.nsi` (English): Program
  Files\Solstice, `HKLM\Software\Solstice`, the "Solstice" uninstall entry,
  publisher "Solstice Project", the Solstice version, `bin\solstice.exe`,
  Start menu and optional desktop shortcuts, the Solstice icon, and optional
  per-user Explorer thumbnails. It never detects or removes Krita and takes
  no file associations. `MakeInstallerNsis.cmake.in` no longer downloads
  krita-nsis. The MSIX manifest uses the identity "Solstice" (publisher
  `CN=Solstice` is a placeholder until a signing certificate exists), the
  Solstice version, `solstice\bin\solstice.exe`, and new tile assets
  generated from the icon (`packaging/windows/msix/pkg/Assets`, 51 PNGs).
  Neither was built here (NSIS and the MSIX tools are not installed).
- **Welcome page: removed** the support link, its icon and text, and the
  krita.org User Manual and Getting Started links
  (`KisWelcomePageWidget::slotUpdateThemeColors()`). The labelled "Upstream
  Krita Website" and the community link stay.
- **Files written by Solstice keep naming Krita** (`.kra` editor, bundle
  generator, SVG comment, ICC description, User-Agent), for compatibility.
- **About data**: short description "Digital painting application";
  `LegalCopyright` of the executables and the installer use the
  `KAboutData` copyright text.
- **Names stay** ("HueSVC", "Puppet", Rest Note capitals, Photobash).
- **Vision ML guide expanded** (`docs/vision-ml.md`).
- No public BUILDING.md.

Still open: gray `Beta`/`Plus` (unused), Apple artwork.
