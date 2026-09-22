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
    // Soft-plus PRE-compressor, applied to I BEFORE Hable sees it. Display-relative
    // light (1.0 = the display's white). It re-spreads the highlight band Hable
    // compresses least (a bright sky at 2-12x white sits in Hable's flat region
    // and reads "hot") so the sky occupies more of the output range, leaving
    // everything below the knee EXACTLY identity:  min(x, knee + softplus/ratio).
    // softKnee <= 0 disables the compressor entirely — identical to before it
    // existed. The curve still normalises by the UNCOMPRESSED content peak.
    float softKnee;
    float softKneeRatio;   // 1.0 = identity above the knee; >1 compresses
    float softKneeWidth;   // softplus transition width around the knee
    float _padK;           // keeps the cbuffer 16-byte aligned
    float hableRefWhiteNits;   // the Hable curve's 1.0 in nits — decouples "normal
                               // scene light" from display white (sdrNits). 0 = fall
                               // back to sdrNits (identical to before). Clouds at
                               // 150-200 nits are below a 200-nit reference but 2x an
                               // 85-nit one — this axis tells the curve they are content.
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

    // Chroma follows the curve down — the same fix as the Hable branch, and it was
    // missing here. This function serves BT-2390, ACES, Reinhard, Mobius and
    // ST.2094-10, all of which here hand back the ORIGINAL absolute Ct/Cp after
    // lowering I. That is over-saturation by construction, and it is why these
    // curves read as DEEPER than Hable: I drops, the chroma does not, so the colour
    // gets relatively more saturated. It is not a property of BT-2390 — it is the
    // pairing, and the harder the curve compresses, the stronger the effect.
    //
    // It is also the same root cause as the blotches: the reconstruction leaves the
    // gamut, and ictcp_to_linear_gamut() then removes saturation per pixel.
    //
    // Per-pixel gain on the WHOLE colour, applied in linear — not on the chroma
    // in the PQ domain. See the long note in the Hable branch: scaling I in
    // linear while scaling Ct/Cp in PQ makes saturation depend on the gain, and
    // the gain depends on the anchor, so a moving anchor drained the colour.
    const float gGain = I_out / max(I_in, 1e-6);
    const float3 ictcpNew = linear_to_ictcp(ictcp_to_linear(ictcp) * gGain);

    float3 mapped = ictcp_to_linear_gamut(ictcpNew, sdrNits);
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

    // ---------------------------------------------------------------------
    // 7 = REFERENCE. The upstream VideoRenderer-master converter, verbatim:
    //
    //     color = ST2084ToLinear(color, LuminanceScale);
    //     color.rgb = ToneMappingHable(color.rgb);
    //     color.rgb = Colorspace_Gamut_Conversion_2020_to_709(color.rgb);
    //     color = pow(color, 1.0 / 2.2);
    //
    // Kept so a regression can be isolated by switching a row instead of
    // rebuilding an old tree. It deliberately reads NONE of:
    //   - the display floor (upstream had no displayMinNits in this shader),
    //   - ICtCp and the chroma bisection (per-channel RGB, as upstream),
    //   - the measured per-scene peak, or maxCLL (fixed 4.8, as upstream),
    //   - the knee, and any mobius coefficients.
    // So it answers "is the change in the curve, or in the machinery around it".
    //
    // outputGamut IS still honoured — upstream applied the 2020->709 matrix
    // unconditionally, and outputGamut 0 selects that same matrix, so at the
    // default the two are identical; at gamut 1 the caller has explicitly asked
    // to keep BT.2020 and the reference must not silently override that.
    //
    // Must stay FIRST: it has to outrank the experimental tone LUT, which is a
    // different axis and would otherwise replace the curve entirely.
    // ---------------------------------------------------------------------
    if (toneCurveSelection > 6.5) {
        color.rgb = ToneMappingHable(color.rgb);

        if (outputGamut < 0.5) {
            color.rgb = Colorspace_Gamut_Conversion_2020_to_709(color.rgb);
        }
    }
    else if (toneLutEnable > 0.5) {
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
        float sdrNits = 10000.0 / max(LuminanceScale, 1e-3);
            // The curve's reference axis, decoupled from display white. 1.0 for the
            // curve = hableRefWhiteNits nits; when unset (=0) that equals sdrNits and
            // reproduces the previous picture exactly. Default ~203 (libplacebo's SDR
            // diffuse white): clouds at 150-200 nits become ordinary content instead
            // of 2x-white highlight, and survive; sdrNits (the display mapping) is
            // untouched — that is the decouple.
            const float refNits = max(hableRefWhiteNits, 1.0f);

        // THE DISPLAY FLOOR IS GONE FROM THIS BRANCH — deliberately, from
        // measurement, 2026-09-20.
        //
        // It used to subtract the panel's measured black from the HABLE input:
        //
        //     float floorN = saturate(displayMinNits / sdrNits);   // 0.170 / 85
        //     float3 disp  = max(color.rgb - floorN, 0.0) / (1.0 - floorN);
        //
        // The reasoning was sound — the panel's L = Lb + (Lw - Lb)*v^gamma has its
        // offset built in, so encoding (L - Lb)/(Lw - Lb) makes it reproduce L
        // exactly. Above the panel's black that is what it did.
        //
        // What it cost was never priced in. `color.rgb` is SCENE light, and the
        // content's black is 0, not Lb — so the subtraction removed 0.170 nits of
        // scene light from every pixel and then clipped: max(..., 0). On dark HDR
        // content the frame's own mean luminance sits BELOW the panel's black, so
        // MOST OF THE PICTURE was being clipped to absolute zero. In panel code the
        // cut lands at (0.170/85)^(1/2.2) = 0.06, and everything under it died.
        //
        // Measured on a static scene, Hable with the floor vs without:
        //   mean output code   0.0105  ->  0.0162   (+54%)
        //   peak output code   0.7777  ->  0.5816
        // The floor was buying 34% more peak by deleting the shadows — and the peak
        // is already above the content's own level either way.
        //
        // Upstream (VideoRenderer-master) had no floor here at all, which is the
        // third reason this is the right default: reference and shipping agree.
        //
        // The convention now is the ordinary tone-mapping one: content 0 -> panel
        // black, content sdrNits -> panel white, so the panel's range is filled
        // rather than trimmed. displayMinNits is still used by the experimental
        // LUT's output range and by ToneMapSelectedCurve, where the value really is
        // display-absolute — it just no longer has any business being subtracted
        // from scene light.
        //
        // Keep the panel's black truthful in the settings. Do not "fix" a lifted
        // black by typing 0 there again: this branch no longer reads it.

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
            float3 disp = color.rgb;   // nits / sdrNits (1.0 = white) — no floor, see above

            float3 ictcp = linear_to_ictcp(disp * sdrNits);
            float I_rel = ST2084ToLinear(float4(ictcp.x, 0.0, 0.0, 0.0), 10000.0).x / max(refNits, 1e-3);
            // The ORIGINAL intensity in nits, kept for the chroma rescale at the
            // reconstruction below. I_rel is reassigned by the curve.
            const float I_origNits = I_rel * max(refNits, 1e-3);

            // Normalise by the CONTENT PEAK, not Hable's fixed 4.8 — see
            // ToneMappingHablePeak().
            //
            // The per-scene measured peak when the extractor has one, the static
            // maxCLL otherwise (the first frames, or a signal the extractor cannot
            // read). The measured value is the pre-tone-map scene peak, so it is a
            // genuine source-referred number rather than a title-level one.
            const float scenePeak = (hableScenePeakNits > 10.0f) ? hableScenePeakNits : maxCLL;
            const float peakSdrUnits = max(scenePeak / max(refNits, 1e-3), 1.0);

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

            // Soft-plus pre-compressor (disabled when softKnee <= 0 — identical to
            // before it existed). Below the knee the map is exactly identity; above it
            // the band Hable compresses least is re-spread before the curve sees it.
            // The normaliser below still uses the UNCOMPRESSED content peak, so the
            // compressed top rides lower on the Hable input and the sky keeps headroom.
            // Overflow-safe softplus: sp(t) = max(t,0) + log(1 + exp(-|t|)).
            float3 hvP = hv;
            if (softKnee > 0.0) {
                const float t  = (hv.x - softKnee) / max(softKneeWidth, 1e-3);
                const float sp = max(t, 0.0) + log(1.0 + exp(-abs(t)));
                const float comp = softKnee + softKneeWidth * sp / max(softKneeRatio, 1e-3);
                hvP = float3(min(hv.x, comp), min(hv.y, comp), min(hv.z, comp));
            }

            const float hMapped = ToneMappingHablePeak(hvP, peakSdrUnits).x;
            const float hFinal = hMapped;   // the mobius knee was removed — see cbuffer comment
            I_rel = hFinal;

            // --- Chroma follows the tone curve down --------------------------------
            // This was the blotches.
            //
            // Ct/Cp are ABSOLUTE chroma. Handing the ORIGINAL ones back after the
            // curve has lowered I makes the colour over-saturated with respect to its
            // new level — so the reconstruction leaves the display gamut and
            // ictcp_to_linear_gamut() has to pull it back with a per-pixel search,
            // on every bright saturated pixel. That search is a saturation REMOVAL,
            // and where it engages across a smooth gradient it leaves visible
            // structure: patches on a face, hard-edged and skin-shaped.
            //
            // It got worse the lower sdrNits went, because the tone map then lowers I
            // by a larger factor while the chroma stays where the source put it. That
            // is exactly what the operator found: raise sdrNits and the patches
            // disappear, because the reduction shrinks and the search stops firing.
            //
            // Scaling the chroma by the same factor I moved means the reconstruction
            // is (near enough) the original colour scaled down — already inside the
            // gamut, so the search becomes a no-op instead of a heavy correction.
            //
            // The curve as a per-pixel GAIN on the whole colour, applied in LINEAR.
            //
            // This replaces `ictcp.yz *= chromaScale` with I re-encoded: that pair
            // scaled I in the LINEAR domain and the chroma in the PQ-ENCODED one,
            // and the two do not compose to a uniform scale. So the effective
            // saturation depended on the gain — which means it depended on the
            // ANCHOR. Since the anchor now ramps smoothly (libplacebo's IIR), that
            // showed up as colour draining out of the picture over about a second:
            // a saturated diffuse light going pale, orange to white.
            //
            // Scaling the original colour in linear and re-deriving the ICtCp is
            // exact: a linear scale of R, G and B cannot move chromaticity, so the
            // anchor may move as much as it likes and the colour holds. This is the
            // same treatment as ps_ml_gamma and ps_ml_shadow.
            const float I_newNits = I_rel * max(refNits, 1e-3);
            const float gGain = I_newNits / max(I_origNits, 1e-6);

            // ictcp still holds the ORIGINAL I here (the curve result lives in
            // I_rel / I_newNits), so ictcp_to_linear returns the original colour.
            const float3 ictcpNew = linear_to_ictcp(ictcp_to_linear(ictcp) * gGain);

            color.rgb = ictcp_to_linear_gamut(ictcpNew, sdrNits) / max(sdrNits, 1e-3);
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
