# Smart Matting Selection

Smart Matting Selection is a native AI-assisted selection tool in Solstice that
combines the quick targeting of a freehand lasso with the fine edge detail of
the BiRefNet matting model. By drawing a loose outline around a subject — such
as a character with fine hair, fur, or foliage — the tool automatically extracts
a soft-alpha matte and turns it into an editable pixel selection.

Inference runs locally through the embedded `vision.cpp` runtime (on CPU or
accelerated via Vulkan GPU). No external Python processes or network calls are
used.

## Finding the tool

- **Toolbox**: located in the **Select** section of the toolbox (alongside the
  rectangular, elliptical, and freehand selection tools).
- **Tool name**: **Smart Matting Selection** (tooltip: _Smart Matting Selection (Vision ML)_).
- **Internal ID / Action name**: `SmartMattingTool` (icon: `tool_smart_matting`).
- **Activation**: click the dashed-lasso icon in the toolbox or assign a custom
  shortcut in **Settings > Configure Solstice... > Keyboard Shortcuts**.

## Workflow and interaction

1. Select a paint layer with the subject you want to isolate.
2. Choose **Smart Matting Selection** in the toolbox.
3. Draw a freehand outline around the subject. The outline does not need to be
   precise, but it should completely enclose the object and its soft boundaries.
4. Release the stylus or mouse button.
5. Solstice crops the bounded region with a small padding margin, passes it to the
   active BiRefNet model, modulates the resulting matte with your lasso outline
   and the source layer alpha, and writes the result into the active selection.

The selection preserves soft, fractional alpha values (feathered hair strands and
translucent edges) rather than binarizing to a hard 1-bit boundary.

## Tool options and sampling modes

When the tool is active, the **Tool Options** docker displays settings for the
selection:

- **Mode**: standard Krita selection actions — **Replace**, **Add**,
  **Subtract**, **Intersect**, and **Symmetric Difference**.
- **Action options**: **Anti-aliasing**, **Grow/Shrink**, and **Feathering**
  sliders to post-process the generated selection bounds.
- **Sample**: controls which layers contribute image data to the matting model:
  - **Current Layer**: samples only the active layer.
  - **All Layers**: samples the composite image projection of all visible layers.
  - **Color Labeled Layers**: samples visible layers assigned specific color
    labels.
    > [!WARNING]
    > **Known issue:** in the current build, Color Labeled Layers mode fails to
    > produce a selection because the layer merge runs asynchronously while the
    > bounding area is inspected immediately on the GUI thread (defect C1 in the
    > stabilization plan). Use **Current Layer** or **All Layers** mode while the
    > fix is pending.

## Model selection and HR matting

In Tool Options, the **Model** dropdown allows selecting the active matting model:

- **BiRefNet Lite (`BiRefNet-lite-F16.gguf`)**: included with a clean install of
  Solstice under `share/krita/visionml/models/birefnet/`. Fast and lightweight,
  suitable for everyday selections.
- **BiRefNet Dynamic (`BiRefNet-dynamic-F16.gguf`)**: optional higher-resolution
  model offering sharper edges on larger canvases. When placed in the models
  folder, Solstice automatically prefers it by default.
- **High-Resolution (HR) Matting variants**: specialized BiRefNet matting GGUF
  checkpoints designed for fine hair and intricate edges.

### Installing extra models

**In-app download**: when BiRefNet Dynamic is not installed, a download
icon appears next to the **Model** dropdown (in Smart Matting tool options
and the Background Removal filter). Clicking it downloads
`BiRefNet-dynamic-F16.gguf` (~900 MB) directly into the user models folder
and selects it automatically.

Additional GGUF models can also be placed manually in your Solstice
resource directory:

```
%APPDATA%\Solstice\resources\visionml\models\birefnet\
```

Clicking the folder icon next to the **Model** selector opens this directory
directly. Newly added models appear in the dropdown without restarting the
application.

**Backend** selects between **GPU** (Vulkan) for fast inference and **CPU**
(fallback compatible with all systems).

## Limitations

- **Model required**: the tool requires at least one BiRefNet model file in the
  models directory to perform matting.
- **Large regions**: processing very large lasso areas (e.g. 4K+ crops) requires
  more RAM and computation time, especially on CPU. Keep the lasso reasonably
  close to the subject.
- **Clipping**: the output mask is strictly clipped to the area inside your lasso
  outline and to non-transparent pixels on the source layer. Pixels outside the
  drawn outline are never selected.
- **8-bit processing**: matting inference operates in 8-bit RGBA color space;
  high-bit-depth images are converted during preparation.

## See also

- [Vision ML Overview](vision-ml.md): overview of all native AI tools in Solstice
- [Developer notes](agent/vision-ml.md): technical architecture and implementation details
