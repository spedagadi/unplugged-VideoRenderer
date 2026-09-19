Texture2D tex : register(t0);
SamplerState samp : register(s0);

// Experimental tone curve (see the exptm namespace in DX11VideoProcessor.cpp).
// A 1D table the CPU builds per scene; sampled at t1/s1. 1D because the curve is
// applied per channel, which is what ToneMappingHable below does too.
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
    float unused0;

    // The "Tone curve" selection, 1..6 as in IVideoRenderer.h (1=ACES, 2=Reinhard,
    // 3=Hable, 4=Mobius, 5=BT.2390, 6=ST.2094-10).
    //
    // This is the value the panel has ALWAYS been setting. Until now the converter
    // ignored it and called ToneMappingHable unconditionally, because the only
    // shader that read it (ps_hdr10_tonemap) is built for an HDR display and never
    // ran here — so the row changed nothing.
    //
    // 0 and 3 both mean "the legacy display-referred ToneMappingHable", so a
    // selection of Hable reproduces the previous picture exactly.
    float toneCurveSelection;
    // HDR10 static metadata, for the curves that derive their knee from it.
    // All zero on any path that carries none (the DeckLink capture path always
    // does), and the CPU applies the same fallbacks ps_hdr10_tonemap relied on
    // before pushing them.
    float maxCLL;
    float maxFALL;
    float masteringMaxNits;
    float masteringMinNits;
    // Per-scene source peak in NITS, measured before the tone map and
    // IIR-smoothed with a snap on scene cuts. Negative until the first reading.
    // Only the Hable branch uses it — deliberately NOT maxCLL, so BT.2390 and
    // ST.2094-10 are unaffected by this change.
    float hableScenePeakNits;
    // Hable knee: display-relative light (1.0 = the display's white) below which
    // the Hable curve is replaced by IDENTITY. 0 = pure Hable, i.e. exactly the
    // behaviour before the knee existed. See IVideoRenderer.h for the rationale.
    float hableKnee;
    float unused1;                 // keeps the cbuffer at 16 floats
};

#include "../convert/conv_matrix.hlsl"
#include "../convert/st2084.hlsl"
#include "../convert/hdr_tone_mapping.hlsl"
#include "../convert/colorspace_gamut_conversion.hlsl"
// The same curve definitions the HDR10 path uses — one file, so the same selection
// cannot mean two different curves depending on which path is live.
#include "../convert/tonemap_curves.hlsl"
// Intensity/chroma separation, so the tone curve can act on I alone and leave the
// chromatic channels untouched. See ToneMapSelectedCurve.
#include "../convert/ictcp.hlsl"

struct PS_INPUT
{
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD;
};

// Sample the experimental curve. `x` is linear light in units of sdrNits.
//
// The table's index axis is uniform in PQ, not linear (libplacebo builds it that
// way: input_scaling is PL_HDR_PQ), so the input is PQ-encoded to index it. The
// table stores LINEAR output, so there is no decode on the way out — the CPU
// already did that once per entry instead of once per pixel per channel.
float3 ToneMapExperimental(float3 x)
{
    // PQ-encode: absolute nits / 10000 == (x * sdrNits) / 10000 == x / LuminanceScale.
    // LinearToST2084 is declared on float4; the alpha it carries is discarded.
    float3 pq = saturate(LinearToST2084(float4(x, 1.0), LuminanceScale)).rgb;

    float3 u = (pq - toneLutPqMin) / max(toneLutPqMax - toneLutPqMin, 1e-6);
    u = saturate(u);

    // Texel centres: entry i sits at (i + 0.5) / N, so map [0,1] onto
    // [0.5/N, 1 - 0.5/N] rather than [0,1] — otherwise the whole curve is
    // sampled a fraction of a texel off, which is a visible offset at the ends.
    u = u * ((toneLutSize - 1.0) / toneLutSize) + (0.5 / toneLutSize);

    return float3(
        toneLut.SampleLevel(sampLut, float2(u.r, 0.5), 0).r,
        toneLut.SampleLevel(sampLut, float2(u.g, 0.5), 0).r,
        toneLut.SampleLevel(sampLut, float2(u.b, 0.5), 0).r);
}

