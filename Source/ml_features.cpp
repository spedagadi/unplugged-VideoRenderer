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

// Summed area table with a 1-cell margin so zone queries need no bounds checks.
float SatQuery(const std::vector<float>& sat, int stride,
               int y0, int y1, int x0, int x1)
{
	return sat[(y1 + 1) * stride + (x1 + 1)]
		- sat[y0 * stride + (x1 + 1)]
		- sat[(y1 + 1) * stride + x0]
		+ sat[y0 * stride + x0];
}

} // namespace

void BuildMLFeatures(const uint16_t* luma16, const int rowPitch, float features[ML_FEATURE_DIM])
{
	const int w = ML_DOWNSAMPLE_W;
	const int h = ML_DOWNSAMPLE_H;
	const int total = w * h;
	const double scale = 1.0 / 65535.0;

	// reusable scratch on the render thread (no per-frame heap churn)
	thread_local uint32_t histogram[65536] = {};
	thread_local std::vector<float> sat((size_t)(w + 1) * (h + 1));
	thread_local std::vector<float> rowMean(h);
	ZeroMemory(histogram, sizeof(histogram));

	int64_t sum = 0, max_bin = 0, bright = 0;

	// single pass: histogram + summed area table + row means
	for (int y = 0; y < h; y++) {
		const uint16_t* row = luma16 + (size_t)y * rowPitch;
		const int ysat = (y + 1) * (w + 1);
		const int ysat0 = y * (w + 1);
		double rowSum = 0.0;
		for (int x = 0; x < w; x++) {
			const int v = row[x];
			const float fv = (float)(v * scale);
			histogram[v]++;
			sum += v;
			if (v > max_bin) max_bin = v;
			if (v > 32767) bright++;
			rowSum += fv;
			sat[ysat + x + 1] = fv + sat[ysat0 + x + 1] + sat[ysat + x] - sat[ysat0 + x];
		}
		rowMean[y] = (float)(rowSum / w);
	}

	// [0..8] global luma stats
	features[0] = (float)((double)max_bin * scale);
	features[1] = (float)((double)sum / (65535.0 * total));
	features[2] = (float)((double)bright / total);
	const float targets[6] = { 0.25f, 0.50f, 0.75f, 0.90f, 0.95f, 0.99f };
	for (int i = 0; i < 6; i++) {
		features[3 + i] = HistogramPercentile(histogram, total, targets[i]);
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
					const uint16_t* row = luma16 + (size_t)y * rowPitch;
					for (int x = x0; x <= x1; x++) {
						const float fv = (float)(row[x] * scale);
						if (fv > max_rect) max_rect = fv;
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

	// [87] compression_strength = output_max / scene peak (HDR branch)
	const float f0_den = std::min(0.90f, std::max(features[0], 1e-6f));
	features[87] = 0.5444f / f0_den;
}