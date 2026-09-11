// Zion ML grade — unified ICtCp tone curve ("The Oracle v3").
// Three XGBoost models define a single tone curve on the I (intensity) channel:
//   Shadow toe  [0, 0.4]  — DV L2 offset pedestal lift, anchors the black point
//   Gamma       [0.5, 1.5] — DV L2 power midtone contrast, lifts/lowers midrange
//   Highlight   [-0.3, 0.5] — DV gold curve shoulder deviation, shapes the rolloff
// Operating in ICtCp preserves Ct/Cp (chrominance) — no skin tone or hue shift.
#include "../convert/st2084.hlsl"
#include "../convert/ictcp.hlsl"

Texture2D tex : register(t0);
SamplerState samp : register(s0);
Texture2D skin_lut : register(t1);   // trained 96x96 skin LUT (stage-5 tone leg gate)
SamplerState sampSkin : register(s1);
Texture2D sky_mask : register(t2);   // 640x360 R8G8 semseg (R=sky, G=person)
SamplerState sampSky : register(s2);
Texture2D src_tex : register(t3);    // pre-grade source (convert output) for the skin tone leg
SamplerState sampSrc : register(s3);

struct PS_INPUT
{
	float4 Pos : SV_POSITION;
	float2 Tex : TEXCOORD;
};

cbuffer MLParams : register(b0)
{
	float MLGamma;
	float MLShadowToe;
	float MLShadowKnee;
	float MLShadowDehaze;
	float MLHighlightRolloff;
	float MLHighlightKnee;
	float MLGuard;
	float MLHunt;
	float MLL1Max;
	float MLHiDesat;
	float IsPQ;
	float Padding;
	float MLChromaNeutral;
	float MLChromaFire;
	float MLChromaKnee;
	float MLChromaSkinProtect;
	float MLSkinLUTCR0;
	float MLSkinLUTCR1;
	float MLSkinLUTCB0;
	float MLSkinLUTCB1;
	float MLChromaGate;
	float MLHuntGate;
	float _reserved0;
	float _reserved1;
	float _reserved2;
	float _reserved3;
	float _reserved4;
	float _reserved5;
	float uBarTop;
	float uBarBottom;
	float uBarV0;
	float uBarSpan;
	float uBarFeather;
	float uBarBlack;
	float uBarHighlight;
	float _padBar;
	float MCStrength;
	float MCZoneLo;
	float MCSkinProtect;
	float MCRatioCap;
	float FaceProtect;
	float FaceFeather;
	float FaceExpand;
	float FaceDebugVis;
	int   NumFaces;
	float3 _padFace;
	float4 FaceBox[4];
	float4 FaceTrackId;
	// Sky tone leg (stage 3) — appended tail, offsets of prior fields unchanged
	float MLSkyStrength;
	float MLSkyGuard;
	float MLSkyLuma;
	float MLSkyUVOffX;
	float MLSkyUVOffY;
	float MLSkyUVScX;
	float MLSkyUVScY;
	float _padSky;
	// Person skin protection (stage 4) — appended tail, offsets above unchanged
	float MLSkinPersonStrength;  // 0..1 LUT-only-inside-people gate; 0 = legacy
	float MLSkinPersonDebugVis;  // 0/1 magenta person-mask overlay
	float _padPerson0;
	float _padPerson1;
	// Skin tone-preservation (stage 5) — appended tail
	float MLSkinAnchor;    // skin-set mean source luma (IIR, gamma-2.2)
	float MLSkinPreserve;  // 0..1 keep-scale of source skin structure
	float MLSkinPad0;
	float MLSkinPad1;
};

static const float3 luma_w = float3(0.2126, 0.7152, 0.0722);

