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
Texture2D sky_mask : register(t2);   // 160x90 R8 sky-probability map (class 10)
SamplerState sampSky : register(s2);

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
};

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
	const float rawS = sky_mask.Sample(sampSky, saturate(cuv)).r * inContent;
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

	ictcp.x = I_norm * I_peak;

	// Inverse: ICtCp → linear nits → output encoding
	float3 rgb_out = ictcp_to_linear(ictcp);

	if (IsPQ > 0.5) {
		color.rgb = LinearToST2084(float4(rgb_out, 0.0), 10000.0).rgb;
	} else {
		color.rgb = pow(saturate(rgb_out / SDR_PEAK_NITS), 1.0 / 2.2);
	}

	// Debug: sky-mask overlay (validation only — no tone effect yet). Cyan on
// the sky region from the R8 t2 mask; rides _reserved5 (bMLSkyDebugVis).
	if (_reserved5 > 0.5) {
		// Full-res 640x360 mask at the CONTENT uv — same remap as the tone leg,
		// so the cyan overlay shows exactly which pixels the tone is affecting.
		// Uses rawS (not strength-gated S) so the mask is visible at any strength.
		float S2 = smoothstep(0.25, 0.75, rawS);
		color.rgb = lerp(color.rgb, float3(0.0, 1.0, 1.0) * 0.6, S2 * 0.5);
	}

	// Debug: track-colored ellipses
	if (FaceDebugVis > 0.5 && NumFaces > 0) {
		uint texw, texh;
		tex.GetDimensions(texw, texh);
		float2 texsz = float2((float)texw, (float)texh);
		float2 uv = input.Tex;
		float lineW = 2.5 / texsz.x;
		float trackIds[4] = { FaceTrackId.x, FaceTrackId.y, FaceTrackId.z, FaceTrackId.w };

		if (skin_blend > 0.01) {
			color.rgb = lerp(color.rgb, float3(0.0, 1.0, 0.3), skin_blend * 0.4);
		}

		for (int fi = 0; fi < NumFaces; fi++) {
			float4 box = FaceBox[fi];
			float2 center = box.xy + box.zw * 0.5;
			float2 radius = box.zw * 0.5 * FaceExpand;
			float2 dd = (uv - center) / max(radius, 0.001);
			float r = length(dd);

			float hue = frac(trackIds[fi] * 0.618034);
			float3 trkColor = saturate(abs(frac(hue + float3(0.0, 0.6667, 0.3333)) * 6.0 - 3.0) - 1.0);
			if (abs(r - 1.0) < lineW / min(radius.x, radius.y)) {
				color.rgb = trkColor;
			}
		}
	}

	return color;
}
