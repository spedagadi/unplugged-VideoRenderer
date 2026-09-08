// Zion ML grade — face detection color downsample.
// 2x2 box-filter downsample from full-res color to small BGRA8 render target.
// When the source is PQ (HDR), applies PQ→linear→gamma2.2 so the face
// detector sees SDR-like contrast (YuNet was trained on SDR images).
#include "../convert/st2084.hlsl"

Texture2D tex : register(t0);
SamplerState samp : register(s0);

cbuffer FaceDownParams : register(b0)
{
	float IsPQ;
	float3 _pad;
};

struct PS_INPUT
{
	float4 Pos : SV_POSITION;
	float2 Tex : TEXCOORD;
};

float4 main(PS_INPUT input) : SV_Target
{
	uint texw, texh;
	tex.GetDimensions(texw, texh);
	float2 halfTexel = 0.5 / float2((float)texw, (float)texh);

	float3 acc  = tex.Sample(samp, input.Tex + float2(-halfTexel.x, -halfTexel.y)).rgb;
	acc        += tex.Sample(samp, input.Tex + float2( halfTexel.x, -halfTexel.y)).rgb;
	acc        += tex.Sample(samp, input.Tex + float2(-halfTexel.x,  halfTexel.y)).rgb;
	acc        += tex.Sample(samp, input.Tex + float2( halfTexel.x,  halfTexel.y)).rgb;
	acc *= 0.25;

	if (IsPQ > 0.5) {
		// PQ → linear (10000 nits ref) → SDR gamma 2.2
		// Clamp to 400 nits (typical SDR peak) then normalize
		float3 lin = ST2084ToLinear(float4(acc, 1.0), 10000.0).rgb;
		lin = saturate(lin / 400.0);
		acc = pow(max(lin, 0.0), 1.0 / 2.2);
	}

	return float4(acc, 1.0);
}
