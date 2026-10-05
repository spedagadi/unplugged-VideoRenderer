// -----------------------------------------------------------------------------
// curve_probe.cpp — step 1 of "one curve per frame", measured on real frames.
//
// WHY THIS EXISTS
//
// The shipping grade is a chain of ~14 scalars (refWhite, sdrNits, alpha, gtAdapt,
// softKnee/ratio/width, toeStrength, gamma, shadow, rolloff, S/G/R, gamutCap,
// gt7Desat) that each secretly implement part of ONE monotonic function from input
// light to output light. Every failure of the last week is that fact surfacing in a
// different costume: a dial with no effect because another scalar cancels it; a
// clamp on one scalar silently handing a whole-image job to a noisy statistic; eight
// separate temporal filters because eight quantities can each move.
//
// The replacement is to make that function the OUTPUT — solve it directly from
// measured source statistics against stated house targets — so there is one object
// to stabilise instead of eight, and no composition order to get wrong.
//
// Before building any of it, one question has to be answered on real data, at zero
// risk to the renderer:
//
//     Can a single 1-D curve hit the house targets across genuinely different
//     titles, and how far does it MOVE between frames while doing it?
//
// The second half is the one that matters. "Per frame" is only safe if the answer is
// "barely". If a free curve still lurches, then a 1-D luminance function is the wrong
// instrument and the honest answer is a windowed/tiled operator — a much larger
// project, and far better discovered here than after five more build-and-watch cycles.
//
// WHAT THIS PROGRAM DOES
//
// Reads raw rgb48le frames (PQ code values, point-sampled from 4K so no code value
// is invented by filtering), decodes to nits through Polyphony's OWN eotfSt2084 —
// the same function the HLSL port is validated against, so the decode cannot disagree
// with the renderer — and reports the source luminance distribution the solver will
// have to work from: mean in nits, and percentiles from p1 to p99.9 plus the max.
//
// The percentile list is not incidental. The curve will be parameterised by knots AT
// THESE SAME POSITIONS, so that the offline experiment and the runtime solve are driven
// by identical telemetry — the renderer already logs p25/p50/p75/p90/p95/p99 and the
// p99.5 peak. If the knots were placed anywhere else, a passing step 1 would prove
// nothing about the runtime.
//
// Build:
//   cl /EHsc /O2 /std:c++17 /I C:\htpc curve_probe.cpp /Fe:curve_probe.exe
// Run:
//   curve_probe.exe frames\*.raw
// -----------------------------------------------------------------------------

#define main gt7_reference_main
#include "gt7_tone_mapping.cpp"     // eotfSt2084 / inverseEotfSt2084, verbatim
#undef main

#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

static const int kW = 960, kH = 540;      // must match the extraction scale

// BT.2020 luma weights, because the source IS bt2020nc. Using the 709 weights here
// would mis-weight red by ~40% relative, and red is exactly where HDR film differs
// most from SDR — the error would show up as a wrong peak and be attributed to the
// solver.
static const double kY_R = 0.2627, kY_G = 0.6780, kY_B = 0.0593;

// The percentile set. p99.5 is the shipping anchor statistic; p99.9 and max are here
// specifically to show how far the "peak" moves depending on which one is chosen —
// that spread is the flare problem that cost several rounds this week.
static const double kPct[] = { 1, 5, 10, 25, 50, 75, 90, 95, 99, 99.5, 99.9 };
static const int    kNPct  = (int)(sizeof(kPct) / sizeof(*kPct));

static bool LoadFrameNits(const char* path, std::vector<double>& y)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb")) return false;
    const size_t n = (size_t)kW * kH;
    std::vector<unsigned short> buf(n * 3);
    const size_t got = fread(buf.data(), 2, n * 3, f);
    fclose(f);
    if (got != n * 3) {
        printf("   %s: short read (%zu of %zu samples) — wrong frame size?\n",
               path, got, n * 3);
        return false;
    }
    y.resize(n);
    for (size_t i = 0; i < n; ++i) {
        // rgb48le is R,G,B 16-bit little-endian, and the code values are still
        // PQ-encoded: ffmpeg inverts the Y'CbCr encoding but never the transfer
        // characteristic, which is exactly what we want to decode ourselves.
        const double r = eotfSt2084(buf[i * 3 + 0] / 65535.0) * 100.0;
        const double g = eotfSt2084(buf[i * 3 + 1] / 65535.0) * 100.0;
        const double b = eotfSt2084(buf[i * 3 + 2] / 65535.0) * 100.0;
        y[i] = kY_R * r + kY_G * g + kY_B * b;
    }
    return true;
}

