// BT.2100 ICtCp conversion matrices (SMPTE ST 2084 PQ transfer)
// Shared by all ML grade passes.

// Linear BT.2020 RGB → LMS
static const float3x3 rgb2lms = float3x3(
	1688.0/4096.0, 2146.0/4096.0,  262.0/4096.0,
	 683.0/4096.0, 2951.0/4096.0,  462.0/4096.0,
	  99.0/4096.0,  309.0/4096.0, 3688.0/4096.0
);

// LMS → Linear BT.2020 RGB
static const float3x3 lms2rgb = float3x3(
	 3.43661, -2.50645,  0.06984,
	-0.79133,  1.98360, -0.19227,
	-0.02595, -0.09891,  1.12486
);

// PQ-encoded LMS → ICtCp
static const float3x3 pqlms2ictcp = float3x3(
	 2048.0/4096.0,  2048.0/4096.0,      0.0/4096.0,
	 6610.0/4096.0, -13613.0/4096.0,  7003.0/4096.0,
	17933.0/4096.0, -17390.0/4096.0,  -543.0/4096.0
);

// ICtCp → PQ-encoded LMS
static const float3x3 ictcp2pqlms = float3x3(
	1.0,  0.008609037037933,  0.111029625003026,
	1.0, -0.008609037037933, -0.111029625003026,
	1.0,  0.560031335710679, -0.320627174987319
);

static const float SDR_PEAK_NITS = 203.0;
static const float SDR_I_PEAK_PQ = 0.5893;

// Forward: linear nits → ICtCp
float3 linear_to_ictcp(float3 lin)
{
	float3 lms = mul(rgb2lms, lin);
	float3 pq_lms = LinearToST2084(float4(max(lms, 0.0), 0.0), 10000.0).rgb;
	return mul(pqlms2ictcp, pq_lms);
}

// Inverse: ICtCp → linear nits
float3 ictcp_to_linear(float3 ictcp)
{
	float3 pq_lms = mul(ictcp2pqlms, ictcp);
	float3 lms = ST2084ToLinear(float4(max(pq_lms, 0.0), 0.0), 10000.0).rgb;
	return max(mul(lms2rgb, max(lms, 0.0)), 0.0);
}

// Inverse, desaturating ONLY as far as the gamut requires.
//
// Ct/Cp are ABSOLUTE chroma, not relative: tone-mapping I downward while handing
// the original Ct/Cp back makes the colour relatively more saturated than the
// source, and the reconstruction can leave the gamut. Plain ictcp_to_linear then
// clamps negative L'M'S' to zero, which is a hue error rather than a graceful
// falloff — the same family of failure as per-channel RGB clamping, one layer up.
//
// So pull the chroma toward the achromatic axis at the NEW intensity until the
// RGB fits. This keeps the hue (the Ct/Cp ratio is scaled uniformly, so it is
// unchanged) and keeps the tone-mapped intensity; only saturation gives.
//
// A search rather than a fixed factor because PQ decoding is nonlinear: there is
// no closed form for the scale that just reaches the boundary. The in-gamut case
// — almost every pixel — costs one reconstruction and zero iterations.
// `maxNits` is the DISPLAY CEILING, in the same units ictcp_to_linear returns —
// which is ABSOLUTE NITS, not normalised. Getting this wrong is not subtle: a
// ceiling of 1.0 means "1 nit", so every pixel fails the test, the bisection runs
// on all of them, and the picture goes black and white because the chroma scale
// converges toward the achromatic axis. The caller knows the ceiling; this
// function cannot.
float3 ictcp_to_linear_gamut(float3 ictcp, float maxNits)
{
	float3 rgb = ictcp_to_linear(ictcp);
	if (min(min(rgb.r, rgb.g), rgb.b) >= 0.0 &&
	    max(max(rgb.r, rgb.g), rgb.b) <= maxNits)
		return rgb;                       // in gamut: nothing to do

	// Bisect on the chroma scale. s = 0 is the achromatic value of the same I,
	// which is in gamut by construction: the curve bounds I, and zero chroma
	// reconstructs that same bounded level on all three channels.
	//
	// ITERATION COUNT IS LOAD-BEARING, and 5 was wrong. The loop leaves sLo on a
	// multiple of 1/2^N, so N = 5 quantises the chroma scale to steps of 1/32 —
	// 3.1%, which is tens of codes of saturation at 10-bit. Because sLo is a STEP
	// function of the input, every pixel whose reconstruction exceeds the ceiling
	// gets its saturation snapped to that grid, and smooth shading in the affected
	// region breaks into flat patches. A lit face is exactly where the clamp
	// engages, so the artefact lands on skin.
	//
	// 12 iterations puts the grid at 1/4096 — below one output code, so the step is
	// not representable and the correction is effectively continuous. The cost is
	// bounded and only paid by out-of-gamut pixels: the in-gamut case above returns
	// after a single reconstruction and never enters this loop.
	float sLo = 0.0, sHi = 1.0;
	for (int i = 0; i < 12; ++i) {
		const float s = 0.5 * (sLo + sHi);
		const float3 c = ictcp_to_linear(float3(ictcp.x, ictcp.y * s, ictcp.z * s));
		if (min(min(c.r, c.g), c.b) >= 0.0 && max(max(c.r, c.g), c.b) <= maxNits)
			sLo = s;
		else
			sHi = s;
	}
	return ictcp_to_linear(float3(ictcp.x, ictcp.y * sLo, ictcp.z * sLo));
}
