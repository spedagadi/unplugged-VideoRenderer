// -----------------------------------------------------------------------------
// curve_render.cpp — step 3: LOOK at the analytic curve before the renderer does.
//
// Steps 1 and 2 validated the curve against STATISTICS — delivered mean, p99.5 floor,
// clip fraction, motion between frames. Every one of those can be satisfied by a
// mapping that looks wrong, because a percentile is not an image. This applies the
// solved curve to a real frame and writes it out, so the question "is this the look"
// is answered by an eye instead of by a table.
//
// THE COLOUR RULE, and why it is the interesting part
//
// The curve is a luminance function. Applying it to RGB needs a rule for chroma, and
// this project has been dialling that rule by hand for weeks (gt7Desat, gamutCap,
// hlN) — which is the same smell as dialling the tone curve's shape by hand. The
// principled rule is: preserve the pixel's RGB RATIOS, i.e. its chromaticity, and let
// the curve do all of its work in luminance:
//
//     RGB_out_linear = (RGB_in / Y_in) * Y_out_linear
//
// Then everything else — how much saturation survives a compressed highlight, how much
// the gamut mapping has to remove — FOLLOWS from the curve instead of being a control.
// If that looks right, three dials become consequences. If it looks wrong, that is
// also the answer, and much cheaper to find here.
//
// Output is written as raw rgb24 and converted to PNG by ffmpeg (see the run line at
// the bottom); keeping an image encoder out of the probe means nothing to link.
//
// Build:
//   cl /EHsc /O2 /std:c++17 /I C:\htpc curve_render.cpp /Fe:curve_render.exe
// Run:
//   curve_render.exe frames\e00900.raw out.rgb24 85
//   ffmpeg -y -f rawvideo -pix_fmt rgb24 -s 960x540 -i out.rgb24 out.png
// -----------------------------------------------------------------------------

#define main gt7_reference_main
#include "gt7_tone_mapping.cpp"
#undef main

#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>

// Frame size is set from argv — a 960x540 point-sampled frame is fine for judging the
// LOOK but cannot support a detail comparison against a 4K madVR grab, and silently
// doing so would flatter or penalise the curve for reasons that have nothing to do with
// the curve. Defaults to the extraction size so existing invocations still work.
static int kW = 960, kH = 540;
static const double kY_R = 0.2627, kY_G = 0.6780, kY_B = 0.0593;   // BT.2020 luma

// BT.2020 -> BT.709, linear light (D65 on both sides, so no adaptation matrix).
static const double k2020to709[3][3] = {
    {  1.66049, -0.58764, -0.07285 },
    { -0.12448,  1.13290, -0.00842 },
    { -0.01820, -0.10059,  1.11879 },
};

// ---- house targets (identical to curve_solve.cpp — these two must not drift) ----
static const double kMeanAtRef  = 0.30;
static const double kRefSrcNits = 20.0;
static const double kDamp       = 0.16;
static const double kP995Floor  = 0.85;
static const double kM50        = 0.24;
static const double kM995       = 0.90;
static const double kShadowTarget = 0.118;   // where source p25 should land (GT7 measured)
// Gamut capacity target. 1.0 = map to the boundary exactly (no extra compression) —
// the honest first test of "does the constraint, not the clamp, recover the detail".
// The shipping dial measured ~0.55 against madVR on the Everest sky; that is a house
// look choice and is deliberately NOT applied here until the mapping is shown to work.
static const double kGamutCap   = 1.00;

static const int    kHBins = 512;
static const double kHNitsLo = 0.002, kHNitsHi = 6000.0;
static const double kLogLo = std::log(kHNitsLo), kLogHi = std::log(kHNitsHi);

struct Hist { double count[kHBins]; double centre[kHBins]; double total; };

static int BinOf(double n)
{
    double u = (std::log(std::max(n, kHNitsLo)) - kLogLo) / (kLogHi - kLogLo);
    int b = (int)(u * kHBins);
    return std::min(std::max(b, 0), kHBins - 1);
}

struct AParam { double k, x0, g, p; };
static inline double APEval(const AParam& a, double nits)
{
    if (nits <= 0.0) return 0.0;
    const double b = 1.0 - std::exp(-a.k * std::pow(nits / a.x0, a.g));
    return std::pow(std::max(b, 0.0), a.p);
}

static double MeanOf(const AParam& a, const Hist& h)
{
    double acc = 0.0;
    for (int i = 0; i < kHBins; ++i) if (h.count[i] > 0.0) acc += h.count[i] * APEval(a, h.centre[i]);
    return acc / h.total;
}
static double P995Of(const AParam& a, const Hist& h)
{
    double cum = 0.0; const double need = 0.995 * h.total;
    for (int i = 0; i < kHBins; ++i) {
        if (h.count[i] <= 0.0) continue;
        cum += h.count[i];
        if (cum >= need) return APEval(a, h.centre[i]);
    }
    return 1.0;
}

