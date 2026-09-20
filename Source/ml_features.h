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

#pragma once

#define ML_FEATURE_DIM   88
#define ML_DOWNSAMPLE_W  256
#define ML_DOWNSAMPLE_H  144

// Luma value domain carried by the readback texture.
enum class MLLumaDomain {
	PQ,        // true source-referred PQ (HDR passthrough frame)
	SDRGamma22 // gamma-2.2 encoded SDR (an HDR->SDR converted frame); the
	           // samples are remapped into source-PQ units before stats run
};

struct MLBarBounds {
	int   contentTop = 0;      // first content row (0-based); h if blank
	int   contentBottom = -1;  // last content row (0-based); -1 if blank
	float threshold = 0.0f;
};

// Builds the 88-entry ML feature vector from a 256x144 R16_UNORM luma
// readback in the given domain. Port of libplacebo's extract_features_cpu
// mirrored by build_model_features — the exact contract the Zion XGBoost
// models were trained on. `rowPitch` is in uint16 elements.
//
// Slots: [0] maxscl, [1] avg, [2] fraction bright, [3..8] p25..p99,
// [9..26] 3x3 zone means+maxes, [27..76] 5x5 zone means+maxes,
// [77..84] spline knots, [85]/[86] top/bottom bar norm, [87] compression.
// `outPeakP995`, when given, receives the 99.995th percentile peak in the
// feature domain — the noise-robust scene peak libplacebo's peak detection is
// built on (PL_PEAK_DETECT_HQ_DEFAULTS, percentile 99.995). On a 256x144
// readback that discards the top TWO pixels, so it is not a low percentile that
// loses small highlights: it drops the stray speck and keeps the real peak.
//
// The distinction matters because features[0] is a MAX, and a max is set by a
// single bright pixel — sensor noise, grain, ringing. That makes it the least
// stable statistic available, and a caller that normalises a curve by it will
// shimmer. A percentile is a statistic.
//
// Emitted SEPARATELY rather than as a feature slot on purpose: features[] is the
// exact contract the Zion XGBoost models were trained on, so adding to it would
// shift every prediction.
//
// `outActiveFrac`, when given, receives the fraction of 16x16 CELLS containing
// at least one pixel at or above the crop (non-black) level — an analogue of
// libplacebo's frame_wg_active / frame_wg_count, which scales its scene-change
// delta so a mostly-black or letterboxed frame cannot trigger a change on its
// own. A cell counts if ANY of its pixels is active, exactly as a workgroup does.
//
// It was a pixel fraction, and that was a bug with a visible cost: on a sparse
// dark frame a pixel fraction is small, which scaled the delta below libplacebo's
// own thresh_low (0.01) and pinned its hysteresis mix at 0 — so no scene change
// ever snapped and the IIR ramp alone set the peak. Measured: filt_pq ramping
// 0.3202 -> 0.3785 over ~80 frames with mix = 0.000 on every line.
void BuildMLFeatures(const uint16_t* luma16, const int rowPitch,
                     float features[ML_FEATURE_DIM],
                     MLLumaDomain domain = MLLumaDomain::PQ,
                     MLBarBounds* bars = nullptr,
                     float* outTextureEnergy = nullptr,
                     float* outPeakP995 = nullptr,
                     float* outActiveFrac = nullptr);