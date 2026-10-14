# SPDX-FileCopyrightText: © 2022-2026 Wojciech Trybus <wojtryb@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

"""
The only python file directly run by krita during plugin init phase.

Checks whether plugin requirements are met and aborts the start if not.
"""
import os
import sys

# Appending this file location to python PATH allows to directly import
# main packages instead of using relative imports.
sys.path.append(directory := os.path.dirname(__file__))


def main() -> None:
    from PyQt.QtCore import Qt
    from PyQt.QtWidgets import QMessageBox

    from .api_krita import Krita
    from .INFO import __required_krita_version__, __version__

    # If krita version is known and too low, abort the plugin start
    # If krita version is unknown (built from source), assume it is correct
    if Krita.version.is_known and Krita.version < __required_krita_version__:
        warning_box = QMessageBox()
        warning_box.setIcon(QMessageBox.Icon.Warning)
        warning_box.setWindowTitle("Shortcut composer version mismatch")
        warning_box.setTextFormat(Qt.TextFormat.RichText)
        warning_box.setText(
            "Shortcut Composer will not load.<br><br>"
            f"The plugin in version {__version__} requires higher "
            "Krita version.<br><br>"
            f"Krita version: {Krita.version}<br>"
            f"Required Krita version: {__required_krita_version__}<br><br>"
            "Upgrade your Krita, or downgrade the plugin.")
        warning_box.setStandardButtons(QMessageBox.StandardButton.Ok)
        warning_box.exec()
        return

    # Solstice renders the canvas with its Vulkan engine; there is no
    # OpenGL fallback toggle, so upstream's "OpenGL is not being used"
    # startup warning is removed here — the pie menus render through Qt
    # widgets regardless of the canvas backend.

    from .shortcut_composer import ShortcutComposer  # noqa
    Krita.add_extension(ShortcutComposer)


main()
