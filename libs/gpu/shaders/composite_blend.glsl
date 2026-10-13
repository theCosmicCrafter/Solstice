/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Shared CPU-compatible RGBA blend functions for layers and dabs.
 */
// Must match KisGpuBlendOp in KisGpuLayerCompositor.h
const uint OpOver = 0;
const uint OpMultiply = 1;
const uint OpScreen = 2;
const uint OpAdd = 3;
const uint OpSubtract = 4;
const uint OpDarken = 5;
const uint OpLighten = 6;
const uint OpDifference = 7;
const uint OpOverlay = 8;
const uint OpHardLight = 9;
const uint OpExclusion = 10;
const uint OpErase = 11;
const uint OpLinearBurn = 12;
const uint OpLinearLight = 13;
const uint OpPinLight = 14;
const uint OpSoftLightSvg = 15;
const uint OpSoftLightPhotoshop = 16;
const uint OpColorDodge = 17;
const uint OpColorBurn = 18;
const uint OpDivide = 19;
const uint OpVividLight = 20;
const uint OpHardMix = 21;
const uint OpHardMixPhotoshop = 22;
const uint OpHardMixSofterPhotoshop = 23;
const uint OpGrainMerge = 24;
const uint OpGrainExtract = 25;
const uint OpNegation = 26;
const uint OpAllanon = 27;
const uint OpHue = 28;
const uint OpSaturation = 29;
const uint OpColor = 30;
const uint OpLuminosity = 31;
const uint OpDarkerColor = 32;
const uint OpLighterColor = 33;
// Phase 4.107: the other HSX modes; HSI, HSL and HSV keep the order
// Color, Hue, Saturation, Increase/Decrease Saturation, Lightness,
// Increase/Decrease Lightness (blendHSY() decodes it).
const uint OpTint = 34;
const uint OpIncreaseSaturation = 35;
const uint OpDecreaseSaturation = 36;
const uint OpIncreaseLuminosity = 37;
const uint OpDecreaseLuminosity = 38;
const uint OpColorHsi = 39;

// Modes that amplify rounding differences over repeated dabs (Color Burn
// first; phase 4.107 Vivid Light, Hard Mix Softer and the HSX modes): the
// alpha, weighted colour, interpolation and division follow the CPU's
// rounding exactly.
bool exactBlend(uint op)
{
    return op == OpColorBurn || op == OpVividLight || op == OpHardMixSofterPhotoshop || op >= OpHue;
}

// a * b rounded to float, in a form the driver cannot fuse into a following
// addition: NVIDIA fused "l + t * s" into one fma despite NoContraction
// (phase 4.107), which changes results at rounding ties.
float roundedProduct(float a, float b)
{
    precise float p = a * b;
    precise float error = fma(a, b, -p);
    return fma(a, b, -error);
}
vec3 roundedProduct(vec3 a, float b)
{
    return vec3(roundedProduct(a.r, b), roundedProduct(a.g, b), roundedProduct(a.b, b));
}
vec3 roundedProduct(vec3 a, vec3 b)
{
    return vec3(roundedProduct(a.r, b.r), roundedProduct(a.g, b.g), roundedProduct(a.b, b.b));
}

// Arithmetic::mul(a, b, c) for half: the product in double (the composite
// type), rounded once (a * b is exact with fma; the c term rounds at most once)
float mul3(float a, float b, float c)
{
    precise float p = a * b;
    precise float e = fma(a, b, -p);
    precise float value = fma(p, c, e * c);
    return value;
}
vec3 mul3(float a, float b, vec3 c)
{
    return vec3(mul3(a, b, c.r), mul3(a, b, c.g), mul3(a, b, c.b));
}

float maskCoverage(uint value, bool exact)
{
    // Match the CPU's float(byte) / 255 lookup. A rounded reciprocal multiply
    // alone differs by one ULP for some bytes; repeated HDR dabs amplify it.
    precise float scaled = float(value) * (1.0 / 255.0);
    // Retain the established rounding of existing modes. The new modes use
    // the CPU lookup's exact conversion, needed for repeated Linear Light.
    if (!exact) return scaled;
    float residual = fma(-scaled, 255.0, float(value));
    return fma(residual, 1.0 / 255.0, scaled);
}

