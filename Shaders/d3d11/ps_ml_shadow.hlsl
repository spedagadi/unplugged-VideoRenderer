// Zion ML grade — dual-band detail enhancement.
// Band 1 (shadow detail): DoG σ=1/σ=2 — fine texture in shadows.
// Band 2 (contrast recovery): DoG σ=2/σ=4 — medium structure in midtones.
// Frequency-separated: no double-boost between bands.
// Soft-curve compressor, applied as luma ratio to preserve hue.
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
	float MLShadowToe;       // model prediction [0.0, 0.4] — gates shadow band
	float MLShadowKnee;      // 0.25 (unused)
	float MLShadowStrength;  // shadow band gain (0 = off)
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
	float _reserved0;        // debug toggle
	float _reserved1;        // shadow zone ceiling
	float _reserved2;        // shadow skin protect
	float _reserved3;        // shadow ratio cap
	float _reserved4;
	float _reserved5;
	// P0 — black bars (skip over)
	float uBarTop;
	float uBarBottom;
	float uBarV0;
	float uBarSpan;
	float uBarFeather;
	float uBarBlack;
	float uBarHighlight;
	float _padBar;
	// Contrast recovery params (appended)
	float MCStrength;        // contrast band gain (0 = off)
	float MCZoneLo;          // zone floor (default 0.15)
	float MCSkinProtect;     // skin protection
	float MCRatioCap;        // ratio cap
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
	float4 csrc = tex.Sample(samp, input.Tex);

	if (IsPQ > 0.5) {
		csrc.rgb = ST2084ToLinear(csrc, 10000.0f).rgb;
		csrc.rgb = pow(max(csrc.rgb, 0.0), 1.0 / 2.2);
	}

	float3 center = csrc.rgb;
	float y_c = dot(center, luma_w);

	// Shadow zone: full below zone_ceil*0.25, fading to 0 at zone_ceil
	float zone_ceil = _reserved1;
	float sh_gate = smoothstep(0.01, 0.05, MLShadowToe);
	float sh_zone = smoothstep(zone_ceil, zone_ceil * 0.25, y_c);
	float sh_mask = sh_gate * sh_zone;

	// Contrast zone: complementary — fading in above MCZoneLo, full at MCZoneLo*2.5
	float mc_zone = smoothstep(MCZoneLo, MCZoneLo * 2.5, y_c);
	float mc_hi = smoothstep(0.95, 0.80, y_c);
	float mc_mask = mc_zone * mc_hi;

	bool sh_debug = _reserved0 > 0.5;
	bool mc_debug = _reserved4 > 0.5;
	bool debug = sh_debug || mc_debug;
	bool sh_active = MLShadowStrength > 0.001 && sh_mask > 0.001;
	bool mc_active = MCStrength > 0.001 && mc_mask > 0.001;

	if (!debug && !sh_active && !mc_active) {
		if (IsPQ > 0.5) {
			float3 pq = pow(max(center, 0.0), 2.2);
			return float4(LinearToST2084(float4(pq, 1.0), 10000.0f).rgb, csrc.a);
		}
		return float4(center, csrc.a);
	}
	if (!debug && y_c < 0.001) {
		if (IsPQ > 0.5) {
			float3 pq = pow(max(center, 0.0), 2.2);
			return float4(LinearToST2084(float4(pq, 1.0), 10000.0f).rgb, csrc.a);
		}
		return float4(center, csrc.a);
	}

	uint texw, texh;
	tex.GetDimensions(texw, texh);
	const float2 texel = 1.0 / float2((float)texw, (float)texh);

	// Skin detection (shared by both bands)
	float r_res = center.r - y_c;
	float b_res = center.b - y_c;
	float cb = b_res / 1.8556;
	float cr = r_res / 1.5748;
	float2 skin_uv = saturate(float2((cr - MLSkinLUTCR0) / (MLSkinLUTCR1 - MLSkinLUTCR0),
	                                  (cb - MLSkinLUTCB0) / (MLSkinLUTCB1 - MLSkinLUTCB0)));
	float skin_w = skin_lut.Sample(sampSkin, skin_uv).r;

	// σ=2 blur is shared between both bands
	float y_s2 = blur_s2(input.Tex, texel);

	// --- Band 1: shadow detail (σ=1 − σ=2) ---
	float sh_boost = 0.0;
	if (sh_active || debug) {
		float y_s1 = blur_s1(input.Tex, texel);
		float sh_detail = y_s1 - y_s2;
		sh_detail *= smoothstep(0.012, 0.003, abs(sh_detail));

		float sh_max_ratio = _reserved3;
		float sh_max_boost = y_c * (sh_max_ratio - 1.0);
		float sh_d = sh_detail * sh_mask;
		float sh_gain = sh_d * MLShadowStrength / max(sh_max_boost, 0.0001);
		sh_boost = sh_max_boost * sh_gain / (1.0 + abs(sh_gain));
	}

	// --- Band 2: contrast recovery (σ=2 − σ=4) ---
	float mc_boost = 0.0;
	if (mc_active || debug) {
		float y_s4 = blur_s4(input.Tex, texel);
		float mc_detail = y_s2 - y_s4;
		mc_detail *= smoothstep(0.03, 0.008, abs(mc_detail));

		float mc_max_ratio = MCRatioCap;
		float mc_max_boost = y_c * (mc_max_ratio - 1.0);
		float mc_d = mc_detail * mc_mask;
		float mc_gain = mc_d * MCStrength / max(mc_max_boost, 0.0001);
		mc_boost = mc_max_boost * mc_gain / (1.0 + abs(mc_gain));
	}

	// Combine both bands, then suppress on skin
	float y_new = max(y_c + sh_boost + mc_boost, 0.001);
	float skin_protect = max(_reserved2, MCSkinProtect);
	float skin_guard = pow(1.0 - saturate(skin_w), 1.0 + skin_protect * 4.0);
	y_new = lerp(y_c, y_new, skin_guard);
	float ratio = y_new / y_c;
	float3 out_rgb = saturate(center * ratio);

	if (debug) {
		float3 dbg = center * 0.4;
		if (sh_debug && mc_debug) {
			dbg.g += max(sh_mask, mc_mask) * 0.6;
			float skin_atten = max(_reserved2 * saturate(skin_w),
			                       MCSkinProtect * saturate(skin_w));
			dbg.r += skin_atten * 0.8;
			dbg.b += saturate((ratio - 1.0) * 5.0);
		} else if (sh_debug) {
			dbg.g += sh_mask * 0.6;
			dbg.r += _reserved2 * saturate(skin_w) * 0.8;
			float sh_ratio = max(y_c + sh_boost, 0.001) / y_c;
			dbg.b += saturate((sh_ratio - 1.0) * 5.0);
		} else {
			dbg.g += mc_mask * 0.6;
			dbg.r += MCSkinProtect * saturate(skin_w) * 0.8;
			float mc_ratio = max(y_c + mc_boost, 0.001) / y_c;
			dbg.b += saturate((mc_ratio - 1.0) * 5.0);
		}
		return float4(saturate(dbg), csrc.a);
	}

	if (IsPQ > 0.5) {
		out_rgb = pow(max(out_rgb, 0.0), 2.2);
		out_rgb = LinearToST2084(float4(out_rgb, 1.0), 10000.0f).rgb;
	}

	return float4(out_rgb, csrc.a);
}
