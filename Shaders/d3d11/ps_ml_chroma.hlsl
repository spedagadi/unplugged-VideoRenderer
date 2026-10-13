// Zion ML grade — chroma vector tuning (ICtCp "Spectrum Shift v2").
// Adaptive chroma boost on Ct/Cp channels in ICtCp perceptual space.
// Skin protection via trained P(skin) LUT (detection in Cr/Cb, boost in Ct/Cp).
// When MLChromaGate is off the pass outputs its input unchanged.
#include "../convert/st2084.hlsl"
#include "../convert/ictcp.hlsl"
#include "../convert/colorspace_gamut_conversion.hlsl"

Texture2D tex : register(t0);
SamplerState samp : register(s0);
Texture2D skin_lut : register(t1);
SamplerState sampSkin : register(s1);
Texture2D sky_mask : register(t2);   // 640x360 R8G8 semseg mask (R=sky, G=person)
SamplerState sampSky : register(s2);

struct PS_INPUT
{
	float4 Pos : SV_POSITION;
	float2 Tex : TEXCOORD;
};

// The C++ MLParams_t is a single 320-byte layout shared by every ps_ml_* pass.
// Offsets are absolute (computed by the compiler from THIS declaration), so this
// shader must mirror the full struct — a short declaration would shift the
// person fields (and MLSkyUV*) onto the wrong bytes.
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
	float MLSkyStrength;
	float MLSkyGuard;
	float MLSkyLuma;
	float MLSkyUVOffX;
	float MLSkyUVOffY;
	float MLSkyUVScX;
	float MLSkyUVScY;
	float _padSky;
	// Person skin protection (stage 4) — appended tail, offsets above unchanged
	float MLSkinPersonStrength;  // 0..1 LUT-only-inside-people gate; 0 = legacy
	float MLSkinPersonDebugVis;  // 0/1 magenta person-mask overlay
	float _padPerson0;
	float _padPerson1;
	// Skin tone-preservation (stage 5) — appended tail
	float MLSkinAnchor;    // skin-set mean source luma (IIR, gamma-2.2)
	float MLSkinPreserve;  // 0..1 luma re-anchor toward source skin tone (unused in chroma; gamma t4)
	float MLSkinPad0;
	float MLSkinPad1;
	// GT tail. These MUST be declared in this order: MLParams_t is one shared layout
	// and the offsets are absolute. An earlier edit put MLGamutCap at _reserved0,
	// which is a different byte entirely — the pass then read the shadow-debug flag
	// (0) as the capacity target, and cap 0 means "collapse to neutral", which washed
	// out a whole frame on both gamut settings.
	float MLGammaTopUp;
	float MLShadowPedestal;
	float MLGamutCap;      // BT.709 capacity target; 1.0 = no mapping
	float MLGamutMap709;   // 1 = output gamut is BT.709, so the mapping applies here
};

static const float3 luma_w = float3(0.2126, 0.7152, 0.0722);

// The gamut constraint, applied to this pass's ENCODED output.
//
// This pass is the last stage in the chain that can change chroma: the boost below
// multiplies Ct/Cp by up to 1.5x. A capacity limit applied upstream of it — which is
// where it lived until today — gets undone by exactly that boost, which is why the
// sky ignored the cap while near-white snow (nothing to restore) obeyed it. The
// constraint has to see the colour the display actually receives.
//
// The mapping itself works in linear primaries, so decode and re-encode around it.
// PQ output is skipped: the 2020->709 conversion only exists on the SDR path.
float4 GamutConstrain(float4 c)
{
	// Gated on the OUTPUT gamut, not just the cap: with BT.2020 selected there is no
	// 709 triangle to constrain to, and running the constraint anyway is what made
	// both gamut settings look identically washed out.
	if (MLGamutMap709 < 0.5 || MLGamutCap > 0.999 || IsPQ > 0.5)
		return c;
	// Constrain, NOT convert: this colour is already in 709 primaries (the correction
	// shader did the 2020->709 matrix upstream), so running that matrix again here
	// treats a 709 colour as 2020 and shrinks the gamut twice — which is exactly what
	// washed a whole frame out the first time this moved.
	const float3 lin = pow(max(c.rgb, 0.0), 2.2);
	c.rgb = pow(saturate(Colorspace_Gamut_Constrain(lin, MLGamutCap)), 1.0 / 2.2);
	return c;
}