// qFuzzyIsNull(float)
bool fuzzyZero(float v)
{
    return abs(v) <= 0.00001;
}

// qFuzzyCompare(v, 1.0f)
bool fuzzyUnit(float v)
{
    return abs(v - 1.0) * 100000.0 <= min(abs(v), 1.0);
}

// qFuzzyCompare(v, 0.5f)
bool fuzzyHalf(float v)
{
    return abs(v - 0.5) * 100000.0 <= min(abs(v), 0.5);
}

float hardLight(float s, float d)
{
#ifdef TILE_F16
    if (abs(s - 0.5) < 0.001) return d;
    float s2 = s + s;
    if (s > 0.5) {
        s2 = float(float16_t(s2 - 1.0));
        return s2 + d - float(float16_t(s2 * d));
    }
    return float(float16_t(s2)) * d;
#else
    if (fuzzyHalf(s)) {
        return d;
    }
    float s2 = s + s;
    if (s > 0.5) {
        s2 -= 1.0;
        return s2 + d - s2 * d;
    }
    return s2 * d;
#endif
}

float finiteSDRDivision(float numerator, float denominator)
{
    float value = numerator / denominator;
    return isinf(value) || isnan(value) ? 1.0 : value;
}

// a / b rounded like an IEEE division (the GPU divides by a reciprocal)
float exactQuotient(float a, float b)
{
    precise float q = a / b;
    if (isinf(q) || isnan(q) || b == 0.0) return q;
    precise float residual = fma(-q, b, a);
    precise float reciprocal = 1.0 / b;
    precise float value = fma(residual, reciprocal, q);
    return value;
}

// 1 - a / b computed in double and rounded to float once, as
// CFVividLight does in its composite type (0 for an infinite quotient)
float oneMinusQuotient(float a, float b)
{
    const float q = exactQuotient(a, b);
    if (isinf(q) || isnan(q)) return 0.0;
    precise float residual = fma(-q, b, a); // exact: a - q * b
    precise float low = residual / b;       // a / b - q
    precise float t = 1.0 - q;
    // TwoSum: the rounding error of t
    precise float tb = t - 1.0;
    precise float error = (1.0 - (t - tb)) + (-q - tb);
    precise float value = t + (error - low);
    return value;
}

float colorDodge(float s, float d)
{
    if (s == 1.0) return d <= 0.0 ? 0.0 : 1.0;
#ifdef TILE_F16
    // Arithmetic::inv<half> rounds before the division.
    return clamp(finiteSDRDivision(d, float(float16_t(1.0 - s))), 0.0, 1.0);
#else
    return clamp(finiteSDRDivision(d, 1.0 - s), 0.0, 1.0);
#endif
}

float colorBurn(float s, float d)
{
    if (d == 1.0) return 1.0;
#ifdef TILE_F16
    // Both inv(dst) and clampToSDR<half>(quotient) return half values.
    float value = float(float16_t(1.0 - d)) / s;
    if (isinf(value) || isnan(value)) return 0.0;
    return 1.0 - float(float16_t(clamp(value, 0.0, 1.0)));
#else
    precise float numerator = 1.0 - d;
    precise float value = numerator / s;
    if (isinf(value) || isnan(value)) return 0.0;
    // Recover the rounding lost by the GPU's reciprocal-based division.
    float residual = fma(-value, s, numerator);
    value = fma(residual, 1.0 / s, value);
    return 1.0 - clamp(value, 0.0, 1.0);
#endif
}

