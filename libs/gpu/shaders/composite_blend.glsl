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
// Phase 4.108: the remaining modes (separable 63-106, HSL functors 107-110,
// special composite ops 111-116).
const uint OpHardMixHdr = 63;
const uint OpColorDodgeHdr = 64;
const uint OpVividLightHdr = 65;
const uint OpHardOverlay = 66;
const uint OpHardOverlayHdr = 67;
const uint OpGeometricMean = 68;
const uint OpParallel = 69;
const uint OpInterpolation = 70;
const uint OpInterpolation2x = 71;
const uint OpPenumbraA = 72;
const uint OpPenumbraB = 73;
const uint OpPenumbraC = 74;
const uint OpPenumbraD = 75;
const uint OpSoftLightIfsIllusions = 76;
const uint OpSoftLightPegtopDelphi = 77;
const uint OpGammaLight = 78;
const uint OpGammaIllumination = 79;
const uint OpFlatLight = 80;
const uint OpPNormA = 81;
const uint OpPNormB = 82;
const uint OpSuperLight = 83;
const uint OpTintIfsIllusions = 84;
const uint OpFogLightenIfsIllusions = 85;
const uint OpEasyDodge = 86;
const uint OpGammaDark = 87;
const uint OpShadeIfsIllusions = 88;
const uint OpFogDarkenIfsIllusions = 89;
const uint OpEasyBurn = 90;
const uint OpInverseSubtract = 91;
const uint OpModulo = 92;
const uint OpModuloShift = 93;
const uint OpModuloShiftContinuous = 94;
const uint OpArcTangent = 95;
const uint OpEquivalence = 96;
const uint OpAdditiveSubtractive = 97;
const uint OpReflect = 98;
const uint OpGlow = 99;
const uint OpFreeze = 100;
const uint OpHeat = 101;
const uint OpGleat = 102;
const uint OpHelow = 103;
const uint OpReeze = 104;
const uint OpFrect = 105;
const uint OpFhyrd = 106;
const uint OpLambertLighting = 107;
const uint OpLambertLightingGamma22 = 108;
const uint OpTangentNormalmap = 109;
const uint OpCombineNormal = 110;
const uint OpLuminositySai = 111;
const uint OpBehind = 112;
const uint OpGreater = 113;
const uint OpCopyRed = 114;
const uint OpCopyGreen = 115;
const uint OpCopyBlue = 116;

#ifndef BASIC_BLEND_ONLY
float blendChannelExtra(uint op, float s, float d);
bool extraClampsSource(uint op);
bool extraClampsDestination(uint op);
bool extraClampsBottom(uint op);
vec3 blendHslExtra(uint op, vec3 s, vec3 d);
#endif

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
    if (isinf(p) || isnan(p)) return p;
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
    precise float plain = p * c;
    if (isinf(plain) || isnan(plain)) return plain;
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
    return isinf(value) || isnan(value) ? q : value;
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
#ifndef BASIC_BLEND_ONLY
    if (op >= OpHardMixHdr) return blendChannelExtra(op, s, d);
#endif
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
    if (op >= OpHardMixHdr) {
        if (extraClampsSource(op)) return clamp(s, 0.0, 1.0);
        if (extraClampsBottom(op)) return max(s, 0.0);
        return s;
    }
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
    if (op >= OpHardMixHdr) {
        if (extraClampsDestination(op)) return clamp(d, 0.0, 1.0);
        if (extraClampsBottom(op) || op == OpHardOverlay || op == OpHardOverlayHdr) return max(d, 0.0);
        return d;
    }
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
    if (op >= OpLambertLighting) return blendHslExtra(op, s, d);
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

// ---- Phase 4.108: the remaining separable, HSL-functor and special modes.
// T = float follows KoColorSpaceMathsTraits<float> (float composite type);
// the qreal parts of the CPU functions are computed in float here (the
// transcendental ones have no double version in GLSL), so these modes are
// tolerance-based rather than bit-identical.
// KoColorSpaceMathsTraits<T>::max and epsilon; clamp<T>() itself is the
// identity for float and half
#ifdef TILE_F16
const float FltMax = 65504.0;
const float ChannelEpsilon = 0.0009765625; // HALF_EPSILON
#else
const float FltMax = 3.402823466e+38;
const float ChannelEpsilon = 1.192092896e-7; // FLT_EPSILON
#endif
// A T result: half values round like Imath
float tv(float v)
{
#ifdef TILE_F16
    return float(float16_t(v));
#else
    return v;
#endif
}

