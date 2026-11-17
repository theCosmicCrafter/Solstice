/*
 *  shader for handling scaling
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

uniform sampler2D texture0;

in vec4 v_textureCoordinate;
out vec4 fragColor;

const float eps = 1e-6;

#if defined HIGHQ_SCALING || defined DIRECT_LOD_FETCH
uniform float viewportScale;
#endif /* defined HIGHQ_SCALING || defined DIRECT_LOD_FETCH */

#ifdef DIRECT_LOD_FETCH
uniform float fixedLodLevel;
#endif

#ifdef HIGHQ_SCALING

uniform float texelSize;

vec4 filterPureLinear8(sampler2D texture, vec2 texcoord)
{
    float newTexelSize = texelSize / viewportScale;
    float support = 0.5 * newTexelSize;
    float step = texelSize * 1.0;

    float offset = support - 0.5*texelSize;

    float level = 0.0;


    if (viewportScale < 0.03125) {
        level = 4.0;
    } else if (viewportScale < 0.0625) {
        level = 3.0;
    } else if (viewportScale < 0.125) {
        level = 2.0;
    } else if (viewportScale < 0.25) {
        level = 1.0;
    }

/*
    vec4 p1 = textureLod(texture, vec2(texcoord.x - offset, texcoord.y - offset), level);
    vec4 p2 = 2.0*textureLod(texture, vec2(texcoord.x         , texcoord.y - offset), level);
    vec4 p3 = textureLod(texture, vec2(texcoord.x + offset, texcoord.y - offset), level);

    vec4 p4 = 2.0*textureLod(texture, vec2(texcoord.x - offset, texcoord.y), level);
    vec4 p5 = 5.0*textureLod(texture, vec2(texcoord.x         , texcoord.y), level);
    vec4 p6 = 2.0*textureLod(texture, vec2(texcoord.x + offset, texcoord.y), level);

    vec4 p7 = textureLod(texture, vec2(texcoord.x - offset, texcoord.y + offset), level);
    vec4 p8 = 2.0*textureLod(texture, vec2(texcoord.x         , texcoord.y + offset), level);
    vec4 p9 = textureLod(texture, vec2(texcoord.x + offset, texcoord.y + offset), level);

    return (p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9) / 17.0;
*/

    vec4 p1 = textureLod(texture, vec2(texcoord.x - offset, texcoord.y - offset), level);
    vec4 p2 = textureLod(texture, vec2(texcoord.x + offset, texcoord.y - offset), level);

    vec4 p3 = vec4(4.0) * textureLod(texture, vec2(texcoord.x, texcoord.y), level);

    vec4 p4 = textureLod(texture, vec2(texcoord.x - offset, texcoord.y + offset), level);
    vec4 p5 = textureLod(texture, vec2(texcoord.x + offset, texcoord.y + offset), level);

    vec4 p6 = vec4(3.0) * textureLod(texture, vec2(texcoord.x, texcoord.y), level + 1.0);

    return (p1 + p2 + p3 + p4 + p5 + p6) / vec4(11.0);

}


// 4-tap bicubic B-spline upscale for high zoom (viewportScale > 1).
// GL_LINEAR must be bound for this to work — each tap relies on
// hardware bilinear between two texels; the cubic weighting lives in
// the 4-sample blend. Gives visibly smoother edges than plain bilinear
// at 2x+ (Procreate-style stroke smoothing) at ~4 texture reads.
uniform float smoothUpscale;

vec4 filterBicubicUpscale(sampler2D tex, vec2 uv, float tSize)
{
    vec2 texSize = vec2(1.0) / vec2(tSize);
    vec2 t = uv * texSize - 0.5;
    vec2 i = floor(t);
    vec2 f = fract(t);
    vec2 f2 = f * f;
    vec2 f3 = f2 * f;
    // B-spline basis weights
    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    vec2 w3 = f3 / 6.0;
    // Merge each axis' 2x2 taps into 1 bilinear tap each
    vec2 g0 = w0 + w1;
    vec2 g1 = w2 + w3;
    vec2 h0 = i - 1.0 + w1 / max(g0, vec2(eps));
    vec2 h1 = i + 1.0 + w3 / max(g1, vec2(eps));
    // h0/h1 are texel-index coordinates: texel k centers at index k,
    // so UV = (index + 0.5) * tSize — the +0.5 keeps taps on texel centers
    vec2 s00 = (vec2(h0.x, h0.y) + 0.5) * tSize;
    vec2 s10 = (vec2(h1.x, h0.y) + 0.5) * tSize;
    vec2 s01 = (vec2(h0.x, h1.y) + 0.5) * tSize;
    vec2 s11 = (vec2(h1.x, h1.y) + 0.5) * tSize;
    return g0.x * g0.y * texture(tex, s00)
         + g1.x * g0.y * texture(tex, s10)
         + g0.x * g1.y * texture(tex, s01)
         + g1.x * g1.y * texture(tex, s11);
}

