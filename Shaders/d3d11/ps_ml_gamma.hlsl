// Zion ML grade — ICtCp midtone gamma ("The Oracle v2").
// Gamma adjustments on the I (intensity) channel in ICtCp perceptual space
// do not shift Ct/Cp (chrominance), preserving skin tones and hues by design.
#include "../convert/st2084.hlsl"
#include "../convert/ictcp.hlsl"

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
	float MCStrength;
	float MCZoneLo;
	float MCSkinProtect;
	float MCRatioCap;
	float FaceProtect;
	float FaceFeather;
	float FaceExpand;
	float FaceDebugVis;
	int   NumFaces;
	float3 _padFace;
	float4 FaceBox[4];
	float4 FaceTrackId;
};

float4 main(PS_INPUT input) : SV_Target
{
	float4 color = tex.Sample(samp, input.Tex);

	// Convert to linear nits → ICtCp
	float3 lin;
	if (IsPQ > 0.5) {
		lin = ST2084ToLinear(float4(color.rgb, 0.0), 10000.0).rgb;
	} else {
		lin = pow(max(color.rgb, 0.0), 2.2) * SDR_PEAK_NITS;
	}
	float3 ictcp = linear_to_ictcp(lin);

	float I = ictcp.x;
	float I_pre = I;
	float I_peak = (IsPQ > 0.5) ? max(MLL1Max, 0.01) : SDR_I_PEAK_PQ;
	float I_norm = I / I_peak;
	float l2_gamma = MLGamma;

	// Gamma grade on I channel
	if (l2_gamma != 1.0 && I_norm > 0.001 && I_norm < 0.999) {
		float hi = smoothstep(MLGuard, 0.98, I_norm);
		float fade = smoothstep(0.08, 0.45, I_norm);
		float eff = fade * (1.0 - hi);
		I_norm = lerp(I_norm, pow(I_norm, 1.0 / l2_gamma), eff);
	}

	I = I_norm * I_peak;

	// Face blending disabled — testing pure ICtCp output
	float skin_blend = 0.0;

	ictcp.x = I;

	// Inverse: ICtCp → linear nits → output encoding
	float3 rgb_out = ictcp_to_linear(ictcp);

	if (IsPQ > 0.5) {
		color.rgb = LinearToST2084(float4(rgb_out, 0.0), 10000.0).rgb;
	} else {
		color.rgb = pow(saturate(rgb_out / SDR_PEAK_NITS), 1.0 / 2.2);
	}

	// Debug: track-colored ellipses
	if (FaceDebugVis > 0.5 && NumFaces > 0) {
		uint texw, texh;
		tex.GetDimensions(texw, texh);
		float2 texsz = float2((float)texw, (float)texh);
		float2 uv = input.Tex;
		float lineW = 2.5 / texsz.x;
		float trackIds[4] = { FaceTrackId.x, FaceTrackId.y, FaceTrackId.z, FaceTrackId.w };

		if (skin_blend > 0.01) {
			color.rgb = lerp(color.rgb, float3(0.0, 1.0, 0.3), skin_blend * 0.4);
		}

		for (int fi = 0; fi < NumFaces; fi++) {
			float4 box = FaceBox[fi];
			float2 center = box.xy + box.zw * 0.5;
			float2 radius = box.zw * 0.5 * FaceExpand;
			float2 dd = (uv - center) / max(radius, 0.001);
			float r = length(dd);

			float hue = frac(trackIds[fi] * 0.618034);
			float3 trkColor = saturate(abs(frac(hue + float3(0.0, 0.6667, 0.3333)) * 6.0 - 3.0) - 1.0);
			if (abs(r - 1.0) < lineW / min(radius.x, radius.y)) {
				color.rgb = trkColor;
			}
		}
	}

	return color;
}
