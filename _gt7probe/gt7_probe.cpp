// -----------------------------------------------------------------------------
// Phase 0 harness for adding a GT7 tone curve to MPCVR's PQ->SDR path.
//
// Purpose: settle WHICH quantity drives the curve's ceiling and which drives the
// input gain, before any HLSL exists. The design constraint that makes this a
// question at all is that GT7 is a ~2:1 display-referred limiter (identity below
// 0.444*ceiling, only 0.835*ceiling AT the ceiling, its own clamp starts binding at
// 2.05*ceiling) while PQ->SDR has to fit a 1000-4000 nit master into 25-400 nits.
//
// Two failure modes are measured, not assumed:
//   T1 scale invariance - relative to its own ceiling the curve is scale invariant
//      except for the absolute toe breakpoint, so a parameterisation that scales
//      everything together makes hdr.sdrNits PROVABLY inert.
//   T2 the headroom fit - main() ends with `if (mTop > 1) color /= mTop`, which
//      cancels any uniform scaling exactly. Reproduced here because a dial that is
//      live in the curve can still be dead on screen.
//
// Section 6 is the one that answers the actual question ("Hable drifts warm, will
// GT7 fix it?"): it models the SHIPPING Hable branch and compares both on the
// project's own metric, yellow-delta in 8-bit codes.
//
// Every GT7 number comes from Polyphony's own code, not a re-transcription: the
// reference is included verbatim with its main() renamed, so initializeParameters
// and applyToneMapping are theirs. Build:
//   cl /EHsc /O2 /std:c++17 /I C:\htpc gt7_probe.cpp /Fe:gt7_probe.exe
// -----------------------------------------------------------------------------

#define main gt7_reference_main
#include "gt7_tone_mapping.cpp"
#undef main

#include <cstdio>
#include <cmath>
#include <algorithm>
#include <vector>

static const double kPi = 3.14159265358979;

// One candidate mapping. All quantities in PHYSICAL NITS unless noted.
//   ceiling     -> GT7's physicalTargetLuminance (peakIntensity_, its own clamp)
//   gain        -> input scale: x = N * gain / 100, source nits to frame-buffer units
//   divisor     -> what maps to code 1.0: out_rel = out_fb * 100 / divisor
//   pre         -> optional pre-compressor applied to the curve input (the shader's
//                  existing soft-plus), or nullptr
struct Candidate {
    const char* name;
    const char* note;
    bool        verbatimSdr;
    bool        autoWhite;   // solve `gain` so content white lands on kWhiteTargetCode
    double    (*ceiling)(double P, double W, double S);
    double    (*gain)(double P, double W, double S);
    double    (*divisor)(double P, double W, double S);
    double    (*pre)(double x, double P, double W, double S);
};

// The soft-plus pre-compressor already in the Hable branch
// (ps_convert_pq_to_sdr.hlsl ~line 355), with the shader's shipped defaults
// softKnee 0.60 / ratio 2.48 / width 0.42 expressed against the curve's ceiling.
// Overflow-safe softplus: sp(t) = max(t,0) + log(1 + exp(-|t|)).
static double SoftPlus(double x, double ceilFb, double kneeFrac, double ratio, double widthFrac)
{
    const double knee  = kneeFrac * ceilFb;
    const double width = std::max(widthFrac * ceilFb, 1e-3);
    const double t  = (x - knee) / width;
    const double sp = std::max(t, 0.0) + std::log(1.0 + std::exp(-std::fabs(t)));
    const double comp = knee + width * sp / std::max(ratio, 1e-3);
    return std::min(x, comp);
}
static double preSoft(double x, double P, double, double)
{
    return SoftPlus(x, P / 100.0, 0.60, 2.48, 0.42);
}

// --- the GT7 operator, driven with a chosen ceiling / gain / divisor -------------
static double RunGain(const Candidate& c, double N, double P, double W, double S, double gain)
{
    GT7ToneMapping tm;
    double divisor;
    if (c.verbatimSdr) {
        tm.initializeAsSDR();               // ceiling 250 paper white, sdrCorrection 0.4
        divisor = 1.0;                      // their output is already [0,1]
    } else {
        tm.initializeParameters((float)c.ceiling(P, W, S));
        tm.sdrCorrectionFactor_ = 1.0f;     // we do our own display mapping
        divisor = c.divisor(P, W, S);
    }

    double x = N * gain / 100.0;            // source nits -> frame-buffer units
    if (c.pre) x = c.pre(x, P, W, S);
    const float in[3] = { (float)x, (float)x, (float)x };
    float out[3];
    tm.applyToneMapping(in, out);
    return c.verbatimSdr ? out[0] : (out[0] * 100.0 / divisor);
}