// Contrast-Adaptive Sharpening (FidelityFX CAS)
// Delivers game-engine edge-adaptive clarity for viewports without halo artifacts or overshooting
vec4 filterCAS(sampler2D tex, vec2 uv, float tSize, float vScale)
{
    // 5-tap cross pattern: center (e) and cardinal neighbors
    vec4 a = texture(tex, uv + vec2(0.0, -tSize));
    vec4 b = texture(tex, uv + vec2(-tSize, 0.0));
    vec4 e = texture(tex, uv);
    vec4 d = texture(tex, uv + vec2(tSize, 0.0));
    vec4 c = texture(tex, uv + vec2(0.0, tSize));

    // Per-channel local min and max across neighborhood
    vec3 minRgb = min(min(min(a.rgb, b.rgb), min(d.rgb, c.rgb)), e.rgb);
    vec3 maxRgb = max(max(max(a.rgb, b.rgb), max(d.rgb, c.rgb)), e.rgb);

    // Smooth reciprocal approximation for adaptive edge weight
    vec3 ampRgb = clamp(min(minRgb, 2.0 - maxRgb) / max(maxRgb, vec3(eps)), 0.0, 1.0);

    // Sharpen only minified views: full strength below ~97%, ramps to
    // zero approaching 100% so a 1:1 view stays pixel-true, and stays
    // off at >=100% where magnification/bicubic takes over.
    float zoomFade = clamp(2.0 - vScale, 0.0, 1.0)
                   * smoothstep(0.0, 0.03, 1.0 - vScale);
    // Never sharpen against (semi-)transparent neighbours: their RGB is arbitrary and would create halos
    zoomFade *= smoothstep(0.0, 0.999, min(min(a.a, b.a), min(min(c.a, d.a), e.a)));
    vec3 wRgb = -sqrt(ampRgb) * (0.16 * zoomFade);

    // Filter resolve
    vec3 colRgb = (a.rgb + b.rgb + d.rgb + c.rgb) * wRgb + e.rgb;
    colRgb /= (4.0 * wRgb + vec3(1.0));

    // clamp below at 0 to guard sharpening undershoot, but do NOT clamp
    // above: HDR canvas textures (RGBA16F/32F) carry values > 1 that the
    // display transform (OCIO) maps, clipping here destroys highlights
    return vec4(max(colRgb, vec3(0.0)), e.a);
}

#endif /* HIGHQ_SCALING */

void main() {
    vec4 col;

#if defined HIGHQ_SCALING || defined DIRECT_LOD_FETCH

    if (viewportScale < 0.5 - eps) {

#ifdef DIRECT_LOD_FETCH

        if (fixedLodLevel > eps) {
            col = textureLod(texture0, v_textureCoordinate.st, fixedLodLevel);
        } else

#endif /* DIRECT_LOD_FETCH */

        {

#ifdef HIGHQ_SCALING
            col = filterPureLinear8(texture0, v_textureCoordinate.st);
#else /* HIGHQ_SCALING */
            col = texture(texture0, v_textureCoordinate.st);
#endif /* HIGHQ_SCALING */

        }
    } else

#endif /* defined HIGHQ_SCALING || defined DIRECT_LOD_FETCH */

    {
#ifdef DIRECT_LOD_FETCH

        if (fixedLodLevel > eps) {
            col = textureLod(texture0, v_textureCoordinate.st, fixedLodLevel);
        } else

#endif /* DIRECT_LOD_FETCH */
        {
#ifdef HIGHQ_SCALING
            if (viewportScale > 1.0 + eps && smoothUpscale > 0.5 && texelSize > eps) {
                col = filterBicubicUpscale(texture0, v_textureCoordinate.st, texelSize);
            } else {
                col = (texelSize > eps) ? filterCAS(texture0, v_textureCoordinate.st, texelSize, viewportScale) : texture(texture0, v_textureCoordinate.st);
            }
#else /* HIGHQ_SCALING */
            col = texture(texture0, v_textureCoordinate.st);
#endif /* HIGHQ_SCALING */
        }
    }

#ifdef USE_OCIO
    fragColor = OCIODisplay(col);
#else /* USE_OCIO */
    fragColor = col;
#endif /* USE_OCIO */

}
