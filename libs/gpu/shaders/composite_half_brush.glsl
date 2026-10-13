/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 * CPU scalar half brush arithmetic, shared by dabs and Wash merges.
 */
#ifndef COMPOSITE_HALF_BRUSH_GLSL
#define COMPOSITE_HALF_BRUSH_GLSL
float halfValue(float value) { return float(float16_t(value)); }
float halfLerp(float a, float b, float blend)
{
    // roundedProduct(): the driver may fuse a precise multiply-add (phase 4.109)
    precise float value = roundedProduct(b - a, blend) + a;
    return halfValue(value);
}
vec4 overHalf(vec4 dst, vec4 src, float opacity, float maskByte, bool masked, uint channels, bool explicitFlags)
{
    // KoCompositeOpAlphaBase<half>: mask multiplication uses the byte directly.
    opacity = halfValue(opacity);
    precise float product = (maskByte * src.a) * opacity;
    float a = masked ? halfValue(product / 255.0) : halfValue(src.a * opacity);
    if (a == 0.0) return dst;
    float alpha = dst.a;
    float blend = a;
    if ((channels & 8) != 0 && dst.a != 1.0) {
        if (dst.a == 0.0) {
            if (explicitFlags) dst.rgb = vec3(0.0);
            alpha = a;
            blend = 1.0;
        } else {
            alpha = halfValue(dst.a + halfValue(halfValue(1.0 - dst.a) * a));
            blend = halfValue(a / alpha);
        }
    }
    precise vec3 color = blend == 1.0 ? src.rgb : roundedProduct(src.rgb - dst.rgb, blend) + dst.rgb;
    if ((channels & 1) == 0) color.r = dst.r;
    if ((channels & 2) == 0) color.g = dst.g;
    if ((channels & 4) == 0) color.b = dst.b;
    return vec4(color, alpha);
}
vec4 eraseHalf(vec4 dst, vec4 src, float opacity, float coverage, bool masked)
{
    // KoCompositeOpErase<half> rounds coverage, unlike scalar Normal.
    float a = masked ? halfValue(src.a * halfValue(coverage)) : src.a;
    a = halfValue(a * halfValue(opacity));
    dst.a = halfValue(halfValue(1.0 - a) * dst.a);
    return dst;
}
#endif
