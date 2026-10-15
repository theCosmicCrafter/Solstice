# Trace to Vector Layer

**Layer > Convert > Trace to Vector Layer** turns the active layer's
painted pixels into a filled vector shape on a new vector layer. The
result stays crisp at any zoom and survives document scaling, unlike a
scaled-up paint layer.

This is Solstice's first step of the vector-quality roadmap toward a
resolution-independent workflow next to
[vector layers](canvas-display.md) and the
[Brush Stroke Layer](brush-stroke-layer.md).

## Using it

1. Paint on a regular paint layer — the trace uses the layer's *visible*
   alpha, so masks, filter layers and groups work too.
2. Pick the fill color: the traced shape is filled with the current
   **foreground color**.
3. **Layer > Convert > Trace to Vector Layer**.
4. A new "Traced Layer" appears above the source with the filled shape.
   The source layer is left untouched — hide it to see the pure vector
   result, or edit the shape with the Shape tools.

The whole operation is one undo step: Ctrl+Z removes the layer and the
shape together.

## Scope and limitations

- **Single color.** The entire outline gets the foreground color.
  Multi-color tracing (one path per color) is not implemented.
- **Hard edge.** Pixels are counted as painted when their alpha is at
  least 50 %, so soft antialiased edges become a hard boundary — normal
  for vector output.
- **Pixel-accurate outlines.** The path follows the raster edge
  polygonally; it is not smoothed. For the cleanest results trace from a
  high-resolution source, or simplify the resulting path with the shape
  editing tools.
- Empty or fully transparent layers produce a short notice and no new
  layer.

For technical details see
[`agent/trace-to-vector.md`](agent/trace-to-vector.md).
