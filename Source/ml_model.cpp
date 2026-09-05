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
#include "ml_model.h"
#include <cstdio>

#define ML_MODEL_MAGIC   0x47584c50u // "PLXG"
#define ML_MODEL_VERSION 1u

CMLModel::~CMLModel()
{
	Unload();
}

bool CMLModel::Load(const wchar_t* path)
{
	Unload();

	FILE* file = nullptr;
	if (_wfopen_s(&file, path, L"rb") != 0 || !file) {
		return false;
	}

	auto ReadExact = [&](void* data, size_t size) -> bool {
		return fread(data, 1, size, file) == size;
	};

	uint32_t magic = 0, version = 0, reserved = 0;
	bool ok = ReadExact(&magic, 4) && ReadExact(&version, 4) &&
		ReadExact(&m_FeatureCount, 4) && ReadExact(&m_TreeCount, 4) &&
		ReadExact(&reserved, 4) && ReadExact(&m_BaseScore, 4) &&
		ReadExact(&m_LearningRate, 4) &&
		magic == ML_MODEL_MAGIC && version == ML_MODEL_VERSION &&
		m_FeatureCount != 0 && m_TreeCount != 0;

	if (!ok) {
		fclose(file);
		Unload();
		return false;
	}

	m_Trees.resize(m_TreeCount);
	for (uint32_t t = 0; t < m_TreeCount && ok; t++) {
		uint32_t nodeCount = 0;
		if (!ReadExact(&nodeCount, 4) || nodeCount == 0) {
			ok = false;
			break;
		}
		Tree& tree = m_Trees[t];
		tree.nodes.resize(nodeCount);
		tree.weights.resize(nodeCount);
		for (uint32_t n = 0; n < nodeCount; n++) {
			Node& node = tree.nodes[n];
			ok = ReadExact(&node.left, 4) && ReadExact(&node.right, 4) &&
				ReadExact(&node.feature, 4) && ReadExact(&node.threshold, 4) &&
				ReadExact(&node.default_left, 1) && ReadExact(&node.split_type, 1);
			if (!ok) break;
		}
		if (ok) {
			ok = ReadExact(tree.weights.data(), nodeCount * sizeof(float));
		}
	}

	fclose(file);

	if (!ok) {
		Unload();
		return false;
	}

	m_bLoaded = true;
	return true;
}

bool CMLModel::Predict(const float* features, int featureCount, float& value) const
{
	if (!m_bLoaded || !features || featureCount != (int)m_FeatureCount) {
		return false;
	}

	double result = m_BaseScore;
	for (const Tree& tree : m_Trees) {
		int32_t node = 0;
		const int32_t nodeCount = (int32_t)tree.nodes.size();
		for (int32_t steps = 0; steps < nodeCount; steps++) {
			const Node& current = tree.nodes[node];
			if (current.left < 0 || current.right < 0) {
				// exported base_weights already contain the effective leaf
				// contribution; do not apply learning_rate.
				result += tree.weights[node];
				break;
			}
			if (current.feature < 0 || current.feature >= featureCount) {
				return false;
			}
			node = features[current.feature] < current.threshold
				? current.left : current.right;
			if (node < 0 || node >= nodeCount) {
				return false;
			}
		}
	}

	value = (float)result;
	return true;
}

void CMLModel::Unload()
{
	m_bLoaded = false;
	m_FeatureCount = 0;
	m_TreeCount = 0;
	m_BaseScore = 0.0f;
	m_LearningRate = 0.0f;
	m_Trees.clear();
}