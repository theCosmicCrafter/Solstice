# SPDX-License-Identifier: GPL-3.0-or-later
#
# PyQt5.QtXml -> PyQt6.QtXml (Qt5Compat QDom* classes, when shipped)

try:
    from PyQt6.QtXml import *  # noqa: F401,F403
except ImportError:
    # QtXml was dropped from Qt6 proper; PyQt6 ships it only when the
    # Qt5Compat module is present. If it is absent, a plugin that needs
    # QDomDocument has to be patched at vendor time (see
    # docs/agent/plugin-intake.md).
    raise ImportError(
        "PyQt5.QtXml compat unavailable: QtXml needs Qt5Compat in PyQt6. "
        "Rewrite the plugin's QDom* usage on QXmlStreamReader/QtXmlPatterns "
        "or patch it during vendoring."
    )
