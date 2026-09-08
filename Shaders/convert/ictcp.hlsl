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
