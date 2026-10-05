// -----------------------------------------------------------------------------
// curve_solve.cpp — step 1b: solve ONE monotone curve per frame against the house
// targets, and measure whether it stays still while doing it.
//
// The question this answers, and nothing else:
//
//     Given a damped mean target, a highlight floor and a clip ceiling, does a free
//     1-D curve hit them on real film frames — and how far does it MOVE between
//     frames?
//
// The second half is the whole bet. "Per frame" is only safe if the curve barely
// moves within a scene; if it lurches, a 1-D luminance function is the wrong
// instrument and the answer is a windowed operator. That is decided here, on
// numbers, before anything touches the renderer.
//
// HOUSE TARGETS (proposed 2026-10-05, derived from the accepted Everest frames and
// madVR's reference grab — see the discussion; they are inputs, not magic):
//
//   T1  mid-tone anchor   delivered mean code = 0.30 at a source average of 20 nits
//   T2  damping           exponent 0.16 on the source mean, so a 42:1 input swing
//                         becomes ~1.8:1 in delivered brightness. Damping is not a
//                         taste choice here: a rigid target is INFEASIBLE on dark
//                         scenes, the reach ceiling clamps, and once clamped the
//                         peak takes over brightness — which is exactly the pumping
//                         this project has been fighting.
//   T3  highlight floor   delivered p99.5 >= 0.85 of white
//   T4  clip ceiling      fraction of pixels delivered >= 0.98 must be <= 0.1%
//
// WHY A HISTOGRAM AND NOT THE PIXELS: the objective is evaluated thousands of times
// per solve. 512 log bins reproduce mean/percentiles/clip to well under a code while
// making each evaluation ~500 additions instead of ~500k.
//
// Build:
//   cl /EHsc /O2 /std:c++17 /I C:\htpc curve_solve.cpp /Fe:curve_solve.exe
// -----------------------------------------------------------------------------

#define main gt7_reference_main
#include "gt7_tone_mapping.cpp"
#undef main

#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

static const int kW = 960, kH = 540;
static const double kY_R = 0.2627, kY_G = 0.6780, kY_B = 0.0593;   // BT.2020 luma

// ---- house targets -----------------------------------------------------------
static const double kMeanAtRef   = 0.30;    // T1
static const double kRefSrcNits  = 20.0;
static const double kDamp        = 0.16;    // T2
static const double kP995Floor   = 0.85;    // T3
static const double kClipCeil    = 0.001;   // T4, fraction
static const double kClipCode    = 0.98;

// ---- histogram ---------------------------------------------------------------
static const int    kHBins = 512;
static const double kHNitsLo = 0.002, kHNitsHi = 6000.0;
static const double kLogLo = std::log(kHNitsLo), kLogHi = std::log(kHNitsHi);

struct Hist {
    double count[kHBins];
    double centre[kHBins];
    double total;
};

static int BinOf(double nits)
{
    double u = (std::log(std::max(nits, kHNitsLo)) - kLogLo) / (kLogHi - kLogLo);
    int b = (int)(u * kHBins);
    if (b < 0) b = 0;
    if (b >= kHBins) b = kHBins - 1;
    return b;
}