float blendChannel(uint op, float s, float d)
{
    switch (op) {
    case OpMultiply:
        return s * d;
    case OpScreen:
#ifdef TILE_F16
        return s + d - float(float16_t(s * d));
#else
        return s + d - s * d;
#endif
    case OpAdd:
        return s + d;
    case OpSubtract:
        return d - s;
    case OpDarken:
        return min(s, d);
    case OpLighten:
        return max(s, d);
    case OpDifference:
        return max(s, d) - min(s, d);
    case OpOverlay:
        return hardLight(d, s);
    case OpHardLight:
        return hardLight(s, d);
    case OpExclusion: {
#ifdef TILE_F16
        // CFExclusion stores Arithmetic::mul<half> before widening to double.
        const float x = float(float16_t(s * d));
#else
        const float x = s * d;
#endif
        return d + s - (x + x);
    }
    case OpLinearBurn:
        // CFLinearBurn<..., ClampAsFloatHDR>: no upper clamp on the result.
        return max(s + d - 1.0, 0.0);
    case OpLinearLight: {
        // cfLinearLight's Arithmetic::clamp<float> leaves HDR/negative values intact.
        // Preserve CPU rounding before interpolation, even across many dabs.
        precise float result = (s + s + d) - 1.0;
        return result;
    }
    case OpPinLight:
        return max(s + s - 1.0, min(d, s + s));
#ifndef BASIC_BLEND_ONLY
    case OpSoftLightSvg:
    case OpSoftLightPhotoshop:
        if (s > 0.5) {
            float curve = op == OpSoftLightSvg && d <= 0.25 ? ((16.0 * d - 12.0) * d + 4.0) * d : sqrt(d);
            return d + (2.0 * s - 1.0) * (curve - d);
        }
        return d - (1.0 - 2.0 * s) * d * (1.0 - d);
    case OpColorDodge:
        return colorDodge(s, d);
    case OpColorBurn:
        return colorBurn(s, d);
    case OpDivide:
        if (d == 0.0) return 0.0;
        return s == 0.0 ? 1.0 : min(d / s, 1.0);
    case OpVividLight:
#ifdef TILE_F16
        // CFVividLight<half>: inv() returns a half; the quotient is exact
        // (double on the CPU) before the half rounding of blendColor().
        if (s < 0.5) {
            if (s == 0.0) return d >= 1.0 ? 1.0 : 0.0;
            return clamp(oneMinusQuotient(float(float16_t(1.0 - d)), s + s), 0.0, 1.0);
        }
        if (s == 1.0) return d <= 0.0 ? 0.0 : 1.0;
        {
            float inverse = float(float16_t(1.0 - s));
            float q = exactQuotient(d, inverse + inverse);
            return isinf(q) || isnan(q) ? 1.0 : clamp(q, 0.0, 1.0);
        }
#else
        // CFVividLight<float>: float arithmetic with IEEE divisions (phase 4.107)
        if (s < 0.5) {
            if (s == 0.0) return d >= 1.0 ? 1.0 : 0.0;
            precise float inverse = 1.0 - d;
            const float q = exactQuotient(inverse, s + s);
            if (isinf(q) || isnan(q)) return 0.0;
            precise float value = 1.0 - q;
            return clamp(value, 0.0, 1.0);
        }
        if (s == 1.0) return d <= 0.0 ? 0.0 : 1.0;
        {
            precise float inverse = 1.0 - s;
            const float q = exactQuotient(d, inverse + inverse);
            return isinf(q) || isnan(q) ? 1.0 : clamp(q, 0.0, 1.0);
        }
#endif
    case OpHardMix:
        return d > 0.5 ? colorDodge(s, d) : colorBurn(s, d);
    case OpHardMixPhotoshop:
        return s + d > 1.0 ? 1.0 : 0.0;
    case OpHardMixSofterPhotoshop: {
#ifdef TILE_F16
        // Arithmetic::inv<half> returns a half; the rest is exact in float.
        precise float value = 3.0 * d - 2.0 * float(float16_t(1.0 - s));
        return clamp(value, 0.0, 1.0);
#else
        // float on the CPU, without a fused multiply-add
        precise float inverse = 1.0 - s;
        precise float value = roundedProduct(3.0, d) - 2.0 * inverse;
        return clamp(value, 0.0, 1.0);
#endif
    }
    case OpGrainMerge:
        return clamp(d + s - 0.5, 0.0, 1.0);
    case OpGrainExtract:
        return clamp(d - s + 0.5, 0.0, 1.0);
    case OpNegation:
        return 1.0 - abs(1.0 - s - d);
    case OpAllanon:
        return (s + d) * 0.5;
#endif
    }
    return s;
}

