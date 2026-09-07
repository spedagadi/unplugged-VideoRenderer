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
void BuildMLFeatures(const uint16_t* luma16, const int rowPitch,
                     float features[ML_FEATURE_DIM],
                     MLLumaDomain domain = MLLumaDomain::PQ,
                     MLBarBounds* bars = nullptr);