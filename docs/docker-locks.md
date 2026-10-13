# Docker locks

Docker locks keep the sizes of docked dockers while you work, similar to Clip
Studio Paint's options for fixing the palette dock width and height.

## Options

The options are at the end of **Settings → Dockers**, below the list of
dockers. Each one is a check box that applies at once to every window and is
remembered between sessions:

| Option | Effect |
| --- | --- |
| **Lock Docked Docker Widths** | The separators that change widths (between the dock areas and the canvas, and between dockers side by side) can no longer be dragged. |
| **Lock Docked Docker Heights** | The separators that change heights (between dockers stacked in a column, and between the top or bottom dock area and the canvas) can no longer be dragged. |

While a lock is on, the resize cursor no longer appears over the locked
separators.

## Notes and limitations

- The locks only stop dragging. Resizing the main window, showing or hiding a
  docker, or loading a workspace can still change the dockers' sizes.
- Docked dockers can still be dragged to another dock area, into a tab
  group, or out of the dock areas to float.
- The separators inside a floating group of tabbed dockers are not locked.
- A docker locked with its own title bar lock button keeps that lock.