// kisBoundFast(): qMax(lo, qMin(hi, v)); NaN gives lo, like the CPU
float kbound(float lo, float v, float hi)
{
    const float m = hi < v ? hi : v;
    return lo < m ? m : lo;
}
vec3 kbound(float lo, vec3 v, float hi)
{
    return vec3(kbound(lo, v.r, hi), kbound(lo, v.g, hi), kbound(lo, v.b, hi));
}
float kmax(float lo, float v) { return lo < v ? v : lo; }
float quietNaN() { return uintBitsToFloat(0x7fc00000u); }
bool finiteValue(float v) { return !isinf(v) && !isnan(v); }

// pow() for x > 0 with ln(x) accurate to a few float ulps relative (GLSL
// log2() has an absolute error, useless near 1 with large exponents)
float accurateLog(float x)
{
    int e;
    float m = frexp(x, e); // [0.5, 1)
    if (m < 0.70710678) {
        m *= 2.0;
        e -= 1;
    }
    precise float t = (m - 1.0) / (m + 1.0);
    precise float t2 = t * t;
    precise float series =
        t * (2.0 + t2 * (2.0 / 3.0 + t2 * (0.4 + t2 * (2.0 / 7.0 + t2 * (2.0 / 9.0 + t2 * (2.0 / 11.0))))));
    // e * ln2 as an exact high part (ln2 to 16 bits) plus the rest
    precise float high = float(e) * 0.693145751953125;
    precise float low = float(e) * 1.4286068203094172e-6 + series;
    precise float value = high + low;
    return value;
}
// ln(x) as value + error, for products with large exponents
vec2 accurateLogParts(float x)
{
    int e;
    float m = frexp(x, e);
    if (m < 0.70710678) {
        m *= 2.0;
        e -= 1;
    }
    precise float t = (m - 1.0) / (m + 1.0);
    precise float t2 = t * t;
    precise float series =
        t * (2.0 + t2 * (2.0 / 3.0 + t2 * (0.4 + t2 * (2.0 / 7.0 + t2 * (2.0 / 9.0 + t2 * (2.0 / 11.0))))));
    precise float high = float(e) * 0.693145751953125;
    precise float low = float(e) * 1.4286068203094172e-6 + series;
    precise float value = high + low;
    precise float error = low - (value - high);
    return vec2(value, error);
}
// exp(p + e) by range reduction (GLSL exp() loses 2 ulps per unit of |x|)
float accurateExp(float p, float e)
{
    if (isinf(p) || isnan(p)) return exp(p);
    const float k = round(p * 1.4426950408889634);
    if (k > 128.0) return uintBitsToFloat(0x7f800000u);
    if (k < -160.0) return 0.0;
    precise float r = (p - k * 0.693145751953125) - k * 1.4286068203094172e-6 + e;
    return ldexp(exp(r), int(k));
}
// x^(y + yLow) for x > 0; yLow carries an exponent computed in double
float accuratePow(float x, float y, float yLow)
{
    if (isinf(x)) return y > 0.0 ? x : 0.0;
    if (x == 1.0) return 1.0;
    const vec2 l = accurateLogParts(x);
    precise float p = y * l.x;
    precise float e = fma(y, l.x, -p) + y * l.y + yLow * l.x;
    return accurateExp(p, e);
}

