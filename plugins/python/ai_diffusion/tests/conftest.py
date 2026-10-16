"""Prevent pytest from importing the real ai_diffusion package __init__.

When pytest collects test modules inside a package directory it imports the
package's __init__.py during setup (Package node). ai_diffusion/__init__.py
requires the Krita runtime (and a bundled websockets install), which is not
available in a plain pytest environment. Tests in this directory only load
pure-stdlib modules directly by file path, so a stub is registered here to
keep collection working.
"""

import sys
import types

sys.modules.setdefault("ai_diffusion", types.ModuleType("ai_diffusion"))
