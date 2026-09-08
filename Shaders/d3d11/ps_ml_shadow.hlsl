// Zion ML grade — shadow micro-contrast enhancement.
// DoG bandpass (σ=1/σ=2) isolates texture detail, amplitude gate
// rejects edges, soft-curve compressor from user slider scales the effect.
// Applied as luma ratio to preserve hue and saturation.
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
	float MLShadowToe;       // model prediction [0.0, 0.4] — gates enhancement
	float MLShadowKnee;      // 0.25 (unused)
	float MLShadowStrength;  // continuous gain for soft-curve compressor (0 = off)
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

// σ=1 Gaussian blur on luma — 3×3 bilinear-optimized (9 taps, effective 5×5)
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

// σ=2 Gaussian blur on luma — 5×5 bilinear-optimized (25 taps, effective 9×9)
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

float4 main(PS_INPUT input) : SV_Target
{
	float4 csrc = tex.Sample(samp, input.Tex);

	if (IsPQ > 0.5) {
		csrc.rgb = ST2084ToLinear(csrc, 10000.0f).rgb;
		csrc.rgb = pow(max(csrc.rgb, 0.0), 1.0 / 2.2);
	}

	float3 center = csrc.rgb;
	float y_c = dot(center, luma_w);

	float zone_ceil = _reserved1;
	float gate = smoothstep(0.01, 0.05, MLShadowToe);
	float zone = smoothstep(zone_ceil, zone_ceil * 0.25, y_c);
	float mask = gate * zone;

	if (_reserved0 > 0.5) {
		// fall through to debug overlay even with low mask
	}
	else if (MLShadowStrength < 0.001 || y_c < 0.001 || mask < 0.001) {
		if (IsPQ > 0.5) {
			float3 pq = pow(max(center, 0.0), 2.2);
			return float4(LinearToST2084(float4(pq, 1.0), 10000.0f).rgb, csrc.a);
		}
		return float4(center, csrc.a);
	}

	uint texw, texh;
	tex.GetDimensions(texw, texh);
	const float2 texel = 1.0 / float2((float)texw, (float)texh);

	float y_s1 = blur_s1(input.Tex, texel);
	float y_s2 = blur_s2(input.Tex, texel);
	float detail = y_s1 - y_s2;

	detail *= smoothstep(0.012, 0.003, abs(detail));

	float r_res = center.r - y_c;
	float b_res = center.b - y_c;
	float cb = b_res / 1.8556;
	float cr = r_res / 1.5748;
	float2 skin_uv = saturate(float2((cr - MLSkinLUTCR0) / (MLSkinLUTCR1 - MLSkinLUTCR0),
	                                  (cb - MLSkinLUTCB0) / (MLSkinLUTCB1 - MLSkinLUTCB0)));
	float skin_w = skin_lut.Sample(sampSkin, skin_uv).r;
	float skin_scale = 1.0 - _reserved2 * saturate(skin_w);

	float max_ratio = _reserved3;
	float max_boost = y_c * (max_ratio - 1.0);
	float d = detail * mask * skin_scale;
	float gain = d * MLShadowStrength / max(max_boost, 0.0001);
	float boost = max_boost * gain / (1.0 + abs(gain));
	float y_new = max(y_c + boost, 0.001);
	float ratio = y_new / y_c;
	float3 out_rgb = saturate(center * ratio);

	if (_reserved0 > 0.5) {
		float3 dbg = center * 0.4;
		dbg.g += mask * 0.6;
		dbg.r += (1.0 - skin_scale) * 0.8;
		dbg.b += saturate((ratio - 1.0) * 5.0);
		return float4(saturate(dbg), csrc.a);
	}

	if (IsPQ > 0.5) {
		out_rgb = pow(max(out_rgb, 0.0), 2.2);
		out_rgb = LinearToST2084(float4(out_rgb, 1.0), 10000.0f).rgb;
	}

	return float4(out_rgb, csrc.a);
}
