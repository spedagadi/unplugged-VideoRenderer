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
    float hableNeutralStrength;  // 0..1: highlight neutral roll in the Hable
                                 // branch (0 = off, byte-identical to pre-feature)
    float hableRefWhiteNits;   // the Hable curve's 1.0 in nits
    float hableChromaExp;      // 1.0 = none; >1 desaturates compressed highlights — decouples "normal
                               // scene light" from display white (sdrNits). 0 = fall
                               // back to sdrNits (identical to before). Clouds at
                               // 150-200 nits are below a 200-nit reference but 2x an
                               // 85-nit one — this axis tells the curve they are content.
    // The GT7 operator's own two knobs (selection 8). Everything else about it is
    // Polyphony's shipped constants; see convert/gt7_tonemap.hlsl.
    //   gt7Blend: 0 = the raw per-channel "skewed" curve, 1 = luminance-from-skewed
    //     with chroma taken from the ORIGINAL colour. GT ships 0.6. This is the dial
    //     that decides hue behaviour, so it is the one worth having.
    //   gt7FadeStart/End: the chromaCurve window, in units of the ceiling's ICtCp I.
    //     GT ships 0.98/1.16. Because that ratio is in PQ CODES, not linear nits, the
    //     window sits far lower in nits than it looks — measured, for a 225-nit
    //     ceiling 0.98 is 201 nits and 1.16 is 546 nits, i.e. exactly the video
    //     highlight band. Below fadeStart the source's chroma is passed through
    //     untouched; by fadeEnd it is fully gone and the top lands on neutral.
    float gt7Blend;
    float gt7FadeStart;
    float gt7FadeEnd;
    // GT7 Adapt (selection 10): weight of the MEASURED scene peak in the peak this
    // curve maps FROM. 0 = plain GT7 (metadata peak alone), 1 = scene peak alone.
    // cbuffer[24] — see the 16-byte alignment note in DX11VideoProcessor.cpp.
    float gt7Adapt;
    // GT7_ALPHA, 0.25 = Polyphony. This is the shoulder's REACH, not its shape: the
    // curve lands on the ceiling at 1.21x ceiling at 0.50, 1.47x at 0.25 and 1.87x at
    // 0.10, and clamps above that. Raising refWhite buys exposure but pushes content
    // past that point, so this is the term that lets exposure rise without the
    // highlights dying. 0 = fall back to the shipped constant. cbuffer[25].
    float gt7Alpha;
    // How much of the shoulder's luminance compression is passed on to chroma.
    // 0 = none (previous behaviour), 1 = chroma compresses exactly as hard as
    // luminance. cbuffer[26].
    float gt7Desat;
    // Gamut capacity target for the BT.2020 -> BT.709 mapping: colours are landed at
    // this fraction of their distance to the gamut edge rather than on the edge.
    // 1.0 = boundary behaviour (nothing in-gamut touched), ~0.55 matches madVR's
    // measured sky. 0 = treated as 1.0. cbuffer[27].
    float gamutCap;
    // 1 = THIS shader must apply the gamut mapping; 0 = the chroma pass will, after
    // its own boost. The mapping has to run exactly once, and it has to run AFTER the
    // last stage that can change chroma — which is ps_ml_chroma when the grade is up,
    // and this shader when it is not. cbuffer[28].
    float gamutMapHere;
    // DYNAMIC & AUTO CURVE (selection 11): the three parameters of
    //     code(nits) = 1 - exp( -k * (nits/x0)^g )
    // solved on the CPU from the scene-locked source statistics and the house targets.
    // They take the three padding slots, so the buffer stays 36 floats / 144 bytes and
    // no existing offset moves. x0 arrives as 0 until the first solve completes.
    float dynG;
    float dynK;
    float dynX0;
    // GT7 debug view: 0 = off, 1 = chromaScale, 2 = fade ratio, 3 = knee crossing,
    // 4 = ceiling crossing. Renders the chosen quantity as greyscale INSTEAD of the
    // picture, so a spatial seam in any of them is visible directly rather than
    // inferred. cbuffer[32]; buffer is 36 floats / 144 bytes.
    float gt7Dbg;
    float _padD0;
    float _padD1;
    float _padD2;
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
// The GT7 operator (selection 8). Needs ictcp.hlsl above it: it reuses
// linear_to_ictcp / ictcp_to_linear rather than porting GT's own colour math, whose
// matrices are numerically identical to ours.
#include "../convert/gt7_tonemap.hlsl"

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
    float3 ictcpNew = linear_to_ictcp(ictcp_to_linear(ictcp) * gGain);
    {
        // Nonlinear chroma compensation (same guarded+gated control as Hable).
        const float chExp = (abs(hableChromaExp) < 0.001f)
                            ? 1.0f : clamp(hableChromaExp, -1.0f, 3.5f);
        float cr = pow(max(gGain, 1e-6), chExp - 1.0f);
        if (cr > 1.0f) {
            const float chroma0 = length(float2(ictcpNew.y, ictcpNew.z));
            const float gate = smoothstep(0.02f, 0.14f, chroma0);
            cr = 1.0f + (cr - 1.0f) * gate;
        }
        ictcpNew.y *= cr;
        ictcpNew.z *= cr;
    }

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
    // Bounded, NOT `> 6.5`: 8 (GT7) is also above 6.5 and would silently render as
    // the reference curve. Any future selection has to be added to this test.
    if (toneCurveSelection > 6.5 && toneCurveSelection < 7.5) {
        color.rgb = ToneMappingHable(color.rgb);

        if (outputGamut < 0.5) {
            color.rgb = Colorspace_Gamut_Conversion_2020_to_709(color.rgb);
            // The CONSTRAINT is conditional; the conversion above is not. An earlier
            // edit folded the two into one gated call, so when the chroma pass took
            // over the constraint (gamutMapHere = 0) the primaries conversion vanished
            // with it and the frame reached the panel still in 2020 primaries —
            // washed out exactly as if the output gamut were BT.2020.
            if (gamutMapHere > 0.5)
                color.rgb = Colorspace_Gamut_Constrain(color.rgb,
                                                       (gamutCap > 0.001f) ? gamutCap : 1.0f);
        }
    }
    // ---------------------------------------------------------------------
    // 8 = GT7, Polyphony Digital's operator (MIT, v1.0 2025-08-10).
    //
    // ADAPTED FORM (2026-09-26) — the official-verbatim verdict is on the record
    // and it was a cliff, not a curve: with ceiling = sdrNits and a 991-nit
    // master, the operator's clamp at 2.046*ceiling flattened the top ~60% of
    // the source range into white — the "strong contrast" / crushed-shadows S
    // that read badly next to madVR. So the input is now PLACED on the operator's
    // axis before it arrives — content peak -> the curve, via a gain from
    // refWhite and scenePeak — and the operator still takes ceiling = sdrNits,
    // grayPoint = 0.538-rel (now 0.215 * ceiling), blendRatio and the chroma
    // fade window, all Polyphony's. The gain, the soft-plus pre-fit and the
    // sdrNits-free rule live in the branch body below; see it for the measured
    // numbers (0% clip to 4000 nits, both dials live, warmth matches madVR's).
    //
    // Ranked above the experimental tone LUT for the REFERENCE curve's reason: an
    // explicit curve selection must not be replaced by a different axis, and
    // ToneLutActive() excludes GT7 so the two cannot disagree.
    //
    // NOT routed through ToneMapSelectedCurve: it takes luminance from a
    // per-channel pass and chroma from the ORIGINAL colour, so neither that
    // function's I-channel-plus-uniform-gain convention nor the Hable branch's
    // describes it.
    // ---------------------------------------------------------------------
    // Bounded on BOTH sides, and now on both values: 8 = GT7, 10 = GT7 Adapt (this
    // branch, one different input — see gtScenePeak below). 9 (SPLINE) is also above
    // 7.5 and must fall through to the experimental-LUT branch, not be rendered as
    // GT7, which is why the old test could not simply become `> 7.5`.
    else if ((toneCurveSelection > 7.5 && toneCurveSelection < 8.5)
          || (toneCurveSelection > 9.5 && toneCurveSelection < 10.5)) {
        const float sdrNits = 10000.0 / max(LuminanceScale, 1e-3);
        const float refNits = max(hableRefWhiteNits, 1.0f);

        // --------------------------------------------------------------------
        // GT7, ADAPTED FORM — ceiling = display peak, input gain from refWhite
        // and scenePeak. Chosen by measurement (Phase 0 sweep, _gt7probe): it is
        // the only candidate that is simultaneously clip-free to 4000 nits,
        // bright-and-compressed in the madVR direction, warm-only-as-madVR-warm,
        // with BOTH hdr.sdrNits and hdr.refWhite live.
        //
        // Official-verbatim measured (2026-09-26) as a cliff, not a curve: with
        // ceiling = sdrNits = 200 and a 991-nit master, the operator's clamp at
        // 2.046*ceiling = 409 nits flattens the top 60% of the source range into
        // white, leaving the strong-contrast S-curve you rejected. The adapted
        // form feeds the operator an input already placed on its axis:
        //   * the gain (kGT7RangeFit * refWhite / scenePeak) lands the CONTENT
        //     peak on the curve instead of 2.4x past the clamp — scenePeak makes
        //     a 300- or 4000-nit master sit in the same place, so the shoulder
        //     (not the cliff) carries the highlights: measured 0% clip at
        //     300/1000/4000 nits. 2026-09-26: the original 0.764 was solved at a
        //     time when the file-path scene peak read 43 (metadata gap, fixed) —
        //     with the corrected 991 the 0.764 under-drove the mids badly (mean
        //     code 46 vs madVR 84). Re-solved against the Everest HDR still:
        //     2.2 predicts mean ~78 with headroom; 2.5 lands ~82.
        //   * it must NOT contain sdrNits, or the ceiling and the gain cancel and
        //     the dial goes inert (the a5c3e1e failure, one layer up);
        //   * the soft-plus pre-compressor (the SAME softKnee/softKneeRatio/
        //     softKneeWidth dials as the Hable branch) re-spreads the band it
        //     would otherwise flatten, so highlights keep structure to the top.
        //
        // refNits is live here again: it is the input-gain numerator (the source
        // level treated as white), which is what "reference white" means.
        // --------------------------------------------------------------------
        const float kGT7RangeFit = 2.2f;
        // STATIC metadata peak, deliberately NOT the IIR scene peak. Measured
        // 2026-09-26 on the file path: the live extractor read sPk = 43.4 for a
        // master whose metadata is cll=991 — a 9x gain error, and every pixel
        // above ~117 nits slammed into the operator's clamp("completely blown").
        // The adapted form is solved against the metadata peak (that is what the
        // probe measured), so maxCLL is the source of truth here. On the DeckLink
        // path maxCLL comes from the card's ST.2086; on the file path from the
        // title metadata — both are static per content, as the design requires.
        // GT7 ADAPT (selection 10) is the deliberate exception to the above, for the
        // case the paragraph describes but cannot solve: a path that carries NO
        // usable peak (cll=0, no mastering display, a Title peak that is a guess).
        // There the choice is not "metadata vs measurement" — it is "a wrong static
        // number vs a partly-right measured one", and a blend is the safe form of
        // that. It is a blend and not a switch because the failure is asymmetric:
        // a peak too LOW slams the top into the clamp (the sPk=43 disaster above),
        // while one too HIGH costs only a little exposure. At the default w=0.25 a
        // 9x extractor error moves the anchor 25% of the way, not 100%.
        //
        // The timing comes free from hableScenePeakNits: it is already IIR-smoothed
        // with a snap on scene cuts, so this tracks AT a cut, in one frame, and
        // holds inside a scene. Continuous tracking would be auto-gain — which is
        // precisely what the paragraph above is written against.
        float gtScenePeak = maxCLL;
        if (toneCurveSelection > 9.5 && hableScenePeakNits > 0.0) {
            // maxCLL is a FLOOR on the placement, and the measured peak may only RAISE it.
            // Mirrors the CPU's effPeak exactly — the two must not drift, because that
            // number is what auto-exposure and the band controller solve against.
            //
            // Plain lerp(maxCLL, measured, w) let a dark frame drag the divisor down with
            // it, and since the gain is proportional to 1/divisor that is a whole-image
            // brightening: the fade-from-black blowout, and the "adapt jumps crazily" step.
            // With the floor, content at or below the stated peak moves nothing at all, and
            // only a genuine excursion above it buys compression.
            gtScenePeak = lerp(maxCLL, max(maxCLL, hableScenePeakNits), saturate(gt7Adapt));
        }
        const float gtGain = kGT7RangeFit * refNits / max(gtScenePeak, 1.0f);
        // gtScenePeak MUST carry the fast running-max guard. A placement/protection split
        // that divided the gain by the scene-locked anchor alone was tried and reverted
        // 2026-10-04: it blew the highlights completely, because the pre-compressor knee
        // has no authority over content aimed far above the ceiling. The brightness step
        // that follows from a tracking gain is the price of not blowing out, and it is
        // paid here deliberately — see the note at the guard's definition in Render().

        // Into the operator's own units, where the ceiling is sdrNits.
        float3 adapted = color.rgb * sdrNits * gtGain;

        if (softKnee > 0.0) {
            // Detail-preserving (same idea as the Hable branch): compress a local
            // base (4-tap diagonal average, linear, in the operator's units) and
            // keep the per-pixel residual, so a low knee + ratio>1 re-spreads the
            // highlight band without flattening mid-tone texture into 8-bit patches.
            const float2 bd = 0.0025;
            float3 base = ST2084ToLinear(tex.Sample(samp, input.Tex + float2( bd.x,  bd.y)), LuminanceScale).rgb
                        + ST2084ToLinear(tex.Sample(samp, input.Tex + float2(-bd.x,  bd.y)), LuminanceScale).rgb
                        + ST2084ToLinear(tex.Sample(samp, input.Tex + float2( bd.x, -bd.y)), LuminanceScale).rgb
                        + ST2084ToLinear(tex.Sample(samp, input.Tex + float2(-bd.x, -bd.y)), LuminanceScale).rgb;
            base = base * 0.25 * sdrNits * gtGain;
            adapted = GT7PreCompress(adapted, base, softKnee * sdrNits, softKneeRatio,
                                     softKneeWidth * sdrNits);
        }

        // grayPoint anchored RELATIVE to the ceiling (GT7_GRAYPOINT_FRACTION x
        // sdrNits), not the reference's absolute 53.8 nits — rationale in
        // gt7_tonemap.hlsl; measured 2026-09-26, deep-shadow lift only.
        float4 gt7dbg;
        float3 mapped = GT7ToneMap(adapted, sdrNits, GT7_GRAYPOINT_FRACTION * sdrNits,
                                   clamp(gt7Blend, 0.0f, 1.0f),
                                   (gt7FadeEnd > gt7FadeStart) ? gt7FadeStart : GT7_FADE_START,
                                   (gt7FadeEnd > gt7FadeStart) ? gt7FadeEnd   : GT7_FADE_END,
                                   (gt7Alpha > 0.001f) ? gt7Alpha : GT7_ALPHA,
                                   // The fade window is keyed to the CONTENT peak, not
                                   // the ceiling: see gt7_tonemap.hlsl. This is the same
                                   // quantity the gain lands on the curve.
                                   kGT7RangeFit * refNits,
                                   gt7dbg);

        // Debug view: replace the picture with one of the operator's internal
        // quantities so a spatial seam shows up as an edge in a greyscale image
        // instead of having to be inferred from the finished frame.
        if (gt7Dbg > 0.5) {
            const float v = (gt7Dbg < 1.5) ? gt7dbg.x
                          : (gt7Dbg < 2.5) ? gt7dbg.y
                          : (gt7Dbg < 3.5) ? gt7dbg.z
                          :                  gt7dbg.w;
            mapped = saturate(v) * sdrNits;
        }

        // LUMINANCE-LINKED DESATURATION.
        //
        // HDR can carry a cobalt blue at 300 nits. SDR at 85 cannot show that chroma
        // at that luminance without it reading garish — the colour is inside the
        // BT.709 triangle, so gamut mapping has no jurisdiction over it at all. What
        // has to give is chroma, in proportion to how much luminance the curve
        // compressed. madVR does this; we did not, because the grade scales RGB
        // uniformly (deliberately, to stop hue drifting) and GT7's own chroma fade
        // only operates at 0.98..1.16 of the ceiling's I — nowhere near a mid-bright
        // sky. So a saturated master arrives at SDR with its HDR chroma intact.
        //
        // compression < 1 only where the shoulder actually worked, so below the knee
        // this is a no-op by construction: untouched midtones, no global wash.
        //
        // Lerping toward the pixel's own neutral point keeps HUE exactly and removes
        // only saturation — the same geometry the gamut compression uses, for the same
        // reason. gt7Desat 0 = lerp(1, x, 0) = 1 = the previous picture, byte for byte.
        if (gt7Desat > 0.001f) {
            const float lumIn  = dot(adapted, float3(0.2126, 0.7152, 0.0722));
            const float lumOut = dot(mapped,  float3(0.2126, 0.7152, 0.0722));
            const float compression = lumOut / max(lumIn, 1e-4);
            const float chromaScale = saturate(lerp(1.0, compression, saturate(gt7Desat)));
            const float lum = lumOut;
            mapped = lerp(float3(lum, lum, lum), mapped, chromaScale);
        }

        // Display mapping. Nothing else scales this, so sdrNits reaches the picture
        // through the ceiling AND through here — and because the gain is free of
        // sdrNits, the two compose instead of cancelling (measured 158 codes of
        // travel across 25..400).
        color.rgb = mapped / max(sdrNits, 1e-3);

        if (outputGamut < 0.5) {
            color.rgb = Colorspace_Gamut_Conversion_2020_to_709(color.rgb);
            // The CONSTRAINT is conditional; the conversion above is not. An earlier
            // edit folded the two into one gated call, so when the chroma pass took
            // over the constraint (gamutMapHere = 0) the primaries conversion vanished
            // with it and the frame reached the panel still in 2020 primaries —
            // washed out exactly as if the output gamut were BT.2020.
            if (gamutMapHere > 0.5)
                color.rgb = Colorspace_Gamut_Constrain(color.rgb,
                                                       (gamutCap > 0.001f) ? gamutCap : 1.0f);
        }
    }
    // ---------------------------------------------------------------------
    // 11 = DYNAMIC & AUTO. One solved transfer function replaces the whole
    // scalar chain: the CPU derives g, k and x0 from the scene-locked source
    // distribution and the house targets (see SolveDynamicAutoCurve), and this
    // branch applies them.
    //
    // Bounded on both sides like every other selection here — an open `> 10.5`
    // would capture any future curve the way GT7's `> 7.5` once swallowed the
    // spline.
    // ---------------------------------------------------------------------
    else if (toneCurveSelection > 10.5 && toneCurveSelection < 11.5) {
        const float sdrNits = 10000.0 / max(LuminanceScale, 1e-3);
        const float3 nits = color.rgb * sdrNits;
        // BT.2020 luma, because the source IS bt2020nc. The 709 weights would
        // mis-weight red by ~40% relative, and red is where HDR film differs most
        // from SDR — the error would land in the one quantity that places the
        // entire image.
        const float Y = max(dot(nits, float3(0.2627, 0.6780, 0.0593)), 1e-6);

        // Until the first solve lands, fall back to a shape a typical scene would
        // solve to rather than rendering something unmapped. One frame at most:
        // the solve runs in the same Render() call, before the draw.
        const float x0 = (dynX0 > 1.0) ? dynX0 : max(maxCLL, 100.0);
        const float g  = (dynX0 > 1.0) ? dynG : 0.6;
        const float k  = (dynX0 > 1.0) ? dynK : 2.303;

        const float code = 1.0 - exp(-k * pow(Y / x0, g));
        // The curve outputs DISPLAY-REFERRED CODE (0..1 of panel white), which is what
        // the house targets are stated in and what the probe measured. Encoding is
        // gamma 2.2 here, so the rest of the pipeline sees linear light as it does for
        // every other curve.
        const float yLin = pow(saturate(code), 2.2);

        // Chroma follows luminance by preserving the pixel's RGB RATIOS, applied in
        // LINEAR light — the same rule the reference path uses for gGain, and for the
        // same documented reason: scaling luma in linear while scaling chroma in the
        // code domain makes saturation depend on the gain, so a moving statistic
        // drains the colour. Here there is no separate chroma term at all: whatever
        // saturation survives is a consequence of the curve, not a dial.
        color.rgb = (nits / Y) * yLin;

        if (outputGamut < 0.5) {
            color.rgb = Colorspace_Gamut_Conversion_2020_to_709(color.rgb);
            // MAPPED, not clamped. Clamping collapses every out-of-gamut pixel onto
            // the same edge point: measured on a cyan down jacket at 20% out of
            // triangle, it rendered as a featureless slab with the weave deleted,
            // while the red beanie beside it kept its texture. Moving each pixel a
            // fraction of the way toward its OWN neutral keeps hue and gives up only
            // saturation, so neighbours stay neighbours.
            if (gamutMapHere > 0.5)
                color.rgb = Colorspace_Gamut_Constrain(color.rgb,
                                                       (gamutCap > 0.001f) ? gamutCap : 1.0f);
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
            // The CONSTRAINT is conditional; the conversion above is not. An earlier
            // edit folded the two into one gated call, so when the chroma pass took
            // over the constraint (gamutMapHere = 0) the primaries conversion vanished
            // with it and the frame reached the panel still in 2020 primaries —
            // washed out exactly as if the output gamut were BT.2020.
            if (gamutMapHere > 0.5)
                color.rgb = Colorspace_Gamut_Constrain(color.rgb,
                                                       (gamutCap > 0.001f) ? gamutCap : 1.0f);
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
            //
            // DETAIL-PRESERVING (2026-09-27). The plain pointwise form scales local
            // luminance texture by ~1/ratio across the WHOLE above-knee band. At the
            // low knee this needs (0.21 — the value that keeps ultra-bright DV skies
            // from clipping), that band is most of the image, and on an 8-bit output
            // the shrunken texture rounds to flat patches: the "oil-painting"
            // background. So compress a LOCAL BASE (the tone) and hand the per-pixel
            // residual (the texture) back at full amplitude — the sky still gets its
            // headroom, the foliage keeps its detail. Base = a 4-tap diagonal average
            // of linear luma in I_rel units; the residual carries everything the base
            // smoothed away, including the I-vs-luma offset at the centre pixel.
            float3 hvP = hv;
            if (softKnee > 0.0) {
                const float2 bd = 0.0025;   // base radius in UV (~a few px at 4K)
                const float3 lumaW = float3(0.2627, 0.6780, 0.0593);  // BT.2020 luma
                float base = dot(ST2084ToLinear(tex.Sample(samp, input.Tex + float2( bd.x,  bd.y)), LuminanceScale).rgb, lumaW)
                           + dot(ST2084ToLinear(tex.Sample(samp, input.Tex + float2(-bd.x,  bd.y)), LuminanceScale).rgb, lumaW)
                           + dot(ST2084ToLinear(tex.Sample(samp, input.Tex + float2( bd.x, -bd.y)), LuminanceScale).rgb, lumaW)
                           + dot(ST2084ToLinear(tex.Sample(samp, input.Tex + float2(-bd.x, -bd.y)), LuminanceScale).rgb, lumaW);
                base = base * 0.25 / max(refNits, 1e-3);
                const float det  = I_rel - base;
                const float t    = (base - softKnee) / max(softKneeWidth, 1e-3);
                const float sp   = max(t, 0.0) + log(1.0 + exp(-abs(t)));
                const float comp = softKnee + softKneeWidth * sp / max(softKneeRatio, 1e-3);
                // Residual (detail) is restored at FULL amplitude only in the mid-
                // tones, and attenuated toward the highlight band. Restoring it
                // fully on a bright base pushes micro-texture back up to the clip
                // point — the artificial white "peaking" on bright walls/skies —
                // because compressing highlights is exactly what the pre-compressor
                // is FOR. Gate on base level using only knee/width: below
                // knee+width keep all detail (no painting), by knee+3*width keep
                // only the compressed fraction (no peaking).
                const float keep = 1.0 - (1.0 - 1.0 / max(softKneeRatio, 1e-3))
                                   * smoothstep(softKnee + softKneeWidth,
                                                softKnee + 3.0 * softKneeWidth, base);
                const float Ic   = min(base, comp) + det * keep;
                hvP = float3(Ic, Ic, Ic);
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

            // Highlight saturation compensation (BT.2390 §7.2 saturation-preservation
            // shape; libplacebo does the same to IPT's T/P in colorspace.c). The
            // whole-color gain above preserves the source's chroma 1:1 through the
            // map, so under compression the top of the range keeps MORE saturation
            // than a reference that desaturates highlights — measured 2026-09-23 on
            // Everest: our top-10% snow chroma (max−min ≈ 47) is 2× madVR's (≈ 23).
            // Scaling chroma by an extra pow(gGain, γ−1) desaturates only where the
            // map compresses: γ=1.0 is byte-identical to today, γ>1 pulls highlights
            // toward neutral. DIRECTION PROBE — the exponent becomes a setting once
            // the direction is confirmed on the bench.
            float3 ictcpNew = linear_to_ictcp(ictcp_to_linear(ictcp) * gGain);
            {
                // Nonlinear chroma compensation (BT.2390-style). Allowed range
                // -1..3.5: >1 desaturates, <1 re-saturates. The BOOST side
                // (>1 ratio) is gated by the pixel's STARTING chroma so saturated
                // colours get the full push while near-achromatic pixels (warm
                // whites) get none — boosting cannot re-introduce the warm cast.
                // Desat (ratio<1) is left ungated: near-neutral chroma is
                // already ~0, so it is inert there anyway. 0-sentinel = none.
                const float chExp = (abs(hableChromaExp) < 0.001f)
                                    ? 1.0f : clamp(hableChromaExp, -1.0f, 3.5f);
                float cr = pow(max(gGain, 1e-6), chExp - 1.0f);
                if (cr > 1.0f) {
                    const float chroma0 = length(float2(ictcpNew.y, ictcpNew.z));
                    const float gate = smoothstep(0.02f, 0.14f, chroma0);
                    cr = 1.0f + (cr - 1.0f) * gate;
                }
                ictcpNew.y *= cr;
                ictcpNew.z *= cr;
            }

            // Display mapping: the curve works in refNits units (shape axis), the
            // PANEL works in sdrNits — so the conversion back to display space
            // divides by sdrNits. a5c3e1e made this ÷refNits ("brightness
            // invariant"), which silently made the entire Hable branch independent
            // of sdrNits: the LuminanceScale read at the top of main() is
            // multiplied away at the ICtCp step and never divided back in. The
            // xref/sdr amplification a5c3e1e feared (clouds clipping at 203/85) is
            // exactly what the hue-preserving headroom fit at the end of main()
            // (added 2026-09-23, after that commit) now absorbs: the >1 top scales
            // onto white without per-channel hue skew, the mids ride the target.
            color.rgb = ictcp_to_linear_gamut(ictcpNew, refNits) / max(sdrNits, 1e-3);

            // Highlight neutral roll (DIRECTION PROBE 2, 2026-09-23). Probes 1
            // (headroom clip-gain) and 2 (chroma exponent) were BOTH measured
            // no-ops: the Everest cast is in low-chroma warm grays — Ct/Cp near
            // zero, nothing over the ceiling — so neither could act. The only
            // lever that restores blue to an (R≈G>B) warm gray is pulling the
            // head toward its own LUMA level-dependently: as the top of the
            // range approaches the scene peak, the colour drains toward gray.
            // This is the shape madVR's highlight treatment takes (its head
            // lands on neutral white) and it matches the measured target: our
            // top-10% mean (223,221,176) vs madVR (216,212,193); B needs +17,
            // R/G −7 — exactly a pull toward Y of that band.
            {
                const float iTop = max(color.r, max(color.g, color.b));
                const float w = smoothstep(0.55f, 0.85f, iTop);
                const float Y = dot(color.rgb, float3(0.2126f, 0.7152f, 0.0722f));
                // Verified 2026-09-23 on Everest vs madVR. The reference head is WARM
                // (madVR highlight yellow-Δ = +20.6, B≈193), so strength 0.9 is the
                // match (our Δ then 19.3 ≈ 20.6); 0.45 under-corrects (+32). Now a
                // live cbuffer dial (hdr.highlightNeutral): 0 = off.
                color.rgb = lerp(color.rgb, float3(Y, Y, Y), w * hableNeutralStrength);
            }
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
            // The CONSTRAINT is conditional; the conversion above is not. An earlier
            // edit folded the two into one gated call, so when the chroma pass took
            // over the constraint (gamutMapHere = 0) the primaries conversion vanished
            // with it and the frame reached the panel still in 2020 primaries —
            // washed out exactly as if the output gamut were BT.2020.
            if (gamutMapHere > 0.5)
                color.rgb = Colorspace_Gamut_Constrain(color.rgb,
                                                       (gamutCap > 0.001f) ? gamutCap : 1.0f);
        }
    }

    // Linear to sRGB. Encoding stretches the shadow end, so the bottom of the
    // range is where a wrong floor shows up first — which is how the double-count
    // above was caught.

    // Headroom fit onto the DISPLAY white, hue-preserving (VERIFIED on the bench
    // 2026-09-23 as a warm-highlight cast): a warm pixel has R≈G>B, so at the top
    // of the range red and green hit the 1.0 ceiling while blue is still rising —
    // per-channel clipping then flattens R,G and the brightest whites tilt yellow
    // (measured blue −17 in snow highlights vs madVR, identical at 85 and 400
    // nits, identical in BT.709 and BT.2020, grade-inert). Scaling the whole
    // colour so the max channel lands on 1.0 clips the WHITE first and keeps the
    // blue — the top stays neutral, and only deep-highlight headroom is given up,
    // which is the same trade madVR's peak-onto-white makes.
    const float mTop = max(color.r, max(color.g, color.b));
    if (mTop > 1.0f) {
        color.rgb = color.rgb / mTop;
    }

    color = saturate(color);
    color = pow(color, 1.0 / 2.2);

    return color;
}