// std::pow() edge cases (GLSL pow() is undefined for x <= 0)
float cpow(float x, float y, float yLow)
{
    if (y == 0.0) return 1.0;
    if (isnan(x) || isnan(y)) return quietNaN();
    if (x == 0.0) return y > 0.0 ? 0.0 : uintBitsToFloat(0x7f800000u);
    if (x < 0.0) {
        if (floor(y) != y) return quietNaN();
        const float m = accuratePow(-x, y, yLow);
        return mod(y, 2.0) != 0.0 ? -m : m;
    }
    return accuratePow(x, y, yLow);
}
float cpow(float x, float y) { return cpow(x, y, 0.0); }
// 1.039999999 of cfEasyDodge/cfEasyBurn as float + rest (the CPU multiplies in double)
const float EasyFactor = 1.0399999618530273;
const float EasyFactorLow = 3.7146972609036766e-08;
float csqrt(float x) { return x < 0.0 ? quietNaN() : sqrt(x); }

// isZeroValueFuzzy<T>() / isUnitValueFuzzy<T>()
bool fuzzyZeroT(float v)
{
#ifdef TILE_F16
    return abs(v) < 0.002;
#else
    return fuzzyZero(v);
#endif
}
bool fuzzyUnitT(float v)
{
#ifdef TILE_F16
    return abs(v - 1.0) < 0.002;
#else
    return fuzzyUnit(v);
#endif
}

// qFuzzyIsNull(double) / qFuzzyCompare(double, 1.0)
bool fuzzyZeroDouble(float v) { return abs(v) <= 1e-12; }
bool fuzzyUnitDouble(float v) { return abs(v - 1.0) * 1e12 <= min(abs(v), 1.0); }

// CFColorDodge with the SDR clamp policy, IEEE division
float colorDodgeSdr(float s, float d)
{
    if (s == 1.0) return d <= 0.0 ? 0.0 : 1.0;
    const float q = exactQuotient(d, tv(1.0 - s));
    return finiteValue(q) ? tv(kbound(0.0, q, 1.0)) : 1.0;
}
// CFColorDodge<T, ClampAsFloatHDR>
float colorDodgeHdr(float s, float d)
{
    if (s == 1.0) return d <= 0.0 ? 0.0 : 1.0;
    const float q = exactQuotient(d, tv(1.0 - s));
    return finiteValue(q) ? tv(q) : FltMax;
}

float hardOverlay(float s, float d, bool hdr)
{
    if (d == 0.0) return 0.0;
    if (s == 1.0) return hdr ? FltMax : 1.0;
    if (s >= 0.5) {
        const float inverse = tv(1.0 - s);
        float q = exactQuotient(d, 2.0 * inverse);
        if (!finiteValue(q)) q = hdr ? FltMax : 1.0;
        return tv(hdr ? q : kbound(0.0, q, 1.0));
    }
    precise float p = d * (2.0 * s);
    return tv(hdr ? p : kbound(0.0, p, 1.0));
}

float interpolation(float s, float d)
{
    if (d == 0.0 && s == 0.0) return 0.0;
    const float pi = 3.14159265358979323846;
    precise float value = 0.5 - 0.25 * cos(pi * s) - 0.25 * cos(pi * d);
    return tv(value);
}

float penumbraB(float s, float d)
{
    if (d == 1.0) return 1.0;
    if (d + s < 1.0) return tv(colorDodgeSdr(d, s) / 2.0);
    if (s == 0.0) return 0.0;
    const float inverse = tv(1.0 - d);
    return tv(1.0 - tv(exactQuotient(inverse, s) / 2.0));
}

float arcTangent(float s, float d)
{
    if (s == 0.0 && d == 0.0) return 0.0;
    const float pi = 3.14159265358979323846;
    return 2.0 * atan(s, d) / pi;
}

float penumbraD(float s, float d)
{
    if (d == 1.0) return 1.0;
    return arcTangent(s, tv(1.0 - d));
}

float gammaDark(float s, float d)
{
    if (fuzzyZeroDouble(d)) return 0.0;
    if (fuzzyUnitDouble(d)) return 1.0;
    if (s == 0.0) return 0.0; // 1 / src is not finite
    return min(cpow(d, 1.0 / s), FltMax);
}