static bool LoadFrameHist(const char* path, Hist& h)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb")) return false;
    const size_t n = (size_t)kW * kH;
    std::vector<unsigned short> buf(n * 3);
    if (fread(buf.data(), 2, n * 3, f) != n * 3) { fclose(f); return false; }
    fclose(f);

    for (int i = 0; i < kHBins; ++i) {
        h.count[i] = 0.0;
        const double u = (i + 0.5) / kHBins;
        h.centre[i] = std::exp(kLogLo + u * (kLogHi - kLogLo));
    }
    h.total = 0.0;

    // Letterbox trim inline: rows whose maximum is below a hair of black are bars,
    // exactly as the renderer's content-rows crop and curve_probe's TrimBars do.
    std::vector<double> rowMax(kH, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const double r = eotfSt2084(buf[i * 3 + 0] / 65535.0) * 100.0;
        const double g = eotfSt2084(buf[i * 3 + 1] / 65535.0) * 100.0;
        const double b = eotfSt2084(buf[i * 3 + 2] / 65535.0) * 100.0;
        const double y = kY_R * r + kY_G * g + kY_B * b;
        const int row = (int)(i / kW);
        if (y > rowMax[row]) rowMax[row] = y;
    }
    int top = 0, bot = kH;
    while (top < bot && rowMax[top] < 0.005) ++top;
    while (bot > top && rowMax[bot - 1] < 0.005) --bot;

    for (int row = top; row < bot; ++row) {
        const size_t base = (size_t)row * kW;
        for (int c = 0; c < kW; ++c) {
            const size_t i = base + c;
            const double r = eotfSt2084(buf[i * 3 + 0] / 65535.0) * 100.0;
            const double g = eotfSt2084(buf[i * 3 + 1] / 65535.0) * 100.0;
            const double b = eotfSt2084(buf[i * 3 + 2] / 65535.0) * 100.0;
            const double y = kY_R * r + kY_G * g + kY_B * b;
            h.count[BinOf(y)] += 1.0;
            h.total += 1.0;
        }
    }
    return h.total > 1000.0;
}

// ---- the curve: uniform knots in log-nits, PCHIP (monotone cubic) --------------
static const int kKnots = 16;
static const double kKnotLo = 0.01, kKnotHi = 3000.0;

struct Curve { double y[kKnots]; };   // delivered CODE (0..1) at each knot

static double KnotNits(int i)
{
    const double u = (double)i / (kKnots - 1);
    return std::exp(std::log(kKnotLo) + u * (std::log(kKnotHi) - std::log(kKnotLo)));
}

// Fritsch-Carlson monotone cubic on a uniform log grid. Monotone input data
// (enforced by the projection in the solver) gives a monotone, C1 curve — which is
// what keeps 8-bit output free of banding at the knots.
static double CurveEval(const Curve& c, double nits)
{
    const double u = (std::log(std::max(nits, kKnotLo)) - std::log(kKnotLo))
                   / (std::log(kKnotHi) - std::log(kKnotLo)) * (kKnots - 1);
    if (u <= 0.0) return c.y[0];
    if (u >= (double)(kKnots - 1)) return c.y[kKnots - 1];
    const int i = (int)u;
    const double t = u - i;

    double m[kKnots];
    for (int k = 0; k < kKnots - 1; ++k) m[k] = c.y[k + 1] - c.y[k];   // uniform h=1
    m[kKnots - 1] = m[kKnots - 2];
    double d[kKnots];
    d[0] = m[0]; d[kKnots - 1] = m[kKnots - 2];
    for (int k = 1; k < kKnots - 1; ++k) {
        if (m[k - 1] * m[k] <= 0.0) d[k] = 0.0;
        else {
            const double w1 = 2.0 * m[k] + m[k - 1], w2 = m[k] + 2.0 * m[k - 1];
            d[k] = (w1 + w2) / (w1 / m[k - 1] + w2 / m[k]);
        }
    }
    const double h00 = (1 + 2 * t) * (1 - t) * (1 - t);
    const double h10 = t * (1 - t) * (1 - t);
    const double h01 = t * t * (3 - 2 * t);
    const double h11 = t * t * (t - 1);
    return h00 * c.y[i] + h10 * d[i] + h01 * c.y[i + 1] + h11 * d[i + 1];
}

// ---- delivered statistics from a histogram ------------------------------------
struct Delivered { double mean, p50, p995, clipFrac, maxCode; };

static Delivered Deliver(const Curve& c, const Hist& h)
{
    Delivered d; d.mean = 0; d.p50 = 0; d.p995 = 0; d.clipFrac = 0; d.maxCode = 0;
    double acc = 0.0, cum = 0.0;
    const double need50 = 0.50 * h.total, need995 = 0.995 * h.total;
    bool got50 = false, got995 = false;
    for (int i = 0; i < kHBins; ++i) {
        if (h.count[i] <= 0.0) continue;
        const double code = CurveEval(c, h.centre[i]);
        acc += code * h.count[i];
        if (code > d.maxCode) d.maxCode = code;
        if (code >= kClipCode) d.clipFrac += h.count[i];
        cum += h.count[i];
        if (!got50 && cum >= need50)  { d.p50 = code; got50 = true; }
        if (!got995 && cum >= need995){ d.p995 = code; got995 = true; }
    }
    d.mean = acc / h.total;
    d.clipFrac /= h.total;
    return d;
}

