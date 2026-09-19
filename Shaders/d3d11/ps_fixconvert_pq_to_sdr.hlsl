Texture2D tex : register(t0);
SamplerState samp : register(s0);

// Experimental tone curve — see ps_convert_pq_to_sdr.hlsl, which carries the
// full explanation. Duplicated rather than shared through an include because
// these two shaders are compiled as separate units and this one has no
// PS_COLOR_TRANSFORM stage to fold it into.
Texture2D toneLut : register(t1);
SamplerState sampLut : register(s1);

cbuffer PS_PARAMETERS : register(b0)
{
    float LuminanceScale;   // 10000 / sdrNits, so the decoded linear value is nits/sdrNits
    float displayMinNits;   // display's measured black, nits (was an unused zero)
    float outputGamut;      // 0 = convert to BT.709 (standard SDR), 1 = keep BT.2020
    float toneLutEnable;    // 1 = use the experimental curve, 0 = the curve below
    float toneLutPqMin;     // the table's index axis, in PQ
    float toneLutPqMax;
    float toneLutSize;      // entries in the table
};

#include "../convert/conv_matrix.hlsl"
#include "../convert/st2084.hlsl"
#include "../convert/hdr_tone_mapping.hlsl"
#include "../convert/colorspace_gamut_conversion.hlsl"

static const float4x4 fix_bt2020_matrix = mul(ycbcr2020nc_rgb, rgb_ycbcr709);

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

    // PQ to Linear. After this, 1.0 == sdrNits nits.
    color = saturate(color);
    color = ST2084ToLinear(color, LuminanceScale);

    if (toneLutEnable > 0.5) {
        // Already display-relative — see ps_convert_pq_to_sdr.hlsl.
        color.rgb = ToneMapExperimental(color.rgb);

        if (outputGamut < 0.5) {
            color.rgb = Colorspace_Gamut_Conversion_2020_to_709(color.rgb);
        }
    }
    else {
        // Display floor — see ps_convert_pq_to_sdr.hlsl for the full explanation.
        // Converts absolute light to display-relative so the plain pow(v, 1/2.2)
        // encodes it correctly, and deliberately does NOT add the floor back: the
        // panel's own black offset supplies it, and re-adding it doubles the black.
        // floor == 0 makes this a no-op.
        float sdrNits = 10000.0 / max(LuminanceScale, 1e-3);
        float floorN  = saturate(displayMinNits / max(sdrNits, 1e-3));
        float scale   = max(1.0 - floorN, 1e-3);

        color.rgb = max(color.rgb - floorN, 0.0) / scale;

        color.rgb = ToneMappingHable(color.rgb);

        // Output gamut — see ps_convert_pq_to_sdr.hlsl. 1 keeps BT.2020.
        if (outputGamut < 0.5) {
            color.rgb = Colorspace_Gamut_Conversion_2020_to_709(color.rgb);
        }
    }

    // Linear to sRGB
    color = saturate(color);
    color = pow(color, 1.0 / 2.2);

    return color;
}
