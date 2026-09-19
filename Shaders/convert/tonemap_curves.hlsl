// ---------------------------------------------------------------------------
// The scene-referred tone-mapping curves, shared.
//
// These used to live inside ps_hdr10_tonemap.hlsl, which is MPCVR's HDR-OUTPUT
// path and is only built when the display is HDR. The PQ->SDR converter — the
// path that actually runs for an SDR display — had Hable hardcoded, so the
// "Tone curve" selection reached a shader that was never compiled and the control
// did nothing.
//
// They live in one file so the two paths cannot drift: the same selection must
// produce the same curve wherever it is applied. Every function takes its metadata
// as PARAMETERS rather than reading a cbuffer, because the two callers have
// different constant buffers.
//
// TWO FAMILIES, and the distinction matters:
//
//   * Self-normalising, in NITS: BT2390, ST209410. They read absolute nits and
//     return absolute nits, deriving their own knee from maxCLL/displayMaxNits.
//
//   * Normalised to [0,1]: ACES, Reinhard, Habel, Mobius. The caller scales nits
//     by the content peak (effectiveMaxLum), applies the curve, then expands the
//     result across the display range. See ToneMapNormalisedCurve below, which
//     exists so that expansion is written once.
//
// REQUIRES st2084.hlsl to already be included by the caller (both callers do).
// It is deliberately NOT included here: neither it nor this file carries an
// include guard, and fxc ignores `#pragma once` (warning X3568), so including it
// from both sides would redefine ST2084_m1 and friends.
// ---------------------------------------------------------------------------

#ifndef TONEMAP_CURVES_HLSL_INCLUDED
#define TONEMAP_CURVES_HLSL_INCLUDED

float3 ACESFilmTonemap(float3 color)
{
	// Constants used in the ACES Filmic tone mapping
	static float A = 2.51f;
	static float B = 0.03f;
	static float C = 2.43f;
	static float D = 0.59f;
	static float E = 0.14f;

	// Apply the ACES RRT + ODT
	color = (color * (A * color + B)) / (color * (C * color + D) + E);

	return color;
}

float3 ReinhardTonemap(float3 color)
{
	return color / (1.0 + color);
}

// The HDR10 shader's Habel variant: raw, NOT normalised by hable(4.8). It
// asymptotes to 0.933 rather than 1.0, so white lands at 93% of the display
// range. Kept as-is deliberately — this is the curve the HDR10 path has always
// used, and "fixing" it would silently change that path's picture.
//
// NOTE: this is NOT the same curve as ToneMappingHable (hdr_tone_mapping.hlsl),
// which IS normalised so 4.8 -> 1.0 and is what the SDR converter has always
// used. The SDR converter keeps ToneMappingHable for its "Hable" selection, so
// that selecting Hable reproduces exactly the picture that was there before the
// selection became live.
float3 HabelTonemap(float3 color)
{
	float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
	return ((color * (A * color + C * B) + D * E) / (color * (A * color + B) + D * F)) - E / F;
}

float3 MobiusTonemap(float3 color, float displayMaxNits)
{
	static float epsilon = 1e-6;
	float maxL = displayMaxNits;
	return color / (1.0 + color / (maxL + epsilon));
}

