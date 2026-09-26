// ---------------------------------------------------------------------------
// The GT7 tone mapping operator, for the PQ->SDR converter.
//
// Source: "Sample implementation of the GT7 Tone Mapping operator" v1.0
// (2025-08-10), Copyright (c) 2025 Polyphony Digital Inc., MIT licensed.
// Ported from gt7_tone_mapping.cpp. The curve math, the per-channel "skew", the
// luminance-from-skewed / chroma-from-original recombination and the highlight
// chroma fade are theirs; only the units and the plumbing are ours.
//
// UNITS. The reference works in "frame buffer" units where 1.0 == 100 nits
// (REFERENCE_LUMINANCE). This file works in ABSOLUTE NITS, because that is what
// the converter has to hand about (color.rgb * sdrNits). The curve is a linear-
// homogeneous function of its peak EXCEPT for one constant, so the translation is
// exact with a single caveat:
//
//   kA = peak*linearSection + peak*k,  kB = -peak*k*exp(linearSection/k),
//   kC = -1/(k*peak)   with k = (linearSection-1)/(alpha-1)
//
// are all proportional to the peak's unit, so any linear unit works; but
// grayPoint (GT's 0.538, i.e. 53.8 nits) is an ABSOLUTE shadow breakpoint. It is
// therefore a PARAMETER here rather than a constant. That is not tidiness — it is
// the whole design lever: relative to its own ceiling this curve is scale
// invariant, so the one thing that can make a display-target dial (hdr.sdrNits)
// affect the picture is where the absolute anchors sit. Callers choose; see
// _gt7probe\gt7_probe.cpp, which measures the choices before any of them is wired
// into a pixel.
//
// SHAPE FACTS, worth knowing before tuning anything (alpha .25, linearSection .444,
// grayPoint 53.8nits, toeStrength 1.28 as shipped):
//   identity              between grayPoint and 0.444*ceiling  (slope 1, no change)
//   C(ceiling)            = 0.835*ceiling      -> the ceiling is NOT reached at x=ceiling
//   asymptote             = 1.185*ceiling
//   C(x) = ceiling at      x = 2.046*ceiling   -> where the clamp below starts binding
//   total shoulder range   ~1.9:1
// So this is a display-referred ~2:1 limiter, sized for content that peaks around
// 4.6x its reference white (0.444 -> 2.046 is exactly that ratio). A 1000-4000 nit
// HDR master onto a 25-400 nit SDR target needs up to ~12:1, which this operator
// cannot supply. The PQ->SDR converter ships the OFFICIAL form regardless — ceiling
// = the display's target peak, no input gain, no pre-fit — so on a wide master it
// does what it says on the tin and limits hard. _gt7probe measures the alternatives
// (a gain from refWhite and scenePeak, an adaptive knee, a soft-plus pre-fit) and
// their invariants, for whoever takes that on next.
//
// CALLS linear_to_ictcp / ictcp_to_linear from ictcp.hlsl, whose matrices are
// byte-identical to this operator's own rgbToICtCp / iCtCpToRgb (same rgb2lms,
// pqlms2ictcp, lms2rgb, ictcp2pqlms constants), so the operator's colour space is
// not re-ported. Note the equivalence on units: the reference's rgbToICtCp takes a
// frame-buffer value and does the x100 / /10000 inside its own PQ helpers, so
//   linear_to_ictcp(nits)  ==  rgbToUcs(nits / 100) * (1,1,1 scale)
// exactly — the same space, the same numbers, one less round trip.
//
// Requires the caller to have included st2084.hlsl, ictcp.hlsl and
// colorspace_gamut_conversion.hlsl first, like tonemap_curves.hlsl: neither this
// file nor those carry include guards that fxc respects (#pragma once is ignored,
// warning X3568), so including from both sides would redefine ST2084_m1 and friends.
// ---------------------------------------------------------------------------

#ifndef GT7_TONEMAP_HLSL_INCLUDED
#define GT7_TONEMAP_HLSL_INCLUDED

