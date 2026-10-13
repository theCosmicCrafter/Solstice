# Experimental Windows builds

Solstice's **Windows x64 trial build** GitHub Actions workflow produces an
unsigned ZIP for testing. It is started manually by a repository maintainer;
ordinary pushes and pull requests do not start it.

Open [Actions](https://github.com/udtren/Solstice/actions/workflows/windows-build.yml),
choose a successful run and download its **Solstice-windows-x64** artifact
(downloading requires a GitHub account). Extract the artifact, then extract the
application ZIP inside it. Launch `bin/solstice.exe` from the extracted folder
(`bin/solstice.com` starts it from a console).

The ZIP includes a `shellex` folder with the Krita Shell Extension. Run
`shellex/register-thumbnails.cmd` to show `.kra` and `.krz` thumbnails in
Explorer for your account; see [Explorer thumbnails](windows-shell-thumbnails.md).

These are temporary development artifacts, not signed releases or an installer.
Artifacts expire after seven days. `BUILD-INFO.txt`, the dependency lock file,
and `SHA256SUMS.txt` accompany the ZIP. Windows may show a warning for an
unsigned application.

The workflow compiles the Vulkan engine and shaders, but does not verify GPU
rendering or Vulkan/OpenGL interoperation on supported hardware. Those checks
are done on a developer's machine. A successful CI build does not establish
runtime correctness or support for another GPU.

Build logs are uploaded separately, including on failures. No personal build
environment script, credentials, configuration, or artwork documents are
included in the workflow.

The CI dependency versions can differ from the local development build. Check
the included dependency lock and build logs when comparing behavior. Packaging
includes the application's QML modules as well as the Qt runtime.

The CI build uses Qt 6.11 and does not include Python plugins yet (SIP and
PyQt6 are not available to it). Native features, Vision ML and the Vulkan
engine are included.
