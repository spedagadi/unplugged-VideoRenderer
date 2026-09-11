// Zion ML grade — chroma vector tuning (ICtCp "Spectrum Shift v2").
// Adaptive chroma boost on Ct/Cp channels in ICtCp perceptual space.
// Skin protection via trained P(skin) LUT (detection in Cr/Cb, boost in Ct/Cp).
// When MLChromaGate is off the pass outputs its input unchanged.
#include "../convert/st2084.hlsl"
#include "../convert/ictcp.hlsl"

Texture2D tex : register(t0);
SamplerState samp : register(s0);
Texture2D skin_lut : register(t1);
SamplerState sampSkin : register(s1);
Texture2D sky_mask : register(t2);   // 640x360 R8G8 semseg mask (R=sky, G=person)
SamplerState sampSky : register(s2);

struct PS_INPUT
{
	float4 Pos : SV_POSITION;
	float2 Tex : TEXCOORD;
};

// The C++ MLParams_t is a single 320-byte layout shared by every ps_ml_* pass.
// Offsets are absolute (computed by the compiler from THIS declaration), so this
// shader must mirror the full struct — a short declaration would shift the
// person fields (and MLSkyUV*) onto the wrong bytes.
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
	float MLSkinPreserve;  // 0..1 luma re-anchor toward source skin tone (unused in chroma; gamma t4)
	float MLSkinPad0;
	float MLSkinPad1;
};

static const float3 luma_w = float3(0.2126, 0.7152, 0.0722);

float4 main(PS_INPUT input) : SV_Target
{
	float4 color = tex.Sample(samp, input.Tex);

	if (MLChromaGate < 0.5)
		return color;

	// Convert to linear nits → ICtCp
	float3 lin;
	if (IsPQ > 0.5) {
		lin = ST2084ToLinear(float4(color.rgb, 0.0), 10000.0).rgb;
	} else {
		lin = pow(max(color.rgb, 0.0), 2.2) * SDR_PEAK_NITS;
	}
	float3 ictcp = linear_to_ictcp(lin);

	// Skin detection: use gamma-2.2 Cr/Cb with existing trained LUT
	float3 g22;
	if (IsPQ > 0.5) {
		g22 = pow(max(lin, 0.0), 1.0 / 2.2);
	} else {
		g22 = color.rgb;
	}
	float y_g22 = dot(g22, luma_w);
	float cr = (g22.r - y_g22) / 1.5748;
	float cb = (g22.b - y_g22) / 1.8556;
	float2 skin_uv = saturate(float2(
		(cr - MLSkinLUTCR0) / (MLSkinLUTCR1 - MLSkinLUTCR0),
		(cb - MLSkinLUTCB0) / (MLSkinLUTCB1 - MLSkinLUTCB0)));
	float skin_weight = skin_lut.Sample(sampSkin, skin_uv).r;

	// Person gate (stage 4): LUT weight is kept as-is OUTSIDE people, and
	// multiplied by the person mask INSIDE people — protection concentrates on
	// actual people (kills red/orange-object false positives). Strength 0 falls
	// back to the plain LUT exactly (lerp(a,b,0) is IEEE-exact) → bit-identical.
	float eff = skin_weight;
	if (MLSkinPersonStrength > 0.001) {
		float2 cuv = (input.Tex - float2(MLSkyUVOffX, MLSkyUVOffY))
			/ max(float2(MLSkyUVScX, MLSkyUVScY), 0.001f);
		const float inContent = (all(cuv >= 0.0) && all(cuv <= 1.0)) ? 1.0 : 0.0;
		float P = sky_mask.Sample(sampSky, saturate(cuv)).g * inContent;
		eff = lerp(skin_weight, skin_weight * P, MLSkinPersonStrength);
	}

	// Chroma boost curve: 1.0 at black → neutral at knee → fire at peak.
	// Monotonic ramp matches tone-mapping desaturation profile (highlights
	// lose more saturation than shadows). No boost on near-black noise.
	float I_peak = (IsPQ > 0.5) ? max(MLL1Max, 0.01) : SDR_I_PEAK_PQ;
	float I_norm = ictcp.x / I_peak;
	float luma_boost;
	if (I_norm <= MLChromaKnee) {
		float t = I_norm / max(MLChromaKnee, 0.01);
		luma_boost = lerp(1.0, MLChromaNeutral, t);
	} else {
		float t = saturate((I_norm - MLChromaKnee) / max(1.0 - MLChromaKnee, 0.01));
		luma_boost = lerp(MLChromaNeutral, MLChromaFire, pow(t, 1.5));
	}

	// Suppress boost on skin (person-gated LUT weight)
	float chroma_scalar = lerp(luma_boost, 1.0,
	                           saturate(eff * MLChromaSkinProtect));
	chroma_scalar = clamp(chroma_scalar, 1.0, 1.50);

	// Scale Ct/Cp — perceptually uniform chroma boost
	ictcp.y *= chroma_scalar;
	ictcp.z *= chroma_scalar;

	// Inverse: ICtCp → linear nits → output
	float3 rgb_out = ictcp_to_linear(ictcp);

	if (IsPQ > 0.5) {
		color.rgb = LinearToST2084(float4(rgb_out, 0.0), 10000.0).rgb;
	} else {
		color.rgb = pow(saturate(rgb_out / SDR_PEAK_NITS), 1.0 / 2.2);
	}

	return color;
}