// GT's shipped curve constants. peakIntensity and grayPoint are NOT here: the
// first is the caller's ceiling choice, the second is the absolute anchor (see
// the UNITS note above).
#define GT7_ALPHA          0.25f
#define GT7_LINEAR_SECTION 0.444f
#define GT7_TOE_STRENGTH   1.0f
// grayPoint in nits, i.e. the reference's 0.538 frame-buffer units x 100.
#define GT7_GRAYPOINT_NITS 53.8f
// Toe exponent: Polyphony ships 1.280, which DARKENS everything below the gray
// point (toe = grayPoint*(x/grayPoint)^1.28 < x). Set to 1.0 the toe is exactly
// identity — zero shadow crush — chosen 2026-09-26 because the 991-nit master
// read with a heavy 37%-of-pixels shadow pile vs madVR's 29% and the user
// reported losing shadow detail to a strong S-curve. 1.280 is one edit away; the
// highlight shoulder is untouched either way.
// THE SAME point expressed as a fraction of the ceiling — GT7's own relative
// placement. Reference-calibrated: 53.8 / 250 (its SDR paper-white ceiling) =
// 0.2152. Kept next to the absolute form because the two anchored DIFFERENT
// things:
//   * the reference ships grayPoint as an ABSOLUTE 53.8 nits — correct for a
//     game whose 250-nit ceiling never moves, and catastrophic for video, where
//     it is the caller's job to steer the one anchor the curve actually has;
//   * Re-anchored at 0.215*ceiling the toe occupies the same 21.5% of the
//     output range at sdrNits=200 (43 nits) and sdrNits=85 (18 nits) that it
//     occupies at GT's own 250. Measured 2026-09-26 on the 991-nit master at
//     target 200: the absolute 53.8 form crushed 38.4% of the frame to near
//     black versus madVR's 29.3%; the re-anchor restores the shadow structure.
#define GT7_GRAYPOINT_FRACTION 0.215f
// The reference's own blend/fade defaults, kept as the documented starting point.
#define GT7_BLEND_RATIO    0.6f
#define GT7_FADE_START     0.98f
#define GT7_FADE_END       1.16f

// The GT curve, in whatever linear unit `peakNits` and `grayPointNits` are given
// in. Returns the same unit. This is GTToneMappingCurveV2::evaluateCurve.
float GT7EvaluateCurve(float x, float peakNits, float kA, float kB, float kC,
                       float kneeNits, float grayPointNits)
{
    if (x <= 0.0f) {
        return 0.0f;
    }

    // initializeCurve()'s three precomputed constants, folded into the caller so a
    // single (peak) evaluation can serve all three channels.
    const float shoulder = kA + kB * exp(x * kC);

    if (x < kneeNits) {
        // weightLinear ramps 0 -> 1 across the toe, so below grayPoint the picture
        // is crushed by toeStrength and above it the map is EXACTLY identity.
        const float weightLinear = smoothstep(0.0f, grayPointNits, x);
        const float toeMapped = grayPointNits * pow(x / grayPointNits, GT7_TOE_STRENGTH);
        return (1.0f - weightLinear) * toeMapped + weightLinear * x;
    }

    return shoulder;
}

// Precompute the shoulder constants for one ceiling. They depend only on the
// ceiling, so fxc hoists them out of the three channel calls below.
void GT7InitCurve(float peakNits, out float kA, out float kB, out float kC, out float kneeNits)
{
    const float k  = (GT7_LINEAR_SECTION - 1.0f) / (GT7_ALPHA - 1.0f);
    kA       = peakNits * GT7_LINEAR_SECTION + peakNits * k;
    kB       = -peakNits * k * exp(GT7_LINEAR_SECTION / k);
    kC       = -1.0f / (k * peakNits);
    kneeNits = GT7_LINEAR_SECTION * peakNits;
}