// ---- objective ----------------------------------------------------------------
static double Objective(const Curve& c, const Hist& h, double srcMean)
{
    const double tMean = kMeanAtRef * std::pow(std::max(srcMean, 0.05) / kRefSrcNits, kDamp);
    const Delivered d = Deliver(c, h);

    double L = 0.0;
    L += 1.0  * (d.mean - tMean) * (d.mean - tMean);
    const double p = kP995Floor - d.p995;   if (p > 0) L += 3.0 * p * p;
    const double q = d.clipFrac - kClipCeil; if (q > 0) L += 40.0 * q * q;
    const double r = d.maxCode - 0.995;      if (r > 0) L += 20.0 * r * r;

    // Smoothness: penalise second differences of the knot codes, so the solve cannot
    // buy a target with a wiggle. A wiggle is exactly what bands.
    for (int i = 1; i < kKnots - 1; ++i) {
        const double s = c.y[i + 1] - 2.0 * c.y[i] + c.y[i - 1];
        L += 0.02 * s * s;
    }
    return L;
}

static double SourceMean(const Hist& h)
{
    double acc = 0.0;
    for (int i = 0; i < kHBins; ++i) acc += h.count[i] * h.centre[i];
    return acc / h.total;
}

// ---- the solve: projected coordinate descent ----------------------------------
// Monotonicity is enforced by projection (sort + clamp), not by the parameterisation,
// because a softplus parameterisation makes the objective surface harder to walk and
// this is offline — robustness beats elegance here.
static Curve Solve(const Hist& h)
{
    const double srcMean = SourceMean(h);

    // Warm start: a plain power law that lands the mean target. Deliberately naive —
    // if the solve cannot improve on this, that is itself the finding.
    Curve c;
    for (int i = 0; i < kKnots; ++i) {
        const double n = KnotNits(i);
        c.y[i] = std::min(0.995, std::pow(n / 120.0, 0.42));
    }

    const double steps[] = { 0.08, 0.04, 0.02, 0.01, 0.005, 0.002 };
    for (int sweep = 0; sweep < 6; ++sweep) {
        const double st = steps[sweep];
        for (int i = 0; i < kKnots; ++i) {
            const double orig = c.y[i];
            double best = Objective(c, h, srcMean);
            double bestV = orig;
            for (int s = -3; s <= 3; ++s) {
                if (s == 0) continue;
                c.y[i] = std::min(1.0, std::max(0.0, orig + s * st));
                const double L = Objective(c, h, srcMean);
                if (L < best) { best = L; bestV = c.y[i]; }
            }
            c.y[i] = bestV;
            // projection: keep the curve monotone
            if (i > 0 && c.y[i] < c.y[i - 1]) c.y[i] = c.y[i - 1];
            if (i < kKnots - 1 && c.y[i] > c.y[i + 1]) c.y[i + 1] = c.y[i];
        }
    }
    return c;
}