vec3 blendColor(uint op, vec3 s, vec3 d)
{
    vec3 color = vec3(blendChannel(op, s.r, d.r), blendChannel(op, s.g, d.g), blendChannel(op, s.b, d.b));
#ifdef TILE_F16
    color = vec3(f16vec3(color));
#endif
    return color;
}

bool blendZero(uint op, float value)
{
#ifdef TILE_F16
    return abs(value) < 0.002;
#endif
    return fuzzyZero(value);
}

bool blendUnit(uint op, float value)
{
#ifdef TILE_F16
    return abs(value - 1.0) < 0.002;
#endif
    return fuzzyUnit(value);
}

float blendAlpha(uint op, float s, float d)
{
#ifdef TILE_F16
    return float(float16_t(s + d - float(float16_t(s * d))));
#endif
#ifndef BASIC_BLEND_ONLY
    if (exactBlend(op)) {
        precise float alpha = s + d - roundedProduct(s, d);
        return alpha;
    }
#endif
    return s + d - s * d;
}

vec3 blendWeightedColor(uint op, vec4 dst, vec4 src, float srcAlpha, vec3 blended)
{
#ifdef TILE_F16
#ifndef BASIC_BLEND_ONLY
    if (exactBlend(op)) {
        vec3 a = vec3(f16vec3(mul3(float(float16_t(1.0 - srcAlpha)), dst.a, dst.rgb)));
        vec3 b = vec3(f16vec3(mul3(float(float16_t(1.0 - dst.a)), srcAlpha, src.rgb)));
        vec3 c = vec3(f16vec3(mul3(dst.a, srcAlpha, blended)));
        precise vec3 sum = a + b + c;
        return vec3(f16vec3(sum));
    }
#endif
    {
        // Arithmetic::blend<half> rounds each multiply result and the sum.
        vec3 a = vec3(f16vec3((float(float16_t(1.0 - srcAlpha)) * dst.a) * dst.rgb));
        vec3 b = vec3(f16vec3((float(float16_t(1.0 - dst.a)) * srcAlpha) * src.rgb));
        vec3 c = vec3(f16vec3((dst.a * srcAlpha) * blended));
        return vec3(f16vec3(a + b + c));
    }
#endif
#ifndef BASIC_BLEND_ONLY
    if (exactBlend(op)) {
        precise float da = (1.0 - srcAlpha) * dst.a;
        precise float sa = (1.0 - dst.a) * srcAlpha;
        precise float both = dst.a * srcAlpha;
        precise vec3 result = roundedProduct(dst.rgb, da) + roundedProduct(src.rgb, sa) + roundedProduct(blended, both);
        return result;
    }
#endif
    return (1.0 - srcAlpha) * dst.a * dst.rgb + (1.0 - dst.a) * srcAlpha * src.rgb + dst.a * srcAlpha * blended;
}

vec3 clampSource(uint op, vec3 s)
{
#ifndef BASIC_BLEND_ONLY
    if (op == OpDivide) return max(s, 0.0);
    if (op >= OpSoftLightSvg && op <= OpNegation) return clamp(s, 0.0, 1.0);
#endif
    // Clamp policies of the functors (KoCompositeOpGenericFunctorBase.h)
    if (op == OpOverlay || op == OpHardLight || op == OpExclusion || op == OpLinearBurn || op == OpPinLight) {
        return clamp(s, 0.0, 1.0);
    }
    return s;
}

