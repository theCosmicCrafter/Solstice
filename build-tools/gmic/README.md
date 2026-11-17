# Pinned CPU G'MIC runtime

Build after installing Solstice's `kritaqmicinterface` library and public
headers. Use Python 3.13+, CMake, Ninja, LLVM-MinGW and the same Qt6/KF6
dependency prefix as Solstice. See the [G'MIC maintenance reference](../../docs/agent/gmic.md).

`build-runtime.py --work <scratch> --install <application-prefix>
--deps <dependency-prefix> --compiler-bin <llvm-mingw>/bin --jobs 2`

`--archive <gmic-3.7.4.1.tar.gz>` permits an offline rebuild. Source and patches
are installed under `share/gmic/source`. The archive checksum and exact patch
contexts are checked; the archive includes CImg and the compressed community
standard library, so no floating source downloads are needed. The runtime is
CPU-only, with OpenMP and FFTW, and is compiled into `krita_gmic_qt.dll`.

Source: https://github.com/vanyossi/gmic/releases/tag/v3.7.4.1 (the maintained
Krita-native successor to the archived amyspark/gmic distribution).
G'MIC-Qt is GPL-3.0-or-later; the G'MIC/CImg source includes its CeCILL/CeCILL-C
licenses and third-party notices in the archive. Keep the source archive and
patches with redistributed packages. The runtime uses Solstice's separately
distributed Qt/KF/FFTW/LLVM runtime dependencies and their notices.

The pinned native host only offers in-place output. The Solstice patches keep
settings in the host-selected INI file, display the Solstice name, clean up
runtime state on early failure, and report unsupported output images.
Output transfer references the CImg result and copies directly to the
host-owned buffer, avoiding the owning CImg copy and temporary slice. Volume
outputs still transfer the first z slice of each channel. Host ownership is
retained because applying the result can outlive the runtime's image list.