// ============================ THE ANALYTIC FORM ================================
//
// WHY THE KNOT SOLVE IS THE WRONG PARAMETERISATION
//
// Sixteen free knots against three binding conditions (mean target, p99.5 floor, clip
// ceiling) leaves thirteen directions in which the objective is flat. The descent
// therefore lands wherever it starts — which is precisely the 4-code wobble measured
// between two frames whose source means differed by 0.01 nits. That was never a bug in
// the descent; it was freedom in the model, and at runtime that freedom IS a pump.
//
// So shrink the model until the conditions determine it EXACTLY. Three parameters,
// three conditions, and a closed form for two of them:
//
//     code(x) = 1 - exp( -k * (x/x0)^g )
//
// Monotone, smooth, and asymptotic to 1.0 — it CANNOT clip, so the clip constraint
// disappears rather than being enforced. Imposing the two placement conditions:
//
//     y(p50)  = M50   ->  k*u = -ln(1-M50)     where u = (p50/x0)^g
//     y(p995) = M995  ->  k*v = -ln(1-M995)    where v = (p995/x0)^g
//
// dividing:  v/u = (p995/p50)^g  =  ln(1/(1-M995)) / ln(1/(1-M50))
//
//     g = ln( 8.391 ) / ln( p99.5 / p50 )          for M50=0.24, M995=0.90
//
// The scene's own dynamic range therefore sets its contrast — a narrow-range interior
// gets g near 1 and a 200:1 night-with-a-lamp gets a low g that opens the shadows. That
// is not a tuned behaviour, it falls out of the two placement conditions.
//
// One parameter (u, equivalently the x0/k pair taken together) is left free and is
// fixed by the mean target with a bisection. Deterministic, no local minima, and every
// quantity it consumes — p50, p99.5, mean — is already measured and already scene-locked
// by the exposure memory.
// ======================================================================================

static const double kM50  = 0.24;   // delivered mid code: madVR's accepted Everest grab
static const double kM995 = 0.90;   // delivered p99.5: leaves headroom to ~0.97 at max
static const double kMeanYield = 0.10;  // max the mean may rise above target when the
                                        // highlight floor overrides it (house option A)

struct AParam { double k, x0, g; };

// defined below; APSolve's stage 2 needs it, so it is declared here
static void ApDeliver(const AParam& a, const Hist& h, Delivered& d);

static inline double APEval(const AParam& a, double nits)
{
    if (nits <= 0.0) return 0.0;
    return 1.0 - std::exp(-a.k * std::pow(nits / a.x0, a.g));
}

// WELL-POSED FORM — one unknown, one equation, monotone.
//
// The first version pinned BOTH y(p50) and y(p99.5) and then tried to fit the mean with
// the leftover parameter. It cannot: those two conditions already determine the curve
// completely, because
//
//     k*(x/x0)^g  ==  TA*(x/p50)^g        for every x0 and k that satisfy both
//
// — x0 and k are redundant against the pair of anchors. The bisection therefore drove
// them to 2.7e4 and 3e12 while the curve never changed, and the mean target missed by up
// to 0.08. A degenerate parameterisation cannot be fixed by a better search; the model
// has to shed the redundancy.
//
// So: the two quantities that actually matter are BRIGHTNESS (the mean target) and
// HIGHLIGHT PRESERVATION (the p99.5 floor), and those are the constraints. The scene's
// dynamic range sets the SHAPE — g, from the same log-log law as before — and is not a
// constraint, so it cannot starve the solve of freedom. One unknown (the scale C) and
// one monotone equation (the mean) then determines everything else, and the delivered
// p99.5 is REPORTED rather than forced: if the floor still holds under a shape rule
// fitted from real frames, that is a finding; if it breaks, that is also the finding.
static AParam APSolve(const Hist& h, double srcMean, double p50, double p995)
{
    AParam a;
    p50 = std::max(p50, 0.02); p995 = std::max(p995, p50 * 1.2);

    // shape from the scene's own range: the contrast that carries p50's separation up to
    // p99.5 without crushing it. Narrow-range interiors land near g=1, a 200:1 night
    // interior with a lamp lands near 0.4 and opens the shadows.
    a.g = std::log(std::log(1.0 / (1.0 - kM995)) / std::log(1.0 / (1.0 - kM50)))
        / std::log(p995 / p50);
    a.x0 = p995;                                    // anchor the shape at the highlight

    const double tMean = kMeanAtRef * std::pow(std::max(srcMean, 0.05) / kRefSrcNits, kDamp);
    double lo = 1e-6, hi = 1e6;                     // monotone in C: bigger C -> brighter
    for (int it = 0; it < 60; ++it) {
        const double mid = std::sqrt(lo * hi);
        a.k = mid;
        double acc = 0.0;
        for (int i = 0; i < kHBins; ++i)
            if (h.count[i] > 0.0) acc += h.count[i] * APEval(a, h.centre[i]);
        if (acc / h.total > tMean) hi = mid; else lo = mid;
    }
    a.k = std::sqrt(lo * hi);

    // STAGE 2 — HIGHLIGHTS WIN (house decision, 2026-10-05, option A).
    //
    // On ~2 of 12 real frames the mean target and the p99.5 floor are jointly
    // infeasible: a scene with a 2.6-nit mean and a 226-nit lamp cannot put its average
    // at 0.216 of white AND keep its highlights at 0.85 — that asks for more
    // shadow-to-highlight separation than the display has. Something must yield, and the
    // house answer is that the highlights do not. So when stage 1's solution violates the
    // floor, k is raised (brightening) until the floor holds, and the mean lands above
    // target. The yield is reported, never hidden.
    //
    // delP995 is monotone increasing in k, so this is another one-unknown bisection.
    // Bounded: k may rise only until the floor holds, and the mean may yield by at most
    // kMeanYield — a sanity rail for pathological frames (black leader with one specular)
    // so a degenerate histogram cannot blow the exposure wide open.
    Delivered d; ApDeliver(a, h, d);
    if (d.p995 < kP995Floor) {
        double klo = a.k, khi = a.k * 64.0;
        for (int it = 0; it < 50; ++it) {
            const double mid = std::sqrt(klo * khi);
            a.k = mid;
            ApDeliver(a, h, d);
            if (d.p995 >= kP995Floor) khi = mid; else klo = mid;
        }
        a.k = std::sqrt(klo * khi);
        ApDeliver(a, h, d);
        if (d.mean > tMean + kMeanYield) {
            // rail: give back the excess by re-solving the mean at the capped level
            const double cap = tMean + kMeanYield;
            double mlo = 1e-6, mhi = a.k;
            for (int it = 0; it < 50; ++it) {
                const double mid = std::sqrt(mlo * mhi);
                a.k = mid;
                ApDeliver(a, h, d);
                if (d.mean > cap) mhi = mid; else mlo = mid;
            }
            a.k = std::sqrt(mlo * mhi);
        }
    }
    return a;
}

