// Zion ML grade — highlight roll-off.
// Port of libplacebo's highlight_bilateral_hook (src/ml_render.c).
//
// Soft shoulder shaping above highlight_knee:
//   shoulder = rolloff * t^2 * (1 - y),  t = (y - knee) / (1 - knee)
// Positive rolloff lifts the shoulder (recovery), negative rolls it off
// (compression). Grain-gated the same way as the shadow pass.
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
	float MLHighlightRolloff; // predicted roll-off [-0.3, 0.4]
	float MLHighlightKnee;    // adaptive on gamma
	float MLGuard;
	float MLHunt;
	float MLL1Max;
	float MLHiDesat;
	float IsPQ;
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
	float rolloff_eff = MLHighlightRolloff;
	float3 out_rgb = center;

	if (abs(rolloff_eff) > 0.005 && y_c > MLHighlightKnee && y_c < 0.995) {
		float span = max(1.0 - MLHighlightKnee, 0.01);
		float t = (y_c - MLHighlightKnee) / span;
		float shoulder = rolloff_eff * t * t * (1.0 - y_c);
		float y_new = saturate(y_c + shoulder);
		float ratio_hi = (y_c > 0.001) ? y_new / y_c : 1.0;
		out_rgb = saturate(center * ratio_hi);
	}

	if (IsPQ > 0.5) {
		out_rgb = pow(max(out_rgb, 0.0), 2.2);
		out_rgb = LinearToST2084(float4(out_rgb, 1.0), 10000.0f).rgb;
	}

	return float4(out_rgb, csrc.a);
}