float4 main(PS_INPUT input) : SV_Target
{
	float4 color = tex.Sample(samp, input.Tex);

	// The GRADE's boost and the USER's highlight desat are two different switches and
	// they used to share one gate: `if (MLChromaGate < 0.5) return` exited before the
	// desat too, so turning Zion's chroma tuning off silently killed hdr.hiDesat — the
	// exact "my dial does nothing" confusion the rail exists to avoid. Measured on the
	// Dune 12 dusk sky: chroma off, cyan unchanged, and Hi-Desat at max only denting it
	// when chroma was back on. The pass now runs whenever EITHER wants it, and the boost
	// alone is gated below.
	//
	// Both off still short-circuits: no ICtCp round trip, no LUT sample, and the output
	// is bit-identical to what this pass returned before this existed.
	if (MLChromaGate < 0.5 && MLHiDesat <= 0.001f)
		return GamutConstrain(color);

	// Convert to linear nits → ICtCp
	float3 lin;
	if (IsPQ > 0.5) {
		lin = ST2084ToLinear(float4(color.rgb, 0.0), 10000.0).rgb;
	} else {
		lin = pow(max(color.rgb, 0.0), 2.2) * SDR_PEAK_NITS;
	}
	float3 ictcp = linear_to_ictcp(lin);

	// Skin detection: use gamma-2.2 Cr/Cb with existing trained LUT
	float3 g22;
	if (IsPQ > 0.5) {
		g22 = pow(max(lin, 0.0), 1.0 / 2.2);
	} else {
		g22 = color.rgb;
	}
	float y_g22 = dot(g22, luma_w);
	float cr = (g22.r - y_g22) / 1.5748;
	float cb = (g22.b - y_g22) / 1.8556;
	float2 skin_uv = saturate(float2(
		(cr - MLSkinLUTCR0) / (MLSkinLUTCR1 - MLSkinLUTCR0),
		(cb - MLSkinLUTCB0) / (MLSkinLUTCB1 - MLSkinLUTCB0)));
	float skin_weight = skin_lut.Sample(sampSkin, skin_uv).r;

	// Person gate (stage 4): LUT weight is kept as-is OUTSIDE people, and
	// multiplied by the person mask INSIDE people — protection concentrates on
	// actual people (kills red/orange-object false positives). Strength 0 falls
	// back to the plain LUT exactly (lerp(a,b,0) is IEEE-exact) → bit-identical.
	float eff = skin_weight;
	if (MLSkinPersonStrength > 0.001) {
		float2 cuv = (input.Tex - float2(MLSkyUVOffX, MLSkyUVOffY))
			/ max(float2(MLSkyUVScX, MLSkyUVScY), 0.001f);
		const float inContent = (all(cuv >= 0.0) && all(cuv <= 1.0)) ? 1.0 : 0.0;
		float P = sky_mask.Sample(sampSky, saturate(cuv)).g * inContent;
		eff = lerp(skin_weight, skin_weight * P, MLSkinPersonStrength);
	}

	// One level axis shared by both operators below: this pass's own normalised
	// intensity. Both key on it so they cannot disagree about where a pixel sits.
	float I_peak = (IsPQ > 0.5) ? max(MLL1Max, 0.01) : SDR_I_PEAK_PQ;
	float I_norm = ictcp.x / I_peak;

	// ── the GRADE's boost, gated by MLChromaGate ────────────────────────────────────
	// 1.0 at black -> neutral at knee -> fire at peak. Monotonic ramp matches a
	// tone-mapping desaturation profile (highlights lose more saturation than shadows).
	// No boost on near-black noise.
	float chroma_scalar = 1.0f;
	if (MLChromaGate >= 0.5) {
		float luma_boost;
		if (I_norm <= MLChromaKnee) {
			float tb = I_norm / max(MLChromaKnee, 0.01);
			luma_boost = lerp(1.0, MLChromaNeutral, tb);
		} else {
			float tb = saturate((I_norm - MLChromaKnee) / max(1.0 - MLChromaKnee, 0.01));
			luma_boost = lerp(MLChromaNeutral, MLChromaFire, pow(tb, 1.5));
		}
		// Suppress boost on skin (person-gated LUT weight)
		chroma_scalar = clamp(lerp(luma_boost, 1.0, saturate(eff * MLChromaSkinProtect)),
		                      1.0f, 1.50f);
	}

	// Scale Ct/Cp — perceptually uniform chroma boost
	ictcp.y *= chroma_scalar;
	ictcp.z *= chroma_scalar;

	// Post-curve highlight desaturation — the term the tone curve structurally cannot
	// have, and the counterweight to the boost above.
	//
	// WHY IT BELONGS HERE. The converter desaturates by pow(gGain, chromaExp-1), where
	// gGain is the CURVE's compression of that pixel. But the band steps run afterwards
	// and move I in ICtCp while holding Ct/Cp ABSOLUTE (see ps_ml_gamma.hlsl:6) —
	// constant chroma magnitude over changed intensity is, by definition, a change of
	// RELATIVE saturation. A band lift washes colour out, a band crush pushes it up,
	// and the converter's exponent has already been decided and cannot see either. This
	// pass is the last stage that can change chroma, so it is the only place the
	// DELIVERED level is known: keying here accounts for the curve, the bands and
	// micro-contrast at once, without modelling any of them on the CPU.
	//
	// SAME DOMAIN, ON PURPOSE. The window is keyed on I_norm, the very quantity whose
	// Ct/Cp it scales. Neither MLChromaKnee (the BOOST's knee, a different question) nor
	// the grade's MLHighlightKnee (ps_ml_gamma's domain) is reused for it: comparing
	// either against I_norm is the I-vs-chroma split that has made saturation drift with
	// the curve three times already in this project. The two edges below are fitted
	// constants belonging to THIS operator, expressed in THIS pass's units.
	//
	// SDR PATH ONLY (IsPQ < 0.5). When the grade runs on a PQ image the tone curve has
	// not run yet, so there is no delivered level to refer to and the converter's
	// exponent is the whole story — applying this there would desaturate twice.
	//
	// SHAPE: A WINDOW, NOT A RAMP TO THE CEILING. Fitted, not chosen — fillmap.ps1 on the
	// Dune 12 dusk frame (2026-10-12), seven matched bands against madVR, taking the
	// chroma scale madVR uses relative to ours (his gamut fill / ours) at each band's
	// I_norm:
	//     I_norm   0.44   0.55   0.57   0.59   0.61   0.66   0.78
	//     needed   0.96   0.81   0.76   0.71   0.66   0.54   0.54
	//     window   1.00   0.82   0.76   0.71   0.66   0.54   0.50   <- smoothstep .45->.70, dial .50
	//     fill     0.58   0.61   0.59   0.57   0.55   0.43   0.28
	//     madVR    0.56   0.60   0.60   0.58   0.54   0.43   0.31
	// mean |error| 0.014 across the seven. The linear ramp this term shipped with scores
	// 0.125 — nine times worse — and its failure is the reported symptom: only 13% engaged
	// where the complaint lives (I_norm 0.59) and fully engaged by the ceiling, i.e.
	// "hiDesat at max barely changes the sky but flattens the speculars". madVR's
	// desaturation is a WINDOW that closes by I_norm ~0.70, not a ramp that finishes at
	// the top of the range. The window's upper edge also stops the term from spending
	// itself on the speculars it was never meant to touch.
	const float kHiDesatStart = 0.45f;
	const float kHiDesatEnd   = 0.70f;
	// Skin keeps its colour at any setting: the pull is damped by the same person-gated
	// P(skin) weight that suppresses the boost, so this cannot drain a face — the one rule
	// that is not negotiable here. MLHiDesat 0 leaves the branch entirely.
	if (MLHiDesat > 0.001 && IsPQ < 0.5) {
		const float guard = 1.0 - saturate(eff * MLChromaSkinProtect);
		const float w = smoothstep(kHiDesatStart, kHiDesatEnd, I_norm);
		const float desat = 1.0 - clamp(MLHiDesat, 0.0f, 1.0f) * w * guard;
		ictcp.y *= desat;
		ictcp.z *= desat;
	}

	// Inverse: ICtCp → linear nits → output
	float3 rgb_out = ictcp_to_linear(ictcp);

	if (IsPQ > 0.5) {
		color.rgb = LinearToST2084(float4(rgb_out, 0.0), 10000.0).rgb;
	} else {
		color.rgb = pow(saturate(rgb_out / SDR_PEAK_NITS), 1.0 / 2.2);
	}

	return GamutConstrain(color);
}