// The reference white target, in CODE space (after the fit and gamma). 0.70 is what
// a diffuse-white patch should read as on an SDR panel; madVR lands near there.
static const double kWhiteTargetCode = 0.70;

struct Curve {
    std::vector<double> nits;
    std::vector<double> code;
    double              mTop;
    double              clipFrac;
    double              gainUsed;
};

static const int kGrid = 257;

static double GridNits(int i, double S) { return S * std::pow((double)i / (kGrid - 1), 2.0); }

static Curve MakeCurveGain(const Candidate& c, double P, double W, double S, bool applyFit, double gain)
{
    Curve cu;
    std::vector<double> rel(kGrid);
    double top = 0.0;
    for (int i = 0; i < kGrid; ++i) {
        rel[i] = RunGain(c, GridNits(i, S), P, W, S, gain);
        top = std::max(top, rel[i]);
    }
    int clipped = 0;
    for (int i = 0; i < kGrid; ++i) {
        double v = rel[i];
        if (applyFit && top > 1.0) v /= top;
        // AT-or-over, not strictly over. The operator's own min(blended, target)
        // lands the output on exactly 1.0, so a `> 1.0` test reports 0.0% for a
        // mapping that is saturated across most of its top end — measured, and it
        // made the official form look clean at sdrNits 250 when it is not.
        if (rel[i] >= 1.0 - 1e-6) ++clipped;
        cu.nits.push_back(GridNits(i, S));
        cu.code.push_back(std::pow(std::min(std::max(v, 0.0), 1.0), 1.0 / 2.2));
    }
    cu.mTop = top;
    cu.clipFrac = (double)clipped / kGrid;
    cu.gainUsed = gain;
    return cu;
}

static double CodeAt(const Curve& cu, double nits)
{
    if (nits <= cu.nits.front()) return cu.code.front();
    if (nits >= cu.nits.back())  return cu.code.back();
    for (size_t i = 1; i < cu.nits.size(); ++i) {
        if (cu.nits[i] >= nits) {
            const double t = (nits - cu.nits[i - 1]) / (cu.nits[i] - cu.nits[i - 1] + 1e-12);
            return cu.code[i - 1] + t * (cu.code[i] - cu.code[i - 1]);
        }
    }
    return cu.code.back();
}

// Solve the input gain so that content at refWhite reads as kWhiteTargetCode.
// This is the degree of freedom the fixed-gain candidates cannot honour: with a
// ~2:1 curve, "where white lands" and "where the peak lands" cannot both be pinned
// by hand, so pin white and let the peak fall where the shoulder puts it.
static double SolveGainForWhite(const Candidate& c, double P, double W, double S)
{
    // Memoised: the liveness sweeps ask for the same (candidate, P, W, S) dozens of
    // times and each solve is 44 curve evaluations.
    struct Ent { const Candidate* c; double P, W, S, g; };
    static std::vector<Ent> cache;
    for (const auto& e : cache)
        if (e.c == &c && e.P == P && e.W == W && e.S == S) return e.g;

    double lo = 1e-4, hi = 20.0;
    for (int it = 0; it < 44; ++it) {
        const double mid = std::sqrt(lo * hi);
        const Curve cu = MakeCurveGain(c, P, W, S, true, mid);
        if (CodeAt(cu, W) > kWhiteTargetCode) hi = mid; else lo = mid;
    }
    const double g = std::sqrt(lo * hi);
    cache.push_back({ &c, P, W, S, g });
    return g;
}

static Curve MakeCurve(const Candidate& c, double P, double W, double S, bool applyFit)
{
    double g;
    if (c.verbatimSdr)   g = 1.0;
    else if (c.autoWhite) g = SolveGainForWhite(c, P, W, S);
    else                  g = c.gain(P, W, S);
    return MakeCurveGain(c, P, W, S, applyFit, g);
}

// ---- the candidate family -----------------------------------------------------
static double ceilP(double P, double, double) { return P; }
static double ceilW(double, double W, double)  { return W; }
static double gNone(double, double, double)    { return 1.0; }
static double gPeakToCeiling(double P, double, double S) { return P / S; }
static double gWhiteToKnee(double P, double W, double)   { return 0.444 * P / W; }
static double gClampAtPeak(double, double W, double S)   { return 2.046 * W / S; }
static double dP(double P, double, double)               { return P; }