// x mod 1 of KoColorSpaceMaths<double>::modulus() for x = hi + lo (lo the
// rounding error of hi): x / (1 + DBL_EPSILON) before floor(), so a positive
// integer gives 1, zero and negative integers 0
float modOne(float hi, float lo)
{
    const float whole = floor(hi);
    // f + fe == hi - whole exactly (not exact in one float for tiny negative hi)
    precise float f = hi - whole;
    precise float fb = f - hi;
    precise float fe = (hi - (f - fb)) + (-whole - fb);
    precise float low = fe + lo;
    // t + error == f + low
    precise float t = f + low;
    precise float tb = t - f;
    precise float error = (f - (t - tb)) + (low - tb);
    const bool positive = hi > 0.0 || (hi == 0.0 && lo > 0.0);
    if (t < 0.0) {
        precise float value = t + 1.0;
        return value;
    }
    if (t > 1.0) {
        precise float value = t - 1.0;
        return value;
    }
    if (t == 1.0) {
        if (error > 0.0) return error;
        if (error < 0.0) return 1.0;
        return whole + 1.0 > 0.0 ? 1.0 : 0.0; // an integer
    }
    if (t == 0.0) {
        if (error > 0.0) return error;
        if (error < 0.0) return 1.0;
        return positive ? 1.0 : 0.0; // an integer
    }
    return t;
}
// ceil(hi + lo)
float ceilExact(float hi, float lo)
{
    const float whole = floor(hi);
    precise float t = (hi - whole) + lo;
    return whole + ceil(t);
}
float moduloShift(float s, float d)
{
    if (s == 1.0 && d == 0.0) return 0.0;
    precise float hi = d + s;
    precise float b = hi - d;
    precise float lo = (d - (hi - b)) + (s - b);
    return modOne(hi, lo);
}

// mul(), inv() and the clamped quotient are T values (half overflows to
// infinity in RGBA16F)
float glow(float s, float d)
{
    if (d == 1.0) return 1.0;
    precise float square = s * s;
    precise float inverse = 1.0 - d;
    return tv(exactQuotient(tv(square), tv(inverse)));
}
float heat(float s, float d)
{
    if (s == 1.0) return 1.0;
    if (d == 0.0) return 0.0;
    const float inverse = tv(1.0 - s);
    precise float square = inverse * inverse;
    precise float value = 1.0 - tv(exactQuotient(tv(square), d));
    return tv(value);
}
float helow(float s, float d)
{
    if (s + d > 1.0) return heat(s, d);
    if (fuzzyZeroT(s)) return 0.0;
    return glow(s, d);
}
float frect(float s, float d)
{
    if (s + d > 1.0) return heat(d, s);
    if (fuzzyZeroT(d)) return 0.0;
    return glow(d, s);
}
float gleat(float s, float d)
{
    if (fuzzyUnitT(d)) return 1.0;
    if (s + d > 1.0) return glow(s, d);
    return heat(s, d);
}

