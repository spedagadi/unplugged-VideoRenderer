#include "../convert/st2084.hlsl"
// The curves themselves live in one shared file, because the PQ->SDR converter now
// applies the SAME selection on the SDR output path. Two copies would drift, and
// then the same "Tone curve" choice would mean two different curves depending on
// which path happened to be live.
#include "../convert/tonemap_curves.hlsl"

Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct PS_INPUT
{
	float4 Pos : SV_POSITION;
	float2 Tex : TEXCOORD;
};

cbuffer HDRParamsConstantBuffer : register(b0)
{
	float MasteringMinLuminanceNits;
	float MasteringMaxLuminanceNits;
	float maxCLL;
	float maxFALL;
	float displayMaxNits;
	uint selection; // 1 = ACES, 2 = Reinhard, 3 = Habel, 4 = Möbius, 5 = BT2390, 6 = ST 2094-10
	// Destination BLACK, nits (display floor; e.g. 0.16 for a projector at bb1.0).
	// Occupies one of the two former padding floats, so the cbuffer size is
	// unchanged. 0.0 = the mapping assumes an ideal black (previous behaviour).
	//
	// This matters in PQ space, where 0.16 nits is ~13% of the code range — NOT
	// in linear, where it is 0.18% of a 90-nit display. Hence the PQ-domain
	// curves (BT.2390, ST.2094-10) are where it bites; the [0,1] curve family
	// below evaluates in linear and barely moves.
	float displayMinNits;
	float padding[1];
};

cbuffer DolbyConstants : register(b1)
{
	float ChromaWeight;
	float SaturationGain;
	float TrimSlope;
	float TrimOffset;
	float TrimPower;
	uint L2Enabled;
	float L2Padding[2];
};

// The curve functions are defined in ../convert/tonemap_curves.hlsl and shared
// with the PQ->SDR converter, so one selection cannot mean two different curves.
// They take their metadata as PARAMETERS now; the call sites in main() pass the
// cbuffer values explicitly.

float3 RGB_to_ICTCP(float3 rgb_nits)
{
	float3 lms;
	lms.x = (1688.0f * rgb_nits.x + 2146.0f * rgb_nits.y + 262.0f * rgb_nits.z) / 4096.0f;
	lms.y = (683.0f * rgb_nits.x + 2951.0f * rgb_nits.y + 462.0f * rgb_nits.z) / 4096.0f;
	lms.z = (99.0f * rgb_nits.x + 309.0f * rgb_nits.y + 3688.0f * rgb_nits.z) / 4096.0f;

	lms.x = LinearToST2084(lms.x, 10000.0f).x;
	lms.y = LinearToST2084(lms.y, 10000.0f).x;
	lms.z = LinearToST2084(lms.z, 10000.0f).x;

	float3 ictcp;
	ictcp.x = (2048.0f * lms.x + 2048.0f * lms.y) / 4096.0f;
	ictcp.y = (6610.0f * lms.x - 13613.0f * lms.y + 7003.0f * lms.z) / 4096.0f;
	ictcp.z = (17933.0f * lms.x - 17390.0f * lms.y - 543.0f * lms.z) / 4096.0f;

	return ictcp;
}

float3 ICTCP_to_RGB(float3 ictcp)
{
	float3 lms;
	lms.x = 1.0f * ictcp.x + 0.00860904f * ictcp.y + 0.11102963f * ictcp.z;
	lms.y = 1.0f * ictcp.x - 0.00860904f * ictcp.y - 0.11102963f * ictcp.z;
	lms.z = 1.0f * ictcp.x + 0.56003134f * ictcp.y - 0.32062717f * ictcp.z;

	lms.x = ST2084ToLinear(lms.x, 10000.0f).x;
	lms.y = ST2084ToLinear(lms.y, 10000.0f).x;
	lms.z = ST2084ToLinear(lms.z, 10000.0f).x;

	float3 rgb_nits;
	rgb_nits.x = 3.43660669f * lms.x - 2.50645212f * lms.y + 0.06984542f * lms.z;
	rgb_nits.y = -0.79132956f * lms.x + 1.98360045f * lms.y - 0.19227090f * lms.z;
	rgb_nits.z = -0.02594990f * lms.x - 0.09891371f * lms.y + 1.12486361f * lms.z;

	return rgb_nits;
}

float3 ApplyL2Trim(float3 linearRGB)
{
	float3 ictcp = RGB_to_ICTCP(linearRGB);
	float originalI = ictcp.x;

	ictcp.x = max(ictcp.x * TrimSlope + TrimOffset, 0.0f);
	ictcp.x = pow(ictcp.x, max(TrimPower, 0.1f));

	float saturationFactor = max(SaturationGain, 0.0f);
	float highlightWeight = saturate(originalI * 2.0f);

	float targetSaturationForHighlights = 1.0;
	float effectiveSaturation = lerp(saturationFactor, targetSaturationForHighlights, highlightWeight * (1.0 - ChromaWeight));

	ictcp.yz *= effectiveSaturation;

	return ICTCP_to_RGB(ictcp);
}

float4 DolbyVisionTrims(float4 color)
{
	color = LinearToST2084(color, 10000.0f);

	color = pow((color * TrimSlope) + TrimOffset, TrimPower);

	color.rgb = max(color.rgb, 0.00001);

	float Y = 0.2627f * color.r + 0.6780f * color.g + 0.0593f * color.b;

	color = color * pow((1.0 + ChromaWeight) * color / Y, SaturationGain);

	color = ST2084ToLinear(color, 10000.0f);

	return color;
}

float4 main(PS_INPUT input) : SV_Target
{
	// Sample texture and convert from PQ to linear
	float4 color = tex.Sample(samp, input.Tex);
    color = saturate(color);
	color = ST2084ToLinear(color, 10000.0f); // Convert PQ to Linear space

	if (L2Enabled)
	{
		color = DolbyVisionTrims(color);
	}

	if (selection == 5)
	{
		color.rgb = BT2390Tonemap(color.rgb, maxCLL, MasteringMaxLuminanceNits, displayMaxNits);
		color = LinearToST2084(color, 10000.0f);
		return float4(color.rgb, color.a);
	}

	if (selection == 6)
	{
		color.rgb = ST209410Tonemap(color.rgb, maxCLL, maxFALL, MasteringMinLuminanceNits,
									displayMaxNits, displayMinNits);
		color = LinearToST2084(color, 10000.0f);
		return float4(color.rgb, color.a);
	}

	// Normalise by the content peak clamped to the display, apply the selected curve,
	// expand across the display range. On an HDR target 1.0 is the display's peak, so
	// the content's own range fills the display. One shared implementation — it used
	// to be inline here, and the SDR converter needs the same arithmetic with a
	// different reference (see ToneMapNormalisedCurve).
	float baseLum = max(displayMaxNits, MasteringMaxLuminanceNits);
	float effectiveMaxLum = min(baseLum, maxCLL);
	color.rgb = ToneMapNormalisedCurve(color.rgb, (int)selection,
									  effectiveMaxLum, maxFALL,
									  displayMaxNits, displayMinNits);

	// Convert back from linear to PQ color space
	color = LinearToST2084(color, 10000.0f); // Convert Linear to PQ

	return float4(color.rgb, color.a); // Final output
}
