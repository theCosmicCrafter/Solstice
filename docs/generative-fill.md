# AI Generative Fill

**Select > AI Generative Fill** fills the current selection with
AI-generated content in one click. The bundled
[AI Diffusion](https://github.com/Acly/krita-ai-diffusion) plugin runs an
inpaint job on its managed ComfyUI server, and the finished result lands
as a new paint layer — no docker visit and no Apply step needed.

## Using it

1. Make a selection (rectangle, lasso, contiguous, …) over the area to
   fill.
2. **Select > AI Generative Fill**.
3. A new layer named `[Generated] <prompt> (<seed>)` appears once the job
   finishes. The job also stays in the AI Diffusion docker's job history,
   so its other batch results and prompts remain available there.

The result is a normal paint layer: edit it, mask it, and undo the whole
operation with Ctrl+Z like any layer add.

### Prompt behavior

- **Empty prompt** (the AI Diffusion prompt field is blank): the
  workflow conditions on the surrounding pixels — a content-aware fill,
  no text needed.
- **Prompt filled in**: the fill is guided by the docker's current
  prompt, like Photoshop's Generative Fill text box. Type a prompt in the
  AI Diffusion docker first, then run the menu action.

The menu action always runs a full-strength fill (the docker's
inpaint-mode and strength settings are used for this job only and are
restored afterwards).

## Requirements and errors

- Requires the **AI Diffusion** plugin enabled (default) and a working
  ComfyUI setup — the same one the docker uses.
- Without a selection the menu item is greyed out; if it still fires (a
  just-cleared selection), a short notice explains that a selection is
  needed.
- When the server is not running, the action starts the plugin's normal
  managed-server connect flow first and fills once it is up; if the
  connection cannot be established, the plugin's standard error surfaces
  explain what is missing (open the AI Diffusion docker for details).

## Scope and limitations

- The first result of the first job is applied as a layer; extra batch
  results (`Batch` count in the docker) stay in the job history.
- A selection covering the whole canvas behaves like the docker's
  Generate (full-image generation rather than inpaint).
- A selection that reaches the canvas edges triggers the plugin's
  outpaint/expand path instead of fill, same as the docker's automatic
  inpaint mode.

For technical details see
[`agent/generative-fill.md`](agent/generative-fill.md).