// Letterbox trim, mirroring the renderer's own m_MLBarDetected / content-rows crop.
//
// The first run of this tool reported p1..p25 as 0.0 nits on EVERY frame. That is not
// the film — Everest is 2.39:1 on a 16:9 carrier, so ~18% of the pixels are bars coded
// at exactly zero. Solving a curve against that distribution pins the shadow knots to a
// black level no frame of the actual movie ever shows, and drags the mean down by the
// same fraction. The renderer already excludes them; an offline experiment that does not
// is measuring a different image.
//
// Bars are detected as whole rows whose MAXIMUM is below a hair of black. A maximum, not
// a mean, so a bright object sitting in a dark band cannot be mistaken for content — and
// so a genuinely dark scene (max 0.02 nits) is still kept, because only rows that are
// empty at ANY level are bars.
static std::vector<double> TrimBars(const std::vector<double>& y,
                                    int& rowsTop, int& rowsBot)
{
    const double kBarMax = 0.005;      // nits; true bars decode to exactly 0
    std::vector<char> isBar(kH, 0);
    for (int r = 0; r < kH; ++r) {
        double mx = 0.0;
        const size_t base = (size_t)r * kW;
        for (int c = 0; c < kW; ++c) mx = std::max(mx, y[base + c]);
        isBar[r] = (mx < kBarMax) ? 1 : 0;
    }
    int top = 0, bot = kH;
    while (top < bot && isBar[top]) ++top;
    while (bot > top && isBar[bot - 1]) --bot;
    rowsTop = top; rowsBot = kH - bot;

    std::vector<double> out;
    out.reserve((size_t)(bot - top) * kW);
    for (int r = top; r < bot; ++r)
        for (int c = 0; c < kW; ++c) out.push_back(y[(size_t)r * kW + c]);
    return out;
}

static double Percentile(const std::vector<double>& sorted, double p)
{
    const double idx = (p / 100.0) * (sorted.size() - 1);
    const size_t lo = (size_t)idx;
    const size_t hi = std::min(lo + 1, sorted.size() - 1);
    return sorted[lo] + (idx - lo) * (sorted[hi] - sorted[lo]);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Decode calibration, printed first and always: if eotfSt2084 here disagrees with
    // the renderer's PqToLinearNits, every number below is meaningless. The renderer's
    // static PqToLinearNits gives 0.5000 -> 92.2 nits, 0.5811 -> ~98, 0.7519 -> ~1000.
    printf("# decode calibration (PQ code -> nits, via Polyphony's eotfSt2084)\n");
    for (double c : { 0.40, 0.50, 0.5811, 0.7519, 0.90 })
        printf("   code %.4f -> %9.2f nits\n", c, eotfSt2084(c) * 100.0);
    printf("\n");

    if (argc < 2) {
        printf("usage: curve_probe.exe <frame.raw> [more.raw ...]\n");
        return 1;
    }

    printf("# source luminance distribution, CONTENT ROWS ONLY (nits, BT.2020 luma)\n");
    printf("file            bars  mean    ");
    for (int i = 0; i < kNPct; ++i) printf(" p%-5g", kPct[i]);
    printf("     max   max/p50\n");

    // Held for the second report: the stability metric needs consecutive frames.
    std::vector<std::string> names;
    std::vector<std::vector<double> > pcts;

    for (int a = 1; a < argc; ++a) {
        std::vector<double> y;
        if (!LoadFrameNits(argv[a], y)) continue;

        int rTop = 0, rBot = 0;
        const std::vector<double> yc = TrimBars(y, rTop, rBot);

        std::vector<double> sorted = yc;
        std::sort(sorted.begin(), sorted.end());

        double mean = 0.0;
        for (double v : yc) mean += v;
        mean /= (double)yc.size();

        const char* base = strrchr(argv[a], '\\');
        base = base ? base + 1 : argv[a];
        printf("%-14s %4d+%3d %7.2f    ", base, rTop, rBot, mean);

        std::vector<double> row;
        for (int i = 0; i < kNPct; ++i) {
            const double v = Percentile(sorted, kPct[i]);
            row.push_back(v);
            printf(" %7.1f", v);
        }
        const double mx = sorted.back();
        printf("  %7.1f  %6.1f\n", mx, mx / std::max(row[kNPct / 2], 1e-6));

        names.push_back(base);
        pcts.push_back(row);
        pcts.back().push_back(mx);
        pcts.back().push_back(mean);
    }

    // The one number the whole project turns on, reported for the spread of titles
    // rather than claimed: how much does the statistic a curve would be built from
    // move between frames? p99.5 is what the shipping anchor uses; max is what a naive
    // "scene peak" would use. If p99.5 is calm and max is wild, the choice of statistic
    // is doing the work — and the solver must be given the calm one.
    if (pcts.size() >= 2) {
        printf("\n# spread across these frames — what any per-frame solver must track\n");
        const char* lbl[] = { "p1","p25","p50","p90","p95","p99","p99.5","p99.9","max","mean" };
        const int idx[]   = { 0, 3, 4, 6, 7, 8, 9, 10, kNPct, kNPct + 1 };
        for (int c = 0; c < 10; ++c) {
            double lo = 1e30, hi = -1e30;
            for (auto& r : pcts) { lo = std::min(lo, r[idx[c]]); hi = std::max(hi, r[idx[c]]); }
            printf("   %-6s  %9.1f .. %9.1f nits   (%.1f:1 across frames)\n",
                   lbl[c], lo, hi, hi / std::max(lo, 1e-6));
        }
    }

    printf("\n# frames sampled: %d\n", (int)pcts.size());
    printf("# next: solve a monotone curve with knots at these percentile positions\n");
    printf("#       and report max |delta code| between consecutive frames.\n");
    return 0;
}