// A gain that does NOT contain sdrNits. Every candidate above either puts P in the
// gain (H3/H7: white pinned, so the mid band cannot respond to P at all) or omits W
// (H5: white lands at 0.965 and 48% of the range clips). This solves the constant
// ONCE at a nominal configuration so white reads kWhiteTargetCode there, then
// freezes it: gain = c * W / S. Because the divisor is still P, a dimmer target now
// makes the picture brighter (the madVR direction) instead of leaving the mids pinned.
static double SolveFrozenC(double (*preFn)(double, double, double, double))
{
    static std::vector<std::pair<void*, double>> cache;
    for (const auto& e : cache) if (e.first == (void*)preFn) return e.second;

    Candidate tmp{ "solve", "", false, false, ceilP, nullptr, dP, preFn };
    double lo = 1e-3, hi = 20.0;
    for (int it = 0; it < 44; ++it) {
        const double mid = std::sqrt(lo * hi);
        const Curve cu = MakeCurveGain(tmp, 85.0, 225.0, 1000.0, true, mid * 225.0 / 1000.0);
        if (CodeAt(cu, 225.0) > kWhiteTargetCode) hi = mid; else lo = mid;
    }
    const double c = std::sqrt(lo * hi);
    cache.push_back({ (void*)preFn, c });
    return c;
}
static double gFrozen(double, double W, double S)     { return SolveFrozenC(nullptr) * W / S; }
static double gFrozenSoft(double, double W, double S) { return SolveFrozenC(preSoft) * W / S; }

static Candidate kCandidates[] = {
    { "V  verbatim",    "initializeAsSDR(): ceiling 250, output already 0..1",
      true,  false, nullptr, nullptr, nullptr, nullptr },
    { "H1 physical",    "ceiling=display peak, no input gain, /sdrNits",
      false, false, ceilP, gNone, dP, nullptr },
    { "H2 peak-fit",    "ceiling=display peak, content peak lands ON the ceiling",
      false, false, ceilP, gPeakToCeiling, dP, nullptr },
    { "H3 white-knee",  "ceiling=display peak, content white lands ON the knee",
      false, false, ceilP, gWhiteToKnee, dP, nullptr },
    { "H4 refWhite-ceil", "ceiling=refWhite (GT-faithful paper white), physical in, /sdrNits",
      false, false, ceilW, gNone, dP, nullptr },
    { "H5 double-anchor", "ceiling=display peak, gain from refWhite AND scenePeak (P-free)",
      false, false, ceilP, gClampAtPeak, dP, nullptr },
    { "H6 knee+softplus", "H3 plus the shader's existing soft-plus pre-compressor",
      false, false, ceilP, gWhiteToKnee, dP, preSoft },
    { "H7 auto-white",  "ceiling=display peak, gain SOLVED so white reads 0.70 code",
      false, true,  ceilP, nullptr, dP, nullptr },
    { "H8 auto+softplus", "H7 plus the soft-plus pre-compressor",
      false, true,  ceilP, nullptr, dP, preSoft },
    { "H9 P-free white",  "ceiling=display peak, gain = c*W/S with c frozen (P-free)",
      false, false, ceilP, gFrozen, dP, nullptr },
    { "H10 Pfree+softplus", "H9 plus the soft-plus pre-compressor",
      false, false, ceilP, gFrozenSoft, dP, preSoft },
};

