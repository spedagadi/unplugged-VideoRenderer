// Zion ML grade — highlight rolloff (passthrough).
// Shoulder shaping now handled by the unified tone curve in ps_ml_gamma.hlsl.

Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct PS_INPUT
{
	float4 Pos : SV_POSITION;
	float2 Tex : TEXCOORD;
};

float4 main(PS_INPUT input) : SV_Target
{
	return tex.Sample(samp, input.Tex);
}