// The operator. Nits in, nits out, clamped to `ceilingNits` exactly as the
// reference's `min(blended, framebufferLuminanceTarget_)` does.
//
//   ceilingNits    -> GT7ToneMapping::framebufferLuminanceTarget_ / peakIntensity_
//   grayPointNits  -> GT's 0.538 fb units, in the caller's nits
//   blendRatio     -> how much of the output comes from the luminance-and-original-
//                     chroma reconstruction (1.0) versus the raw per-channel skewed
//                     curve (0.0). GT ships 0.6. This is the knob that decides the
//                     hue behaviour: the skewed side is what pulls a warm highlight
//                     back toward neutral (R>G>B, so R is compressed hardest), and
//                     it is also the side that rotates hue everywhere else.
//   fadeStart/End  -> the chromaCurve window, in units of the ceiling's ICtCp I,
//                     below which chroma is handed through untouched.
float3 GT7ToneMap(float3 rgbNits, float ceilingNits, float grayPointNits,
                  float blendRatio, float fadeStart, float fadeEnd)
{
    float kA, kB, kC, kneeNits;
    GT7InitCurve(ceilingNits, kA, kB, kC, kneeNits);

    // Per-channel tone mapping — the reference calls this the "skewed" color.
    const float3 skewedNits = float3(GT7EvaluateCurve(rgbNits.r, ceilingNits, kA, kB, kC, kneeNits, grayPointNits),
                                     GT7EvaluateCurve(rgbNits.g, ceilingNits, kA, kB, kC, kneeNits, grayPointNits),
                                     GT7EvaluateCurve(rgbNits.b, ceilingNits, kA, kB, kC, kneeNits, grayPointNits));

    const float3 ucs       = linear_to_ictcp(rgbNits);
    const float3 skewedUcs = linear_to_ictcp(skewedNits);

    // framebufferLuminanceTargetUcs_ in the reference. For a grey the LMS sums are
    // exactly the input (each row of rgb2lms sums to 1), so I of (c,c,c) is simply
    // the PQ code of c — one pow, no fourth round trip.
    const float targetI = LinearToST2084(float4(ceilingNits, 0.0, 0.0, 0.0), 10000.0).x;

    // chromaCurve(x, a, b) = 1 - smoothStep(x, a, b): chroma survives untouched until
    // 0.98 of the ceiling's I and is gone by 1.16, so the top of the range lands on
    // neutral white instead of carrying the source's colour into the clip.
    const float chromaScale = 1.0f - smoothstep(fadeStart, fadeEnd, ucs.x / max(targetI, 1e-6));

    // Luminance from the skewed color, chroma from the ORIGINAL, scaled.
    const float3 scaledNits = ictcp_to_linear(float3(skewedUcs.x, ucs.y * chromaScale, ucs.z * chromaScale));

    return min(lerp(skewedNits, scaledNits, blendRatio), ceilingNits);
}

// The soft-plus pre-compressor, per channel, in the operator's own units. Same
// shape and dial meanings as the Hable branch's (softKnee/softKneeRatio/
// softKneeWidth): knee and width are display-relative fractions of the ceiling,
// ratio 1.0 = identity above the knee, below the knee exactly identity.
// Overflow-safe: sp(t) = max(t,0) + log(1+exp(-|t|)), exponent always <= 0.
//
// NEEDED for the adapted form (ceiling = sdrNits with the 991-nit master fed in):
// without a pre-fit, 14.8% of a 1000-nit range and 41% of a 4000-nit one lands
// above the operator's clamp (2.046*ceiling) and flattens — the measured
// "strong contrast" at sdrNits=200. With the shipped defaults (knee 0.60, ratio
// 2.48, width 0.42) nothing clips at any peak measured. The gain lands the
// CONTENT peak on the curve; the pre-fit re-spreads the band the clamp would
// flatten, so the shoulder — not the cliff — carries the top of the master.
float3 GT7PreCompress(float3 x, float knee, float ratio, float width)
{
    const float w = max(width, 1e-3);
    const float3 t  = (x - knee) / w;
    const float3 sp = max(t, 0.0) + log(1.0 + exp(-abs(t)));
    const float3 comp = knee + w * sp / max(ratio, 1e-3);
    return min(x, comp);
}

#endif // GT7_TONEMAP_HLSL_INCLUDED
