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

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) { printf("usage: curve_solve.exe <frame.raw> [...]\n"); return 1; }

    printf("# house targets: mean=%.2f@%.0fnits damped^%.2f  p99.5>=%.2f  clip<=%.3f%%\n",
           kMeanAtRef, kRefSrcNits, kDamp, kP995Floor, kClipCeil * 100.0);
    printf("file            srcMean  tgtMean  delMean  delP50  delP995  clip%%   maxCd\n");

    std::vector<Curve> curves;
    std::vector<std::string> names;

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
    return 0;
}
