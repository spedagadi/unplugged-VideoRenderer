// -----------------------------------------------------------------------------
// ZION TASTE STAGE -- the user's last word.
//
// WHY THIS EXISTS AS ITS OWN PASS (2026-10-08)
// The taste dials (exposure trim, shadow detail, punch) used to run inside the
// tone-mapping converter, i.e. BEFORE the automatic grade. That inverted the
// priority: the model's gamma, shadow slope and highlight rolloff were applied on
// top of what the user asked for, so moving a dial could be silently undone and the
// user had no way to see it. The rule now is one owner per axis and the user last:
//
//     PQ -> linear -> tone curve -> grade (automatic look) -> TASTE -> bars
//        -> detail externals -> encode -> calibration -> dither
//
// DOMAIN. The frame arrives in the pipeline's own display-referred encoding:
// gamma-2.2 code in 0..1 (ps_ml_chroma.hlsl:206 and the converter's Reference
// branch both end with pow(x, 1/2.2)). So the shape operators -- which are defined
// on CODE values -- apply directly, with no transfer round-trip. The earlier
// in-converter version encoded with the sRGB curve (1.055 x^(1/2.4) - 0.055) while
// the rest of the chain uses a plain 2.2 power; that mismatch was invisible but
// real, and it is gone here.
//
// WHAT IS NOT HANDLED, deliberately: the HDR-output path (IsPQ) delivers PQ-coded
// values, where a code-space taste curve means something else entirely. Rather
// than apply the wrong maths quietly, this pass becomes a passthrough there and
// says so in the log. Fixing that means parameterising the transfer, which is its
// own change.
// -----------------------------------------------------------------------------

Texture2D tex : register(t0);
SamplerState samp : register(s0);

cbuffer TasteParams : register(b0)
{
	float shadowLift;    // -1..1, normalised from the -5..+5 dial
	float punch;         // -1..1
	float reserved0;     // was expTrimGain: exposure is LIGHT, so it belongs before
	                     // the curve, where the shoulder absorbs it. Kept as a slot so
	                     // the C++ layout and this declaration stay one-to-one.
	float active;        // 0 = every dial at 0 -> passthrough; also 0 on the PQ path
	float dbgView;       // 0 off, 1 = |delta| the dials applied, 2 = shadow window mask
	float _pad0;
	float _pad1;
	float _pad2;
};

// The project's own shadow band: Source/ml_features.cpp:459-460 defines (4,48]/255
// as the toe region, and the same bounds produce the shMean/shN columns of the
// DELIVERED lines in ml_debug.csv. Binding the dial to this constant is what makes
// the control and the measurement describe the same pixels.
static const float kShadowKnee = 48.0 / 255.0;
static const float3 kW709 = float3(0.2126, 0.7152, 0.0722);

struct PS_INPUT
{
	float4 Pos : SV_POSITION;
	float2 Tex : TEXCOORD;
};

// Rec.709 luma of a CODE-space colour. Weights sum to 1, so a grey pixel's luma is
// its channel value -- which is what makes the operators below code-space, not
// linear-space, statements.
float LumaCode(float3 c) { return dot(c, kW709); }