// Apply the selected standard curve. Returns a DISPLAY-RELATIVE value (0 = the
// panel's measured black, 1 = its white) — the same domain the legacy Hable branch
// below produces, so the gamut conversion and the encode after this are shared.
//
// The standard curves are SCENE-REFERRED: they are defined over absolute nits and
// derive their own knee from the content peak, so they are fed nits and their
// output is converted back through the display's range. That is a different domain
// from the legacy Hable, which is display-referred — which is exactly why Hable is
// kept on the legacy path and can reproduce the previous picture exactly.
float3 ToneMapSelectedCurve(float3 nits, float sdrNits)
{
    // Tone-map the ICtCp INTENSITY channel, not RGB.
    //
    // Per-channel RGB mapping scales R, G and B by different amounts - each
    // channel sits at a different point on the curve - so the R:G:B ratio changes
    // with level and the hue moves with it. A warm highlight shows it first: R
    // reaches the top before G, the ratio collapses toward yellow, and faces go
    // yellow.
    //
    // ICtCp separates intensity (I) from chromaticity (Ct, Cp). Mapping I alone,
    // and handing the original Ct/Cp back untouched, cannot rotate the hue:
    // hue IS the Ct/Cp relationship, which is the whole reason BT.2100 defines
    // the space this way. Saturation is free to drift; hue is not.
    float3 ictcp = linear_to_ictcp(nits);
    float  I_in  = ST2084ToLinear(float4(ictcp.x, 0.0, 0.0, 0.0), 10000.0).x;

    // Broadcast to float3 so the existing curve definitions are reused unchanged:
    // they are per-channel functions, so the scalar result is the .x component.
    float3 v = float3(I_in, I_in, I_in);
    float  I_out;
    if (toneCurveSelection == 5.0) {
        I_out = BT2390Tonemap(v, maxCLL, masteringMaxNits, sdrNits).x;
    } else if (toneCurveSelection == 6.0) {
        I_out = ST209410Tonemap(v, maxCLL, maxFALL, masteringMinNits, sdrNits, displayMinNits).x;
    } else {
        // Reference the curve by the SDR WHITE, not the content peak: these curves
        // are written for input where 1.0 is diffuse white.
        I_out = ToneMapNormalisedCurve(v, (int)toneCurveSelection, sdrNits,
                                       0.0f /* maxFALL: none on this path */,
                                       sdrNits, displayMinNits).x;
    }

    // Re-encode I, hand back the ORIGINAL chroma, and return to RGB.
    ictcp.x = LinearToST2084(float4(I_out, 0.0, 0.0, 0.0), 10000.0).x;
    float3 mapped = ictcp_to_linear_gamut(ictcp, sdrNits);
    // Nits -> display-relative. The same arithmetic as the legacy floor step, which
    // is what makes the two branches land in one domain.
    return saturate((mapped - displayMinNits) / max(sdrNits - displayMinNits, 1e-3));
}