vec3 clampDestination(uint op, vec3 d)
{
#ifndef BASIC_BLEND_ONLY
    if (op == OpDivide) return max(d, 0.0);
    if (op == OpSoftLightSvg || op == OpSoftLightPhotoshop || op == OpColorBurn ||
        (op >= OpHardMixPhotoshop && op <= OpNegation)) return clamp(d, 0.0, 1.0);
#endif
    if (op == OpExclusion || op == OpPinLight) {
        return clamp(d, 0.0, 1.0);
    }
    return d;
}

vec3 interpolateBlend(uint op, vec3 a, vec3 b, float alpha)
{
    if (op == OpLinearLight
#ifndef BASIC_BLEND_ONLY
        || exactBlend(op)
#endif
    ) {
        // Repeated unbounded/nonlinear blending amplifies rounding differences.
        precise vec3 result = roundedProduct(b - a, alpha) + a;
        return result;
    }
    return (b - a) * alpha + a;
}

#ifndef BASIC_BLEND_ONLY
// HSX families of KoColorSpaceMaths.h (HSYType, HSIType, HSLType, HSVType)
// and the HSL functors of KoCompositeOpFunctions.h, in float like the CPU.
const uint HsxY = 0;
const uint HsxI = 1;
const uint HsxL = 2;
const uint HsxV = 3;
const float FloatEpsilon = 1.192092896e-7;

float max3(vec3 c) { return max(max(c.r, c.g), c.b); }
float min3(vec3 c) { return min(min(c.r, c.g), c.b); }

float hsxLightness(uint type, vec3 c)
{
    precise float light;
    if (type == HsxI) {
        light = (c.r + c.g + c.b) * 0.33333333333333333333;
    } else if (type == HsxL) {
        light = (max3(c) + min3(c)) * 0.5;
    } else if (type == HsxV) {
        light = max3(c);
    } else {
        light = roundedProduct(0.299, c.r) + roundedProduct(0.587, c.g) + roundedProduct(0.114, c.b);
    }
    return light;
}

float hsxSaturation(uint type, vec3 c)
{
    const float high = max3(c);
    const float low = min3(c);
    precise float chroma = high - low;
    if (type == HsxI) {
        if (chroma <= FloatEpsilon) return 0.0;
        precise float value = 1.0 - exactQuotient(low, hsxLightness(HsxI, c));
        return value;
    }
    if (type == HsxL) {
        precise float light = (high + low) * 0.5;
        precise float div = 1.0 - abs(2.0 * light - 1.0);
        return div > FloatEpsilon ? exactQuotient(chroma, div) : 0.0;
    }
    if (type == HsxV) return high > FloatEpsilon ? exactQuotient(chroma, high) : 0.0;
    return chroma;
}

// setSaturation(): the same sort, so that ties pick the same channels
vec3 hsxSetSaturation(vec3 c, float saturation)
{
    float rgb[3] = float[3](c.r, c.g, c.b);
    int low = 0, mid = 1, high = 2, swap;
    if (rgb[mid] < rgb[low]) { swap = low; low = mid; mid = swap; }
    if (rgb[high] < rgb[mid]) { swap = mid; mid = high; high = swap; }
    if (rgb[mid] < rgb[low]) { swap = low; low = mid; mid = swap; }
    precise float range = rgb[high] - rgb[low];
    if (range <= FloatEpsilon) return vec3(0.0);
    precise float scaled = (rgb[mid] - rgb[low]) * saturation;
    rgb[mid] = exactQuotient(scaled, range);
    rgb[high] = saturation;
    rgb[low] = 0.0;
    return vec3(rgb[0], rgb[1], rgb[2]);
}