float blendChannelExtra(uint op, float s, float d)
{
    switch (op) {
    case OpHardMixHdr:
        return d > 0.5 ? colorDodgeHdr(s, d) : colorBurn(s, d);
    case OpColorDodgeHdr:
        return colorDodgeHdr(s, d);
    case OpVividLightHdr:
        if (s < 0.5) {
            if (s == 0.0) return d >= 1.0 ? d : 0.0;
            const float inverse = tv(1.0 - d);
            float q = exactQuotient(inverse, s + s);
            if (!finiteValue(q)) q = FltMax;
            precise float value = 1.0 - q;
            return tv(kmax(0.0, value));
        }
        if (s == 1.0) return d <= 0.0 ? 0.0 : 1.0;
        {
            const float inverse = tv(1.0 - s);
            const float q = exactQuotient(d, inverse + inverse);
            return finiteValue(q) ? tv(q) : FltMax;
        }
    case OpHardOverlay:
        return hardOverlay(s, d, false);
    case OpHardOverlayHdr:
        return hardOverlay(s, d, true);
    case OpGeometricMean:
        if (fuzzyUnitT(s) && fuzzyUnitT(d)) return 1.0;
        return csqrt(d * s);
    case OpParallel: {
        if (s < 1e-6 || d < 1e-6) return 0.0;
        const float inverseS = exactQuotient(1.0, s);
        const float inverseD = exactQuotient(1.0, d);
        return exactQuotient(2.0, inverseD + inverseS);
    }
    case OpInterpolation:
        return interpolation(s, d);
    case OpInterpolation2x: {
        const float i = interpolation(s, d); // a T value
        return interpolation(i, i);
    }
    case OpPenumbraA:
        return penumbraB(d, s);
    case OpPenumbraB:
        return penumbraB(s, d);
    case OpPenumbraC:
        return penumbraD(d, s);
    case OpPenumbraD:
        return penumbraD(s, d);
    case OpSoftLightIfsIllusions:
        return cpow(d, exp2(2.0 * (0.5 - s)));
    case OpSoftLightPegtopDelphi: {
        // unionShapeOpacity(), mul() and inv() return T values
        precise float union_ = (s + d) - tv(roundedProduct(s, d));
        const float screen = tv(union_);
        precise float sum = tv(roundedProduct(d, screen)) + tv(roundedProduct(tv(s * d), tv(1.0 - d)));
        return tv(sum);
    }
    case OpGammaLight:
        if (fuzzyZeroDouble(d)) return 0.0;
        if (fuzzyUnitDouble(d)) return 1.0;
        return cpow(d, s);
    case OpGammaIllumination:
        return 1.0 - tv(gammaDark(tv(1.0 - s), tv(1.0 - d)));
    case OpFlatLight:
        if (fuzzyZeroT(s)) return 0.0;
        return (1.0 - s) + d > 1.0 ? penumbraB(s, d) : penumbraB(d, s);
    case OpPNormA:
        return cpow(cpow(d, 2.3333333333333333) + cpow(s, 2.3333333333333333), 0.428571428571434);
    case OpPNormB:
        return cpow(cpow(d, 4.0) + cpow(s, 4.0), 0.25);
    case OpSuperLight:
        if (s < 0.5) return 1.0 - cpow(cpow(1.0 - d, 2.875) + cpow(1.0 - 2.0 * s, 2.875), 1.0 / 2.875);
        return cpow(cpow(d, 2.875) + cpow(2.0 * s - 1.0, 2.875), 1.0 / 2.875);
    case OpTintIfsIllusions: {
        precise float value = s * (1.0 - d) + csqrt(d);
        return value;
    }
    case OpFogLightenIfsIllusions: {
        precise float inverse = 1.0 - s;
        if (s < 0.5) {
            precise float value = (1.0 - inverse * s) - (1.0 - d) * inverse;
            return value;
        }
        precise float value = s - (1.0 - d) * inverse + inverse * inverse;
        return value;
    }
    case OpEasyDodge:
        if (s == 1.0) return 1.0;
        {
            precise float inverse = 1.0 - s;
            precise float y = inverse * EasyFactor;
            precise float yLow = fma(inverse, EasyFactor, -y) + inverse * EasyFactorLow;
            return cpow(d, y, yLow);
        }
    case OpGammaDark:
        return gammaDark(s, d);
    case OpShadeIfsIllusions: {
        precise float value = 1.0 - ((1.0 - d) * s + csqrt(1.0 - s));
        return value;
    }
    case OpFogDarkenIfsIllusions: {
        if (s < 0.5) {
            precise float value = (1.0 - s) * s + s * d;
            return value;
        }
        precise float value = s * d + s - s * s;
        return value;
    }
    case OpEasyBurn: {
        // inv(src != 1.0 ? src : .999999999999) in double
        const float base = s != 1.0 ? 1.0 - s : 9.999778782798785e-13;
        precise float y = d * EasyFactor;
        precise float yLow = fma(d, EasyFactor, -y) + d * EasyFactorLow;
        return 1.0 - cpow(base, y, yLow);
    }
    case OpInverseSubtract:
        return d - tv(1.0 - s);
    case OpModulo: {
        // KoColorSpaceMaths<T>::modulus(dst, src) with the channel epsilon
        const float eps = ChannelEpsilon;
        const float divisor = (s != -eps ? s : 0.0) + eps;
        precise float value = d - floor(exactQuotient(d, divisor)) * (s + eps);
        return value;
    }
    case OpModuloShift:
        return moduloShift(s, d);
    case OpModuloShiftContinuous: {
        if (s == 1.0 && d == 0.0) return 1.0;
        const float shifted = moduloShift(s, d);
        precise float hi = d + s;
        precise float b = hi - d;
        precise float lo = (d - (hi - b)) + (s - b);
        return mod(ceilExact(hi, lo), 2.0) != 0.0 || d == 0.0 ? shifted : 1.0 - shifted;
    }
    case OpArcTangent:
        return arcTangent(s, d);
    case OpEquivalence:
        return abs(d - s);
    case OpAdditiveSubtractive:
        return abs(csqrt(d) - csqrt(s));
    case OpReflect:
        return glow(d, s);
    case OpGlow:
        return glow(s, d);
    case OpFreeze:
        return heat(d, s);
    case OpHeat:
        return heat(s, d);
    case OpGleat:
        return gleat(s, d);
    case OpHelow:
        return helow(s, d);
    case OpReeze:
        return gleat(d, s);
    case OpFrect:
        return frect(s, d);
    case OpFhyrd: {
        precise float value = (frect(s, d) + helow(s, d)) * 0.5;
        return tv(value);
    }
    }
    return s;
}

