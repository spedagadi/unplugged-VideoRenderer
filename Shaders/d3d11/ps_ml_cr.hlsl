// Zion ML grade — Construct CR (log-space unsharp mask).
// Port of libplacebo cr_unsharpmask_hook (ml_render.c), WITHOUT grain gating
// (user decision: grain detection is buggy). 5x5 Gaussian in log-luma space,
// stride 4 (~16 px radius at 4K), high/low luma suppression, border fade.
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
	float MLCRStrength;   // [0, 0.5], 0 = off
	float MLCRGate;
	float MLRadiKnee;
	float MLRadiStrength;
	float MLRadiShoulder;
	float MLRadiGate;
};

static const float3 luma_w = float3(0.2126, 0.7152, 0.0722);

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
	const float2 pt = 1.0 / texsz;

	float3 center = color.rgb;
	float y_c = dot(center, luma_w);
	const float eps = 1e-4;
	const float log_yc = log(max(y_c, eps));

	if (MLCRGate > 0.5 && MLCRStrength > 0.001 && y_c > 0.02) {
		// 5x5 Gaussian blur of log-luma, stride 4
		float blur_acc = 0.0, tw = 0.0;
		[unroll] for (int dy = -2; dy <= 2; dy++) {
			[unroll] for (int dx = -2; dx <= 2; dx++) {
				float sy = dot(tex.Sample(samp, input.Tex + float2((float)dx, (float)dy) * 4.0 * pt).rgb, luma_w);
				if (sy > 0.02) {
					const float d2 = (float)(dx * dx + dy * dy);
					const float w = exp(-d2 * 0.25);
					blur_acc += w * log(max(sy, eps));
					tw += w;
				}
			}
		}
		const float log_blur = blur_acc / max(tw, 0.001);
		float detail = log_yc - log_blur;

		float cr_edge = smoothstep(0.0, 9.0 / texsz.y, min(input.Tex.y, 1.0 - input.Tex.y))
		              * smoothstep(0.0, 9.0 / texsz.x, min(input.Tex.x, 1.0 - input.Tex.x));
		if (cr_edge > 0.001) {
			const float hi_sup = 1.0 - smoothstep(0.85, 1.0, y_c);
			const float lo_sup = smoothstep(0.01, 0.05, y_c);
			const float log_new = log_yc + MLCRStrength * detail * hi_sup * lo_sup * cr_edge;
			const float y_new = clamp(exp(log_new) - eps, 0.0, 1.0);
			const float ratio = y_new / y_c;
			color.rgb = clamp(center * ratio, 0.0, 1.0);
		}
	}

	if (IsPQ > 0.5) {
		color.rgb = pow(max(color.rgb, 0.0), 2.2);
		color.rgb = LinearToST2084(color, 10000.0f).rgb;
	}

	return color;
}