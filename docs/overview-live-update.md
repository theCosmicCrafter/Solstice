# Overview live update

The **Overview** docker follows the canvas while you paint: the part of its
thumbnail that changes is updated about ten times a second during a stroke,
instead of only after the image stops changing.

- After the image stops changing, the whole thumbnail is regenerated as
  before, so it stays exact.
- Large changes at once, such as a fill, a filter or a transform covering
  more than a quarter of the image, appear when the change is finished.
- With instant preview (level of detail) active, the full-resolution result
  is shown when it is computed after the stroke.

The option is on by default. Turn it off in **Settings → Configure Solstice →
General → Window → Update the Overview docker while painting**; the Overview
then updates only after the image stops changing.

The live update reads the changed area of the image at the thumbnail's size
on a background thread. With the GPU engine, this reads the changed area back
from the GPU while painting.