float4 main(PS_INPUT input) : SV_Target
{
	float4 color = tex.Sample(samp, input.Tex);

	// Convert to linear nits → ICtCp
	float3 lin;
	if (IsPQ > 0.5) {
		lin = ST2084ToLinear(float4(color.rgb, 0.0), 10000.0).rgb;
	} else {
		lin = pow(max(color.rgb, 0.0), 2.2) * SDR_PEAK_NITS;
	}
	float3 ictcp = linear_to_ictcp(lin);

	float I = ictcp.x;
	float I_peak = (IsPQ > 0.5) ? max(MLL1Max, 0.01) : SDR_I_PEAK_PQ;
	float I_norm = I / I_peak;

	// Sky mask (t2, content-aligned 640x360) — sampled once at the CONTENT uv
	// (input.Tex spans the window; the content sits at uvOff..(uvOff+uvSc)),
	// used by the tone leg below and the debug overlay at the end.
	float2 cuv = (input.Tex - float2(MLSkyUVOffX, MLSkyUVOffY))
		/ max(float2(MLSkyUVScX, MLSkyUVScY), 0.001f);
	// In-content gate: window pixels OUTSIDE the content rect (letterbox/pillarbox
	// bars) must not read the clamped mask edge — S=0 kills both the tone leg and
	// the overlay there, so bars stay clean. (all() on a float2 is lane-wise.)
	const float inContent = (all(cuv >= 0.0) && all(cuv <= 1.0)) ? 1.0 : 0.0;
	const float2 maskRG = sky_mask.Sample(sampSky, saturate(cuv)).rg * inContent;
	const float rawS = maskRG.r;   // sky (class 10)
	const float rawP = maskRG.g;   // person | rider (class 11|12)
	float S = (MLSkyStrength > 0.001) ? rawS : 0.0f;

	// === UNIFIED TONE CURVE (shadow toe → gamma → highlight rolloff) ===

	// 1. Shadow toe: DV L2 offset pedestal lift — raises dark end toward knee
	if (MLShadowToe > 0.005 && I_norm < MLShadowKnee && I_norm > 0.0) {
		float t = 1.0 - I_norm / max(MLShadowKnee, 0.01);
		I_norm += MLShadowToe * I_norm * t * t;
		I_norm = saturate(I_norm);
	}

	// 2. Gamma with a sky-relaxed guard leg. Both legs use the SAME curve
	// family and differ only in the highlight guard, so they coincide below
	// the guard band (no seam) and S=0 reproduces the global path exactly.
	float l2_gamma = MLGamma;
	if (l2_gamma != 1.0 && I_norm > 0.001 && I_norm < 0.999) {
		float fade = smoothstep(0.08, 0.45, I_norm);
		float hi_g = smoothstep(MLGuard, 0.98, I_norm);
		float hi_s = smoothstep(MLSkyGuard, 0.98, I_norm);
		float p = pow(I_norm, 1.0 / l2_gamma);
		float I_g = lerp(I_norm, p, fade * (1.0 - hi_g));
		float I_s = lerp(I_norm, p, fade * (1.0 - hi_s));
		I_norm = lerp(I_g, I_s, S * MLSkyStrength);
	}

	// 3. Highlight rolloff — softened inside sky so the shoulder keeps the
	// sky's luminance structure instead of crushing it.
	if (abs(MLHighlightRolloff) > 0.005 && I_norm > MLHighlightKnee && I_norm < 0.995) {
		float span = max(1.0 - MLHighlightKnee, 0.01);
		float t = (I_norm - MLHighlightKnee) / span;
		float shoulder = MLHighlightRolloff * t * t * (1.0 - I_norm);
		float skyScale = 1.0 - 0.5 * S * MLSkyStrength;
		I_norm = saturate(I_norm + shoulder * skyScale);
	}

	// 4. Sky cloud-band lift: the guard-only leg cannot reach I < MLSkyGuard,
	// which is exactly where clouds live — so give the upper-mid luma band a
	// small extra allocation inside sky. Range re-allocation, not re-curving.
	{
		float band = smoothstep(0.40f, 0.55f, I_norm) * (1.0f - smoothstep(0.85f, 0.95f, I_norm));
		I_norm += S * MLSkyStrength * 0.06f * band;
		I_norm = saturate(I_norm);
	}

	// Face blending disabled — testing pure ICtCp output
	float skin_blend = 0.0;

	// Skin tone-preservation (stage 5): steer the WHOLE person region's luma toward
	// the source skin tone. Gate = PERSON mask (bilinear → spatially smooth, no
	// skin-LUT boundaries → no hot spots/patches). The skin subset only defines
	// the target: MLSkinAnchor (CPU IIR skin-set mean source luma). Bounded, and
	// OFF by default (MLSkinPreserve = 0 → bit-identical).
	if (MLSkinPersonStrength > 0.001 && MLSkinPreserve > 0.001 && MLSkinAnchor > 0.01f) {
		// Blurred person gate: a raw mask edge would print the silhouettes into the
		// tone field (discontinuity). Wide plus-blur (~3.5 mask texels, ~40 px at 4K)
		// so the correction fades over a physically large span — bright-uniform
		// backgrounds otherwise expose a residual seam at the silhouette.
		uint mw, mh; sky_mask.GetDimensions(mw, mh);
		const float2 mt = float2(1.0f / (float)mw, 1.0f / (float)mh) * 3.5f;
		const float Pa = sky_mask.Sample(sampSky, saturate(cuv + float2(-mt.x, 0.0f))).g;
		const float Pb = sky_mask.Sample(sampSky, saturate(cuv + float2(mt.x, 0.0f))).g;
		const float Pc = sky_mask.Sample(sampSky, saturate(cuv + float2(0.0f, -mt.y))).g;
		const float Pd = sky_mask.Sample(sampSky, saturate(cuv + float2(0.0f, mt.y))).g;
		const float Pe = sky_mask.Sample(sampSky, saturate(cuv)).g;
		const float PbS = (0.25f * (Pa + Pb + Pc + Pd) + 0.5f * Pe) * inContent;
		// Erode + soften: the fade sits INSIDE the silhouette (no outer halo); the
		// high plateau holds only deep inside so the boundary tail is long.
		const float P = smoothstep(0.10f, 0.88f, PbS);
		if (P > 0.005) {
			float3 srcRGB = src_tex.Sample(sampSrc, saturate(cuv)).rgb;
			float3 srcLin;
			if (IsPQ > 0.5) srcLin = ST2084ToLinear(float4(srcRGB, 0.0), 10000.0).rgb;
			else            srcLin = pow(max(srcRGB, 0.0), 2.2) * SDR_PEAK_NITS;
			float3 sg22 = (IsPQ > 0.5) ? pow(saturate(srcLin / 400.0f), 1.0f / 2.2f) : max(srcRGB, 0.0f);
			float sy = dot(sg22, luma_w);
			// Soft skin evidence (continuous 0..1 — no hard gate → no patches): the
			// correction concentrates on pixels that actually read as skin, so hair,
			// clothing and mirror reflections get ~no re-anchor (kills reflection shimmer).
			float2 skuv = saturate(float2(
				(sg22.r - sy) / 1.5748f - MLSkinLUTCR0,
				(sg22.b - sy) / 1.8556f - MLSkinLUTCB0) / float2(
				MLSkinLUTCR1 - MLSkinLUTCR0, MLSkinLUTCB1 - MLSkinLUTCB0));
			const float skinEv = smoothstep(0.25f, 0.70f, skin_lut.Sample(sampSkin, skuv).r);
			float srcLum = sy;
			float outLum = (IsPQ > 0.5) ? pow(max(dot(lin, luma_w), 0.0), 1.0f / 2.2f)
			                            : dot(max(color.rgb, 0.0), luma_w);
			float desired = clamp(MLSkinAnchor + (srcLum - MLSkinAnchor) * MLSkinPreserve, 0.0, 1.0);
			float corr = clamp(desired - outLum, -0.10f, 0.10f);
			I_norm = saturate(I_norm + corr * P * skinEv * MLSkinPersonStrength * 0.32f);
		}
	}
	ictcp.x = I_norm * I_peak;

	// Inverse: ICtCp → linear nits → output encoding
	float3 rgb_out = ictcp_to_linear(ictcp);

	if (IsPQ > 0.5) {
		color.rgb = LinearToST2084(float4(rgb_out, 0.0), 10000.0).rgb;
	} else {
		color.rgb = pow(saturate(rgb_out / SDR_PEAK_NITS), 1.0 / 2.2);
	}

	// Debug: sky-mask overlay (validation only — no tone effect yet). Cyan on
// the sky region from the t2 mask; rides _reserved5 (bMLSkyDebugVis).
	if (_reserved5 > 0.5) {
		// Full-res 640x360 mask at the CONTENT uv — same remap as the tone leg,
		// so the cyan overlay shows exactly which pixels the tone is affecting.
		// Uses rawS (not strength-gated S) so the mask is visible at any strength.
		float S2 = smoothstep(0.25, 0.75, rawS);
		color.rgb = lerp(color.rgb, float3(0.0, 1.0, 1.0) * 0.6, S2 * 0.5);
	}

	// Debug: person-mask overlay (stage 4, validation). Magenta on people from
	// the G channel, same content UV — gates only on the debug flag so it shows
	// at any person strength (rawP, not the strength-gated weight).
	if (MLSkinPersonDebugVis > 0.5) {
		float P2 = smoothstep(0.25, 0.75, rawP);
		color.rgb = lerp(color.rgb, float3(1.0, 0.0, 1.0) * 0.6, P2 * 0.5);
	}

	// Debug: track-colored ellipses — DISABLED (face detection removed; person
// instances replace it). FaceBox/FaceTrackId cbuffer decls stay for layout.
	if (0) {
		// face-track ellipse draw removed
	}

	return color;
}