static void ApDeliver(const AParam& a, const Hist& h, Delivered& d)
{
    d = Delivered{ 0,0,0,0,0 };
    double acc = 0.0, cum = 0.0;
    const double n50 = 0.50 * h.total, n995 = 0.995 * h.total;
    bool g50 = false, g995 = false;
    for (int i = 0; i < kHBins; ++i) {
        if (h.count[i] <= 0.0) continue;
        const double code = APEval(a, h.centre[i]);
        acc += code * h.count[i];
        if (code > d.maxCode) d.maxCode = code;
        if (code >= kClipCode) d.clipFrac += h.count[i];
        cum += h.count[i];
        if (!g50 && cum >= n50)  { d.p50 = code; g50 = true; }
        if (!g995 && cum >= n995){ d.p995 = code; g995 = true; }
    }
    d.mean = acc / h.total;
    d.clipFrac /= h.total;
}

// Inverse quantile: the source nits at which the curve reaches a given output code.
//
// This is the representation the parametric form will be fitted in, and it is chosen
// over the forward one for a specific reason. Forward (nits -> code), a curve that
// adapts moves every sample point at once, so two curves that differ only in
// placement look different everywhere. Inverse (code -> nits), a placement change is
// a uniform shift of the whole list and a contrast change is a scaling of it — the
// two things a solver is actually doing separate into two numbers that mean something.
// If those numbers turn out to be simple functions of the four statistics, that
// function is the runtime curve generator, and no iterative solve is needed at all.
static double NitsAtCode(const Curve& c, double code)
{
    double lo = kKnotLo, hi = kKnotHi;
    for (int it = 0; it < 60; ++it) {
        const double mid = std::sqrt(lo * hi);
        if (CurveEval(c, mid) > code) hi = mid; else lo = mid;
    }
    return std::sqrt(lo * hi);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) { printf("usage: curve_solve.exe <frame.raw> [...]\n"); return 1; }

    printf("# house targets: mean=%.2f@%.0fnits damped^%.2f  p99.5>=%.2f  clip<=%.3f%%\n",
           kMeanAtRef, kRefSrcNits, kDamp, kP995Floor, kClipCeil * 100.0);
    printf("file            srcMean  tgtMean  delMean  delP50  delP995  clip%%   maxCd\n");

    std::vector<Curve> curves;
    std::vector<std::string> names;
    // The four statistics the runtime will actually have: mean, p50, p99.5, max.
    // Everything the curve is fitted from must be expressible through these, or the
    // parametric form cannot be built at runtime — the histogram is offline-only.
    std::vector<double> vMean, vP50, vP995, vMax;
    std::vector<Hist> hists;

    for (int a = 1; a < argc; ++a) {
        Hist h;
        if (!LoadFrameHist(argv[a], h)) { printf("%s: load failed\n", argv[a]); continue; }
        const double srcMean = SourceMean(h);
        const double tMean = kMeanAtRef * std::pow(std::max(srcMean, 0.05) / kRefSrcNits, kDamp);
        const Curve c = Solve(h);
        const Delivered d = Deliver(c, h);

        const char* base = strrchr(argv[a], '\\');
        base = base ? base + 1 : argv[a];
        printf("%-14s %8.2f %8.3f %8.3f %8.3f %8.3f %6.3f %7.3f\n",
               base, srcMean, tMean, d.mean, d.p50, d.p995, d.clipFrac * 100.0, d.maxCode);
        curves.push_back(c);
        names.push_back(base);

        // source-side statistics, in nits
        double cum = 0.0, q50 = 0.0, q995 = 0.0, mx = 0.0;
        const double n50 = 0.50 * h.total, n995 = 0.995 * h.total;
        bool g50 = false, g995 = false;
        for (int i = 0; i < kHBins; ++i) {
            if (h.count[i] <= 0.0) continue;
            if (h.centre[i] > mx) mx = h.centre[i];
            cum += h.count[i];
            if (!g50 && cum >= n50)  { q50 = h.centre[i]; g50 = true; }
            if (!g995 && cum >= n995){ q995 = h.centre[i]; g995 = true; }
        }
        vMean.push_back(srcMean); vP50.push_back(q50); vP995.push_back(q995); vMax.push_back(mx);
        hists.push_back(h);
    }

    // ---- the analytic form, and whether it reproduces the knot solve --------------
    {
        printf("\n# ANALYTIC FORM  code = 1 - exp(-k*(x/x0)^g)   targets M50=%.2f M995=%.2f\n",
               kM50, kM995);
        printf("file            g      k       x0    | delMean  delP50  delP995  maxCd  clip%%"
               " |  yield  | vs knots\n");
        printf("                (yield = delivered mean minus its target; non-zero means the\n");
        printf("                 highlight floor overrode the mean — house option A)\n");
        double worstFit = 0.0;
        for (size_t i = 0; i < curves.size(); ++i) {
            const AParam a = APSolve(hists[i], vMean[i], vP50[i], vP995[i]);
            Delivered d; ApDeliver(a, hists[i], d);
            const double tMean = kMeanAtRef
                * std::pow(std::max(vMean[i], 0.05) / kRefSrcNits, kDamp);
            const double yield = d.mean - tMean;
            double mx = 0.0;
            for (int g2 = 0; g2 <= 400; ++g2) {
                const double n = std::exp(std::log(0.02) + (g2 / 400.0) * (std::log(3000.0) - std::log(0.02)));
                mx = std::max(mx, std::abs(APEval(a, n) - CurveEval(curves[i], n)));
            }
            worstFit = std::max(worstFit, mx);
            printf("%-14s %6.3f %7.3f %8.2f | %7.3f %7.3f %7.3f %6.3f %6.3f | %+7.3f | %6.1f cd\n",
                   names[i].c_str(), a.g, a.k, a.x0,
                   d.mean, d.p50, d.p995, d.maxCode, d.clipFrac * 100.0, yield, mx * 255.0);
        }
        printf("\n   worst analytic-vs-knot difference: %.1f codes\n", worstFit * 255.0);
        printf("   (the knot curves are free 16-knot fits and are NOT the goal — agreement\n");
        printf("    within a few codes means the small family reaches the same targets.)\n");
    }

    // THE STABILITY METRIC. Max |delta code| between consecutive solved curves, over a
    // fine grid. Within a burst this is the pump; across titles it is the adaptation
    // the user has to accept. Both are reported because only the first is a defect.
    if (curves.size() >= 2) {
        printf("\n# curve motion between consecutive frames (max |delta code| over the grid)\n");
        double worst = 0.0;
        for (size_t i = 1; i < curves.size(); ++i) {
            double mx = 0.0;
            for (int g = 0; g <= 200; ++g) {
                const double n = std::exp(std::log(0.05) + (g / 200.0) * (std::log(2000.0) - std::log(0.05)));
                mx = std::max(mx, std::abs(CurveEval(curves[i], n) - CurveEval(curves[i - 1], n)));
            }
            worst = std::max(worst, mx);
            printf("   %-14s -> %-14s  max|dcode| = %.4f  (%.1f codes)\n",
                   names[i - 1].c_str(), names[i].c_str(), mx, mx * 255.0);
        }
        printf("\n   worst motion in this set: %.4f (%.1f codes)\n", worst, worst * 255.0);
        printf("   for reference: 1 code at 8-bit is ~0.4%% of range; the current shipping\n");
        printf("   chain's visible pump was measured as a multi-code crawl over ~1s.\n");
    }

    // ---- the inverse view: what the parametric form has to reproduce --------------
    {
        static const double kCodes[] = { 0.10, 0.25, 0.50, 0.75, 0.90 };
        printf("\n# inverse quantiles — SOURCE NITS that produce each output code\n");
        printf("file            srcMean   srcP50  srcP995   srcMax |");
        for (double cd : kCodes) printf("  nits@%.2f", cd);
        printf(" | spread@.9/.1\n");
        for (size_t i = 0; i < curves.size(); ++i) {
            printf("%-14s %8.2f %8.2f %8.1f %8.1f |",
                   names[i].c_str(), vMean[i], vP50[i], vP995[i], vMax[i]);
            double at[5];
            for (int k = 0; k < 5; ++k) { at[k] = NitsAtCode(curves[i], kCodes[k]); printf(" %9.2f", at[k]); }
            printf(" | %8.2f\n", at[4] / std::max(at[0], 1e-6));
        }
        printf("\n# ratios that a parametric form would have to predict from the statistics\n");
        printf("file            nits@.50/srcP50   nits@.90/srcMax   nits@.10/srcP50\n");
        for (size_t i = 0; i < curves.size(); ++i) {
            const double a10 = NitsAtCode(curves[i], 0.10);
            const double a50 = NitsAtCode(curves[i], 0.50);
            const double a90 = NitsAtCode(curves[i], 0.90);
            printf("%-14s %14.3f %16.4f %17.4f\n", names[i].c_str(),
                   a50 / std::max(vP50[i], 1e-6),
                   a90 / std::max(vMax[i], 1e-6),
                   a10 / std::max(vP50[i], 1e-6));
        }

        FILE* f = nullptr;
        if (0 == fopen_s(&f, "curves.csv", "w")) {
            fprintf(f, "file,srcMean,srcP50,srcP995,srcMax");
            for (int k = 0; k < kKnots; ++k) fprintf(f, ",k%02d_nits,k%02d_code", k, k);
            fprintf(f, "\n");
            for (size_t i = 0; i < curves.size(); ++i) {
                fprintf(f, "%s,%.4f,%.4f,%.4f,%.4f", names[i].c_str(),
                        vMean[i], vP50[i], vP995[i], vMax[i]);
                for (int k = 0; k < kKnots; ++k) fprintf(f, ",%.4f,%.6f", KnotNits(k), curves[i].y[k]);
                fprintf(f, "\n");
            }
            fclose(f);
            printf("\n wrote curves.csv (%d frames x %d knots)\n", (int)curves.size(), kKnots);
        }
    }
    return 0;
}
