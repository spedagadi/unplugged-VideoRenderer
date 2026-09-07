// Zion ML grade — Resonance (adaptive radiance highlight lift).
// Port of libplacebo's radiance_hook (ml_radiance.c): bell-shaped mid-highlight
// lift with a soft shoulder so the boost retreats before specular whites.
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
	float MLCRStrength;
	float MLCRGate;
	float MLRadiKnee;
	float MLRadiStrength;   // [0, 0.6]
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

	if (MLRadiGate > 0.5 && MLRadiStrength > 0.001) {
		const float y = dot(color.rgb, luma_w);
		// rising bell above knee
		const float t = saturate((y - MLRadiKnee) / max(1.0 - MLRadiKnee, 0.001));
		const float envelope = t * t * (3.0 - 2.0 * t);
		// soft shoulder: fade back above shoulder
		const float s = saturate((y - MLRadiShoulder) / max(1.0 - MLRadiShoulder, 0.001));
		const float shoulder_fade = 1.0 - s * s * (3.0 - 2.0 * s);
		const float eff_env = envelope * shoulder_fade;

		const float target_y = lerp(y, 1.0, eff_env);
		const float final_y = lerp(y, target_y, MLRadiStrength);
		const float chroma_taper = 1.0 - 0.14 * eff_env * MLRadiStrength;

		const float cb = (color.b - y) * chroma_taper;
		const float cr = (color.r - y) * chroma_taper;
		color.r = saturate(final_y + cr);
		color.g = saturate(final_y - 0.2126 / 0.7152 * cr - 0.0722 / 0.7152 * cb);
		color.b = saturate(final_y + cb);
	}

	if (IsPQ > 0.5) {
		color.rgb = pow(max(color.rgb, 0.0), 2.2);
		color.rgb = LinearToST2084(color, 10000.0f).rgb;
	}

	return color;
}