// -----------------------------------------------------------------------------
// The two taste operators, applied to LUMINANCE ONLY.
//
// Both act on the luma of the pixel and are carried back onto the colour by
// preserving the RGB ratios in linear light: RGB_out = (RGB_in / Y_in) * Y_out.
// Shaping R, G and B independently is a colour operation, not a tone one -- a face
// at (0.90,0.30,0.30) pivoted about 0.5 drives R up and G,B down, so saturation and
// hue ride the dial. That is what made the picture change colour and go waxy past
// about +/-3.
//
// Clipping: brightening by a scalar can push a saturated channel past 1.0, which
// CLIPS -- a colour change again, and an ugly one. So when the scale brightens it is
// capped at the largest value that keeps every channel in range: saturated
// highlights roll gently toward white instead of clipping.
// -----------------------------------------------------------------------------
float3 Taste(float3 code)
{
	// Transfer pair: decode is ^(2.2), encode is ^(1/2.2). Getting these to MATCH is
	// what makes neutral dials an exact no-op -- the first version squared on the way
	// in (an approximation) and raised to 2.2 on the way OUT, which compounded the
	// frame to code^4.4 and crushed it to black for any non-zero setting. An
	// identity check at neutral catches that class of error before a build does;
	// _gt7probe/taste_check.ps1 asserts it.
	const float3 lin = pow(max(code, 0.0), 2.2);

	// NO exposure trim here. Trim changes LIGHT, so it is applied before the curve
	// (ps_convert_pq_to_sdr.hlsl), where Hable's shoulder absorbs it. Applying it
	// post-curve blew saturated highlights, because a display-referred curve has
	// already promised its top end to 1.0 and a gain after that has nowhere to go.

	// 1. Shape the luma, in code space.
	float3 linq = max(lin, 1e-6);
	float yIn = LumaCode(linq);
	float y = pow(yIn, 1.0 / 2.2);

	// Shadow detail: LOCAL to the normalised window [0, kShadowKnee]. Positive
	// spreads adjacent dark codes apart (that IS the detail), negative compresses
	// them. Identity at and above the knee. Monotone by construction: both ends of
	// the window are fixed and the pieces meet continuously at the knee. (Weighting
	// the deviation by a falling ramp instead was measured to FOLD -- deep shadows
	// coming out brighter than mid shadows.)
	const float q = exp2(shadowLift * 0.6);               // -1..1 -> 0.66..1.52
	if (y < kShadowKnee) {
		// saturate() is not a clamp for correctness here -- the guard above already
		// puts y/k in [0,1) -- it is how the compiler is told so. Without it fxc emits
		// X3571 (pow with a possibly-negative base), and a warning that outlives its
		// cause gets "fixed" by someone who cannot tell it was already impossible.
		y = kShadowKnee * (1.0 - pow(saturate(1.0 - y / kShadowKnee), q));
	}

	// Punch: contrast about mid grey. s<1 (positive) drives both arms away from the
	// pivot, s>1 flattens. Black, mid grey and white stay exactly put.
	const float s = exp2(-punch);                         // -1..1 -> 2..0.5
	const float d = abs(y - 0.5);
	const float sgn = (y > 0.5) ? 1.0 : ((y < 0.5) ? -1.0 : 0.0);
	y = saturate(0.5 + sgn * 0.5 * pow(max(2.0 * d, 0.0), s));

	// 2. Carry the shaped luma back without touching colour.
	const float yOut = max(pow(y, 2.2), 1e-6);
	float scale = yOut / yIn;
	{
		const float hi = max(linq.r, max(linq.g, linq.b));
		if (scale > 1.0) {
			scale = min(scale, 1.0 / hi);
		}
	}
	return pow(saturate(linq * scale), 1.0 / 2.2);        // re-encode to 2.2 code
}

float4 main(PS_INPUT input) : SV_Target
{
	float4 c = tex.Sample(samp, input.Tex);
	if (active < 0.5) {
		return c;
	}

	const float3 before = c.rgb;
	const float3 after = Taste(before);

	// INSTRUMENTATION. Renders the operators themselves instead of the picture, so
	// "is this dial doing anything" is answered by the frame rather than by a model
	// of the maths.
	//   1 = |delta| the dials applied, x3 so a small move is visible. A uniformly
	//       BLACK frame at punch=+5 proves the value never reached this pass --
	//       which looks identical on screen to "the operator is too weak" and is a
	//       completely different bug.
	//   2 = the shadow window mask: white where the shadow dial may act.
	if (dbgView > 0.5) {
		if (dbgView < 1.5) {
			return float4(saturate(length(after - before) * 3.0).xxx, c.a);
		}
		return float4((1.0 - step(kShadowKnee, LumaCode(before))).xxx, c.a);
	}

	return float4(after, c.a);
}
