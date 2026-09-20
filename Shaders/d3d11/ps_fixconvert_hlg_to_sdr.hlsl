Texture2D tex : register(t0);
SamplerState samp : register(s0);

// Experimental tone curve — see ps_convert_pq_to_sdr.hlsl.
Texture2D toneLut : register(t1);
SamplerState sampLut : register(s1);

#include "../convert/conv_matrix.hlsl"
#include "../convert/hlg.hlsl"
#include "../convert/st2084.hlsl"
#include "../convert/hdr_tone_mapping.hlsl"
#include "../convert/colorspace_gamut_conversion.hlsl"

static const float4x4 fix_bt2020_matrix = mul(ycbcr2020nc_rgb, rgb_ycbcr709);

cbuffer PS_PARAMETERS : register(b0)
{
    float LuminanceScale;
    float displayMinNits;   // was `param2`, an unused zero — see the floor below
    float outputGamut;      // 0 = convert to BT.709, 1 = keep BT.2020
    float toneLutEnable;    // 1 = use the experimental curve, 0 = Hable
    float toneLutPqMin;     // the table's index axis, in PQ
    float toneLutPqMax;
    float toneLutSize;      // entries in the table
};

struct PS_INPUT
{
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD;
};

float3 ToneMapExperimental(float3 x)
{
    float3 pq = saturate(LinearToST2084(float4(x, 1.0), LuminanceScale)).rgb;

    float3 u = (pq - toneLutPqMin) / max(toneLutPqMax - toneLutPqMin, 1e-6);
    u = saturate(u);
    u = u * ((toneLutSize - 1.0) / toneLutSize) + (0.5 / toneLutSize);

    return float3(
        toneLut.SampleLevel(sampLut, float2(u.r, 0.5), 0).r,
        toneLut.SampleLevel(sampLut, float2(u.g, 0.5), 0).r,
        toneLut.SampleLevel(sampLut, float2(u.b, 0.5), 0).r);
}

float4 main(PS_INPUT input) : SV_Target
{
    float4 color = tex.Sample(samp, input.Tex); // original pixel

    // Fix incorrect (unsupported) conversion from YCbCr BT.2020 to RGB in D3D11 VP
    color = mul(fix_bt2020_matrix, color);

    // HLG to PQ
    color = saturate(color);
    color.rgb = HLGtoLinear(color.rgb);
    color = LinearToST2084(color, 10000.0);

    // PQ to Linear
    color = saturate(color);
    color = ST2084ToLinear(color, LuminanceScale);

    if (toneLutEnable > 0.5) {
        // Already display-relative — see ps_convert_pq_to_sdr.hlsl.
        color.rgb = ToneMapExperimental(color.rgb);
    }
    else {
        // Display floor REMOVED — same terms as the PQ converters, so the behaviour is
        // one thing across sources. See ps_convert_pq_to_sdr.hlsl for the account:
        // subtracting the panel's black from scene light clipped the shadows.
        color.rgb = ToneMappingHable(color.rgb);
    }

    // Output gamut. This shader applied the 2020->709 matrix unconditionally, so
    // the Output gamut control did nothing at all on an HLG source. The flag is
    // read here now: 0 (the default) is the existing behaviour, unchanged.
    if (outputGamut < 0.5) {
        color.rgb = Colorspace_Gamut_Conversion_2020_to_709(color.rgb);
    }

    // Linear to sRGB
    color = saturate(color);
    color = pow(color, 1.0 / 2.2);

    return color;
}