// The solve, verbatim in behaviour from curve_solve.cpp: shape from the scene's own
// dynamic range, placement from the mean target, then the highlight floor overrides.
static AParam Solve(const Hist& h, double srcMean, double p50, double p995)
{
    AParam a;
    p50 = std::max(p50, 0.02); p995 = std::max(p995, p50 * 1.2);
    a.g = std::log(std::log(1.0 / (1.0 - kM995)) / std::log(1.0 / (1.0 - kM50)))
        / std::log(p995 / p50);
    a.x0 = p995;
    // Mirrors SolveDynamicAutoCurve after the toe: g from the mean target, p from the
    // shadow anchor (allowed below 1, i.e. a lift), k closed form from the highlight
    // anchor. The earlier floor/ceiling clamp on k is gone — it was the accidental
    // regulator that hid the once-a-second pump.
    const double tMean = kMeanAtRef * std::pow(std::max(srcMean, 0.05) / kRefSrcNits, kDamp);
    auto codeAt = [&](double gg, double pp, double Y) {
        const double kk = -std::log(1.0 - std::pow(kM995, 1.0 / pp));
        const double b = 1.0 - std::exp(-kk * std::pow(std::max(Y, 0.0) / p995, gg));
        return std::pow(std::max(b, 0.0), pp);
    };
    auto meanAt = [&](double gg, double pp) {
        double acc = 0.0;
        for (int i = 0; i < kHBins; ++i) if (h.count[i] > 0.0) acc += h.count[i] * codeAt(gg, pp, h.centre[i]);
        return acc / h.total;
    };
    auto solveP = [&](double gg) {
        double lo = 0.25, hi = 8.0;
        for (int it = 0; it < 34; ++it) {
            const double mid = std::sqrt(lo * hi);
            if (codeAt(gg, mid, p50) > kShadowTarget) lo = mid; else hi = mid;
        }
        return std::sqrt(lo * hi);
    };
    double glo = 0.20, ghi = 2.5;
    for (int it = 0; it < 40; ++it) {
        const double gm = std::sqrt(glo * ghi);
        if (meanAt(gm, solveP(gm)) > tMean) glo = gm; else ghi = gm;
    }
    a.g = std::sqrt(glo * ghi);
    a.p = solveP(a.g);
    a.k = -std::log(1.0 - std::pow(kM995, 1.0 / a.p));
    return a;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        printf("usage: curve_render.exe <in.raw> <out.rgb24> [width height]\n");
        return 1;
    }
    if (argc >= 5) { kW = atoi(argv[3]); kH = atoi(argv[4]); }

    FILE* f = nullptr;
    if (fopen_s(&f, argv[1], "rb")) { printf("cannot open %s\n", argv[1]); return 1; }
    // Verify the declared size against the file. A mismatch here would otherwise be
    // read as image data and produce a plausible-looking but completely wrong render —
    // the kind of error that survives review because it looks like a picture.
    _fseeki64(f, 0, SEEK_END);
    const long long bytes = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    if (bytes != (long long)kW * kH * 6) {
        printf("%s is %lld bytes, which is not %dx%d rgb48 (%d bytes)\n",
               argv[1], bytes, kW, kH, kW * kH * 6);
        fclose(f);
        return 1;
    }
    const size_t n = (size_t)kW * kH;
    std::vector<unsigned short> buf(n * 3);
    if (fread(buf.data(), 2, n * 3, f) != n * 3) { fclose(f); printf("short read\n"); return 1; }
    fclose(f);

    // decode to nits once; a point-sampled 4K frame is 2M pixels and pow() is not free
    std::vector<double> R(n), G(n), B(n), Y(n);
    Hist h; for (int i = 0; i < kHBins; ++i) { h.count[i] = 0.0;
        const double u = (i + 0.5) / kHBins; h.centre[i] = std::exp(kLogLo + u * (kLogHi - kLogLo)); }
    h.total = 0.0;

    std::vector<double> rowMax(kH, 0.0);
    for (size_t i = 0; i < n; ++i) {
        R[i] = eotfSt2084(buf[i * 3 + 0] / 65535.0) * 100.0;
        G[i] = eotfSt2084(buf[i * 3 + 1] / 65535.0) * 100.0;
        B[i] = eotfSt2084(buf[i * 3 + 2] / 65535.0) * 100.0;
        Y[i] = kY_R * R[i] + kY_G * G[i] + kY_B * B[i];
        const int row = (int)(i / kW);
        if (Y[i] > rowMax[row]) rowMax[row] = Y[i];
    }
    int top = 0, bot = kH;
    while (top < bot && rowMax[top] < 0.005) ++top;
    while (bot > top && rowMax[bot - 1] < 0.005) --bot;

    std::vector<double> ys;
    for (int r = top; r < bot; ++r)
        for (int c = 0; c < kW; ++c) { const double v = Y[(size_t)r * kW + c]; h.count[BinOf(v)] += 1.0; h.total += 1.0; ys.push_back(v); }
    std::sort(ys.begin(), ys.end());
    double acc = 0.0; for (int i = 0; i < kHBins; ++i) acc += h.count[i] * h.centre[i];
    const double sMean = acc / h.total;
    const double sP50  = ys[(size_t)(0.50 * ys.size())];
    const double sP995 = ys[(size_t)(0.995 * ys.size())];

    const AParam a = Solve(h, sMean, sP50, sP995);
    printf("# curve: g=%.4f  k=%.4f  x0=%.2f nits   (bars %d+%d trimmed)\n", a.g, a.k, a.x0, top, kH - bot);
    printf("# source mean=%.2f p50=%.2f p99.5=%.1f nits   ->  delivered mean=%.3f p99.5=%.3f\n",
           sMean, sP50, sP995, MeanOf(a, h), P995Of(a, h));

    // ---- apply ----------------------------------------------------------------
    std::vector<unsigned char> out(n * 3);
    double maxOut = 0.0; int over = 0;
    for (size_t i = 0; i < n; ++i) {
        const double y = Y[i];
        const double code = APEval(a, y);                 // 0..1 display electrical
        if (code > maxOut) maxOut = code;
        const double yLin = std::pow(std::min(std::max(code, 0.0), 1.0), 2.2);
        // Chromaticity-preserving application: the curve owns luminance, the pixel
        // keeps its RGB ratios, and saturation is whatever the curve implies.
        double ro, go, bo;
        if (y < 1e-6) { ro = go = bo = 0.0; }
        else { ro = R[i] / y * yLin; go = G[i] / y * yLin; bo = B[i] / y * yLin; }
        double r7 = k2020to709[0][0]*ro + k2020to709[0][1]*go + k2020to709[0][2]*bo;
        double g7 = k2020to709[1][0]*ro + k2020to709[1][1]*go + k2020to709[1][2]*bo;
        double b7 = k2020to709[2][0]*ro + k2020to709[2][1]*go + k2020to709[2][2]*bo;
        if (r7 < -1e-4 || g7 < -1e-4 || b7 < -1e-4 || r7 > 1.0001 || g7 > 1.0001 || b7 > 1.0001) ++over;

        // GAMUT MAPPING, not clamping. The first version of this render clamped, and it
        // produced a featureless cyan slab where a jacket should be: on the 10:14:44
        // frame 20.3% of pixels fell outside the 709 triangle, and a clamp collapses all
        // of them onto the same edge point, deleting the texture the curve had just
        // worked to preserve.
        //
        // The renderer already has the right rule (colorspace_gamut_conversion.hlsl):
        // find where the pixel's own ray leaves the triangle and move it to a FRACTION of
        // the way there, toward its own neutral. That keeps hue exactly and gives up only
        // saturation, so neighbouring pixels stay neighbours instead of merging. Reusing
        // it here rather than inventing a second one is the point of doing this offline.
        const double y7 = 0.2126 * r7 + 0.7152 * g7 + 0.0722 * b7;
        if (y7 > 1e-6) {
            auto edge = [y7](double c) {
                if (c < y7) { const double d = y7 - c; return (d > 1e-6) ? y7 / d : 1e6; }
                const double d = c - y7;
                return (d < 1e-6) ? 1e6 : (1.0 - y7) / d;
            };
            const double S = std::min(std::min(edge(r7), edge(g7)), edge(b7));
            const double capEff = 0.95 + (kGamutCap - 0.95) * std::min(1.0, std::max(0.0, y7 / 0.40));
            const double kk = std::min(1.0, capEff * S);
            r7 = y7 + kk * (r7 - y7);
            g7 = y7 + kk * (g7 - y7);
            b7 = y7 + kk * (b7 - y7);
        }
        r7 = std::min(std::max(r7, 0.0), 1.0);
        g7 = std::min(std::max(g7, 0.0), 1.0);
        b7 = std::min(std::max(b7, 0.0), 1.0);
        out[i * 3 + 0] = (unsigned char)(std::pow(r7, 1.0 / 2.2) * 255.0 + 0.5);
        out[i * 3 + 1] = (unsigned char)(std::pow(g7, 1.0 / 2.2) * 255.0 + 0.5);
        out[i * 3 + 2] = (unsigned char)(std::pow(b7, 1.0 / 2.2) * 255.0 + 0.5);
    }
    printf("# delivered max code %.3f; pixels outside the 709 triangle before clamp: %.2f%%\n",
           maxOut, 100.0 * over / (double)n);

    FILE* o = nullptr;
    if (fopen_s(&o, argv[2], "wb")) { printf("cannot write %s\n", argv[2]); return 1; }
    fwrite(out.data(), 1, n * 3, o);
    fclose(o);
    printf("wrote %s (%zu bytes, %dx%d rgb24)\n", argv[2], n * 3, kW, kH);
    return 0;
}
