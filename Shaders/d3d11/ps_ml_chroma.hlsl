// Zion ML grade — chroma vector tuning ("Spectrum Shift").
// Port of libplacebo's chroma_tuner_hook (src/ml_render.c, trained-LUT body).
//
// Decomposes the gamma-2.2 signal into luma + chroma residuals, applies an
// adaptive chroma boost that ramps neutral->fire above the knee, and protects
// skin tones via a trained P(skin) LUT (WIDER FACE). When MLChromaGate is off
// (feature disabled or DoVi preferred) the pass outputs its input unchanged.
#include "../convert/st2084.hlsl"

Texture2D tex : register(t0);
SamplerState samp : register(s0);
Texture2D skin_lut : register(t1);
SamplerState sampSkin : register(s1);

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
	// Phase 1b — chroma vector tuning
	float MLChromaNeutral;     // chroma gain at/below the knee (1.0 == off)
	float MLChromaFire;        // chroma gain above the knee
	float MLChromaKnee;        // luma knee for the transition
	float MLChromaSkinProtect; // 0..1, pulls the boost toward 1 on skin
	float MLSkinLUTCR0;        // P(skin) grid bounds (cr/cb)
	float MLSkinLUTCR1;
	float MLSkinLUTCB0;
	float MLSkinLUTCB1;
	float MLChromaGate;        // 1.0 = pass active
	float MLHuntGate;
	float _reserved0;
	float _reserved1;
	float _reserved2;
	float _reserved3;
	float _reserved4;
	float _reserved5;
};

static const float3 luma_w = float3(0.2126, 0.7152, 0.0722);

float4 main(PS_INPUT input) : SV_Target
{
	float4 color = tex.Sample(samp, input.Tex);

	if (IsPQ > 0.5) {
		color.rgb = ST2084ToLinear(color, 10000.0f).rgb;
		color.rgb = pow(max(color.rgb, 0.0), 1.0 / 2.2);
	}

	if (MLChromaGate > 0.5) {
		const float y = dot(color.rgb, luma_w);
		float r_res = color.r - y;
		float b_res = color.b - y;
		float cb = b_res / 1.8556;
		float cr = r_res / 1.5748;

		// trained P(skin) lookup (sampled like libplacebo: lattice uv directly)
		float2 uv = float2((cr - MLSkinLUTCR0) / (MLSkinLUTCR1 - MLSkinLUTCR0),
		                   (cb - MLSkinLUTCB0) / (MLSkinLUTCB1 - MLSkinLUTCB0));
		uv = saturate(uv);
		float skin_weight = skin_lut.Sample(sampSkin, uv).r;

		float luma_boost = MLChromaNeutral;
		float denom = 1.0 - MLChromaKnee;
		if (y > MLChromaKnee && denom > 0.001) {
			float t = saturate((y - MLChromaKnee) / denom);
			luma_boost = lerp(MLChromaNeutral, MLChromaFire, pow(t, 1.5));
		}
		float chroma_scalar = lerp(luma_boost, 1.0,
		                           saturate(skin_weight * MLChromaSkinProtect));
		chroma_scalar = clamp(chroma_scalar, 1.0, 1.50);

		// green-safe taper: never let the reconstruction take G negative
		float g_denom = (0.2126 / 0.7152) * r_res + (0.0722 / 0.7152) * b_res;
		if (g_denom > 0.0) {
			float max_safe = y / g_denom;
			if (max_safe < chroma_scalar) {
				chroma_scalar = max(1.0, max_safe);
			}
		}

		r_res *= chroma_scalar;
		b_res *= chroma_scalar;
		color.r = saturate(y + r_res);
		color.g = saturate(y - (0.2126 / 0.7152) * r_res - (0.0722 / 0.7152) * b_res);
		color.b = saturate(y + b_res);
	}

	if (IsPQ > 0.5) {
		color.rgb = pow(max(color.rgb, 0.0), 2.2);
		color.rgb = LinearToST2084(color, 10000.0f).rgb;
	}

	return color;
}