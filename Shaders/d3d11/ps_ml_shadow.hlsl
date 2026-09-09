// Zion ML grade — micro-contrast (texture enhancement, ICtCp).
// Multiplicative clarity on a gamma-2.2 luma surface, applied to the ICtCp I
// channel: I_out = I * exp(MCStrength * zone * log(lum / blur_s1(lum))).
// Halos are impossible (multiplicative, ratio-form), smooth ramps are
// identity (lum ≈ blur), and shadows are gated by MCZoneLo. MCStrength is
// slider × per-scene automatic texture gate (computed CPU-side).
// Dual-band DoG infra (blur_s2/s4) retained unused for future detail bands.
#include "../convert/st2084.hlsl"
#include "../convert/ictcp.hlsl"

Texture2D tex : register(t0);
SamplerState samp : register(s0);
Texture2D skin_lut : register(t1);   // bound by C++ but unused in ICtCp path
SamplerState sampSkin : register(s1);
Texture2D sky_mask : register(t2);   // 640x360 R8 sky mask (content-aligned)
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
	// Face section — declared only to keep cbuffer offsets in sync (unused here)
	float FaceProtect;
	float FaceFeather;
	float FaceExpand;
	float FaceDebugVis;
	int   NumFaces;
	float3 _padFace;
	float4 FaceBox[4];
	float4 FaceTrackId;
	// Sky tone leg tail — the two uv fields remap the mask sample
	float MLSkyStrength;
	float MLSkyGuard;
	float MLSkyLuma;
	float MLSkyUVOffX;
	float MLSkyUVOffY;
	float MLSkyUVScX;
	float MLSkyUVScY;
	float _padSky;
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
	float4 color = tex.Sample(samp, input.Tex);

	// Fast path: micro-contrast disabled this frame (slider off or the
	// automatic texture gate zeroed this scene). Skip it only when the debug
	// heat is also off — heat must still draw to show the gate is live.
	if (MCStrength < 0.001 && _reserved4 < 0.5)
		return color;

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

	// Micro-contrast clarity (Design B, multiplicative — halo-impossible):
	// per-pixel deviation from a σ≈1 gamma-2.2 surround, lifted in log space.
	uint texw, texh;
	tex.GetDimensions(texw, texh);
	const float2 texel = 1.0f / float2((float)texw, (float)texh);
	float lm    = sample_gamma_luma(input.Tex);
	float b     = blur_s1(input.Tex, texel);   // fine surround (σ≈1)
	float logr  = clamp(log(lm / max(b, 1e-5)), -1.0, 1.0);
	float zone  = smoothstep(MCZoneLo, MCZoneLo + 0.15f, I_norm); // shadow gate
	// Sky-upweighted clarity: within sky, raise the lift so cloud layering
	// survives tone compression. Safe — flat sky has logr≈0 so nothing moves.
	// In-content gate kills bar bleed (see ps_ml_gamma).
	float2 cuv = (input.Tex - float2(MLSkyUVOffX, MLSkyUVOffY))
		/ max(float2(MLSkyUVScX, MLSkyUVScY), 0.001f);
	const float inContent = (all(cuv >= 0.0) && all(cuv <= 1.0)) ? 1.0 : 0.0;
	float S = sky_mask.Sample(sampSky, saturate(cuv)).r * inContent;
	float MCe = MCStrength * (1.0f + 2.0f * S);
	float lift = exp(MCe * zone * logr);

	// Excursion bound: dark side is naturally safe (lift<1); cap the bright side
	// so no pixel exceeds ~(1 + RatioCap×strength) × its input.
	float I_out = I_norm * lift;
	I_out = min(I_out, I_norm * (1.0 + MCRatioCap * MCStrength));

	ictcp.x = I_out * I_peak;

	// Inverse: ICtCp → linear nits → output encoding
	float3 rgb_out = ictcp_to_linear(ictcp);
	if (IsPQ > 0.5) {
		color.rgb = LinearToST2084(float4(rgb_out, 0.0), 10000.0).rgb;
	} else {
		color.rgb = pow(saturate(rgb_out / SDR_PEAK_NITS), 1.0 / 2.2);
	}

	// Debug heat: green shows the zone gate (independent of strength — so a
	// zero-gate scene still proves the pass ran); brightness encodes strength.
	// Channel: _reserved4 (bMLContrastDebugVis).
	if (_reserved4 > 0.5) {
		float g = saturate(zone);
		float s = saturate(MCStrength * 4.0);
		color.rgb = lerp(color.rgb, float3(0.1, 1.0, 0.3) * (0.35f + 0.65f * s), g * 0.6f);
	}

	return color;
}
