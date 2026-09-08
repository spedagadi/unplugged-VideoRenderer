// Zion ML grade — dual-band detail enhancement (ICtCp).
// Band 1 (shadow detail): DoG σ=1/σ=2 — fine texture in shadows.
// Band 2 (contrast recovery): DoG σ=2/σ=4 — medium structure in midtones.
// DoG computed in gamma-2.2 luma (efficient for frequency decomposition).
// Boost applied to ICtCp I channel — no chroma shift, no skin protection needed.
#include "../convert/st2084.hlsl"
#include "../convert/ictcp.hlsl"

Texture2D tex : register(t0);
SamplerState samp : register(s0);
Texture2D skin_lut : register(t1);   // bound by C++ but unused in ICtCp path
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
	float MLShadowStrength;
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
};

static const float3 luma_w = float3(0.2126, 0.7152, 0.0722);

float sample_gamma_luma(float2 uv)
{
	float3 rgb = tex.Sample(samp, uv).rgb;
	if (IsPQ > 0.5) {
		rgb = pow(max(ST2084ToLinear(float4(rgb, 1.0), 10000.0f).rgb, 0.0), 1.0 / 2.2);
	}
	return dot(rgb, luma_w);
}

// σ=1 Gaussian blur — 3×3 bilinear-optimized (9 taps, effective 5×5)
float blur_s1(float2 uv, float2 texel)
{
	static const float w[3] = { 0.40264, 0.29868, 0.29868 };
	static const float o[3] = { 0.0, 1.18261, -1.18261 };
	float sum = 0.0;
	[unroll] for (int j = 0; j < 3; j++) {
		[unroll] for (int i = 0; i < 3; i++) {
			sum += w[i] * w[j] * sample_gamma_luma(uv + float2(o[i] * texel.x, o[j] * texel.y));
		}
	}
	return sum;
}

// σ=2 Gaussian blur — 5×5 bilinear-optimized (25 taps, effective 9×9)
float blur_s2(float2 uv, float2 texel)
{
	static const float w[5] = { 0.20416, 0.30401, 0.30401, 0.09391, 0.09391 };
	static const float o[5] = { 0.0, 1.40733, -1.40733, 3.29424, -3.29424 };
	float sum = 0.0;
	[unroll] for (int j = 0; j < 5; j++) {
		[unroll] for (int i = 0; i < 5; i++) {
			sum += w[i] * w[j] * sample_gamma_luma(uv + float2(o[i] * texel.x, o[j] * texel.y));
		}
	}
	return sum;
}

// σ=4 Gaussian blur — 7×7 bilinear-optimized (49 taps, effective 13×13)
float blur_s4(float2 uv, float2 texel)
{
	static const float w[7] = { 0.11122, 0.20595, 0.20595, 0.15142, 0.15142, 0.08703, 0.08703 };
	static const float o[7] = { 0.0, 1.47657, -1.47657, 3.44564, -3.44564, 5.41487, -5.41487 };
	float sum = 0.0;
	[unroll] for (int j = 0; j < 7; j++) {
		[unroll] for (int i = 0; i < 7; i++) {
			sum += w[i] * w[j] * sample_gamma_luma(uv + float2(o[i] * texel.x, o[j] * texel.y));
		}
	}
	return sum;
}

float4 main(PS_INPUT input) : SV_Target
{
	// Shadow/contrast enhancement disabled — investigating gradient artifacts
	return tex.Sample(samp, input.Tex);
}
