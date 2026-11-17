/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Normal blending of one pixel, shared by composite_layers.comp and
 * composite_stroke_style.comp: KoOptimizedCompositeOpOver128 with opacity,
 * coverage, alpha lock and channel flags.
 */

vec4 compositeOver(vec4 dst, vec4 src, float opacity, bool alphaLocked, float coverage, uint channels)
{
    const float srcAlpha = (src.a * opacity) * coverage;
    if (srcAlpha == 0.0) {
        return dst;
    }
    alphaLocked = alphaLocked || (channels & 8) == 0;

    float newAlpha;
    float srcBlend;
    if (alphaLocked || dst.a == 1.0) {
        newAlpha = dst.a;
        srcBlend = srcAlpha;
    } else if (dst.a == 0.0) {
        newAlpha = srcAlpha;
        srcBlend = 1.0;
        // Scalar CPU Over clears hidden RGB when alpha is unlocked and
        // only some color channels are enabled.
        if ((channels & 7) != 7) dst = vec4(0.0);
    } else {
        newAlpha = dst.a + (1.0 - dst.a) * srcAlpha;
        srcBlend = newAlpha == 0.0 ? 0.0 : srcAlpha / newAlpha;
    }

    vec3 color = srcBlend == 1.0 ? src.rgb : srcBlend * (src.rgb - dst.rgb) + dst.rgb;
    if ((channels & 1) == 0) color.r = dst.r;
    if ((channels & 2) == 0) color.g = dst.g;
    if ((channels & 4) == 0) color.b = dst.b;
    return vec4(color, newAlpha);
}
