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

#include <vector>

// XGBoost model loader/predictor for libplacebo's '.plxgb' binary format.
// Port of libplacebo/src/ml_model.c — additive tree regression, no external deps.
// Used for the "Zion ML" grade (gamma / shadow / highlight prediction).
class CMLModel
{
public:
	CMLModel() = default;
	~CMLModel();

	CMLModel(const CMLModel&) = delete;
	CMLModel& operator=(const CMLModel&) = delete;

	// Loads the model file. Returns false on any parse error; the object is
	// left unloaded. Safe to call again (reloads from scratch).
	bool Load(const wchar_t* path);

	// Runs inference on `featureCount` features (must equal the model's
	// feature count). On success writes the raw regression value.
	bool Predict(const float* features, int featureCount, float& value) const;

	bool IsLoaded() const { return m_bLoaded; }
	uint32_t FeatureCount() const { return m_FeatureCount; }

private:
	struct Node {
		int32_t left;
		int32_t right;
		int32_t feature;
		float   threshold;
		uint8_t default_left;
		uint8_t split_type;
	};
	struct Tree {
		std::vector<Node>  nodes;
		std::vector<float> weights;
	};

	bool m_bLoaded       = false;
	uint32_t m_FeatureCount = 0;
	uint32_t m_TreeCount    = 0;
	float    m_BaseScore    = 0.0f;
	float    m_LearningRate = 0.0f;
	std::vector<Tree> m_Trees;

	void Unload();
};