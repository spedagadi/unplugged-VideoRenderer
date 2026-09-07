// Zion P0 black bars repaint (projector haze fix).
// Post-grade mask: fills top/bottom letterbox bars with display black (or
// debug red). The shader is a no-op when uBarTop=0 and uBarBottom>=1.

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
	float _reserved0;
	float _reserved1;
	float _reserved2;
	float _reserved3;
	float _reserved4;
	float _reserved5;
	float uBarTop;
	float uBarBottom;
	float uBarV0;
	float uBarSpan;
	float uBarFeather;
	float uBarBlack;
	float uBarHighlight;
	float _padBar;
};

float4 main(PS_INPUT input) : SV_Target
{
	if (uBarTop <= 0.0f && uBarBottom >= 1.0f)
		return tex.Sample(samp, input.Tex);
	if (uBarBottom - uBarTop < 0.05f)
		return tex.Sample(samp, input.Tex);

	uint texw, texh;
	tex.GetDimensions(texw, texh);
	const float texH = (float)texh;

	const float winV = input.Pos.y / texH;
	const float fpx = (winV - uBarV0) / max(uBarSpan, 1e-4f) * texH;

	const float ftop = uBarTop * texH;
	const float fbot = uBarBottom * texH;
	const float f = max(uBarFeather, 0.0f);

	const float topM = 1.0f - smoothstep(ftop - f, ftop + f, fpx);
	const float botM = smoothstep(fbot - f, fbot + f, fpx);
	const float mask = max(topM, botM);
	if (mask <= 0.001f)
		return tex.Sample(samp, input.Tex);

	float4 color = tex.Sample(samp, input.Tex);
	const float3 barColor = (uBarHighlight > 0.5f)
		? float3(0.85f, 0.0f, 0.0f)
		: float3(uBarBlack, uBarBlack, uBarBlack);
	color.rgb = lerp(color.rgb, barColor, min(mask, 1.0f));
	return color;
}
