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
// `outMeanNits`, when given, receives the mean luminance in NITS over the content
// rows. Deliberately separate from features[1]: that is the mean of PQ CODES, and
// the mean of codes decodes to a fraction of a nit — useless for anything that
// reasons about exposure, which is why an auto-exposure solver built on it never
// fired. Averaging in nits is the only form of "the scene average" that means
// something physically.
// `outQuantiles`, when given, receives ML_QUANT_N source luminance values in PQ, at
// the evenly spaced quantile positions u = 0, 1/(N-1), ... 1 — i.e. the bottom of the
// distribution at index 0 and the top at index N-1.
//
// This exists because the Dynamic & Auto curve is SOLVED, not dialled, and a solve
// needs the delivered MEAN of a candidate curve, which is an integral over the whole
// distribution. The six percentiles in features[3..8] are not enough to reconstruct
// it: the delivered mean is dominated by the lower half, and the model contract in
// features[] deliberately reports the FULL frame including letterbox bars (see the
// note above outPeakP995), whereas this is measured over the content rows like the
// anchor peak.
//
// Thirty-three points is not a precision choice, it is a cost one: integrating the
// curve over a quantile table is exact for a monotone smooth curve to well under a
// code, and it keeps the interface a 132-byte array instead of a 256 KB histogram
// crossing a module boundary.
constexpr int ML_QUANT_N = 33;

void BuildMLFeatures(const uint16_t* luma16, const int rowPitch,
                     float features[ML_FEATURE_DIM],
                     MLLumaDomain domain = MLLumaDomain::PQ,
                     MLBarBounds* bars = nullptr,
                     float* outTextureEnergy = nullptr,
                     float* outPeakP995 = nullptr,
                     float* outActiveFrac = nullptr,
                     float* outMeanNits = nullptr,
                     float* outQuantiles = nullptr);

// ---------------------------------------------------------------------------
// DELIVERED-FRAME STATISTICS (the closed-loop instrument)
//
// BuildMLFeatures answers "what is the SOURCE like". Nothing in the renderer
// could answer "what did we DELIVER" — the models consume source-PQ statistics
// and act on the rendered frame without ever seeing it, so a change to the tone
// curve (or refWhite, or the calibration LUT) silently invalidates every
// prediction they make. These statistics are the missing measurement.
//
// They are computed on the frame's OWN encoded values (the R16_UNORM readback,
// 0..1, reported here scaled to 0..255 so they compare directly against a screen
// grab) and are NOT remapped into any source domain: a delivered frame is
// display-referred, and remapping it into PQ would be the double-remap bug this
// file has already shipped once.
//
// Deliberately a SEPARATE function rather than more feature slots: features[] is
// the exact contract the Zion XGBoost models were trained on.
struct MLRenderedLumaStats {
	float p01 = -1.0f, p05 = -1.0f, p25 = -1.0f, p50 = -1.0f;
	float p75 = -1.0f, p95 = -1.0f, p99 = -1.0f;
	float mean = -1.0f;
	// Fraction of samples at or below 4/255 — the "crushed to black" measure. A
	// shadow model that never outputs a perfect 0 (measured, repeatedly) still
	// crushes when the toe lands below the display's first visible code, and this
	// is the number that shows it.
	float crushFrac = -1.0f;
	// Mean of the samples in (4, 48]/255 — the toe band. Distinguishes "the shadow
	// band is dark" from "the shadow band is absent".
	float shadowMean = -1.0f;
	int   shadowCount = 0;
	bool  valid = false;
};

// The content RECT, and it has to be two-dimensional. A 2.39:1 film in a 16:9
// window has a letterbox (rows) and a 16:9 window on an ultrawide display has a
// pillarbox (columns) — measured, a 3440x1440 window showing a 2560x1440 video is
// 25.6% black COLUMNS, which no row crop can remove. Cropping only rows produced a
// crushFrac that was mostly matte and sent a whole feature down the wrong path.
//
// The matte is not cosmetic: it pins p01/p05/p25 at 0, dominates crushFrac, and
// drags mean toward black in proportion to how wrong the display's aspect is.
void BuildRenderedLumaStats(const uint16_t* luma16, const int rowPitch,
                            MLRenderedLumaStats& out,
                            int contentTop = 0, int contentBottom = -1,
                            int contentLeft = 0, int contentRight = -1);