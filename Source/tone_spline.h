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

// 256-entry LUT of libplacebo's single-pivot polynomial tone-map spline
// (port of pl_tone_map_spline + pl_tone_map_params_infer/generate from
// src/tone_mapping.c). Used to produce the ML knot features [77..84] in the
// exact units the XGBoost models were trained on (PQ input, output_max = 0.5444).
void BuildToneMapSplineLUT(float input_max /* PQ */, float input_avg /* PQ */,
                           float lut[256], float output_max = 0.5444f);