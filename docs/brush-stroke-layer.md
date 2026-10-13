# Brush Stroke Layer

A Brush Stroke Layer is a paint layer that remembers the brush strokes drawn
on it. The goal, as with Clip Studio Paint's vector layers, is to draw line
art small and enlarge it later without blurring: enlarging the image draws
the remembered strokes again at the new size with the same brush.

The feature is being built in stages. This page describes what works now.

## Creating one

In the Layers docker, open the add-layer menu (**+**) and choose **Brush
Stroke Layer**. It appears below Paint Layer in the menu.

## Painting

- Paint with the brush tools (Freehand Brush, Dynamic Brush, Multibrush).
  Each stroke looks exactly as on a paint layer, and is remembered with the
  brush, colors and settings it was drawn with. The eraser is remembered as
  an erasing stroke.
- Other tools that paint pixels (shapes, fill, gradient and so on) refuse the
  layer, as they do a vector layer.
- Undo and redo remove and restore a stroke together with what it
  remembers. Moving the layer moves its strokes. Duplicating the layer keeps
  its strokes.
- Moving part of the drawing with a selection and the **Move tool** moves
  the strokes inside the selection with it. Strokes must lie wholly inside
  or wholly outside the selection: a stroke the selection's edge cuts in
  two cannot be remembered, and the layer stops matching its strokes (see
  below).

## Enlarging

![A small drawing on a Brush Stroke Layer and an enlarged copy drawn again at the new size](images/brush-stroke-layer.png)

**Image > Scale Image**, **Layer > Scale Layer** and the **Transform tool**
in Free mode draw the remembered strokes again at the new size with the
same brush, so enlarged line art stays sharp; the brush size grows with the
scale. With the Transform tool the preview while dragging shows the scaled
pixels, and the strokes are drawn again when the transform is applied.
Undo returns the layer to its earlier size and strokes.

The layer is redrawn only when its pixels are still exactly its remembered
strokes. If something else changed them (a filter, for example), it is
scaled like a paint layer instead, so no change is ever lost. Rotating,
mirroring, skewing or perspective, scaling within a selection, and the
Transform tool's other modes (Warp, Cage, Liquify, Mesh, Puppet Warp)
transform the pixels as on a paint layer; the remembered strokes then no
longer match, and later enlarging also scales the pixels.

## When the strokes no longer match

After an edit that is not a brush stroke, the layer checks in the
background whether its pixels are still its remembered strokes (it draws
them again, which can take a few seconds for a layer with many strokes). It
also checks when a file is opened. If they no longer match:

- a message on the canvas names the layer and says that scaling it will now
  scale its pixels;
- in the Layers docker, the small brush mark at the corner of the layer's
  thumbnail turns into a yellow warning sign.

Undoing the edit that changed the pixels makes the layer match again; a
message says so and the warning sign goes away.

## Saving

Saving as `.kra` keeps the remembered strokes, so a reopened file can still
be enlarged sharply. Original Krita opens the layer as a normal paint layer
with the same pixels; saving the file again from Krita drops the remembered
strokes.

The file stores each stroke's brush settings and colors, and the brush tip
images, textures, patterns and gradients the strokes use, including brush
tips imported from Photoshop ABR files, so the file enlarges sharply on a
computer that does not have those brushes installed.
Other formats (PSD, ORA, PNG and so on) store the pixels only.

## Current limitations

- Strokes on the layer render their dabs one after another, not on several
  threads, so that each stroke can be drawn again exactly. Very large or
  complex brushes may feel slower on this layer than on a paint layer.
- Strokes drawn with earlier Solstice builds (before 2026-10-10) may not be
  drawn again exactly, for example strokes of brushes that choose their
  images at random or whose spacing depends on pressure; such a layer
  enlarges like a paint layer. Strokes drawn now are drawn again. Files with
  strokes saved now cannot be read by earlier builds, which open the layer
  with its pixels only.

- Filters and other non-brush edits change the pixels but are not
  remembered. The layer then shows the warning sign described above.
- A stroke cut by a moved selection, and moves with the Transform tool
  within a selection, are not remembered.
- A brush texture with a random offset per stroke does not follow the
  stroke when the layer or a selection is moved, so the layer stops
  matching its strokes. Textures without a random offset follow it.