// Clamp policies of the phase 4.108 functors (KoCompositeOpGenericFunctorBase.h)
bool extraClampsSource(uint op)
{
    return op == OpHardMixHdr || op == OpColorDodgeHdr || op == OpVividLightHdr || op == OpHardOverlay
        || op == OpHardOverlayHdr || op == OpParallel || op == OpPenumbraA || op == OpPenumbraB
        || op == OpGammaLight || op == OpGammaDark || op == OpGammaIllumination || op == OpFlatLight
        || op == OpInverseSubtract || (op >= OpGleat && op <= OpFrect);
}
bool extraClampsDestination(uint op)
{
    return op == OpParallel || op == OpPenumbraA || op == OpPenumbraB || op == OpGammaLight || op == OpGammaDark
        || op == OpGammaIllumination || op == OpFlatLight || (op >= OpGleat && op <= OpFrect);
}
bool extraClampsBottom(uint op)
{
    return op == OpGeometricMean || op == OpEquivalence || op == OpAdditiveSubtractive;
}

// The HSL functors of phase 4.108 (KoCompositeOpFunctions.h)
vec3 blendHslExtra(uint op, vec3 s, vec3 d)
{
    if (op == OpLambertLighting || op == OpLambertLightingGamma22) {
        const bool gamma = op == OpLambertLightingGamma22;
        vec3 t = roundedProduct(roundedProduct(s, d), gamma ? 2.0 : 1.0 / 0.215686);
        const float k = gamma ? 0.4 : 0.01925;
        for (int i = 0; i < 3; ++i) {
            if (t[i] > 1.0) {
                precise float excess = t[i] - 1.0;
                precise float value = 1.0 + roundedProduct(roundedProduct(excess, excess), k);
                t[i] = value;
            }
        }
        return hsxToneMapping(HsxI, t);
    }
    if (op == OpTangentNormalmap) {
        precise vec3 value = s + (d - vec3(0.5, 0.5, 1.0));
        return kbound(0.0, value, 1.0);
    }
    // OpCombineNormal: CFReorientedNormalMapCombine
    if (fuzzyZero(s.b)) return d;
    precise vec3 t = vec3(2.0 * s.r - 1.0, 2.0 * s.g - 1.0, 2.0 * s.b);
    precise vec3 u = vec3(-2.0 * d.r + 1.0, -2.0 * d.g + 1.0, 2.0 * d.b - 1.0);
    precise float dotTU = roundedProduct(t.x, u.x) + roundedProduct(t.y, u.y) + roundedProduct(t.z, u.z);
    const float k = exactQuotient(dotTU, t.z);
    precise vec3 r = roundedProduct(t, k) - u;
    precise float length = roundedProduct(r.x, r.x) + roundedProduct(r.y, r.y) + roundedProduct(r.z, r.z);
    if (length == 0.0) return d;
    const float scale = exactQuotient(1.0, sqrt(length));
    precise vec3 value = roundedProduct(roundedProduct(r, scale), 0.5) + 0.5;
    return value;
}

