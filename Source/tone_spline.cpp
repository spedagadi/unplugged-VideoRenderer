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
#include "tone_spline.h"

#include <algorithm>
#include <cmath>

namespace {

inline float fclampf(float x, float lo, float hi) { return std::max(lo, std::min(hi, x)); }
inline float mixf(float a, float b, float x) { return a * (1.0f - x) + b * x; }
inline float smoothstep01(float e0, float e1, float x)
{
	float t = fclampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

// ST.2094 knee pick (port of st2094_pick_knee, tone_mapping.c:228).
void PickKnee(float input_max, float input_avg, float output_max,
              float* out_src_knee, float* out_dst_knee)
{
	const float src_min = 0.0f, src_max = input_max, src_avg = input_avg;
	const float dst_min = 0.0f, dst_max = output_max;

	// PL_TONE_MAP_CONSTANTS defaults
	const float min_knee = 0.1f, max_knee = 0.8f, def_knee = 0.4f, knee_adaptation = 0.4f;

	const float src_knee_min = mixf(src_min, src_max, min_knee);
	const float src_knee_max = mixf(src_min, src_max, max_knee);
	const float dst_knee_min = mixf(dst_min, dst_max, min_knee);
	const float dst_knee_max = mixf(dst_min, dst_max, max_knee);

	float src_knee = (src_avg > 0.0f && (std::isfinite)(src_avg)) ? src_avg : mixf(src_min, src_max, def_knee);
	src_knee = fclampf(src_knee, src_knee_min, src_knee_max);

	const float target = (src_knee - src_min) / (src_max - src_min);
	const float adapted = mixf(dst_min, dst_max, target);

	const float tuning = 1.0f - smoothstep01(max_knee, def_knee, target) *
	                             smoothstep01(min_knee, def_knee, target);
	const float adaptation = mixf(knee_adaptation, 1.0f, tuning);
	const float dst_knee = fclampf(mixf(src_knee, adapted, adaptation), dst_knee_min, dst_knee_max);

	*out_src_knee = src_knee;
	*out_dst_knee = dst_knee;
}

} // namespace

void BuildToneMapSplineLUT(float input_max, float input_avg, float lut[256], float output_max)
{
	// pl_tone_map_generate with input_scaling = output_scaling = PQ:
	// the LUT grid is linear in PQ across [0, input_max].
	input_max = std::max(input_max, std::min(output_max, 0.5080785f)); // 100-nit PQ floor (params_infer)
	input_max = std::max(input_max, input_avg + 1e-3f);

	float src_pivot, dst_pivot;
	PickKnee(input_max, input_avg, output_max, &src_pivot, &dst_pivot);

	// Solve for linear knee (Pa = 0)
	float slope = (dst_pivot - 0.0f) / (src_pivot - 0.0f);

	// Tune the slope at the knee (PL_TONE_MAP_CONSTANTS: slope_tuning 1.5, slope_offset 0.2)
	float ratio = input_max / output_max - 1.0f;
	ratio = fclampf(1.5f * ratio, 0.2f, 1.2f);
	slope = powf(slope, (1.0f - 0.5f) * ratio); // spline_contrast 0.5

	// Normalize around the pivot for P/Q solve
	const float in_min  = 0.0f - src_pivot;
	const float in_max  = input_max - src_pivot;
	const float out_min = 0.0f - dst_pivot;
	const float out_max = output_max - dst_pivot;

	// P(order 2): P(in_min)=out_min, P'(0)=slope, P(0)=0
	const float Pa = (out_min - slope * in_min) / (in_min * in_min);
	const float Pb = slope;

	// Q(order 3): Q(in_max)=out_max, Q''(in_max)=0, Q(0)=0, Q'(0)=slope
	const float t = 2.0f * in_max * in_max;
	const float Qa = (slope * in_max - out_max) / (in_max * t);
	const float Qb = -3.0f * (slope * in_max - out_max) / t;
	const float Qc = slope;

	for (int i = 0; i < 256; i++) {
		float x = mixf(0.0f, input_max, (float)i / 255.0f);
		x -= src_pivot;
		x = x > 0.0f ? ((Qa * x + Qb) * x + Qc) * x : (Pa * x + Pb) * x;
		x += dst_pivot;
		lut[i] = fclampf(x, 0.0f, output_max); // pl_tone_map_generate sanitize
	}
}