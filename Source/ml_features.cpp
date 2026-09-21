/*
 * (C) 2026 see Authors.txt
 *
 * This file is part of MPC-BE.
 *
 * MPC-BE is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-BE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "stdafx.h"
#include "Utils/Util.h"   // DLog — live in every build now (ml_features.cpp logs the ML clamp)
#include "ml_features.h"
#include "tone_spline.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// numpy-style percentile over the 65536-bin histogram (ml_features.c:340).
inline float HistogramPercentile(const uint32_t histogram[65536], int count, float target)
{
	float position = target * (count - 1);
	int lower = (int)position;
	int upper = lower + 1;
	int seen = 0, lower_bin = 0, upper_bin = 0;
	for (int bin = 0; bin < 65536; bin++) {
		int next = seen + (int)histogram[bin];
		if (lower < next) lower_bin = bin;
		if (upper < next) { upper_bin = bin; break; }
		seen = next;
	}
	float frac = position - lower;
	return (lower_bin * (1.0f - frac) + upper_bin * frac) / 65535.0f;
}

// ST.2084 encode of a luminance in nits (ml_render.c sdr_nits_to_pq).
inline float SdrNitsToPQ(float nits)
{
	if (nits <= 0.0f) return 0.0f;
	const float n = fminf(fmaxf(nits / 10000.0f, 1e-6f), 1.0f);
	const float m = powf(n, 0.1593017578125f);
	return powf((0.8359375f + 18.8515625f * m) / (1.0f + 18.6875f * m), 78.84375f);
}

// gamma-2.2 SDR value (bt.1886-ish white 100 nits) -> source-referred PQ.
inline float SdrGammaToPQ(float y_sdr)
{
	return SdrNitsToPQ(100.0f * powf(y_sdr, 2.2f));
}

// Summed area table with a 1-cell margin so zone queries need no bounds checks.
float SatQuery(const std::vector<float>& sat, int stride,
               int y0, int y1, int x0, int x1)
{
	return sat[(y1 + 1) * stride + (x1 + 1)]
		- sat[y0 * stride + (x1 + 1)]
		- sat[(y1 + 1) * stride + x0]
		+ sat[y0 * stride + x0];
}

constexpr float kCropLimit = 0.01f;

} // namespace

void BuildMLFeatures(const uint16_t* luma16, const int rowPitch, float features[ML_FEATURE_DIM],
                     MLLumaDomain domain, MLBarBounds* bars, float* outTextureEnergy,
                     float* outPeakP995, float* outActiveFrac)
{
	const int w = ML_DOWNSAMPLE_W;
	const int h = ML_DOWNSAMPLE_H;
	const int total = w * h;
	const double scale = 1.0 / 65535.0;

	// reusable scratch on the render thread (no per-frame heap churn)
	thread_local uint32_t histogram[65536] = {};
	thread_local std::vector<float> sat((size_t)(w + 1) * (h + 1));
	thread_local std::vector<float> rowMean(h);
	thread_local std::vector<float> tv((size_t)w * h); // per-pixel feature-domain value
	ZeroMemory(histogram, sizeof(histogram));

	// 1) decode the readback into the feature domain (PQ, or SDR gamma -> PQ)
	int64_t sum = 0, max_bin = 0, bright = 0;
	int64_t active = 0;   // pixels at/above the crop level — bar detection only
	int contentTop = h, contentBottom = -1;

	// libplacebo's scene-change bias is a WORKGROUP-active fraction, not a pixel
	// fraction: a workgroup counts as active if ANY of its pixels is above the
	// cutoff, so on any normally-exposed frame the fraction stays near 1.0 and the
	// scene-change delta keeps its full magnitude. A pixel fraction does the
	// opposite — on a sparse dark frame it is a small number, which scales delta
	// below thresh_low and pins the hysteresis mix at 0, so a real scene change
	// never snaps and the IIR alone decides. That is the lag.
	//
	// Cells stand in for workgroups: 16x16 over the 256x144 readback gives 144 of
	// them, the same order as a real dispatch grid.
	constexpr int kCell = 16;
	constexpr int kCellsX = (ML_DOWNSAMPLE_W + kCell - 1) / kCell;
	constexpr int kCellsY = (ML_DOWNSAMPLE_H + kCell - 1) / kCell;
	bool cellActive[kCellsX * kCellsY] = {};

	for (int y = 0; y < h; y++) {
		const uint16_t* row = luma16 + (size_t)y * rowPitch;
		const int cellRow = (y / kCell) * kCellsX;
		for (int x = 0; x < w; x++) {
			const float y16 = (float)(row[x] * scale);
			const float fv = (domain == MLLumaDomain::SDRGamma22) ? SdrGammaToPQ(y16) : y16;
			tv[(size_t)y * w + x] = fv;
			if (y16 >= kCropLimit) {
				active++;
				cellActive[cellRow + (x / kCell)] = true;
				if (contentTop > y) contentTop = y;
				contentBottom = y;
			}
			const int bin = (int)(fminf(fv, 1.0f) * 65535.0f);
			histogram[bin]++;
			sum += bin;
			if (bin > max_bin) max_bin = bin;
			if (fv > 0.5f) bright++;
		}
	}

	// 1b) texture energy — fraction of interior pixels whose feature-domain
	// luma deviates from the 4-neighbor mean by > kTexThreshold. Feeds the
	// micro-contrast gate (Design B): flat/gradient scenes give ~0, textured
	// scenes map to [0, ~0.3]. Same 5% step as the LogMLFrameStats edge scan.
	if (outTextureEnergy) {
		// Relative (2% of local value) + 0.0005 floor. PQ is a perceptual code
		// space: an absolute threshold is a huge nits-jump in the deep shadows
		// and killed the metric on dark HDR content (observed max ~0.006 on an
		// 8x box-averaged dark title card).
		int edges = 0;
		for (int y = 1; y < h - 1; y++) {
			const float* rowUp   = &tv[(size_t)(y - 1) * w];
			const float* rowMid  = &tv[(size_t)y * w];
			const float* rowDown = &tv[(size_t)(y + 1) * w];
			for (int x = 1; x < w - 1; x++) {
				const float mean4 = (rowUp[x] + rowMid[x - 1] + rowMid[x + 1] + rowDown[x]) * 0.25f;
				const float thr = fmaxf(0.0005f, 0.02f * fmaxf(mean4, rowMid[x]));
				if (fabsf(rowMid[x] - mean4) > thr) edges++;
			}
		}
		const int interior = (w - 2) * (h - 2);
		*outTextureEnergy = (interior > 0) ? (float)edges / interior : 0.0f;
	}

	// 2) summed area table + row means in feature space
	for (int y = 0; y < h; y++) {
		const int ysat = (y + 1) * (w + 1);
		const int ysat0 = y * (w + 1);
		double rowSum = 0.0;
		for (int x = 0; x < w; x++) {
			const float fv = tv[(size_t)y * w + x];
			rowSum += fv;
			sat[ysat + x + 1] = fv + sat[ysat0 + x + 1] + sat[ysat + x] - sat[ysat0 + x];
		}
		rowMean[y] = (float)(rowSum / w);
	}

	// 3) [0..8] global luma stats
	features[0] = (float)((double)max_bin / 65535.0);
	features[1] = (float)((double)sum / (65535.0 * total));
	features[2] = (float)((double)bright / total);
	const float targets[6] = { 0.25f, 0.50f, 0.75f, 0.90f, 0.95f, 0.99f };
	for (int i = 0; i < 6; i++) {
		features[3 + i] = HistogramPercentile(histogram, total, targets[i]);
	}

	// outPeakP995 is computed AFTER the bar detection below — it must be measured
	// over the content rows only. See the note there.

	// Cell-active fraction, for libplacebo's scene-change bias. Full-frame content
	// gives ~1.0; a letterboxed or mostly-black frame gives less, and its delta is
	// scaled down by that so it cannot register as a scene change on its own —
	// which is the intent, and a PIXEL fraction overshot it. See the note above.
	if (outActiveFrac) {
		int cellsOn = 0;
		for (int i = 0; i < kCellsX * kCellsY; i++) {
			if (cellActive[i]) cellsOn++;
		}
		*outActiveFrac = (float)cellsOn / (float)(kCellsX * kCellsY);
	}

	// [9..76] zone means + maxes (3x3 then 5x5), column-major zone indexing
	auto extract_zones = [&](int grid, int meanBase, int maxBase) {
		for (int r = 0; r < grid; r++) {
			const int y0 = (r * h) / grid;
			const int y1 = ((r + 1) * h) / grid - 1;
			for (int c = 0; c < grid; c++) {
				const int x0 = (c * w) / grid;
				const int x1 = ((c + 1) * w) / grid - 1;
				const float sum_rect = SatQuery(sat, w + 1, y0, y1, x0, x1);
				const int n_rect = (y1 - y0 + 1) * (x1 - x0 + 1);
				float max_rect = 0.0f;
				for (int y = y0; y <= y1; y++) {
					const float* row = &tv[(size_t)y * w];
					for (int x = x0; x <= x1; x++) {
						if (row[x] > max_rect) max_rect = row[x];
					}
				}
				features[meanBase + r * grid + c] = sum_rect / n_rect;
				features[maxBase + r * grid + c] = max_rect;
			}
		}
	};
	extract_zones(3, 9, 18);
	extract_zones(5, 27, 52);

	// [77..84] spline knots at the trained LUT indices (output_max 0.5444 PQ)
	float spline_lut[256];
	BuildToneMapSplineLUT(features[0], features[1], spline_lut);
	const int knot_indices[8] = { 0, 36, 73, 109, 146, 182, 219, 255 };
	for (int i = 0; i < 8; i++) {
		features[77 + i] = spline_lut[knot_indices[i]];
	}

	// [85]/[86] letterbox detection on the downsampled row means
	const float bar_threshold = 0.002f; // near-black PQ
	int top_bars = 0, bottom_bars = 0;
	for (int y = 0; y < h; y++) {
		if (rowMean[y] < bar_threshold) top_bars = y + 1; else break;
	}
	for (int y = h - 1; y >= 0; y--) {
		if (rowMean[y] < bar_threshold) bottom_bars = h - y; else break;
	}
	features[85] = (float)top_bars / h;
	features[86] = (float)bottom_bars / h;

	// The noise-robust scene peak that drives the Hable ANCHOR — measured over the
	// CONTENT rows only, so player graphics in the letterbox bars cannot move it.
	//
	// libplacebo's own peak detection uses 99.995 (PL_PEAK_DETECT_HQ_DEFAULTS),
	// high enough that on a 256x144 readback it discards the top ~2 pixels — a
	// speck, not a highlight. That is right for a decoded video file. It is not
	// enough for a CAPTURE: a subtitle line is thousands of pixels and survives the
	// percentile intact.
	//
	// A subtitle is white, and in a dark scene it is the brightest thing in frame.
	// So it set the anchor, the anchor set the divisor, and the divisor scales the
	// ENTIRE picture — the film visibly dimmed whenever subtitles appeared, and
	// recovered when they left. Observed on a scope film whose content band is
	// rows 18..125 of 144, i.e. the subtitle sits in the bottom BAR, inside the
	// region that was being measured.
	//
	// Restricting to the content band removes it with no user setting, and unlike
	// lowering the percentile it cannot clip the picture: the anchor is still the
	// peak, just the peak of the PICTURE rather than of the frame.
	//
	// Deliberately NOT applied to features[]. That array is the model contract —
	// its mean and max are trained values, and excluding the bars there would shift
	// every prediction on every scope film. This is a separate statistic for one
	// consumer.
	if (outPeakP995) {
		thread_local uint32_t anchorHist[65536];
		ZeroMemory(anchorHist, sizeof(anchorHist));
		// Bottom band of the PICTURE excluded as well, because subtitles are not
		// always in the letterbox bars. Dune puts them over the active frame, and
		// there the bar crop above removes nothing at all.
		//
		// This is the one exclusion that is a heuristic rather than a fact: subs are
		// placed in a predictable band (roughly the bottom eighth to sixth of the
		// picture, per the title-safe convention), but a genuinely bright object down
		// there is then invisible to the anchor. The cost of that is confined to the
		// bottom band — the rest of the frame is still normalised by a peak that
		// ignores a region the size of a subtitle line, which is the trade that buys
		// back both the dimming AND the colour loss below.
		//
		// The colour is the second symptom of the same cause, and worth stating
		// because it surprises: a subtitle raises the anchor, the anchor raises the
		// divisor, the curve lowers I, and the chroma rescale that keeps the
		// reconstruction inside the gamut scales chroma down WITH it. Brightness and
		// saturation move together because one quantity drives both. Fix the anchor
		// and the colour comes back on its own.
		// ZERO, i.e. OFF. A percentage chosen here would be a guess dressed as a
		// constant: where a player draws subtitles depends on the player, the
		// content's aspect, the disc's own positioning and the user's setting, and
		// none of that is knowable from the frame. It is a property of the SETUP.
		//
		// Left in place, at zero, so the mechanism is here if it becomes a setting
		// the operator sets deliberately for their own player.
		constexpr float kSubtitleBand = 0.0f;            // bottom fraction of the picture
		const int y0 = top_bars;
		const int contentEnd = h - bottom_bars;          // exclusive
		const int contentRows = contentEnd - y0;
		const int y1 = std::max(y0 + 1, contentEnd - (int)(contentRows * kSubtitleBand));
		int anchorCount = 0;
		for (int y = y0; y < y1; y++) {
			const float* row = &tv[(size_t)y * w];
			for (int x = 0; x < w; x++) {
				// CRASH FIX (2026-09-21, ml_features.cpp:301). This used to be
				//   anchorHist[(int)(fminf(row[x], 1.0f) * 65535.0f)]++;
				// with an UNCLAMPED lower bound: a single NaN or NEGATIVE sample
				// (what a stale readback looks like when the signal changes
				// mid-extraction — seen on the forwarder thread after a button
				// press) indexed the histogram out of bounds and access-violated
				// at a fixed address in BuildMLFeatures. The clamp makes one bad
				// sample cost a wrong bin instead of a crash; the sparse log keeps
				// the next occurrence as evidence instead of a corpse.
				const float l = row[x];
				const int bin = (int)(fminf(fmaxf(l, 0.0f), 1.0f) * 65535.0f);
				if (l < 0.0f || l > 1.0f) {
					static LONG s_badPx = 0;
					const LONG n = InterlockedIncrement(&s_badPx);
					if (n == 1 || (n % 1000) == 0)
						DLog(L"[ML] BuildMLFeatures: luma %.6f out of [0,1] at y=%d x=%d (badPx=%ld) — clamped",
							(double)l, y, x, (long)n);
				}
				anchorHist[bin]++;
				anchorCount++;
			}
		}
		// PERCENTILE — 99.5, not 99.995. The name `outPeakP995` is historical.
		//
		// p99.995 on this 256x144 readback (36864 px) discards the top ~2 pixels:
		// a speck. That was chosen to stop the anchor shimmering on grain, and it
		// does — but a LIGHT SOURCE or specular highlight is far bigger than two
		// pixels, so it survived intact and set the anchor for the whole scene.
		//
		// That is what produced the slow dim after a cut. Two scenes either side of
		// a cut can have similar AVERAGE brightness and very different PEAKS, so
		// libplacebo's hysteresis (which measures the average) sees no cut, and the
		// IIR then ramps the anchor up to the specular's level — the picture goes
		// bright on the cut and settles dimmer over about a second.
		//
		// p99.5 discards the top 184 pixels of 36864 — enough to take a light
		// source out of the statistic without touching the body of the picture, so
		// the scene's LEVEL sets the anchor and its speculars do not. Those
		// speculars then sit above the anchor and clip to white, which is what a
		// specular is supposed to look like.
		//
		// One constant, deliberately: if it needs to move, it moves here.
		constexpr float kAnchorPeakPercentile = 0.995f;
		*outPeakP995 = (anchorCount > 0)
			? HistogramPercentile(anchorHist, anchorCount, kAnchorPeakPercentile)
			: HistogramPercentile(histogram, total, kAnchorPeakPercentile);
	}

	if (bars) {
		bars->contentTop = contentTop;
		bars->contentBottom = contentBottom;
		bars->threshold = kCropLimit;
	}

	// [87] compression_strength = output_max / scene peak (HDR branch)
	const float f0_den = std::min(0.90f, std::max(features[0], 1e-6f));
	features[87] = 0.5444f / f0_den;
}