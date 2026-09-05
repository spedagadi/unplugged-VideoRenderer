// Zion ML grade — midtone gamma ("The Oracle").
// Port of libplacebo's l2_hook (src/ml_render.c).
//
// Operates on gamma-2.2 encoded luma: raises/lowers the midtones via the
// smoothstep(0.08, 0.45) fade-in band, guarded from the highlights by an
// adaptive guard threshold, with hunt compensation and highlight desaturation.
// When IsPQ != 0 the signal is wrapped PQ -> linear -> gamma2.2 before the
// grade and unwrapped after (HDR passthrough output); HDR->SDR output is
// already gamma-2.2-like and is graded in place.
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
	float MLGamma;           // predicted gamma [0.5, 1.5], 1.0 = neutral
	float MLShadowToe;       // shared cbuffer; unused here
	float MLShadowKnee;
	float MLShadowDehaze;
	float MLHighlightRolloff;
	float MLHighlightKnee;
	float MLGuard;            // highlight guard (adaptive on scene peak)
	float MLHunt;             // hunt/chroma compensation
	float MLL1Max;            // scene peak in PQ
	float MLHiDesat;          // highlight desaturation strength
	float IsPQ;               // 1 = input is PQ (HDR passthrough), 0 = gamma-2.2 (HDR->SDR)
	float Padding;
	// Phase 1b — chroma vector tuning (unused in this pass, layout shared)
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
	float MLPad1;
	float MLPad2;
};

static const float3 luma_w = float3(0.2126, 0.7152, 0.0722);

// Film-grain gate: 5-neighbour log-luma variance (same metric the libplacebo
// shadow hook computes; used to attenuate the grade on grainy content).
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
	float4 color = tex.Sample(samp, input.Tex);

	if (IsPQ > 0.5) {
		color.rgb = ST2084ToLinear(color, 10000.0f).rgb;
		color.rgb = pow(max(color.rgb, 0.0), 1.0 / 2.2);
	}

	uint texw, texh;
	tex.GetDimensions(texw, texh);
	const float2 texsz = float2((float)texw, (float)texh);

	float3 orig_rgb = color.rgb;
	float y = dot(color.rgb, luma_w);
	float grain = GrainGate(y, texsz, input.Tex);
	float cb = color.b - y;
	float cr = color.r - y;
	float y_pre = y;
	float l2_gamma = MLGamma;

	if (l2_gamma != 1.0 && y > 0.001 && y < 0.999) {
		float hi = smoothstep(MLGuard, 0.98, y);
		if (l2_gamma > 1.0) {
			float fade = smoothstep(0.08, 0.45, y);
			float eff = fade * (1.0 - hi);
			y = lerp(y, pow(y, 1.0 / l2_gamma), eff);
		} else {
			// symmetric gamma: for l2_gamma < 1 the exponent 1/l2_gamma > 1,
			// so darker values get pushed DOWN (midtone darken). (libplacebo's
			// original else-branch used pow(y, lerp(1.0, l2_gamma, ...)) which
			// also raised values - both sides brightened, making 1.5 vs 0.75
			// visually indistinguishable.)
			float fade2 = smoothstep(0.08, 0.45, y);
			float eff = fade2 * (1.0 - hi);
			y = lerp(y, pow(y, 1.0 / l2_gamma), eff);
		}
	}

	// Hunt/desat only accompany an actual gamma; at gamma == 1.0 the pass is
	// exactly identity (libplacebo does not even register the L2 hook then).
	if (l2_gamma != 1.0) {
		float hunt = 1.0 + MLHunt * pow(MLL1Max, 2.0);
		if (y > 0.001 && y_pre > 0.001) {
			float gamma_comp = pow(y_pre / y, 0.15);
			cr *= hunt * gamma_comp;
			cb *= hunt * gamma_comp;
		}
		float desat = smoothstep(MLGuard, 0.95, y);
		cr *= (1.0 - desat * MLHiDesat);
		cb *= (1.0 - desat * MLHiDesat);
	}

	float r_out = y + cr;
	float g_out = y - 0.2126 / 0.7152 * cr - 0.0722 / 0.7152 * cb;
	float b_out = y + cb;
	float mx = max(max(r_out, g_out), b_out);
	float mn = min(min(r_out, g_out), b_out);
	if (mx > 1.0 || mn < 0.0) {
		float s = 1.0;
		if (mx > 1.0 && mx > y + 0.001) s = min(s, (1.0 - y) / (mx - y));
		if (mn < 0.0 && mn < y - 0.001) s = min(s, y / (y - mn));
		cr *= max(s, 0.0);
		cb *= max(s, 0.0);
	}

	color.r = saturate(y + cr);
	color.g = saturate(y - 0.2126 / 0.7152 * cr - 0.0722 / 0.7152 * cb);
	color.b = saturate(y + cb);
	color.rgb = lerp(color.rgb, orig_rgb, grain);

	if (IsPQ > 0.5) {
		color.rgb = pow(max(color.rgb, 0.0), 2.2);
		color.rgb = LinearToST2084(color, 10000.0f).rgb;
	}

	return color;
}