// Zion ML — 256x144 luma downsample for feature extraction.
//
// Renders BT.709 luma of the (PQ-encoded for HDR source) frame into a fixed
// 256x144 R16_UNORM cache, mirroring libplacebo's ml_features luma texture.
// The vertex rect maps the source content into the full target; letterbox
// bars present in the source carry over as near-black luma and are measured
// by the bar-norm features.
Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct PS_INPUT
{
	float4 Pos : SV_POSITION;
	float2 Tex : TEXCOORD;
};

float4 main(PS_INPUT input) : SV_Target
{
	// Box-average the source area each output texel covers: the linear
	// sampler already filters, but sampling the 4 quadrant centers keeps
	// the average closer to a true box (less alias at deep downscales).
	uint texw, texh;
	tex.GetDimensions(texw, texh);
	const float2 texsz = float2((float)texw, (float)texh);
	const float2 texel = 1.0f / texsz;

	float2 base = input.Tex - texel * 0.25f;
	float2 step = texel * 0.5f;

	float3 acc = 0.0f;
	[unroll]
	for (int dy = 0; dy < 2; dy++) {
		[unroll]
		for (int dx = 0; dx < 2; dx++) {
			acc += tex.Sample(samp, base + float2(dx, dy) * step).rgb;
		}
	}
	acc *= 0.25f;

	const float luma = dot(acc, float3(0.2126, 0.7152, 0.0722));
	return float4(luma, 0.0f, 0.0f, 1.0f);
}