// BT.2390 EETF. Nits in, nits out: the knee is derived in PQ from the content
// peak, and the Hermite roll-off is applied per PIXEL above it (avgRGB is the
// pixel's own weighted average, not a frame statistic) — so this curve has no
// per-frame quantity to make dynamic.
float3 BT2390Tonemap(float3 color, float maxCLL, float masteringMaxNits, float displayMaxNits)
{
	// Safe Metadata Fallbacks (Fixes black screens on bad video files)
	float safeMaxCLL = maxCLL;
	if (safeMaxCLL <= 10.0f)
		safeMaxCLL = masteringMaxNits;
	if (safeMaxCLL <= 10.0f)
		safeMaxCLL = 1000.0f; // Ultimate safety fallback
	// Optimization: Skip processing if display is brighter than the content
	if (displayMaxNits >= safeMaxCLL)
		return color;

	// Find the average RGB component to preserve hue and saturation
	float avgRGB = 0.2627 * color.r + 0.6780 * color.g + 0.0593 * color.b;

	// Avoid division by zero on pure black pixels
	if (avgRGB <= 0.000001)
		return color;

	// Convert peaks and current pixel luminance to PQ space
	float maxCLL_PQ = LinearToST2084(safeMaxCLL, 10000.0f).x;
	float target_PQ = LinearToST2084(displayMaxNits, 10000.0f).x;
	float E1 = LinearToST2084(avgRGB, 10000.0f).x;

	// Calculate BT.2390 Knee Start (KS) point
	float KS = 1.5 * target_PQ - 0.5 * maxCLL_PQ;
	KS = max(0.0, KS); // Knee Start cannot be negative

	float E2 = E1;

	// Apply the Hermite Spline roll-off if the pixel is brighter than the Knee
	if (E1 > KS)
	{
		// max(1e-6, ...) prevents division by zero if maxCLL_PQ happens to equal KS
		float T = (E1 - KS) / max(1e-6, maxCLL_PQ - KS);
		float T2 = T * T;
		float T3 = T2 * T;

		E2 = (2.0 * T3 - 3.0 * T2 + 1.0) * KS +
			 (T3 - 2.0 * T2 + T) * (maxCLL_PQ - KS) +
			 (-2.0 * T3 + 3.0 * T2) * target_PQ;
	}

	// Convert the tone-mapped PQ value back to linear light
	float linearMapped = ST2084ToLinear(E2, 10000.0f).x;

	// Scale the original RGB channels equally to preserve the exact color hue
	float3 mappedColor = color * (linearMapped / avgRGB);

	return mappedColor;
}

