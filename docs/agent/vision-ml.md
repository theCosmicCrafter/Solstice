# Vision ML — agent development notes

## Status and locations

- Native implementation: `plugins/visionml/`
- Original reference: `<original-plugin-root>/krita-vision-tools/`
- Native runtime: `plugins/visionml/thirdparty/vision.cpp/`, pinned from
  `Acly/vision.cpp` revision
  `26a752912d49f6c4ff4545b35a1bdf7400d349ed`.

Vision ML is a normal native Krita tool/filter plugin. Do not restore the Python
`ctypes` loader, private `KoToolManager` injection, or installation under
`pykrita`.

## Models and runtime

- Installed defaults live under `share/krita/visionml/models/`.
- User models live under `<resource folder>/visionml/models/`
  (`KoResourcePaths::getAppDataLocation()`; `%APPDATA%\Solstice\resources\visionml\models\`
  by default since the settings moved, `docs/agent/settings-location.md`).
  The user guide is `docs/vision-ml.md` (expanded 2026-10-13).
- `%APPDATA%\krita\pykrita\vision_tools\models\` remains a read-only legacy
  fallback.
- Bundled native models are MobileSAM F16, BiRefNet-lite F16, and MI-GAN 512
  F16.
- Prefer BiRefNet Dynamic for background removal when installed; otherwise use
  bundled BiRefNet Lite.
- Do not bundle BRIA RMBG weights because their license restricts commercial
  use.
- Build Vulkan inference only when the Vulkan SDK and `glslc` are available.
  CPU-only builds must remain functional and show the GPU backend as disabled.
- The development machine has a copy-only LunarG Vulkan SDK 1.4.357.0 at
  `<krita-dev-root>\VulkanSDK\1.4.357.0`; it is intentionally absent from PATH
  and the registry.
- The installed Vulkan runtime has been verified on the NVIDIA RTX PRO 6000
  Blackwell with FP16, BF16, integer dot products, and NVIDIA cooperative
  matrices.

## Build and install

```bat
cmd.exe /d /s /c "call <krita-dev-root>\env.bat && cmake --build <krita-dev-root>\_build --target kritavisionml -j 2"
```

When reconfiguring the build tree, provide the private SDK explicitly:

```bat
cmd.exe /d /s /c "call <krita-dev-root>\env.bat && cmake -S <repository-root> -B <krita-dev-root>\_build -DVulkan_INCLUDE_DIR=<krita-dev-root>\VulkanSDK\1.4.357.0\Include -DVulkan_LIBRARY=<krita-dev-root>\VulkanSDK\1.4.357.0\Lib\vulkan-1.lib -DVulkan_GLSLC_EXECUTABLE=<krita-dev-root>\VulkanSDK\1.4.357.0\Bin\glslc.exe"
```

Install both the module/runtime and models:

```bat
cmake -DCMAKE_INSTALL_LOCAL_ONLY=1 -P <krita-dev-root>\_build\plugins\visionml\src\cmake_install.cmake
cmake -DCMAKE_INSTALL_LOCAL_ONLY=1 -P <krita-dev-root>\_build\plugins\visionml\cmake_install.cmake
```