// Special composite ops (their own composeColorChannels); colours of
// channels the flags exclude are restored by compositeGeneric()
vec4 compositeSpecial(uint op, vec4 dst, vec4 src, float applied, bool alphaLocked)
{
    if (op == OpLuminositySai) {
        // KoCompositeOpGenericSCAlpha with cfAdditionSAI: no normalization
        if (alphaLocked) {
            if (dst.a != 0.0) {
                precise vec3 value = roundedProduct(src.rgb, applied) + dst.rgb;
                dst.rgb = value;
            }
            return dst;
        }
        precise float newAlpha = (applied + dst.a) - roundedProduct(applied, dst.a);
        if (newAlpha != 0.0) {
            precise vec3 value = roundedProduct(src.rgb, applied) + dst.rgb;
            dst.rgb = value;
        }
        return vec4(dst.rgb, newAlpha);
    }
    if (op == OpBehind) {
        // every Arithmetic result is a T value (half in RGBA16F)
        if (dst.a == 1.0 || applied == 0.0) return dst;
        precise float unionAlpha = (dst.a + applied) - tv(roundedProduct(dst.a, applied));
        const float newAlpha = tv(unionAlpha);
        vec3 color = src.rgb;
        if (dst.a != 0.0) {
            for (int c = 0; c < 3; ++c) {
                const float srcMult = tv(roundedProduct(src[c], applied));
                precise float blended = roundedProduct(dst[c] - srcMult, dst.a) + srcMult;
                color[c] = tv(exactQuotient(tv(blended), newAlpha));
            }
        }
        return vec4(color, alphaLocked ? dst.a : newAlpha);
    }
    if (op == OpGreater) {
        if (fuzzyUnitT(dst.a) || fuzzyZeroT(applied)) return dst;
        precise float x = -40.0 * (dst.a - applied);
        const float w = 1.0 / (1.0 + accurateExp(x, 0.0));
        precise float a = dst.a * w + applied * (1.0 - w);
        float alpha = a;
        if (alpha < 0.0) alpha = 0.0;
        if (alpha > 1.0) alpha = 1.0;
        if (alpha < dst.a) alpha = dst.a;
        precise float fakeOpacity = 1.0 - exactQuotient(1.0 - alpha, 1.0 - dst.a + 1e-16);
        const float newAlpha = tv(alpha);
        const float opacity = tv(fakeOpacity);
        vec3 color = dst.rgb;
        if (fuzzyZeroT(newAlpha)) {
        } else if (!fuzzyZeroT(dst.a)) {
            for (int c = 0; c < 3; ++c) {
                const float dstMult = tv(roundedProduct(dst[c], dst.a));
                precise float blended = roundedProduct(src[c] - dstMult, opacity) + dstMult;
                color[c] = tv(min(exactQuotient(tv(blended), newAlpha), FltMax));
            }
        } else {
            color = src.rgb;
        }
        return vec4(color, alphaLocked ? dst.a : newAlpha);
    }
    // OpCopyRed, OpCopyGreen, OpCopyBlue: lerp(dst, src, srcAlpha) on one channel
    const int channel = int(op - OpCopyRed);
    precise float value = roundedProduct(src[channel] - dst[channel], applied) + dst[channel];
    dst[channel] = value;
    return dst;
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
    return vec4(exactQuotient(result.r, alpha), exactQuotient(result.g, alpha), exactQuotient(result.b, alpha), alpha);
}

#endif

vec4 compositeGenericColor(uint op, vec4 dst, vec4 src, float srcAlpha, bool alphaLocked)
{
#ifndef BASIC_BLEND_ONLY
    if (op >= OpLuminositySai) return compositeSpecial(op, dst, src, srcAlpha, alphaLocked);
    if (op >= OpHue && (op < OpHardMixHdr || op >= OpLambertLighting))
        return compositeHSY(op, dst, src, srcAlpha, alphaLocked);
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
        return vec4(exactQuotient(result.r, newAlpha), exactQuotient(result.g, newAlpha),
                    exactQuotient(result.b, newAlpha), newAlpha);
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