// ToneMapping(): the original lightness and extrema for both corrections
vec3 hsxToneMapping(uint type, vec3 c)
{
    const float light = hsxLightness(type, c);
    const float low = min3(c);
    const float high = max3(c);
    if (low < 0.0) {
        precise float stretch = light - low;
        if (light <= 0.00001 || stretch < FloatEpsilon) {
            c = vec3(0.0);
        } else {
            const float scale = exactQuotient(1.0, stretch);
            precise vec3 value = light + roundedProduct((c - light) * light, scale);
            c = value;
        }
    }
    if (high > 1.0) {
        precise float stretch = high - light;
        if (light > 1.0 || stretch < FloatEpsilon) {
            // lightnessIsAverage is false only for HSV
            c = type == HsxV ? min(c, vec3(1.0)) : vec3(1.0);
        } else {
            precise float inverse = 1.0 - light;
            const float scale = exactQuotient(1.0, stretch);
            precise vec3 value = light + roundedProduct((c - light) * inverse, scale);
            c = value;
        }
    }
    return c;
}

vec3 hsxAddLightness(uint type, vec3 c, float light)
{
    precise vec3 value = c + light;
    return hsxToneMapping(type, value);
}

vec3 hsxSetLightness(uint type, vec3 c, float light)
{
    precise float delta = light - hsxLightness(type, c);
    return hsxAddLightness(type, c, delta);
}

vec3 blendHSY(uint op, vec3 s, vec3 d)
{
    if (op == OpDarkerColor) return hsxLightness(HsxY, d) > hsxLightness(HsxY, s) ? s : d;
    if (op == OpLighterColor) return hsxLightness(HsxY, d) < hsxLightness(HsxY, s) ? s : d;
    if (op == OpTint) {
        // CFTint: black becomes the source colour, white stays
        const float light = hsxLightness(HsxY, d);
        precise float inverse = 1.0 - light;
        precise vec3 value = light + roundedProduct(s, inverse);
        return value;
    }
    // Function: 0 Color, 1 Hue, 2 Saturation, 3 Increase and 4 Decrease
    // Saturation, 5 Lightness, 6 Increase and 7 Decrease Lightness
    uint type = HsxY;
    uint function = 7;
    if (op >= OpColorHsi) {
        type = HsxI + (op - OpColorHsi) / 8;
        function = (op - OpColorHsi) % 8;
    } else if (op == OpColor) {
        function = 0;
    } else if (op == OpHue) {
        function = 1;
    } else if (op == OpSaturation) {
        function = 2;
    } else if (op == OpIncreaseSaturation) {
        function = 3;
    } else if (op == OpDecreaseSaturation) {
        function = 4;
    } else if (op == OpLuminosity) {
        function = 5;
    } else if (op == OpIncreaseLuminosity) {
        function = 6;
    }
    const float light = hsxLightness(type, d);
    vec3 c;
    if (function == 0) {
        c = hsxSetLightness(type, s, light);
    } else if (function == 1) {
        c = hsxSetLightness(type, hsxSetSaturation(s, hsxSaturation(type, d)), light);
    } else if (function <= 4) {
        const float source = hsxSaturation(type, s);
        const float destination = hsxSaturation(type, d);
        // Arithmetic::lerp<float>(a, b, t): (b - a) * t + a
        precise float increased = roundedProduct(1.0 - destination, source) + destination;
        precise float decreased = destination * source;
        const float saturation = function == 2 ? source : (function == 3 ? increased : decreased);
        c = hsxSetLightness(type, hsxSetSaturation(d, saturation), light);
    } else if (function == 5) {
        c = hsxSetLightness(type, d, hsxLightness(type, s));
    } else if (function == 6) {
        c = hsxAddLightness(type, d, hsxLightness(type, s));
    } else {
        precise float delta = hsxLightness(type, s) - 1.0;
        c = hsxAddLightness(type, d, delta);
    }
    // possiblyFixNegativeValuesNearZeroPoint()
    return max(c, 0.0);
}