float pl_smoothstep(float edge0, float edge1, float x)
{
	float t = clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

// ST 2094-10 EETF. Nits in, nits out.
//
// This is the one curve in the family whose pivot is meant to be PER SCENE: the
// standard is a dynamic-metadata standard, and src_avg is the scene average. Here
// it is taken from maxFALL, which is static and title-wide — and when maxFALL is
// absent (which it always is on the DeckLink capture path, whose ST.2086 frame
// metadata is unsupported) the ternary below falls back to the FIXED default knee,
// so the curve silently becomes static. Feeding the measured scene average in
// instead of maxFALL is what would make it do what the standard intends.
float3 ST209410Tonemap(float3 color, float maxCLL, float maxFALL,
					   float masteringMinNits, float displayMaxNits, float displayMinNits)
{
	if (displayMaxNits >= maxCLL)
		return color;

	float src_min = LinearToST2084(masteringMinNits, 10000.0f).x;
	float src_max = LinearToST2084(maxCLL, 10000.0f).x;
	float src_avg = LinearToST2084(maxFALL, 10000.0f).x;
	// Destination floor is the display's measured black, not absolute zero: the
	// knee positions below are lerped from dst_min, so a real floor moves the whole
	// shadow adaptation rather than compressing it into a range the panel cannot show.
	float dst_min = LinearToST2084(displayMinNits, 10000.0f).x;
	float dst_max = LinearToST2084(displayMaxNits, 10000.0f).x;

	const float min_knee = 0.1f;
	const float max_knee = 0.8f;
	const float def_knee = 0.4f;
	const float knee_adaptation = 0.4f;

	const float src_knee_min = lerp(src_min, src_max, min_knee);
	const float src_knee_max = lerp(src_min, src_max, max_knee);
	const float dst_knee_min = lerp(dst_min, dst_max, min_knee);
	const float dst_knee_max = lerp(dst_min, dst_max, max_knee);

	float src_knee = (maxFALL > 0.0f) ? src_avg : lerp(src_min, src_max, def_knee);
	src_knee = clamp(src_knee, src_knee_min, src_knee_max);

	float target = (src_knee - src_min) / (src_max - src_min);
	float adapted = lerp(dst_min, dst_max, target);

	float tuning = 1.0f - pl_smoothstep(max_knee, def_knee, target) * pl_smoothstep(min_knee, def_knee, target);
	float adaptation = lerp(knee_adaptation, 1.0f, tuning);

	float dst_knee = lerp(src_knee, adapted, adaptation);
	dst_knee = clamp(dst_knee, dst_knee_min, dst_knee_max);

	float out_src_knee = ST2084ToLinear(src_knee, 10000.0f).x;
	float out_dst_knee = ST2084ToLinear(dst_knee, 10000.0f).x;

	// Solve the rational function y = (c1 + c2*x) / (1 + c3*x) through the three
	// anchor points (masteringMin -> displayMin), (src_knee -> dst_knee) and
	// (maxCLL -> displayMax). Ported verbatim from the HDR10 shader's 3x3 solve.
	float x1 = masteringMinNits;
	float x3 = maxCLL;
	float x2 = out_src_knee;

	float y1 = displayMinNits;   // destination black, was hardcoded 0.0f
	float y3 = displayMaxNits;
	float y2 = out_dst_knee;

	// Build the 3x3 cmat array
	float m00 = x2 * x3 * (y2 - y3);
	float m01 = x1 * x3 * (y3 - y1);
	float m02 = x1 * x2 * (y1 - y2);
	float m10 = x3 * y3 - x2 * y2;
	float m11 = x1 * y1 - x3 * y3;
	float m12 = x2 * y2 - x1 * y1;
	float m20 = x3 - x2;
	float m21 = x1 - x3;
	float m22 = x2 - x1;

	float coef0 = m00 * y1 + m01 * y2 + m02 * y3;
	float coef1 = m10 * y1 + m11 * y2 + m12 * y3;
	float coef2 = m20 * y1 + m21 * y2 + m22 * y3;

	float k = 1.0f / (x3 * y3 * (x1 - x2) + x2 * y2 * (x3 - x1) + x1 * y1 * (x2 - x3));

	float c1 = k * coef0;
	float c2 = k * coef1;
	float c3 = k * coef2;

	float x_nits = 0.2627 * color.r + 0.6780 * color.g + 0.0593 * color.b;

	float y_nits = (c1 + c2 * x_nits) / (1.0f + c3 * x_nits);

	color.rgb *= (x_nits > 0.0f) ? (y_nits / x_nits) : 1.0f;

	return color;
}

// The [0,1] family, written once: scale nits by the reference, apply the selected
// curve, expand back across the display range. Returns NITS.
//
// `referenceNits` is the value that maps to 1.0, and it is deliberately NOT the
// same on the two paths — that is a real difference in what 1.0 MEANS, not drift:
//
//   * HDR output: 1.0 is the display's peak. The reference is the content peak
//     clamped to the display, so the content's own range fills the display.
//
//   * SDR output fed HDR content: 1.0 is the SDR REFERENCE WHITE (diffuse white),
//     which here is the display's white. These curves are written for input where
//     1.0 is diffuse white, not the absolute peak. Normalising by a 1000-nit peak
//     puts a 180-nit mid-tone at 0.18, and ACES then renders it at 22 nits where
//     the display can show ~78 — which a user sees as "ACES is broken", and every
//     linear-normalised curve looks the same way while the PQ-domain ones (Hable,
//     BT.2390) look fine.
//
// selection: 1 = ACES, 2 = Reinhard, 3 = Habel, 4 = Mobius. Anything else falls
// back to ACES, which is what the HDR10 shader always did.
//
// NOTE for the SDR converter: it does NOT use selection 3 from here. Its "Hable"
// is the normalised ToneMappingHable on display-relative values, which is the
// curve that path has always used — see HabelTonemap's note above.
float3 ToneMapNormalisedCurve(float3 colorNits, int selection,
							  float referenceNits, float maxFALL,
							  float displayMaxNits, float displayMinNits)
{
	// max(...,1) guards the division: the reference can be 0 on a path that has no
	// metadata at all, and the fallbacks are the callers' business.
	const float ref = max(referenceNits, 1.0f);
	// Exposure tweak from the frame average, applied ONLY when the average is real.
	//
	// This is an attenuator: min(displayPeak / avg, 1). It was a no-op on the HDR
	// path because the fabricated fallback there sets avg = the peak. On the SDR
	// path the same fabrication would make it displayPeak/contentPeak — 84/1000 —
	// and darken everything ~12x, which is exactly what it did the first time this
	// was referenced by the display white instead of the content peak. The capture
	// path has no real frame-average metadata, so it passes 0 and the term is
	// skipped; deriving an exposure from a number we invented is worse than not
	// adjusting at all.
	const float fallAdjustment = (maxFALL > 1.0f)
		? min(displayMaxNits / maxFALL, 1.0f) : 1.0f;

	float3 c = colorNits * (1.0f / ref);
	c = saturate(c);
	c *= fallAdjustment;

	if (selection == 1)      c = ACESFilmTonemap(c);
	else if (selection == 2) c = ReinhardTonemap(c);
	else if (selection == 3) c = HabelTonemap(c);
	else if (selection == 4) c = MobiusTonemap(c, ref);
	else                     c = ACESFilmTonemap(c);   // historical fallback

	return displayMinNits + c * max(displayMaxNits - displayMinNits, 1.0f);
}

#endif // TONEMAP_CURVES_HLSL_INCLUDED