// ---- the shipping Hable branch, modelled for A/B --------------------------------
// Faithful to ps_convert_pq_to_sdr.hlsl lines ~296-466 as it ships TODAY, i.e. with
// the user's live settings: hlN = 0 (the INI has it pinned there) and chromaExp =
// 1.01 (effectively off, 1.0 = none). Chroma is a uniform linear gain on the whole
// colour, which cannot rotate hue; that is the behaviour under test.
static double hable1(double x)
{
    const double A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

struct RGBd { double r, g, b; };   // not `RGB`: the reference owns that name (its line 441)

static RGBd HableChain(RGBd nitsIn, double P, double W, double S)
{
    const float fb[3] = { (float)(nitsIn.r / 100.0), (float)(nitsIn.g / 100.0), (float)(nitsIn.b / 100.0) };
    float ic[3];
    rgbToICtCp(fb, ic);                                  // reference's own ICtCp
    const double I_nits = eotfSt2084(ic[0]) * 100.0;     // PQ-decoded intensity, nits
    const double I_rel  = I_nits / std::max(W, 1e-3);
    const double peakSdrUnits = std::max(S / std::max(W, 1e-3), 1.0);
    const double hMapped = hable1(I_rel) / std::max(hable1(peakSdrUnits), 1e-4);
    const double gGain = (hMapped * W) / std::max(I_nits, 1e-6);

    float rgbFb[3];
    iCtCpToRgb(ic, rgbFb);                               // original colour, back to fb
    RGBd out{ rgbFb[0] * 100.0 * gGain, rgbFb[1] * 100.0 * gGain, rgbFb[2] * 100.0 * gGain };
    // display mapping: /sdrNits (the uncommitted a5c3e1e fix), then the shared tail
    out.r /= P; out.g /= P; out.b /= P;
    return out;
}

// The same band/clip bookkeeping the GT7 candidates get, for a grey ramp through
// the modelled Hable chain, so the two are compared on identical axes.
static Curve MakeHableCurve(double P, double W, double S)
{
    Curve cu;
    std::vector<double> rel(kGrid);
    double top = 0.0;
    for (int i = 0; i < kGrid; ++i) {
        const double n = GridNits(i, S);
        rel[i] = HableChain({ n, n, n }, P, W, S).r;
        top = std::max(top, rel[i]);
    }
    int clipped = 0;
    for (int i = 0; i < kGrid; ++i) {
        double v = rel[i];
        if (top > 1.0) v /= top;
        if (rel[i] >= 1.0 - 1e-6) ++clipped;   // at-or-over, see MakeCurveGain
        cu.nits.push_back(GridNits(i, S));
        cu.code.push_back(std::pow(std::min(std::max(v, 0.0), 1.0), 1.0 / 2.2));
    }
    cu.mTop = top;
    cu.clipFrac = (double)clipped / kGrid;
    cu.gainUsed = 0.0;
    return cu;
}

static RGBd GT7Chain(const Candidate& c, RGBd nitsIn, double P, double W, double S, double gain)
{
    GT7ToneMapping tm;
    double divisor;
    if (c.verbatimSdr) { tm.initializeAsSDR(); divisor = 1.0; }
    else {
        tm.initializeParameters((float)c.ceiling(P, W, S));
        tm.sdrCorrectionFactor_ = 1.0f;
        divisor = c.divisor(P, W, S);
    }
    double xr = nitsIn.r * gain / 100.0, xg = nitsIn.g * gain / 100.0, xb = nitsIn.b * gain / 100.0;
    if (c.pre) { xr = c.pre(xr, P, W, S); xg = c.pre(xg, P, W, S); xb = c.pre(xb, P, W, S); }
    const float v[3] = { (float)xr, (float)xg, (float)xb };
    float o[3];
    tm.applyToneMapping(v, o);
    const double k = c.verbatimSdr ? 1.0 : 100.0 / divisor;
    return { o[0] * k, o[1] * k, o[2] * k };
}

// The PQ code of an absolute luminance, using the reference's own encoder (it takes
// frame-buffer units, 1.0 == 100 nits). For a GREY the ICtCp I component is exactly
// this value: every row of rgb2lms sums to 1, so lms == (c,c,c) and the I row is
// (2048+2048)/4096 == 1. That is why the fade window below can be computed without
// a colour transform.
static double PqCode(double nits) { return inverseEotfSt2084((float)(nits / 100.0)); }

static double YellowDelta(RGBd rel){
    // The project's own metric (see the hlN comment in ps_convert_pq_to_sdr.hlsl:
    // "madVR highlight yellow-delta = +20.6"). 8-bit codes, after the headroom fit
    // and gamma, (R+G)/2 - B. Positive = warm.
    double m = std::max(rel.r, std::max(rel.g, rel.b));
    if (m > 1.0) { rel.r /= m; rel.g /= m; rel.b /= m; }      // the /mTop fit
    const double R = std::pow(std::min(std::max(rel.r, 0.0), 1.0), 1.0 / 2.2) * 255.0;
    const double G = std::pow(std::min(std::max(rel.g, 0.0), 1.0), 1.0 / 2.2) * 255.0;
    const double B = std::pow(std::min(std::max(rel.b, 0.0), 1.0), 1.0 / 2.2) * 255.0;
    return (R + G) / 2.0 - B;
}

// -----------------------------------------------------------------------------
// Fidelity: a line-by-line mirror of Shaders/convert/gt7_tonemap.hlsl, in the
// ABSOLUTE-NITS domain the shader uses, checked against Polyphony's own
// applyToneMapping in its frame-buffer domain. This is the only way to test the
// port's algebra without a GPU harness — the picture cannot be diffed on a live
// signal and the shader cannot be executed here.
//
// What it actually pins down (the reference's own colour functions are reused, so
// the matrices are NOT under test; the port claims are):
//   1. kA/kB/kC derived for a nits-domain peak instead of a frame-buffer peak,
//   2. the toe/identity/shoulder branch boundaries translated to nits correctly,
//   3. the targetI shortcut — for a grey, ICtCp's I is just the PQ code, which is
//      the claim that every row of rgb2lms sums to 1,
//   4. the blend-then-clamp order and the chroma ratio.
// -----------------------------------------------------------------------------
static double SmoothstepD(double a, double b, double x)
{
    const double t = std::min(std::max((x - a) / (b - a), 0.0), 1.0);
    return t * t * (3.0 - 2.0 * t);
}

static double GtCurveNits(double x, double kA, double kB, double kC, double knee, double gp)
{
    if (x <= 0.0) return 0.0;
    const double shoulder = kA + kB * std::exp(x * kC);
    if (x < knee) {
        const double w   = SmoothstepD(0.0, gp, x);
        const double toe = gp * std::pow(x / gp, 1.280);
        return (1.0 - w) * toe + w * x;
    }
    return shoulder;
}

static RGBd GtPortNits(RGBd nits, double ceilingNits, double gpNits,
                       double blend, double fadeA, double fadeB, double* outTargetErr)
{
    const double k   = (0.444 - 1.0) / (0.25 - 1.0);
    const double kA  = ceilingNits * 0.444 + ceilingNits * k;
    const double kB  = -ceilingNits * k * std::exp(0.444 / k);
    const double kC  = -1.0 / (k * ceilingNits);
    const double knee = 0.444 * ceilingNits;

    const RGBd sk{ GtCurveNits(nits.r, kA, kB, kC, knee, gpNits),
                   GtCurveNits(nits.g, kA, kB, kC, knee, gpNits),
                   GtCurveNits(nits.b, kA, kB, kC, knee, gpNits) };

    // linear_to_ictcp(nits) == the reference's rgbToUcs(nits/100)
    const float fin[3]  = { (float)(nits.r / 100.0), (float)(nits.g / 100.0), (float)(nits.b / 100.0) };
    const float fsk[3]  = { (float)(sk.r   / 100.0), (float)(sk.g   / 100.0), (float)(sk.b   / 100.0) };
    float ic[3], sic[3];
    rgbToICtCp(fin, ic);
    rgbToICtCp(fsk, sic);

    // The shortcut under test: I of a grey == the PQ code of that grey.
    const float gf[3] = { (float)(ceilingNits / 100.0), (float)(ceilingNits / 100.0), (float)(ceilingNits / 100.0) };
    float ghost[3];
    rgbToICtCp(gf, ghost);
    if (outTargetErr) *outTargetErr = std::abs(ghost[0] - PqCode(ceilingNits));

    const double chromaScale = 1.0 - SmoothstepD(fadeA, fadeB, ic[0] / ghost[0]);

    const float scaledUcs[3] = { sic[0], ic[1] * (float)chromaScale, ic[2] * (float)chromaScale };
    float srgb[3];
    iCtCpToRgb(scaledUcs, srgb);

    // blend, then GT's own clamp — the order the HLSL uses.
    return { std::min((srgb[0] * 100.0 - sk.r) * blend + sk.r, ceilingNits),
             std::min((srgb[1] * 100.0 - sk.g) * blend + sk.g, ceilingNits),
             std::min((srgb[2] * 100.0 - sk.b) * blend + sk.b, ceilingNits) };
}

int main()
{
    // Unbuffered: stdout is redirected to gt7_probe.txt, which is fully buffered,
    // so an access violation would otherwise discard the entire report.
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("# GT7 parameterisation probe (v2)\n");
    printf("# operator constants: knee=0.444*ceiling  toe=53.8nits(abs)  C(ceiling)=0.835*ceiling"
           "  clamp binds at 2.046*ceiling\n");
    printf("# white target for the auto-gain candidates: %.2f code\n\n", kWhiteTargetCode);

    printf("## fidelity oracle (Polyphony's own SDR case, ceiling 250, input fb 0.5/1.23/0.75)\n");
    {
        GT7ToneMapping tm;
        tm.initializeAsSDR();
        const float in[3] = { 0.5f, 1.23f, 0.75f };
        float out[3];
        tm.applyToneMapping(in, out);
        printf("   out = %.4f %.4f %.4f   <- the HLSL port must reproduce these\n\n",
               out[0], out[1], out[2]);
    }

    // Port fidelity: the nits-domain mirror of gt7_tonemap.hlsl against theirs.
    printf("## port fidelity - Shaders/convert/gt7_tonemap.hlsl vs applyToneMapping\n");
    {
        double worst = 0.0, worstTargetI = 0.0;
        const double ceilings[] = { 25.0, 85.0, 203.0, 250.0, 400.0, 1000.0, 4000.0 };
        // fb-domain inputs: neutral, deep shadow, saturated warm, deep blue, clipped red
        const RGBd cols[] = { {1.0,1.0,1.0}, {0.05,0.05,0.05}, {4.0,3.2,1.1},
                              {0.3,0.9,2.5}, {8.0,0.2,0.1}, {0.0,0.4,0.0} };
        for (double cn : ceilings) {
            GT7ToneMapping tm;
            tm.initializeParameters((float)cn);
            tm.sdrCorrectionFactor_ = 1.0f;
            for (const RGBd& f : cols) {
                const float in[3] = { (float)f.r, (float)f.g, (float)f.b };
                float out[3];
                tm.applyToneMapping(in, out);
                double tErr = 0.0;
                const RGBd mine = GtPortNits({ f.r * 100.0, f.g * 100.0, f.b * 100.0 },
                                             cn, 53.8, tm.blendRatio_, 0.98, 1.16, &tErr);
                const RGBd  ref = { out[0] * 100.0, out[1] * 100.0, out[2] * 100.0 };
                worst = std::max(worst, std::max(std::abs(mine.r - ref.r),
                                   std::max(std::abs(mine.g - ref.g), std::abs(mine.b - ref.b))));
                worstTargetI = std::max(worstTargetI, tErr);
            }
        }
        printf("   worst channel error over 7 ceilings x 6 colours: %.3e nits\n", worst);
        printf("   targetI shortcut (I of a grey == its PQ code) error: %.3e\n", worstTargetI);
        printf("   %s\n\n", worst < 1e-3 && worstTargetI < 1e-6
             ? "PORT MATCHES - the nits-domain translation is exact"
             : "PORT DIVERGES - do not trust the shader output");
    }

    const double Ps[] = { 25.0, 50.0, 85.0, 200.0, 400.0 };
    const double Ws[] = { 100.0, 203.0, 225.0, 300.0 };
    const double Ss[] = { 300.0, 1000.0, 4000.0 };
    const double P0 = 85.0, W0 = 225.0, S0 = 1000.0;   // the user's live settings

    // ---- 1. shape + liveness ------------------------------------------------
    printf("## candidates at sdrNits=85 refWhite=225 scenePeak=1000\n");
    printf("   name               gain   whiteCd  peakCd   midCd   mTop  clip%%"
           "   dPfit  dPnofit   dW    dirOK\n");
    for (const auto& c : kCandidates) {
        const Curve fit = MakeCurve(c, P0, W0, S0, true);

        double dPf = 0.0, dPn = 0.0, dWf = 0.0;
        for (size_t i = 0; i < sizeof(Ps) / sizeof(*Ps); ++i)
            for (size_t j = i + 1; j < sizeof(Ps) / sizeof(*Ps); ++j) {
                const Curve a = MakeCurve(c, Ps[i], W0, S0, true);
                const Curve b = MakeCurve(c, Ps[j], W0, S0, true);
                const Curve an = MakeCurve(c, Ps[i], W0, S0, false);
                const Curve bn = MakeCurve(c, Ps[j], W0, S0, false);
                for (size_t q = 0; q < a.nits.size(); ++q) {
                    dPf = std::max(dPf, std::abs(a.code[q] - b.code[q]) * 255.0);
                    dPn = std::max(dPn, std::abs(an.code[q] - bn.code[q]) * 255.0);
                }
            }
        for (size_t i = 0; i < sizeof(Ws) / sizeof(*Ws); ++i)
            for (size_t j = i + 1; j < sizeof(Ws) / sizeof(*Ws); ++j) {
                const Curve a = MakeCurve(c, P0, Ws[i], S0, true);
                const Curve b = MakeCurve(c, P0, Ws[j], S0, true);
                for (size_t q = 0; q < a.nits.size(); ++q)
                    dWf = std::max(dWf, std::abs(a.code[q] - b.code[q]) * 255.0);
            }

        // direction, measured in the MID BAND only (0.05..0.5 of the content range).
        // Averaging the whole range is dominated by highlights and hides what the
        // operator actually does to a face or a wall.
        double meanLo = 0.0, meanHi = 0.0;
        int n = 0;
        const Curve cLo = MakeCurve(c, 25.0, W0, S0, true);
        const Curve cHi = MakeCurve(c, 400.0, W0, S0, true);
        for (size_t q = 0; q < cLo.nits.size(); ++q) {
            const double f = cLo.nits[q] / S0;
            if (f >= 0.05 && f <= 0.50) { meanLo += cLo.code[q]; meanHi += cHi.code[q]; ++n; }
        }
        meanLo /= std::max(n, 1); meanHi /= std::max(n, 1);
        const bool dirOK = meanLo > meanHi;   // lower sdrNits must be BRIGHTER

        printf("   %-16s %7.3f %8.3f %7.3f %7.3f %5.2f %5.1f %7.1f %8.1f %6.1f %7s\n",
               c.name, fit.gainUsed, CodeAt(fit, W0), CodeAt(fit, S0), CodeAt(fit, 0.18 * S0),
               fit.mTop, 100.0 * fit.clipFrac, dPf, dPn, dWf, dirOK ? "yes" : "NO");
    }
    printf("\n   dPfit/dPnofit = largest code change over sdrNits 25..400, with and without the\n"
           "                 /mTop fit. The gap IS the cancellation (T2).\n"
           "   dW            = largest code change over refWhite 100..300.\n"
           "   <= ~2 codes   = an inert dial on screen.\n\n");

    // ---- 2. clipping across the peak range ----------------------------------
    printf("## clipped fraction of the source range (fit on)\n");
    printf("   name               S=300    S=1000    S=4000\n");
    for (const auto& c : kCandidates) {
        printf("   %-16s", c.name);
        for (double S : Ss) printf(" %7.1f%%", 100.0 * MakeCurve(c, P0, W0, S, true).clipFrac);
        printf("\n");
    }

    // ---- 3. what does sdrNits actually MOVE? --------------------------------
    // A single "direction" boolean is not enough: the whole point of the dial is
    // WHERE on the curve the content lands. Print code values by band so the shape
    // change is visible, with the shipping Hable chain alongside for reference.
    printf("\n## code value by band, as sdrNits moves (refWhite=225 scenePeak=1000)\n");
    printf("   band = fraction of the content peak. Hable row is the shipping branch\n");
    printf("   modelled with the live settings (hlN=0, chromaExp=1.01).\n");
    {
        const double bands[] = { 0.05, 0.18, 0.40, 0.70, 1.00 };
        for (double P : { 25.0, 85.0, 400.0 }) {
            printf("   --- sdrNits = %.0f ---\n", P);
            const Curve hb = MakeHableCurve(P, W0, S0);
            printf("     %-18s", "Hable(shipping)");
            for (double b : bands) printf(" %7.3f", CodeAt(hb, b * S0));
            printf("   clip %4.1f%%\n", 100.0 * hb.clipFrac);
            for (const auto& c : kCandidates) {
                if (c.verbatimSdr) continue;
                const Curve cu = MakeCurve(c, P, W0, S0, true);
                printf("     %-18s", c.name);
                for (double b : bands) printf(" %7.3f", CodeAt(cu, b * S0));
                printf("   clip %4.1f%%\n", 100.0 * cu.clipFrac);
            }
        }
        printf("   madVR semantics want the LOWER target to sit HIGHER in the mid bands\n");
        printf("   (0.18/0.40) and to compress the top band more.\n");
    }

    // ---- 4. warmth: GT7 vs the shipping Hable, on the project's own metric ----
    printf("\n## WARMTH - the question that started this. yellow-delta in 8-bit codes,\n");
    printf("## positive = warm. Source is the measured Everest head (223,221,176) at the\n");
    printf("## stated level; sdrNits=85 refWhite=225 scenePeak=1000.\n");
    printf("   level    Hable(shipping)   then each GT7 candidate\n");
    for (double level : { 100.0, 150.0, 225.0, 400.0, 700.0, 1000.0 }) {
        const double k = level / 221.0;
        const RGBd src{ 223.0 * k, 221.0 * k, 176.0 * k };
        printf("   %5.0f nits   Hable %+7.2f", level, YellowDelta(HableChain(src, P0, W0, S0)));
        for (const auto& c : kCandidates) {
            double g;
            if (c.verbatimSdr)    g = 1.0;
            else if (c.autoWhite) g = SolveGainForWhite(c, P0, W0, S0);
            else                  g = c.gain(P0, W0, S0);
            printf("   %s %+6.2f", c.name, YellowDelta(GT7Chain(c, src, P0, W0, S0, g)));
        }
        printf("\n");
    }
    printf("\n   A NEGATIVE shift versus Hable = less warm = the improvement being sought.\n");
    printf("   Note GT7's chroma fade acts on I/targetI in PQ CODES, so it engages far\n");
    printf("   lower in nits than the 0.98/1.16 window suggests.\n");

    // ---- 5. where the chroma fade actually engages --------------------------
    printf("\n## chroma fade window, in SOURCE NITS (ceiling = refWhite = 225)\n");
    {
        const double targetI = PqCode(225.0);
        for (double frac : { 0.5, 0.9, 0.98, 1.0, 1.16, 1.4 }) {
            // invert PQ to find the nits whose I equals frac * targetI
            double lo = 0.0, hi = 10000.0;
            for (int it = 0; it < 60; ++it) {
                const double mid = 0.5 * (lo + hi);
                if (PqCode(mid) < frac * targetI) lo = mid; else hi = mid;
            }
            printf("   I/targetI = %.2f  ->  %7.1f nits\n", frac, 0.5 * (lo + hi));
        }
    }

    // ---- 6. Hable vs GT7, as the SHADER actually runs it ---------------------
    // Reported: GT7 reads too bright, and the user's Hable habit is refWhite 250.
    // Two causes with opposite fixes, so separate them by measurement:
    //   (a) CALIBRATION DRIFT. kGT7RangeFit is FROZEN at 0.764, solved for
    //       refWhite=225 / sdrNits=85 / peak=1000. Worse, refWhite sits in the
    //       NUMERATOR of the gain (gain = c*W/S), so raising it pushes content UP
    //       the curve — while for Hable refWhite is the divisor axis, so raising it
    //       pushes content DOWN. Same slider, opposite sense.
    //   (b) INHERENT SHAPE. GT7 passes everything below 0.444*ceiling through as
    //       IDENTITY; a filmic curve like Hable compresses throughout. Matching the
    //       white point then leaves GT7's midtones high, which no constant fixes.
    printf("\n## Hable vs GT7 as the shader runs it (frozen fit)  sdrNits=85 scenePeak=1000\n");
    {
        const double P = 85.0, S = 1000.0;
        // kCandidates[10] is "H10 Pfree+softplus" — gain = SolveFrozenC()*W/S with
        // the soft-plus pre-compressor, i.e. exactly what ships in the shader.
        const Candidate& gt7 = kCandidates[10];
        auto MeanBand = [](const Curve& cu, double lo, double hi) {
            double m = 0.0; int n = 0;
            for (size_t i = 0; i < cu.code.size(); ++i)
                if (cu.nits[i] >= lo && cu.nits[i] <= hi) { m += cu.code[i]; ++n; }
            return n ? m / n : 0.0;
        };
        printf("   refWhite    code@W        code@180nits    code@400nits     mean50-400\n");
        printf("              Hable   GT7    Hable   GT7    Hable   GT7    Hable    GT7\n");
        for (double W : { 203.0, 225.0, 250.0, 280.0, 300.0 }) {
            const Curve hb = MakeHableCurve(P, W, S);
            const Curve gt = MakeCurve(gt7, P, W, S, true);
            printf("   %6.0f     %6.3f %6.3f   %6.3f %6.3f   %6.3f %6.3f   %6.3f %6.3f  %s\n",
                   W, CodeAt(hb, W), CodeAt(gt, W),
                   CodeAt(hb, 180.0), CodeAt(gt, 180.0),
                   CodeAt(hb, 400.0), CodeAt(gt, 400.0),
                   MeanBand(hb, 50.0, 400.0), MeanBand(gt, 50.0, 400.0),
                   MeanBand(gt, 50.0, 400.0) > MeanBand(hb, 50.0, 400.0) ? "GT7 BRIGHTER" : "gt7 darker");
        }
        printf("\n   If GT7's code@W CLIMBS as refWhite rises while Hable's falls, the slider\n"
               "   has OPPOSITE SENSE on the two curves — not merely a constant offset.\n");
    }

    // ---- 7. THE SHIPPED FORM: the official operator across the target dial ----
    // This is what the converter now runs: ceiling = sdrNits, no input gain, no
    // pre-fit, grayPoint absolute at 53.8 nits. Identical to kCandidates[1]
    // ("H1 physical"), which is why the probe can predict the build exactly.
    printf("\n## official GT7 as shipped - ceiling = sdrNits, no gain, no pre-fit\n");
    {
        const Candidate& off = kCandidates[1];
        const double S = 1000.0;
        const double bands[] = { 0.05, 0.18, 0.40, 0.70, 1.00 };
        printf("   scenePeak=1000\n   sdrNits ");
        for (double b : bands) printf("  %4.0fnit", b * S);
        printf("     clip    code@250   mean50-400\n");
        for (double P : { 50.0, 85.0, 150.0, 250.0, 400.0 }) {
            const Curve cu = MakeCurve(off, P, 225.0, S, true);
            printf("   %6.0f ", P);
            for (double b : bands) printf("  %7.3f", CodeAt(cu, b * S));
            double m = 0.0; int n = 0;
            for (size_t i = 0; i < cu.code.size(); ++i)
                if (cu.nits[i] >= 50.0 && cu.nits[i] <= 400.0) { m += cu.code[i]; ++n; }
            printf("   %5.1f%%    %6.3f      %6.3f\n",
                   100.0 * cu.clipFrac, CodeAt(cu, 250.0), n ? m / n : 0.0);
        }
        printf("\n   initializeAsHDR documents its valid range as 250..10000 nits, because the\n"
               "   curve constants assume a 250-nit paper white. Below that the ~2:1 shoulder\n"
               "   cannot span target..peak, so the top of the range flattens. That is the\n"
               "   operator working exactly as written, not a fault in the port.\n");
    }

    // ---- 8. CSV --------------------------------------------------------------
    {
        FILE* f = nullptr;
        if (0 == fopen_s(&f, "gt7_probe.csv", "w")) {
            fprintf(f, "candidate,sdrNits,refWhite,scenePeak,nits,code_fitted,code_raw\n");
            for (const auto& c : kCandidates) {
                const Curve a = MakeCurve(c, P0, W0, S0, true);
                const Curve b = MakeCurve(c, P0, W0, S0, false);
                for (size_t i = 0; i < a.nits.size(); ++i)
                    fprintf(f, "%s,%.0f,%.0f,%.0f,%.4f,%.6f,%.6f\n",
                            c.name, P0, W0, S0, a.nits[i], a.code[i], b.code[i]);
            }
            fclose(f);
            printf("\n wrote gt7_probe.csv\n");
        }
    }
    return 0;
}