vec4 compositeHSY(uint op, vec4 dst, vec4 src, float srcAlpha, bool alphaLocked)
{
    // Unlike the separable functor, GenericHSL uses strict alpha comparisons
    // and interpolates the ORIGINAL (unclamped) source/destination colors.
    if (alphaLocked && dst.a == 0.0) return dst;
    float alpha = alphaLocked ? dst.a : blendAlpha(op, srcAlpha, dst.a);
    if (alpha == 0.0) return vec4(dst.rgb, alpha);
    vec3 blended = blendHSY(op, clamp(src.rgb, 0.0, 1.0), clamp(dst.rgb, 0.0, 1.0));
#ifdef TILE_F16
    blended = vec3(f16vec3(blended));
#endif
    if (alphaLocked) {
        precise vec3 value = roundedProduct(blended - dst.rgb, srcAlpha) + dst.rgb;
        return vec4(value, alpha);
    }
    vec3 result = blendWeightedColor(op, dst, src, srcAlpha, blended);
    // div() rounds like an IEEE division (double for half)
    precise vec3 quotient = result / alpha;
    vec3 residual = fma(-quotient, vec3(alpha), result);
    return vec4(fma(residual, vec3(1.0 / alpha), quotient), alpha);
}

#endif

vec4 compositeGenericColor(uint op, vec4 dst, vec4 src, float srcAlpha, bool alphaLocked)
{
#ifndef BASIC_BLEND_ONLY
    if (op >= OpHue) return compositeHSY(op, dst, src, srcAlpha, alphaLocked);
#endif
    const float dstAlpha = dst.a;
    if (blendZero(op, srcAlpha)) {
        return dst;
    }

    const vec3 s = clampSource(op, src.rgb);
    if (alphaLocked) {
        if (!blendZero(op, dstAlpha)) {
            const vec3 d = clampDestination(op, dst.rgb);
            return vec4(interpolateBlend(op, d, blendColor(op, s, d), srcAlpha), dstAlpha);
        }
        return dst;
    }
    if (blendZero(op, dstAlpha)) {
        return vec4(s, srcAlpha);
    }

    const vec3 d = clampDestination(op, dst.rgb);
    const vec3 cf = blendColor(op, s, d);
    if (blendUnit(op, dstAlpha)) {
        return vec4(interpolateBlend(op, d, cf, srcAlpha), 1.0);
    }
    if (blendUnit(op, srcAlpha)) {
        return vec4(interpolateBlend(op, s, cf, dstAlpha), 1.0);
    }

    const float newAlpha = blendAlpha(op, srcAlpha, dstAlpha);
    if (blendZero(op, newAlpha)) {
        return vec4(dst.rgb, newAlpha);
    }
    const vec3 result = blendWeightedColor(op, vec4(d, dstAlpha), vec4(s, src.a), srcAlpha, cf);
#ifndef BASIC_BLEND_ONLY
    if (exactBlend(op)) {
        precise vec3 quotient = result / newAlpha;
        vec3 residual = fma(-quotient, vec3(newAlpha), result);
        return vec4(fma(residual, vec3(1.0 / newAlpha), quotient), newAlpha);
    }
#endif
    return vec4(result / newAlpha, newAlpha);
}

vec4 compositeGeneric(uint op, vec4 dst, vec4 src, float opacity, bool alphaLocked, float coverage, uint channels)
{
    alphaLocked = alphaLocked || (channels & 8) == 0;
    // KoCompositeOpBase clears hidden RGB for ANY restricted channel set,
    // before testing source alpha/coverage (including zero coverage).
    if ((alphaLocked || channels != 15) && dst.a == 0.0) dst = vec4(0.0);
    float srcAlpha = (src.a * coverage) * opacity;
#ifndef BASIC_BLEND_ONLY
    if (exactBlend(op)) {
        precise float alpha = (src.a * coverage) * opacity;
        srcAlpha = alpha;
    }
#endif
#ifdef TILE_F16
    // Generic CPU operations use half alpha, opacity and intermediates for
    // every blend mode, including the established basic shader family.
    srcAlpha = float(float16_t((src.a * float(float16_t(coverage))) * float(float16_t(opacity))));
#endif
    vec4 result = compositeGenericColor(op, dst, src, srcAlpha, alphaLocked);
    if ((channels & 1) == 0) result.r = dst.r;
    if ((channels & 2) == 0) result.g = dst.g;
    if ((channels & 4) == 0) result.b = dst.b;
    return result;
}