float4 main(PS_INPUT input) : SV_Target
{
    float4 color = tex.Sample(samp, input.Tex); // original pixel

    // PQ to Linear. After this, 1.0 == sdrNits nits.
    color = saturate(color);
    color = ST2084ToLinear(color, LuminanceScale);

    if (toneLutEnable > 0.5) {
        // The experimental table is ALREADY display-relative (0 = the panel's
        // measured black, 1 = its white), because the CPU built it that way. So it
        // feeds the encoder directly and the floor conversion below must not be
        // applied to it — that would take the offset out twice.
        color.rgb = ToneMapExperimental(color.rgb);

        if (outputGamut < 0.5) {
            color.rgb = Colorspace_Gamut_Conversion_2020_to_709(color.rgb);
        }
    }
    else {
        // Display floor. The panel's response is L = Lb + (Lw - Lb)*v^gamma — the
        // offset lives IN the panel, which is what "black output offset 100%" in a
        // calibration report means. So this step converts absolute light into
        // DISPLAY-RELATIVE light (0 = the panel's measured black, 1 = its white),
        // which is exactly what the plain pow(v, 1/2.2) below encodes.
        //
        // Do NOT add the floor back afterwards. Doing so feeds the encoder an
        // absolute value where a relative one is wanted, the panel applies its own
        // offset on top, and source black lands at code 15 instead of code 0 —
        // exactly 2*Lb, which reads on screen as blacks sitting at grey.
        //
        // floor == 0 makes this a no-op, reproducing the pre-floor behaviour.
        float sdrNits = 10000.0 / max(LuminanceScale, 1e-3);
        float floorN  = saturate(displayMinNits / max(sdrNits, 1e-3));
        float scale   = max(1.0 - floorN, 1e-3);

        // A selection of 0 or 3 means Hable — the legacy display-referred curve,
        // kept because it is the one that protects highlights (a curve with an
        // asymptote cannot reach white). Every other selection is a scene-referred
        // standard curve.
        if (toneCurveSelection >= 0.5 && toneCurveSelection != 3.0) {
            color.rgb = ToneMapSelectedCurve(color.rgb * sdrNits, sdrNits);
        }
        else {
            // Hable applied to the ICtCp INTENSITY channel, not per-channel RGB.
            //
            // Per-channel mapping puts R, G and B at different points on the curve,
            // so their ratio changes with level and the hue moves with it: a warm
            // highlight loses red first and slides toward yellow. Hable's highlight
            // protection comes from being display-referred with an asymptote — a
            // property of the CURVE, not of the space it is applied in. So the curve
            // is left exactly as it is and only its argument changes: I.
            //
            // ICtCp is defined on absolute nits and I is PQ-encoded, so the
            // display-relative value is scaled up by sdrNits on the way in and back
            // down on the way out. The curve still sees the same 0..1 domain it
            // always has.
            float3 disp = max(color.rgb - floorN, 0.0) / scale;   // nits / sdrNits (1.0 = white)

            float3 ictcp = linear_to_ictcp(disp * sdrNits);
            float I_rel = ST2084ToLinear(float4(ictcp.x, 0.0, 0.0, 0.0), 10000.0).x / max(sdrNits, 1e-3);

            // Normalise by the CONTENT PEAK, not Hable's fixed 4.8 — see
            // ToneMappingHablePeak().
            //
            // The per-scene measured peak when the extractor has one, the static
            // maxCLL otherwise (the first frames, or a signal the extractor cannot
            // read). The measured value is the pre-tone-map scene peak, so it is a
            // genuine source-referred number rather than a title-level one.
            const float scenePeak = (hableScenePeakNits > 10.0f) ? hableScenePeakNits : maxCLL;
            const float peakSdrUnits = max(scenePeak / max(sdrNits, 1e-3), 1.0);

            // Pre-curve gain: the brightness control for the Hable path.
            //
            // Normalisation alone cannot lift this picture, and that is now a
            // measurement rather than a guess: the content's per-scene peaks land
            // at 290-396 nits against a static MaxCLL of 300, so per-scene and
            // static normalisation differ by ~1.5%, and the whole effect of
            // switching to the measured peak was the 1.145x that comes from
            // hable(4.8) -> hable(3.53).
            //
            // What is left is WHERE the content sits on the curve, and that is what
            // this moves. Raising it lifts the mids more than the highlights
            // because hable compresses: at gain 1.25 a 42-nit midtone gains ~21%
            // while the content peak moves past 1.0 and clips. That trade is the
            // point — brightness bought mostly in the mids.
            //
            // TEMPORARY TEST CONSTANT. It becomes a user setting once the direction
            // is agreed; 1.0 is exactly the behaviour before this line existed.
            const float kHableGain = 1.00f;
            const float3 hv = float3(I_rel * kHableGain, I_rel * kHableGain, I_rel * kHableGain);
            const float  hMapped = ToneMappingHablePeak(hv, peakSdrUnits).x;

            // Hable knee. Below it the value passes through UNTOUCHED; above it
            // Hable applies unchanged. Hable has no knee of its own — it is a
            // global rational curve and compresses everywhere, which is what makes
            // it highlight-safe and also why the whole picture sits low: the
            // shadows and lower mids pay compression only the top of the range
            // needs. A smoothstep band, not a corner, because a hard knee is where
            // banding shows.
            //
            // knee = 0 keeps the curve EXACTLY as it was (w = 1 everywhere), so the
            // A/B anchor survives.
            float w = 1.0;
            if (hableKnee > 0.001) {
                w = smoothstep(hableKnee, min(hableKnee + 0.15, 1.0), I_rel);
            }
            I_rel = lerp(I_rel, hMapped, w);

            ictcp.x = LinearToST2084(float4(I_rel * max(sdrNits, 1e-3), 0.0, 0.0, 0.0), 10000.0).x;

            color.rgb = ictcp_to_linear_gamut(ictcp, sdrNits) / max(sdrNits, 1e-3);
        }

        // Output gamut. 0 = the standard SDR target: map BT.2020 content onto
        // Rec.709. 1 = keep BT.2020, so a display whose gamut EXCEEDS Rec.709 shows
        // the source's real colour instead of having everything outside 709 thrown
        // away. That is not a standards target — the display must be set to match,
        // and a calibration LUT must be built for that same mode.
        //
        // Runs on the display-relative values the curve just produced: the gamut
        // matrices are only valid in linear light, and the offset is already out.
        if (outputGamut < 0.5) {
            color.rgb = Colorspace_Gamut_Conversion_2020_to_709(color.rgb);
        }
    }

    // Linear to sRGB. Encoding stretches the shadow end, so the bottom of the
    // range is where a wrong floor shows up first — which is how the double-count
    // above was caught.
    color = saturate(color);
    color = pow(color, 1.0 / 2.2);

    return color;
}
