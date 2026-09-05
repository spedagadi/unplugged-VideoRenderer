// Zion ML grade — shadow toe-lift + dehaze.
// Port of libplacebo's shadow_bilateral_hook (src/ml_render.c).
//
// Multiplicative toe-lift anchored at black below shadow_knee (0.25):
//   y_lifted = y + toe * (y/knee) * (1 - y/knee)^2
// C1-continuous, monotonic; grain-gated on 5-neighbour log-luma variance.
// Dehaze is carried in the shared params but unused while SDR handling is
// deferred (MLShadowDehaze = 0).
#include "../convert/st2084.hlsl"

Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct PS_INPUT
{
	float4 Pos : SV_POSITION;
	float2 Tex : TEXCOORD;
};

cbuffer MLParams : register(b0)
{
	float MLGamma;
	float MLShadowToe;       // predicted toe-lift [0.0, 0.4]
	float MLShadowKnee;      // 0.25
	float MLShadowDehaze;    // 0.0 (unused while SDR is out of scope)
	float MLHighlightRolloff;
	float MLHighlightKnee;
	float MLGuard;
	float MLHunt;
	float MLL1Max;
	float MLHiDesat;
	float IsPQ;
	float Padding;
};

static const float3 luma_w = float3(0.2126, 0.7152, 0.0722);

float GrainGate(float y_c, float2 texsz, float2 uv)
{
	float2 pt = 1.0 / texsz;
	const float g_eps = 1e-4;
	float g_lc = log(max(y_c, g_eps));
	float g_sum = g_lc;
	float g_sq = g_lc * g_lc;
	float gl1 = log(max(dot(tex.Sample(samp, uv + float2(-2.0, 0.0) * pt).rgb, luma_w), g_eps)); g_sum += gl1; g_sq += gl1 * gl1;
	float gl2 = log(max(dot(tex.Sample(samp, uv + float2( 2.0, 0.0) * pt).rgb, luma_w), g_eps)); g_sum += gl2; g_sq += gl2 * gl2;
	float gl3 = log(max(dot(tex.Sample(samp, uv + float2(0.0, -2.0) * pt).rgb, luma_w), g_eps)); g_sum += gl3; g_sq += gl3 * gl3;
	float gl4 = log(max(dot(tex.Sample(samp, uv + float2(0.0,  2.0) * pt).rgb, luma_w), g_eps)); g_sum += gl4; g_sq += gl4 * gl4;
	float g_mean = g_sum / 5.0;
	float g_var = max(g_sq / 5.0 - g_mean * g_mean, 0.0);
	return smoothstep(0.003, 0.02, g_var);
}

float4 main(PS_INPUT input) : SV_Target
{
	float4 csrc = tex.Sample(samp, input.Tex);

	if (IsPQ > 0.5) {
		csrc.rgb = ST2084ToLinear(csrc, 10000.0f).rgb;
		csrc.rgb = pow(max(csrc.rgb, 0.0), 1.0 / 2.2);
	}

	uint texw, texh;
	tex.GetDimensions(texw, texh);
	const float2 texsz = float2((float)texw, (float)texh);

	float3 center = csrc.rgb;
	float y_c = dot(center, luma_w);
	float grain = GrainGate(y_c, texsz, input.Tex);
	float3 out_rgb = center;

	float toe_eff = MLShadowToe * (1.0 - grain);
	if (toe_eff > 0.001 && y_c < MLShadowKnee && y_c > 0.001) {
		float t = 1.0 - y_c / MLShadowKnee;
		float ramp = y_c / MLShadowKnee;
		float y_lifted = y_c + toe_eff * ramp * t * t;
		float ratio_toe = y_lifted / y_c;
		out_rgb = saturate(center * ratio_toe);
		y_c = y_lifted;
	}

	float dehaze_eff = MLShadowDehaze * (1.0 - grain);
	if (dehaze_eff > 0.01 && y_c > 0.001 && y_c < 0.30) { // dehaze knee fixed at 0.30
		float dh_t = y_c / 0.30;
		float dh_fade = 1.0 - dh_t * dh_t;
		float y_hazed = 0.30 * pow(dh_t, 1.0 + dehaze_eff * 2.0 * dh_fade);
		float dh_ratio = y_hazed / y_c;
		out_rgb = saturate(out_rgb * dh_ratio);
	}

	if (IsPQ > 0.5) {
		out_rgb = pow(max(out_rgb, 0.0), 2.2);
		out_rgb = LinearToST2084(float4(out_rgb, 1.0), 10000.0f).rgb;
	}

	return float4(out_rgb, csrc.a);
}