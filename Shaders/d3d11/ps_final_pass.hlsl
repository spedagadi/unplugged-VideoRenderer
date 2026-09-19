#ifndef QUANTIZATION
    // 255 or 1023 (Maximum quantized integer value)
    #define QUANTIZATION 255
#endif

Texture2D tex : register(t0);
Texture2D texDither : register(t1);
Texture3D cubeLut : register(t2);
SamplerState samp : register(s0);
SamplerState sampDither : register(s1);
SamplerState sampCube : register(s2);

cbuffer PS_CONSTANTS : register(b0)
{
    float2 ditherCoordScale;
    float  cubeLutEnabled;   // 1.0 when a display calibration LUT is loaded
    float  cubeLutSize;      // N of LUT_3D_SIZE N
    float  ditherEnabled;    // 0.0 when this pass runs only to apply the LUT
    float  cubeLutMode;      // 1 = gamma only, 2 = greyscale, 3 = full 3D
};

struct PS_INPUT
{
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD;
};

float4 main(PS_INPUT input) : SV_Target
{
    float4 pixel = tex.Sample(samp, input.Tex);

    // Display calibration LUT (.cube). This pass is the last display-referred
    // stage — after tone mapping, after the ML grade, after scaling — which is
    // the only correct place for a correction that describes the PANEL rather
    // than the signal. Trilinear over a TEXTURE3D; the half-texel offset maps
    // 0.0 and 1.0 exactly onto the corner texels instead of half a texel short.
    if (cubeLutEnabled > 0.5 && cubeLutSize > 1.0 && cubeLutMode > 0.5) {
        float n = cubeLutSize;
        float s = (n - 1.0) / n;      // grid scale
        float o = 0.5 / n;            // half-texel so 0.0 and 1.0 hit the corners
        float3 rgb = pixel.rgb;

        if (cubeLutMode < 1.5) {
            // GAMMA ONLY. The cube's neutral diagonal is its tone response; take
            // its LUMINANCE shape and apply it to all three channels equally, so
            // chromaticity passes through untouched — no cast added, none removed.
            // For a display whose grey balance was already set by hand (projector
            // gain/offset), this is the correction you want: tone, not colour.
            float y = dot(rgb, float3(0.2126, 0.7152, 0.0722));
            float3 d = cubeLut.Sample(sampCube, float3(y, y, y) * s + o).rgb;
            float y2 = dot(d, float3(0.2126, 0.7152, 0.0722));
            pixel.rgb = (y > 0.0001) ? rgb * (y2 / y) : rgb;
        }
        else if (cubeLutMode < 2.5) {
            // GREYSCALE. Per-channel diagonal: tone response AND grey balance,
            // still with no cross-channel mixing, so gamut is untouched.
            float3 d;
            d.r = cubeLut.Sample(sampCube, float3(rgb.r, rgb.r, rgb.r) * s + o).r;
            d.g = cubeLut.Sample(sampCube, float3(rgb.g, rgb.g, rgb.g) * s + o).g;
            d.b = cubeLut.Sample(sampCube, float3(rgb.b, rgb.b, rgb.b) * s + o).b;
            pixel.rgb = d;
        }
        else {
            // FULL 3D. Tone, grey balance and colour/gamut together.
            pixel.rgb = cubeLut.Sample(sampCube, rgb * s + o).rgb;
        }
    }

    if (ditherEnabled > 0.5) {
        float4 ditherValue = texDither.Sample(sampDither, input.Tex * ditherCoordScale);
        pixel = floor(pixel * QUANTIZATION + ditherValue) / QUANTIZATION;
    }

    return pixel;
};