/*
 * (C) 2018-2026 see Authors.txt
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
#include "resource.h"
#include "Helper.h"
#include "DisplayConfig.h"
#include "PropPage.h"

void SetCursor(HWND hWnd, LPCWSTR lpCursorName)
{
	SetClassLongPtrW(hWnd, GCLP_HCURSOR, (LONG_PTR)::LoadCursorW(nullptr, lpCursorName));
}

void SetCursor(HWND hWnd, UINT nID, LPCWSTR lpCursorName)
{
	SetCursor(::GetDlgItem(hWnd, nID), lpCursorName);
}

inline void ComboBox_AddStringData(HWND hWnd, int nIDComboBox, LPCWSTR str, LONG_PTR data)
{
	LRESULT lValue = SendDlgItemMessageW(hWnd, nIDComboBox, CB_ADDSTRING, 0, (LPARAM)str);
	if (lValue != CB_ERR) {
		SendDlgItemMessageW(hWnd, nIDComboBox, CB_SETITEMDATA, lValue, data);
	}
}

inline LONG_PTR ComboBox_GetCurItemData(HWND hWnd, int nIDComboBox)
{
	LRESULT lValue = SendDlgItemMessageW(hWnd, nIDComboBox, CB_GETCURSEL, 0, 0);
	if (lValue != CB_ERR) {
		lValue = SendDlgItemMessageW(hWnd, nIDComboBox, CB_GETITEMDATA, lValue, 0);
	}
	return lValue;
}

void ComboBox_SelectByItemData(HWND hWnd, int nIDComboBox, LONG_PTR data)
{
	LRESULT lCount = SendDlgItemMessageW(hWnd, nIDComboBox, CB_GETCOUNT, 0, 0);
	if (lCount != CB_ERR) {
		for (int idx = 0; idx < lCount; idx++) {
			const LRESULT lValue = SendDlgItemMessageW(hWnd, nIDComboBox, CB_GETITEMDATA, idx, 0);
			if (data == lValue) {
				SendDlgItemMessageW(hWnd, nIDComboBox, CB_SETCURSEL, idx, 0);
				break;
			}
		}
	}
}


// CVRMainPPage

// https://msdn.microsoft.com/ru-ru/library/windows/desktop/dd375010(v=vs.85).aspx

CVRMainPPage::CVRMainPPage(LPUNKNOWN lpunk, HRESULT* phr) :
	CBasePropertyPage(L"MainProp", lpunk, IDD_MAINPROPPAGE, IDS_MAINPROPPAGE_TITLE)
{
	DLog(L"CVRMainPPage()");
}

CVRMainPPage::~CVRMainPPage()
{
	DLog(L"~CVRMainPPage()");
}

namespace {

struct MLSliderRange {
	int nSlider, nEdit;
	float lo, hi;
	float Settings_t::* pField;
};

// libplacebo-authoritative ranges (ml_render.h + manifest clips)
const MLSliderRange kMLSliders[] = {
	{ IDC_SLIDER3,  IDC_EDIT3,  0.50f, 1.50f, &Settings_t::fMLGamma },
	{ IDC_SLIDER4,  IDC_EDIT4,  0.0f,  5.0f,  &Settings_t::fMLShadow },
	{ IDC_SLIDER5,  IDC_EDIT5, -0.30f, 0.40f, &Settings_t::fMLHighlight },
	{ IDC_SLIDER6,  IDC_EDIT7,  0.0f,  1.00f, &Settings_t::fMLHunt },
	{ IDC_SLIDER7,  IDC_EDIT8,  1.0f,  1.50f, &Settings_t::fMLChromaNeutral },
	{ IDC_SLIDER8,  IDC_EDIT9,  1.0f,  1.50f, &Settings_t::fMLChromaFire },
	{ IDC_SLIDER9,  IDC_EDIT10, 0.10f, 0.90f, &Settings_t::fMLChromaKnee },
	{ IDC_SLIDER10, IDC_EDIT11, 0.50f, 1.00f, &Settings_t::fMLChromaSkinProtect },
};

constexpr int kMLSlideRange(const MLSliderRange& r) { return (int)((r.hi - r.lo) * 100.0f + 0.5f); }
int MLSliderPos(float value, const MLSliderRange& r) { return (int)roundf((value - r.lo) * kMLSlideRange(r) / (r.hi - r.lo)); }
float MLSliderValue(int pos, const MLSliderRange& r) { return r.lo + (r.hi - r.lo) * ((float)pos / kMLSlideRange(r)); }

// "0.600000" reads like a bug; a nits value like "1000.000000" reads worse.
// std::to_wstring gives fixed 6 — trim the noise for the readouts.
std::wstring FmtSlider(float v) {
	std::wstring s = std::to_wstring(v);
	if (s.find(L'.') != std::wstring::npos) {
		while (!s.empty() && s.back() == L'0') {
			s.pop_back();
		}
		if (!s.empty() && s.back() == L'.') {
			s.pop_back();
		}
	}
	return s;
}

// Right-column parity sliders — same table-driven mechanism as the ML rows.
// The fade-window slider writes its START only at apply; the width stays
// Polyphony's 0.18 (the engine's parser derives the end the same way).
const MLSliderRange kGradeSliders[] = {
	{ IDC_SLIDER14, IDC_EDIT18, 100.0f, 4000.0f, &Settings_t::fHdrTitlePeakNits },
	{ IDC_SLIDER15, IDC_EDIT19, 0.0f,   2.0f,    &Settings_t::fHdrDisplayMinNits },
	{ IDC_SLIDER16, IDC_EDIT20, 0.20f,  1.00f,   &Settings_t::fGamutCap },
	{ IDC_SLIDER17, IDC_EDIT21, 0.0f,   1.0f,    &Settings_t::fMLHiDesat },
	{ IDC_SLIDER21, IDC_EDIT25, 0.20f,  0.70f,   &Settings_t::fHdrRefWhiteTarget },
	{ IDC_SLIDER22, IDC_EDIT26, 0.0f,   1.0f,    &Settings_t::fHdrGt7Blend },
	{ IDC_SLIDER23, IDC_EDIT27, 0.30f,  2.0f,    &Settings_t::fHdrGt7FadeStart },
	{ IDC_SLIDER24, IDC_EDIT28, 0.0f,   1.0f,    &Settings_t::fHdrGt7Adapt },
	{ IDC_SLIDER25, IDC_EDIT29, 0.02f,  0.90f,   &Settings_t::fHdrGt7Alpha },
	{ IDC_SLIDER26, IDC_EDIT30, 0.0f,   1.0f,    &Settings_t::fHdrGt7Desat },
	{ IDC_SLIDER27, IDC_EDIT31, 0.0f,   0.95f,   &Settings_t::fMLTargetMid },
	{ IDC_SLIDER28, IDC_EDIT32, -0.50f, 0.50f,   &Settings_t::fMLGammaTopUp },
	{ IDC_SLIDER29, IDC_EDIT33, 0.0f,   1.0f,    &Settings_t::fMLShadowDehaze },
	{ IDC_SLIDER30, IDC_EDIT34, -0.20f, 0.20f,   &Settings_t::fMLHighlightRolloffCtrl },
	{ IDC_SLIDER11, IDC_EDIT15, 0.0f,   0.50f,   &Settings_t::fMLContrast },
	{ IDC_SLIDER31, IDC_EDIT35, 0.0f,   0.60f,   &Settings_t::fMLContrastZoneLo },
	{ IDC_SLIDER32, IDC_EDIT36, 1.00f,  2.00f,   &Settings_t::fMLContrastRatioCap },
	{ IDC_SLIDER33, IDC_EDIT37, 0.0f,   1.00f,   &Settings_t::fMLContrastSkinProtect },
	{ IDC_SLIDER34, IDC_EDIT38, 0.0f,   1.00f,   &Settings_t::fMLSkyStrength },
	{ IDC_SLIDER35, IDC_EDIT39, 0.0f,   1.00f,   &Settings_t::fMLPersonStrength },
	{ IDC_SLIDER36, IDC_EDIT40, 0.0f,   1.00f,   &Settings_t::fMLSkinPreserve },
};

// The three taste trims are integer fields; position maps value-lo directly.
struct MLIntSlider {
	int nSlider, nEdit;
	int lo, hi;
	int Settings_t::* pField;
};

const MLIntSlider kIntSliders[] = {
	{ IDC_SLIDER18, IDC_EDIT22, -5, 5, &Settings_t::iHdrExposureTrim },
	{ IDC_SLIDER19, IDC_EDIT23, -5, 5, &Settings_t::iHdrShadowLift },
	{ IDC_SLIDER20, IDC_EDIT24, -5, 5, &Settings_t::iHdrPunch },
};

} // namespace

void CVRMainPPage::InitMLSliders()
{
	for (const auto& r : kMLSliders) {
		const int range = kMLSlideRange(r);
		SendDlgItemMessageW(r.nSlider, TBM_SETRANGE, 1, MAKELONG(0, range));
		SendDlgItemMessageW(r.nSlider, TBM_SETLINESIZE, 0, 1);
		SendDlgItemMessageW(r.nSlider, TBM_SETPAGESIZE, 0, (range / 10 > 1) ? range / 10 : 1);
	}
	for (const auto& r : kGradeSliders) {
		const int range = kMLSlideRange(r);
		SendDlgItemMessageW(r.nSlider, TBM_SETRANGE, 1, MAKELONG(0, range));
		SendDlgItemMessageW(r.nSlider, TBM_SETLINESIZE, 0, 1);
		SendDlgItemMessageW(r.nSlider, TBM_SETPAGESIZE, 0, (range / 10 > 1) ? range / 10 : 1);
	}
	for (const auto& r : kIntSliders) {
		const int range = r.hi - r.lo;
		SendDlgItemMessageW(r.nSlider, TBM_SETRANGE, 1, MAKELONG(0, range));
		SendDlgItemMessageW(r.nSlider, TBM_SETLINESIZE, 0, 1);
		SendDlgItemMessageW(r.nSlider, TBM_SETPAGESIZE, 0, 1);
	}
}

void CVRMainPPage::SetMLSliders()
{
	for (const auto& r : kMLSliders) {
		const float v = m_SetsPP.*(r.pField);
		SendDlgItemMessageW(r.nSlider, TBM_SETPOS, 1, MLSliderPos(v, r));
		SetDlgItemTextW(r.nEdit, std::to_wstring(v).c_str());
	}
	for (const auto& r : kGradeSliders) {
		const float v = m_SetsPP.*(r.pField);
		SendDlgItemMessageW(r.nSlider, TBM_SETPOS, 1, MLSliderPos(v, r));
		SetDlgItemTextW(r.nEdit, FmtSlider(v).c_str());
	}
	for (const auto& r : kIntSliders) {
		const int v = m_SetsPP.*(r.pField);
		SendDlgItemMessageW(r.nSlider, TBM_SETPOS, 1, v - r.lo);
		SetDlgItemTextW(r.nEdit, std::to_wstring(v).c_str());
	}
}

void CVRMainPPage::SetControls()
{
	CheckDlgButton(IDC_CHECK1, m_SetsPP.bUseD3D11             ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK2, m_SetsPP.bShowStats            ? BST_CHECKED : BST_UNCHECKED);

	ComboBox_SelectByItemData(m_hWnd, IDC_COMBO1, m_SetsPP.iTexFormat);

	CheckDlgButton(IDC_CHECK7, m_SetsPP.VPFmts.bNV12          ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK8, m_SetsPP.VPFmts.bP01x          ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK9, m_SetsPP.VPFmts.bYUY2          ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK4, m_SetsPP.VPFmts.bOther         ? BST_CHECKED : BST_UNCHECKED);
	SendDlgItemMessageW(IDC_COMBO9, CB_SETCURSEL, m_SetsPP.iVPDeinterlacing, 0);
	CheckDlgButton(IDC_CHECK3, m_SetsPP.bDeintDouble          ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK5, m_SetsPP.bVPScaling            ? BST_CHECKED : BST_UNCHECKED);
	SendDlgItemMessageW(IDC_COMBO8, CB_SETCURSEL, m_SetsPP.iVPSuperRes, 0);
	CheckDlgButton(IDC_CHECK19, m_SetsPP.bVPRTXVideoHDR       ? BST_CHECKED : BST_UNCHECKED);

	if (m_SetsPP.bHdrPassthrough) {
		ComboBox_SelectByItemData(m_hWnd, IDC_COMBO10, 0);
	} else if (m_SetsPP.bHdrLocalToneMapping) {
		ComboBox_SelectByItemData(m_hWnd, IDC_COMBO10, m_SetsPP.iHdrLocalToneMappingType);
	} else {
		ComboBox_SelectByItemData(m_hWnd, IDC_COMBO10, -1);
	}

	CheckDlgButton(IDC_CHECK18, m_SetsPP.bHdrPreferDoVi       ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK14, m_SetsPP.bConvertToSdr        ? BST_CHECKED : BST_UNCHECKED);

	SendDlgItemMessageW(IDC_COMBO7, CB_SETCURSEL, m_SetsPP.iHdrToggleDisplay, 0);
	SendDlgItemMessageW(IDC_SLIDER1, TBM_SETPOS, 1, m_SetsPP.iHdrOsdBrightness);

	SendDlgItemMessageW(IDC_SLIDER2, TBM_SETPOS, 1, m_SetsPP.iSDRDisplayNits / SDR_NITS_STEP);
	GetDlgItem(IDC_EDIT1).SetWindowTextW(std::to_wstring(m_SetsPP.iSDRDisplayNits).c_str());

	CheckDlgButton(IDC_CHECK6, m_SetsPP.bInterpolateAt50pct   ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK10, m_SetsPP.bUseDither           ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK17, m_SetsPP.bDeintBlend          ? BST_CHECKED : BST_UNCHECKED);

	CheckDlgButton(IDC_CHECK11, m_SetsPP.bExclusiveFS         ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK15, m_SetsPP.bVBlankBeforePresent ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK13, m_SetsPP.bAdjustPresentTime   ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK16, m_SetsPP.bReinitByDisplay     ? BST_CHECKED : BST_UNCHECKED);

	SendDlgItemMessageW(IDC_COMBO6, CB_SETCURSEL, m_SetsPP.iResizeStats, 0);

	SendDlgItemMessageW(IDC_COMBO5, CB_SETCURSEL, m_SetsPP.iChromaScaling, 0);
	SendDlgItemMessageW(IDC_COMBO2, CB_SETCURSEL, m_SetsPP.iUpscaling, 0);
	SendDlgItemMessageW(IDC_COMBO3, CB_SETCURSEL, m_SetsPP.iDownscaling, 0);
	SendDlgItemMessageW(IDC_COMBO4, CB_SETCURSEL, m_SetsPP.iSwapEffect, 0);

	m_SetsPP.iHdrDisplayMaxNits = discard<int>(m_SetsPP.iHdrDisplayMaxNits, HDR_NITS_DEF, HDR_NITS_MIN, HDR_NITS_MAX);
	SetDlgItemTextW(IDC_EDIT_DISPLAYMAX, std::to_wstring(m_SetsPP.iHdrDisplayMaxNits).c_str());

	CheckDlgButton(IDC_CHECK20, m_SetsPP.bMLEnabled ? BST_CHECKED : BST_UNCHECKED);
	SendDlgItemMessageW(IDC_COMBO11, CB_SETCURSEL, m_SetsPP.iMLMode, 0);
	SetMLSliders();
	CheckDlgButton(IDC_CHECK21, m_SetsPP.bMLDebugCopy ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK22, m_SetsPP.bMLDebugLog  ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK35, m_SetsPP.bTrayIcon     ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK23, m_SetsPP.bMLHuntEnabled   ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK24, m_SetsPP.bMLChromaEnabled ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK32, m_SetsPP.bMLToneCurveEnabled   ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK25, m_SetsPP.bMLGammaEnabled       ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK26, m_SetsPP.bMLShadowEnabled      ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK27, m_SetsPP.bMLHighlightEnabled   ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK28, m_SetsPP.bMLChromaNeutralEnabled ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK29, m_SetsPP.bMLChromaFireEnabled   ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK30, m_SetsPP.bMLChromaKneeEnabled   ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK31, m_SetsPP.bMLChromaSkinEnabled   ? BST_CHECKED : BST_UNCHECKED);

	// Right column: HDR delivery / GT7 / grade / micro contrast / segments
	CheckDlgButton(IDC_CHECK33, m_SetsPP.bMLContrastEnabled  ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK36, m_SetsPP.bHdrAutoRefWhite    ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK37, m_SetsPP.bMLController       ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK39, m_SetsPP.bMLBars             ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK40, m_SetsPP.bMLBarsHighlight    ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK41, m_SetsPP.bMLSkyEnabled       ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHECK42, m_SetsPP.bMLPersonEnabled    ? BST_CHECKED : BST_UNCHECKED);
	ComboBox_SelectByItemData(m_hWnd, IDC_COMBO12, m_SetsPP.iOutputGamut);
	ComboBox_SelectByItemData(m_hWnd, IDC_COMBO13, (LONG_PTR)(int)m_SetsPP.fHdrGt7Dbg);
}

void CVRMainPPage::EnableControls()
{
	if (!IsWindows8OrGreater()) { // Windows 7
		const BOOL bEnable = !m_SetsPP.bUseD3D11;
		GetDlgItem(IDC_STATIC1).EnableWindow(bEnable); // not working for GROUPBOX
		GetDlgItem(IDC_STATIC2).EnableWindow(bEnable);
		GetDlgItem(IDC_CHECK7).EnableWindow(bEnable);
		GetDlgItem(IDC_CHECK8).EnableWindow(bEnable);
		GetDlgItem(IDC_CHECK9).EnableWindow(bEnable);
		GetDlgItem(IDC_CHECK4).EnableWindow(bEnable);
		GetDlgItem(IDC_CHECK3).EnableWindow(bEnable);
		GetDlgItem(IDC_CHECK5).EnableWindow(bEnable);
		GetDlgItem(IDC_STATIC3).EnableWindow(bEnable);
		GetDlgItem(IDC_COMBO4).EnableWindow(bEnable);
	}
	else if (IsWindows10OrGreater()) {
		const BOOL bEnable = m_SetsPP.bUseD3D11;
		GetDlgItem(IDC_COMBO10).EnableWindow(bEnable);
		GetDlgItem(IDC_STATIC5).EnableWindow(bEnable);
		GetDlgItem(IDC_COMBO7).EnableWindow(bEnable);
		GetDlgItem(IDC_STATIC6).EnableWindow(bEnable);
		GetDlgItem(IDC_SLIDER1).EnableWindow(bEnable);
		GetDlgItem(IDC_STATIC7).EnableWindow(bEnable && m_SetsPP.bVPScaling);
		GetDlgItem(IDC_COMBO8).EnableWindow(bEnable && m_SetsPP.bVPScaling);
#ifdef _WIN64
		GetDlgItem(IDC_CHECK19).EnableWindow(bEnable && m_SetsPP.bHdrPassthrough);
#endif
	}

	GetDlgItem(IDC_STATIC8).EnableWindow(m_SetsPP.bConvertToSdr);
	GetDlgItem(IDC_EDIT1).EnableWindow(m_SetsPP.bConvertToSdr);
	GetDlgItem(IDC_SLIDER2).EnableWindow(m_SetsPP.bConvertToSdr);
	GetDlgItem(IDC_EDIT_DISPLAYMAX).EnableWindow(m_SetsPP.bHdrLocalToneMapping);

	// Zion ML
	const BOOL bMLEnable  = m_SetsPP.bMLEnabled;
	const BOOL bMLManual  = bMLEnable && m_SetsPP.iMLMode == ML_Manual;
	GetDlgItem(IDC_STATIC101).EnableWindow(bMLEnable);
	GetDlgItem(IDC_COMBO11).EnableWindow(bMLEnable);
	const BOOL bChromaRow  = m_SetsPP.bMLChromaEnabled;
	const BOOL bToneRow   = m_SetsPP.bMLToneCurveEnabled;
	GetDlgItem(IDC_CHECK32).EnableWindow(bMLEnable);                       // tone curve master
	GetDlgItem(IDC_CHECK25).EnableWindow(bMLEnable);                       // gamma row
	GetDlgItem(IDC_SLIDER3).EnableWindow(bMLEnable && bToneRow && m_SetsPP.bMLGammaEnabled);
	GetDlgItem(IDC_EDIT3).EnableWindow(bMLEnable && bToneRow && m_SetsPP.bMLGammaEnabled);
	GetDlgItem(IDC_CHECK26).EnableWindow(bMLEnable);                       // shadow row
	// Band controller ON owns the shadow band: the legacy multiplicative toe
	// stands down rather than double-acting (same rule the companion rail follows).
	GetDlgItem(IDC_SLIDER4).EnableWindow(bMLEnable && m_SetsPP.bMLShadowEnabled && !m_SetsPP.bMLController);
	GetDlgItem(IDC_EDIT4).EnableWindow(bMLEnable && m_SetsPP.bMLShadowEnabled && !m_SetsPP.bMLController);
	GetDlgItem(IDC_CHECK27).EnableWindow(bMLEnable);                       // highlight row
	GetDlgItem(IDC_SLIDER5).EnableWindow(bMLEnable && bToneRow && m_SetsPP.bMLHighlightEnabled);
	GetDlgItem(IDC_EDIT5).EnableWindow(bMLEnable && bToneRow && m_SetsPP.bMLHighlightEnabled);
	GetDlgItem(IDC_CHECK23).EnableWindow(bMLEnable);                       // hunt row
	GetDlgItem(IDC_SLIDER6).EnableWindow(bMLEnable && m_SetsPP.bMLHuntEnabled);
	GetDlgItem(IDC_EDIT7).EnableWindow(bMLEnable && m_SetsPP.bMLHuntEnabled);
	GetDlgItem(IDC_CHECK28).EnableWindow(bMLEnable);                       // ch-neutral row
	GetDlgItem(IDC_SLIDER7).EnableWindow(bMLEnable && bChromaRow && m_SetsPP.bMLChromaNeutralEnabled);
	GetDlgItem(IDC_EDIT8).EnableWindow(bMLEnable && bChromaRow && m_SetsPP.bMLChromaNeutralEnabled);
	GetDlgItem(IDC_CHECK29).EnableWindow(bMLEnable);                       // ch-fire row
	GetDlgItem(IDC_SLIDER8).EnableWindow(bMLEnable && bChromaRow && m_SetsPP.bMLChromaFireEnabled);
	GetDlgItem(IDC_EDIT9).EnableWindow(bMLEnable && bChromaRow && m_SetsPP.bMLChromaFireEnabled);
	GetDlgItem(IDC_CHECK30).EnableWindow(bMLEnable);                       // knee row
	GetDlgItem(IDC_SLIDER9).EnableWindow(bMLEnable && bChromaRow && m_SetsPP.bMLChromaKneeEnabled);
	GetDlgItem(IDC_EDIT10).EnableWindow(bMLEnable && bChromaRow && m_SetsPP.bMLChromaKneeEnabled);
	GetDlgItem(IDC_CHECK31).EnableWindow(bMLEnable);                       // skin row
	GetDlgItem(IDC_SLIDER10).EnableWindow(bMLEnable && bChromaRow && m_SetsPP.bMLChromaSkinEnabled);
	GetDlgItem(IDC_EDIT11).EnableWindow(bMLEnable && bChromaRow && m_SetsPP.bMLChromaSkinEnabled);
	GetDlgItem(IDC_STATIC105).EnableWindow(bMLEnable);
	GetDlgItem(IDC_CHECK21).EnableWindow(bMLEnable);
	GetDlgItem(IDC_CHECK22).EnableWindow(bMLEnable);
	GetDlgItem(IDC_CHECK24).EnableWindow(bMLEnable);                       // chroma master

	// Right column — mirrors the companion rail's exposure rule (CURVES_* sets in
	// s3shell_panel.py): a dial is only live when the selected curve reads it.
	const BOOL bHdrOn   = m_SetsPP.bUseD3D11 && IsWindows10OrGreater()
		&& m_SetsPP.bHdrLocalToneMapping && m_SetsPP.bConvertToSdr;
	const BOOL bGt7Ad   = bHdrOn && m_SetsPP.iHdrLocalToneMappingType == TONE_CURVE_GT7_ADAPT;
	GetDlgItem(IDC_STATIC112).EnableWindow(bGt7Ad);   // title peak: what GT7 Adapt maps FROM
	GetDlgItem(IDC_SLIDER14).EnableWindow(bGt7Ad);
	GetDlgItem(IDC_EDIT18).EnableWindow(bGt7Ad);
	GetDlgItem(IDC_STATIC113).EnableWindow(bHdrOn);   // display black: destination range floor
	GetDlgItem(IDC_SLIDER15).EnableWindow(bHdrOn);
	GetDlgItem(IDC_EDIT19).EnableWindow(bHdrOn);
	GetDlgItem(IDC_STATIC114).EnableWindow(bHdrOn);   // output gamut
	GetDlgItem(IDC_COMBO12).EnableWindow(bHdrOn);
	const BOOL bCap = bHdrOn && m_SetsPP.iOutputGamut == 0;  // capacity acts inside 709 only
	GetDlgItem(IDC_STATIC115).EnableWindow(bCap);
	GetDlgItem(IDC_SLIDER16).EnableWindow(bCap);
	GetDlgItem(IDC_EDIT20).EnableWindow(bCap);
	const BOOL bHiDesat = bHdrOn && bMLEnable;  // delivered in the grade's chroma pass
	GetDlgItem(IDC_STATIC116).EnableWindow(bHiDesat);
	GetDlgItem(IDC_SLIDER17).EnableWindow(bHiDesat);
	GetDlgItem(IDC_EDIT21).EnableWindow(bHiDesat);
	GetDlgItem(IDC_STATIC117).EnableWindow(bHdrOn); // exposure trim — both house curves
	GetDlgItem(IDC_SLIDER18).EnableWindow(bHdrOn);
	GetDlgItem(IDC_EDIT22).EnableWindow(bHdrOn);
	GetDlgItem(IDC_STATIC118).EnableWindow(bHdrOn); // shadow detail
	GetDlgItem(IDC_SLIDER19).EnableWindow(bHdrOn);
	GetDlgItem(IDC_EDIT23).EnableWindow(bHdrOn);
	GetDlgItem(IDC_STATIC119).EnableWindow(bHdrOn); // contrast
	GetDlgItem(IDC_SLIDER20).EnableWindow(bHdrOn);
	GetDlgItem(IDC_EDIT24).EnableWindow(bHdrOn);
	GetDlgItem(IDC_CHECK36).EnableWindow(bGt7Ad);   // auto exposure
	const BOOL bRefTgt = bGt7Ad && m_SetsPP.bHdrAutoRefWhite;
	GetDlgItem(IDC_STATIC120).EnableWindow(bRefTgt);
	GetDlgItem(IDC_SLIDER21).EnableWindow(bRefTgt);
	GetDlgItem(IDC_EDIT25).EnableWindow(bRefTgt);
	GetDlgItem(IDC_STATIC121).EnableWindow(bGt7Ad); // gt7 blend..desat + debug view
	GetDlgItem(IDC_SLIDER22).EnableWindow(bGt7Ad);
	GetDlgItem(IDC_EDIT26).EnableWindow(bGt7Ad);
	GetDlgItem(IDC_STATIC122).EnableWindow(bGt7Ad);
	GetDlgItem(IDC_SLIDER23).EnableWindow(bGt7Ad);
	GetDlgItem(IDC_EDIT27).EnableWindow(bGt7Ad);
	GetDlgItem(IDC_STATIC123).EnableWindow(bGt7Ad);
	GetDlgItem(IDC_SLIDER24).EnableWindow(bGt7Ad);
	GetDlgItem(IDC_EDIT28).EnableWindow(bGt7Ad);
	GetDlgItem(IDC_STATIC124).EnableWindow(bGt7Ad);
	GetDlgItem(IDC_SLIDER25).EnableWindow(bGt7Ad);
	GetDlgItem(IDC_EDIT29).EnableWindow(bGt7Ad);
	GetDlgItem(IDC_STATIC125).EnableWindow(bGt7Ad);
	GetDlgItem(IDC_SLIDER26).EnableWindow(bGt7Ad);
	GetDlgItem(IDC_EDIT30).EnableWindow(bGt7Ad);
	GetDlgItem(IDC_STATIC126).EnableWindow(bGt7Ad);
	GetDlgItem(IDC_COMBO13).EnableWindow(bGt7Ad);
	GetDlgItem(IDC_CHECK37).EnableWindow(bMLEnable); // band controller
	GetDlgItem(IDC_STATIC127).EnableWindow(bMLEnable); // mid-tone target
	GetDlgItem(IDC_SLIDER27).EnableWindow(bMLEnable);
	GetDlgItem(IDC_EDIT31).EnableWindow(bMLEnable);
	const BOOL bTopUp = bMLEnable && !m_SetsPP.bMLController;  // stands down under the controller
	GetDlgItem(IDC_STATIC128).EnableWindow(bTopUp);
	GetDlgItem(IDC_SLIDER28).EnableWindow(bTopUp);
	GetDlgItem(IDC_EDIT32).EnableWindow(bTopUp);
	GetDlgItem(IDC_STATIC129).EnableWindow(bMLEnable); // shadow dehaze
	GetDlgItem(IDC_SLIDER29).EnableWindow(bMLEnable);
	GetDlgItem(IDC_EDIT33).EnableWindow(bMLEnable);
	GetDlgItem(IDC_STATIC130).EnableWindow(bMLEnable); // highlight shoulder
	GetDlgItem(IDC_SLIDER30).EnableWindow(bMLEnable);
	GetDlgItem(IDC_EDIT34).EnableWindow(bMLEnable);
	GetDlgItem(IDC_CHECK33).EnableWindow(bMLEnable);   // micro contrast enable
	const BOOL bMC = bMLEnable && m_SetsPP.bMLContrastEnabled;
	GetDlgItem(IDC_STATIC109).EnableWindow(bMC);
	GetDlgItem(IDC_SLIDER11).EnableWindow(bMC);
	GetDlgItem(IDC_EDIT15).EnableWindow(bMC);
	GetDlgItem(IDC_STATIC131).EnableWindow(bMC);
	GetDlgItem(IDC_SLIDER31).EnableWindow(bMC);
	GetDlgItem(IDC_EDIT35).EnableWindow(bMC);
	GetDlgItem(IDC_STATIC132).EnableWindow(bMC);
	GetDlgItem(IDC_SLIDER32).EnableWindow(bMC);
	GetDlgItem(IDC_EDIT36).EnableWindow(bMC);
	GetDlgItem(IDC_STATIC133).EnableWindow(bMC);
	GetDlgItem(IDC_SLIDER33).EnableWindow(bMC);
	GetDlgItem(IDC_EDIT37).EnableWindow(bMC);
	GetDlgItem(IDC_CHECK39).EnableWindow(bMLEnable);   // repaint bars
	GetDlgItem(IDC_CHECK40).EnableWindow(bMLEnable);   // highlight bars
	GetDlgItem(IDC_CHECK41).EnableWindow(bMLEnable);   // sky seg
	const BOOL bSky = bMLEnable && m_SetsPP.bMLSkyEnabled;
	GetDlgItem(IDC_SLIDER34).EnableWindow(bSky);
	GetDlgItem(IDC_EDIT38).EnableWindow(bSky);
	GetDlgItem(IDC_CHECK42).EnableWindow(bMLEnable);   // person skin
	const BOOL bPerson = bMLEnable && m_SetsPP.bMLPersonEnabled;
	GetDlgItem(IDC_SLIDER35).EnableWindow(bPerson);
	GetDlgItem(IDC_EDIT39).EnableWindow(bPerson);
	GetDlgItem(IDC_STATIC134).EnableWindow(bPerson);   // tone preserve
	GetDlgItem(IDC_SLIDER36).EnableWindow(bPerson);
	GetDlgItem(IDC_EDIT40).EnableWindow(bPerson);

	// Tray icon is renderer-wide, not ML-gated
	GetDlgItem(IDC_CHECK35).EnableWindow(TRUE);
}

HRESULT CVRMainPPage::OnConnect(IUnknown *pUnk)
{
	if (pUnk == nullptr) return E_POINTER;

	m_pVideoRenderer = pUnk;
	if (!m_pVideoRenderer) {
		return E_NOINTERFACE;
	}

	return S_OK;
}

HRESULT CVRMainPPage::OnDisconnect()
{
	if (m_pVideoRenderer == nullptr) {
		return E_UNEXPECTED;
	}

	KillTimer(1001);

	if (m_SetsPP.iSDRDisplayNits != m_oldSDRDisplayNits) {
		// OK or Apply buttons were not pressed. cancel the settings.
		m_pVideoRenderer->GetSettings(m_SetsPP);
		m_SetsPP.iSDRDisplayNits = m_oldSDRDisplayNits;
		m_pVideoRenderer->SetSettings(m_SetsPP);
	}

	m_pVideoRenderer.Release();

	return S_OK;
}

HRESULT CVRMainPPage::OnActivate()
{
	// set m_hWnd for CWindow
	m_hWnd = m_hwnd;

	m_pVideoRenderer->GetSettings(m_SetsPP);
	m_oldSDRDisplayNits = m_SetsPP.iSDRDisplayNits;

	if (!IsWindows7SP1OrGreater()) {
		GetDlgItem(IDC_CHECK1).EnableWindow(FALSE);
		m_SetsPP.bUseD3D11 = false;
	}
	if (!IsWindows10OrGreater()) {
		GetDlgItem(IDC_COMBO10).EnableWindow(FALSE);
		GetDlgItem(IDC_STATIC5).EnableWindow(FALSE);
		GetDlgItem(IDC_COMBO7).EnableWindow(FALSE);
		GetDlgItem(IDC_STATIC6).EnableWindow(FALSE);
		GetDlgItem(IDC_SLIDER1).EnableWindow(FALSE);
		GetDlgItem(IDC_STATIC7).EnableWindow(FALSE);
		GetDlgItem(IDC_COMBO8).EnableWindow(FALSE);
		GetDlgItem(IDC_CHECK19).EnableWindow(FALSE);
	}

#ifndef _WIN64
	GetDlgItem(IDC_STATIC7).EnableWindow(FALSE);
	GetDlgItem(IDC_COMBO8).EnableWindow(FALSE);
	GetDlgItem(IDC_CHECK19).EnableWindow(FALSE);
#endif

	EnableControls();

	SendDlgItemMessageW(IDC_COMBO6, CB_ADDSTRING, 0, (LPARAM)L"Fixed font size");
	SendDlgItemMessageW(IDC_COMBO6, CB_ADDSTRING, 0, (LPARAM)L"Increase font by window");

	ComboBox_AddStringData(m_hWnd, IDC_COMBO1, L"Auto 8/10-bit Integer",  0);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO1, L"8-bit Integer",          8);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO1, L"10-bit Integer",        10);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO1, L"16-bit Floating Point", 16);

	SendDlgItemMessageW(IDC_COMBO9, CB_ADDSTRING, 0, (LPARAM)L"Disable");
	SendDlgItemMessageW(IDC_COMBO9, CB_ADDSTRING, 0, (LPARAM)L"Enable");
	SendDlgItemMessageW(IDC_COMBO9, CB_ADDSTRING, 0, (LPARAM)L"HACK future frames");

	SendDlgItemMessageW(IDC_COMBO8, CB_ADDSTRING, 0, (LPARAM)L"Disable");
	SendDlgItemMessageW(IDC_COMBO8, CB_ADDSTRING, 0, (LPARAM)L"for SD");
	SendDlgItemMessageW(IDC_COMBO8, CB_ADDSTRING, 0, (LPARAM)L"for \x2264 720p");
	SendDlgItemMessageW(IDC_COMBO8, CB_ADDSTRING, 0, (LPARAM)L"for \x2264 1080p");
	SendDlgItemMessageW(IDC_COMBO8, CB_ADDSTRING, 0, (LPARAM)L"for \x2264 1440p");

	SendDlgItemMessageW(IDC_COMBO7, CB_ADDSTRING, 0, (LPARAM)L"Do not change");
	SendDlgItemMessageW(IDC_COMBO7, CB_ADDSTRING, 0, (LPARAM)L"Allow turn on (fullscreen)");
	SendDlgItemMessageW(IDC_COMBO7, CB_ADDSTRING, 0, (LPARAM)L"Allow turn on");
	SendDlgItemMessageW(IDC_COMBO7, CB_ADDSTRING, 0, (LPARAM)L"Allow turn on/off (fullscreen)");
	SendDlgItemMessageW(IDC_COMBO7, CB_ADDSTRING, 0, (LPARAM)L"Allow turn on/off");

	SendDlgItemMessageW(IDC_COMBO5, CB_ADDSTRING, 0, (LPARAM)L"Nearest-neighbor");
	SendDlgItemMessageW(IDC_COMBO5, CB_ADDSTRING, 0, (LPARAM)L"Bilinear");
	SendDlgItemMessageW(IDC_COMBO5, CB_ADDSTRING, 0, (LPARAM)L"Catmull-Rom");
	SendDlgItemMessageW(IDC_COMBO5, CB_ADDSTRING, 0, (LPARAM)L"KrigBilateral");

	SendDlgItemMessageW(IDC_COMBO2, CB_ADDSTRING, 0, (LPARAM)L"Nearest-neighbor");
	SendDlgItemMessageW(IDC_COMBO2, CB_ADDSTRING, 0, (LPARAM)L"Mitchell-Netravali");
	SendDlgItemMessageW(IDC_COMBO2, CB_ADDSTRING, 0, (LPARAM)L"Catmull-Rom");
	SendDlgItemMessageW(IDC_COMBO2, CB_ADDSTRING, 0, (LPARAM)L"Lanczos2");
	SendDlgItemMessageW(IDC_COMBO2, CB_ADDSTRING, 0, (LPARAM)L"Lanczos3");
	SendDlgItemMessageW(IDC_COMBO2, CB_ADDSTRING, 0, (LPARAM)L"Jinc2m");

	SendDlgItemMessageW(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Box");
	SendDlgItemMessageW(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Bilinear");
	SendDlgItemMessageW(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Hamming");
	SendDlgItemMessageW(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Bicubic");
	SendDlgItemMessageW(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Bicubic sharp");
	SendDlgItemMessageW(IDC_COMBO3, CB_ADDSTRING, 0, (LPARAM)L"Lanczos");

	SendDlgItemMessageW(IDC_COMBO4, CB_ADDSTRING, 0, (LPARAM)L"Discard");
	SendDlgItemMessageW(IDC_COMBO4, CB_ADDSTRING, 0, (LPARAM)L"Flip");

	SendDlgItemMessageW(IDC_SLIDER1, TBM_SETRANGE, 0, MAKELONG(0, 2));
	SendDlgItemMessageW(IDC_SLIDER1, TBM_SETTIC, 0, 1);

	SendDlgItemMessageW(IDC_SLIDER2, TBM_SETRANGE, 0, MAKELONG(SDR_NITS_MIN / SDR_NITS_STEP, SDR_NITS_MAX / SDR_NITS_STEP));
	SendDlgItemMessageW(IDC_SLIDER2, TBM_SETTIC, 0, SDR_NITS_DEF / SDR_NITS_STEP);
	SendDlgItemMessageW(IDC_SLIDER2, TBM_SETLINESIZE, 0, 1); // arrow keys
	SendDlgItemMessageW(IDC_SLIDER2, TBM_SETPAGESIZE, 0, 5); // clicks on trackbar's channel

	SetDlgItemTextW(IDC_EDIT2, GetNameAndVersion());

	// The house two — the same face the companion rail shows by default. The other
	// engine curves stay valid stored values but no menu entry; if the stored
	// setting is one of them it appears here as an explicit "(legacy)" row so
	// the dialog never silently rewrites a setting the user did not choose.
	ComboBox_AddStringData(m_hWnd, IDC_COMBO10, L"Ignore", -1);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO10, L"Passthrough", 0);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO10, L"Unplugged", TONE_CURVE_REFERENCE);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO10, L"GT7 Adapt", TONE_CURVE_GT7_ADAPT);
	{
		static const struct { int type; const wchar_t* name; } kLegacyCurves[] = {
			{ 1, L"ACES" }, { 2, L"Reinhard" }, { 3, L"Hable" }, { 4, L"Movius" },
			{ 5, L"BT2390/ST 2094-10" }, { 6, L"ST 2094-10 (experimental)" },
			{ 8, L"GT7" }, { 9, L"Spline" }, { 11, L"Dynamic & Auto" },
		};
		const int stored = m_SetsPP.iHdrLocalToneMappingType;
		if (stored != TONE_CURVE_REFERENCE && stored != TONE_CURVE_GT7_ADAPT) {
			for (const auto& c : kLegacyCurves) {
				if (c.type == stored) {
					const std::wstring label = std::wstring(L"(legacy) ") + c.name;
					ComboBox_AddStringData(m_hWnd, IDC_COMBO10, label.c_str(), c.type);
					break;
				}
			}
		}
	}

	ComboBox_AddStringData(m_hWnd, IDC_COMBO12, L"BT.709 (standard)", 0);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO12, L"BT.2020 (wide)", 1);

	ComboBox_AddStringData(m_hWnd, IDC_COMBO13, L"Off", 0);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO13, L"Chroma scale", 1);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO13, L"Fade ratio", 2);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO13, L"Knee crossing", 3);
	ComboBox_AddStringData(m_hWnd, IDC_COMBO13, L"Ceiling crossing", 4);

	SendDlgItemMessageW(IDC_COMBO11, CB_ADDSTRING, 0, (LPARAM)L"Disable");
	SendDlgItemMessageW(IDC_COMBO11, CB_ADDSTRING, 0, (LPARAM)L"Manual");
	SendDlgItemMessageW(IDC_COMBO11, CB_ADDSTRING, 0, (LPARAM)L"Auto");

	SetControls();

	SetCursor(m_hWnd, IDC_ARROW);
	SetCursor(m_hWnd, IDC_COMBO1, IDC_HAND);

	AddHint(IDC_CHECK5,
		L"It works fast, but it's not always good.\n"
		"Disable it if you want to use shaders for resizing.");
	AddHint(IDC_COMBO8,
		L"Available for Direct3D 11.\n"
		"Requires hardware and driver support:\n"
		"- Intel Graphics UHD 610 or later\n"
		"- Nvidia RTX (x64 only)");
	AddHint(IDC_CHECK19,
		L"Available for Direct3D 11.\n"
		"Requires hardware and driver support:\n"
		"- Nvidia RTX (x64 only)");
	AddHint(IDC_COMBO5,
		L"Used for YUV 4:2:0/4:2:2 input formats\n"
		"when the DVXA2/D3D11 Video Processor is not active.\n"
		"KrigBilateral (Shiandow) is the highest quality, DX11-only.");
	AddHint(IDC_COMBO2,
		L"Used to increase image size when the\n"
		"DVXA2/D3D11 Video Processor is not used for resizing.");
	AddHint(IDC_COMBO3,
		L"Used to reduce image size when the\n"
		"DVXA2/D3D11 Video Processor is not used for resizing.");
	AddHint(IDC_COMBO4,
		L"'Flip' is more efficient, but 'Discard' may work\n"
		"more correctly in some rare situations.");
	AddHint(IDC_COMBO10,
		L"Unplugged - the house curve; respects Dolby Vision mastering.\n"
		"GT7 Adapt - for HDR10/HLG; blends the declared peak with what\n"
		"each scene measures. The trims below work on both curves.");
	AddHint(IDC_SLIDER14,
		L"The content peak the curve maps FROM. Leave 1000 unless the\n"
		"disc's MaxCLL is known.");
	AddHint(IDC_SLIDER15,
		L"Measured black of the display. At 0 the curve assumes perfect\n"
		"black and wastes shadow detail on range the panel cannot show.");
	AddHint(IDC_SLIDER16,
		L"Pulls colors crowded against the BT.709 edge inward.\n"
		"1.00 = off; ~0.55 measures like reference software on deep skies.");
	AddHint(IDC_SLIDER17,
		L"Highlight desaturation linked to the delivered level.\n"
		"Runs inside the grade, so it needs Enable ML grade. 0 = off.");
	AddHint(IDC_SLIDER18,
		L"Input gain in thirds of a stop (+3 = one stop), absorbed by the\n"
		"shoulder - brightness without spending the highlights.");
	AddHint(IDC_SLIDER19,
		L"Positive spreads dark values apart and reveals their texture\n"
		"(detail, not a lift); negative densifies the black end.");
	AddHint(IDC_SLIDER20,
		L"Symmetric S-curve around mid grey: black, mid and white stay put.");
	AddHint(IDC_SLIDER25,
		L"LOWER = longer shoulder = more headroom before bright detail\n"
		"merges to white. The first dial to turn down if explosions clip.");
	AddHint(IDC_CHECK37,
		L"Solves shadow, gamma and highlight together onto fixed house\n"
		"levels each frame. While on, the legacy Shadow band and the\n"
		"Gamma top-up stand down - the controller owns those actuators.");
	AddHint(IDC_SLIDER27,
		L"Where the scene average should land (0 = off). The brightness\n"
		"dial that does not cost the clouds.");

	m_bActivated = true;

	// poll the live Zion ML state for the stats box
	SetTimer(1001, 500);

	InitMLSliders();

	return S_OK;
}

INT_PTR CVRMainPPage::OnReceiveMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == WM_TIMER && wParam == 1001) {
		// refresh live Zion ML values
		if (m_pVideoRenderer) {
			std::wstring str;
			m_pVideoRenderer->GetMLStats(str);
			GetDlgItem(IDC_EDIT6).SetWindowTextW(str.c_str());
		}
		return (LRESULT)1;
	}

	if (uMsg == WM_COMMAND) {
		LRESULT lValue;
		const int nID = LOWORD(wParam);
		int action = HIWORD(wParam);

		if (action == BN_CLICKED) {
			if (nID == IDC_CHECK1) {
				m_SetsPP.bUseD3D11 = IsDlgButtonChecked(IDC_CHECK1) == BST_CHECKED;
				EnableControls();
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK2) {
				m_SetsPP.bShowStats = IsDlgButtonChecked(IDC_CHECK2) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK3) {
				m_SetsPP.bDeintDouble = IsDlgButtonChecked(IDC_CHECK3) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK5) {
				m_SetsPP.bVPScaling = IsDlgButtonChecked(IDC_CHECK5) == BST_CHECKED;
				SetDirty();
				GetDlgItem(IDC_STATIC7).EnableWindow(m_SetsPP.bVPScaling && m_SetsPP.bUseD3D11 && IsWindows10OrGreater());
				GetDlgItem(IDC_COMBO8).EnableWindow(m_SetsPP.bVPScaling && m_SetsPP.bUseD3D11 && IsWindows10OrGreater());
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK6) {
				m_SetsPP.bInterpolateAt50pct = IsDlgButtonChecked(IDC_CHECK6) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK7) {
				m_SetsPP.VPFmts.bNV12 = IsDlgButtonChecked(IDC_CHECK7) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK8) {
				m_SetsPP.VPFmts.bP01x = IsDlgButtonChecked(IDC_CHECK8) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK9) {
				m_SetsPP.VPFmts.bYUY2 = IsDlgButtonChecked(IDC_CHECK9) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK4) {
				m_SetsPP.VPFmts.bOther = IsDlgButtonChecked(IDC_CHECK4) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK10) {
				m_SetsPP.bUseDither = IsDlgButtonChecked(IDC_CHECK10) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK17) {
				m_SetsPP.bDeintBlend = IsDlgButtonChecked(IDC_CHECK17) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK11) {
				m_SetsPP.bExclusiveFS = IsDlgButtonChecked(IDC_CHECK11) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK15) {
				m_SetsPP.bVBlankBeforePresent = IsDlgButtonChecked(IDC_CHECK15) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK13) {
				m_SetsPP.bAdjustPresentTime = IsDlgButtonChecked(IDC_CHECK13) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK16) {
				m_SetsPP.bReinitByDisplay = IsDlgButtonChecked(IDC_CHECK16) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK18) {
				m_SetsPP.bHdrPreferDoVi = IsDlgButtonChecked(IDC_CHECK18) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK14) {
				m_SetsPP.bConvertToSdr = IsDlgButtonChecked(IDC_CHECK14) == BST_CHECKED;
				EnableControls();
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK19) {
				m_SetsPP.bVPRTXVideoHDR = IsDlgButtonChecked(IDC_CHECK19) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK20) {
				m_SetsPP.bMLEnabled = IsDlgButtonChecked(IDC_CHECK20) == BST_CHECKED;
				EnableControls();
				m_pVideoRenderer->SetSettings(m_SetsPP); // live: apply immediately
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK21) {
				m_SetsPP.bMLDebugCopy = IsDlgButtonChecked(IDC_CHECK21) == BST_CHECKED;
				m_pVideoRenderer->SetSettings(m_SetsPP);
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK22) {
				m_SetsPP.bMLDebugLog = IsDlgButtonChecked(IDC_CHECK22) == BST_CHECKED;
				m_pVideoRenderer->SetSettings(m_SetsPP);
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK35) {
				m_SetsPP.bTrayIcon = IsDlgButtonChecked(IDC_CHECK35) == BST_CHECKED;
				m_pVideoRenderer->SetSettings(m_SetsPP);
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK23) {
				m_SetsPP.bMLHuntEnabled = IsDlgButtonChecked(IDC_CHECK23) == BST_CHECKED;
				EnableControls();
				m_pVideoRenderer->SetSettings(m_SetsPP);
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK24) {
				m_SetsPP.bMLChromaEnabled = IsDlgButtonChecked(IDC_CHECK24) == BST_CHECKED;
				EnableControls();
				m_pVideoRenderer->SetSettings(m_SetsPP);
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK32) {
				m_SetsPP.bMLToneCurveEnabled = IsDlgButtonChecked(IDC_CHECK32) == BST_CHECKED;
				EnableControls();
				m_pVideoRenderer->SetSettings(m_SetsPP);
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_CHECK25 || nID == IDC_CHECK26 || nID == IDC_CHECK27 ||
				nID == IDC_CHECK28 || nID == IDC_CHECK29 || nID == IDC_CHECK30 || nID == IDC_CHECK31) {
				const bool bChecked = IsDlgButtonChecked(nID) == BST_CHECKED;
				switch (nID) {
				case IDC_CHECK25: m_SetsPP.bMLGammaEnabled       = bChecked; break;
				case IDC_CHECK26: m_SetsPP.bMLShadowEnabled      = bChecked; break;
				case IDC_CHECK27: m_SetsPP.bMLHighlightEnabled   = bChecked; break;
				case IDC_CHECK28: m_SetsPP.bMLChromaNeutralEnabled = bChecked; break;
				case IDC_CHECK29: m_SetsPP.bMLChromaFireEnabled   = bChecked; break;
				case IDC_CHECK30: m_SetsPP.bMLChromaKneeEnabled   = bChecked; break;
				case IDC_CHECK31: m_SetsPP.bMLChromaSkinEnabled   = bChecked; break;
				}
				EnableControls();
				m_pVideoRenderer->SetSettings(m_SetsPP);
				SetDirty();
				return (LRESULT)1;
			}

			if (nID == IDC_CHECK33 || nID == IDC_CHECK36 || nID == IDC_CHECK37 ||
				nID == IDC_CHECK39 || nID == IDC_CHECK40 || nID == IDC_CHECK41 ||
				nID == IDC_CHECK42) {
				const bool bChecked = IsDlgButtonChecked(nID) == BST_CHECKED;
				switch (nID) {
				case IDC_CHECK33: m_SetsPP.bMLContrastEnabled = bChecked; break; // micro contrast
				case IDC_CHECK36: m_SetsPP.bHdrAutoRefWhite   = bChecked; break; // auto exposure
				case IDC_CHECK37: m_SetsPP.bMLController      = bChecked; break; // band controller
				case IDC_CHECK39: m_SetsPP.bMLBars            = bChecked; break; // repaint bars
				case IDC_CHECK40: m_SetsPP.bMLBarsHighlight   = bChecked; break; // highlight bars
				case IDC_CHECK41: m_SetsPP.bMLSkyEnabled      = bChecked; break; // sky seg
				case IDC_CHECK42: m_SetsPP.bMLPersonEnabled   = bChecked; break; // person skin
				}
				EnableControls();
				m_pVideoRenderer->SetSettings(m_SetsPP); // live: gate changes show at once
				SetDirty();
				return (LRESULT)1;
			}

			if (nID == IDC_BUTTON1) {
				m_SetsPP.SetDefault();
				SetControls();
				EnableControls();
				SetDirty();
				return (LRESULT)1;
			}
		}

		if (action == CBN_SELCHANGE) {
			if (nID == IDC_COMBO6) {
				lValue = SendDlgItemMessageW(IDC_COMBO6, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iResizeStats) {
					m_SetsPP.iResizeStats = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO1) {
				lValue = ComboBox_GetCurItemData(m_hWnd, IDC_COMBO1);
				if (lValue != m_SetsPP.iTexFormat) {
					m_SetsPP.iTexFormat = lValue;
					SetDirty();
#ifdef _WIN64
					GetDlgItem(IDC_CHECK19).EnableWindow(m_SetsPP.bUseD3D11 && m_SetsPP.bHdrPassthrough && m_SetsPP.iTexFormat != TEXFMT_8INT);
#endif
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO9) {
				lValue = SendDlgItemMessageW(IDC_COMBO9, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iVPDeinterlacing) {
					m_SetsPP.iVPDeinterlacing = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO8) {
				lValue = SendDlgItemMessageW(IDC_COMBO8, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iVPSuperRes) {
					m_SetsPP.iVPSuperRes = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO7) {
				lValue = SendDlgItemMessageW(IDC_COMBO7, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iHdrToggleDisplay) {
					m_SetsPP.iHdrToggleDisplay = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO5) {
				lValue = SendDlgItemMessageW(IDC_COMBO5, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iChromaScaling) {
					m_SetsPP.iChromaScaling = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO2) {
				lValue = SendDlgItemMessageW(IDC_COMBO2, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iUpscaling) {
					m_SetsPP.iUpscaling = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO3) {
				lValue = SendDlgItemMessageW(IDC_COMBO3, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iDownscaling) {
					m_SetsPP.iDownscaling = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO4) {
				lValue = SendDlgItemMessageW(IDC_COMBO4, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iSwapEffect) {
					m_SetsPP.iSwapEffect = lValue;
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO10) {
				// The item data IS the engine curve value — the old switch read the
				// position, so every edit of the list silently remapped which curve
				// each entry chose.
				lValue = ComboBox_GetCurItemData(m_hWnd, IDC_COMBO10);
				if (lValue != CB_ERR) {
					m_SetsPP.bHdrPassthrough      = (lValue == 0);
					m_SetsPP.bHdrLocalToneMapping = (lValue > 0);
					if (lValue > 0) {
						m_SetsPP.iHdrLocalToneMappingType = (int)lValue;
					}
					SetDirty();
					EnableControls();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO12) {
				lValue = ComboBox_GetCurItemData(m_hWnd, IDC_COMBO12);
				if (lValue != m_SetsPP.iOutputGamut) {
					m_SetsPP.iOutputGamut = (int)lValue;
					SetDirty();
					EnableControls(); // gamut capacity acts only inside BT.709
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO13) {
				lValue = ComboBox_GetCurItemData(m_hWnd, IDC_COMBO13);
				if ((float)lValue != m_SetsPP.fHdrGt7Dbg) {
					// A debug view is for looking at - it must apply without Apply.
					m_SetsPP.fHdrGt7Dbg = (float)lValue;
					m_pVideoRenderer->SetSettings(m_SetsPP); // live
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_COMBO11) {
				lValue = SendDlgItemMessageW(IDC_COMBO11, CB_GETCURSEL, 0, 0);
				if (lValue != m_SetsPP.iMLMode) {
					m_SetsPP.iMLMode = lValue;
					EnableControls();
					m_pVideoRenderer->SetSettings(m_SetsPP); // live: apply immediately
					SetDirty();
				}
				return (LRESULT)1;
			}
		}
		if (action == EN_CHANGE) {
			if (nID == IDC_EDIT_DISPLAYMAX) {
				SetDirty();
			}
			if (nID == IDC_EDIT3 || nID == IDC_EDIT4 || nID == IDC_EDIT5 ||
				nID == IDC_EDIT7 || nID == IDC_EDIT8 || nID == IDC_EDIT9 ||
				nID == IDC_EDIT10 || nID == IDC_EDIT11) {
				// Zion ML value edits: live-apply on every keystroke, clamping
				// out-of-range entries to the valid range instead of rejecting
				// them (so typing "3" visibly applies 1.5 rather than doing
				// nothing).
				float* pDst = nullptr;
				float def = 0.f, lo = 0.f, hi = 0.f;
				if (nID == IDC_EDIT3) { pDst = &m_SetsPP.fMLGamma; def = 1.0f; lo = 0.5f;  hi = 1.5f; }
				if (nID == IDC_EDIT4) { pDst = &m_SetsPP.fMLShadow; def = 2.0f; lo = 0.0f;  hi = 5.0f; }
				if (nID == IDC_EDIT5) { pDst = &m_SetsPP.fMLHighlight; def = 0.0f; lo = -0.3f; hi = 0.4f; }
				if (nID == IDC_EDIT7) { pDst = &m_SetsPP.fMLHunt; def = 0.80f; lo = 0.0f; hi = 1.0f; }
				if (nID == IDC_EDIT8) { pDst = &m_SetsPP.fMLChromaNeutral; def = 1.20f; lo = 1.0f; hi = 1.5f; }
				if (nID == IDC_EDIT9) { pDst = &m_SetsPP.fMLChromaFire; def = 1.35f; lo = 1.0f; hi = 1.5f; }
				if (nID == IDC_EDIT10) { pDst = &m_SetsPP.fMLChromaKnee; def = 0.55f; lo = 0.1f; hi = 0.9f; }
				if (nID == IDC_EDIT11) { pDst = &m_SetsPP.fMLChromaSkinProtect; def = 0.95f; lo = 0.5f; hi = 1.0f; }
				WCHAR buffer[32];
				::GetDlgItemTextW(m_hWnd, nID, buffer, _countof(buffer));
				const float v = (float)_wtof(buffer);
				if (pDst && buffer[0]) {
					*pDst = discard<float>(v, def, lo, hi);
					m_pVideoRenderer->SetSettings(m_SetsPP); // live: apply immediately
				}
				SetDirty();
			}
		}
	}
	else if (uMsg == WM_HSCROLL) {
		if ((HWND)lParam == GetDlgItem(IDC_SLIDER1)) {
			LRESULT lValue = SendDlgItemMessageW(IDC_SLIDER1, TBM_GETPOS, 0, 0);
			if (lValue != m_SetsPP.iHdrOsdBrightness) {
				m_SetsPP.iHdrOsdBrightness = lValue;
				SetDirty();
			}
			return (LRESULT)1;
		}
		if ((HWND)lParam == GetDlgItem(IDC_SLIDER2)) {
			LRESULT lValue = SendDlgItemMessageW(IDC_SLIDER2, TBM_GETPOS, 0, 0);
			lValue *= SDR_NITS_STEP;
			if (lValue != m_SetsPP.iSDRDisplayNits) {
				m_SetsPP.iSDRDisplayNits = lValue;
				GetDlgItem(IDC_EDIT1).SetWindowTextW(std::to_wstring(m_SetsPP.iSDRDisplayNits).c_str());
				SetDirty();
				{
					// apply only SDRDisplayNits
					Settings_t sets;
					m_pVideoRenderer->GetSettings(sets);
					sets.iSDRDisplayNits = m_SetsPP.iSDRDisplayNits;
					m_pVideoRenderer->SetSettings(sets);
				}
			}
			return (LRESULT)1;
		}

		// Zion ML sliders — live-apply on every move
		for (const auto& r : kMLSliders) {
			if ((HWND)lParam == GetDlgItem(r.nSlider)) {
				const float v = MLSliderValue((int)SendDlgItemMessageW(r.nSlider, TBM_GETPOS, 0, 0), r);
				m_SetsPP.*(r.pField) = v;
				SetDlgItemTextW(r.nEdit, std::to_wstring(v).c_str());
				m_pVideoRenderer->SetSettings(m_SetsPP);
				SetDirty();
				return (LRESULT)1;
			}
		}
		// Right-column sliders — same live-apply grammar
		for (const auto& r : kGradeSliders) {
			if ((HWND)lParam == GetDlgItem(r.nSlider)) {
				const float v = MLSliderValue((int)SendDlgItemMessageW(r.nSlider, TBM_GETPOS, 0, 0), r);
				m_SetsPP.*(r.pField) = v;
				if (r.nSlider == IDC_SLIDER23) {
					// One control for the fade window: the width stays 0.18, as the
					// engine's own parser derives it — no inverted pairs by hand.
					m_SetsPP.fHdrGt7FadeEnd = v + 0.18f;
				}
				SetDlgItemTextW(r.nEdit, FmtSlider(v).c_str());
				m_pVideoRenderer->SetSettings(m_SetsPP);
				SetDirty();
				return (LRESULT)1;
			}
		}
		for (const auto& r : kIntSliders) {
			if ((HWND)lParam == GetDlgItem(r.nSlider)) {
				const int v = (int)SendDlgItemMessageW(r.nSlider, TBM_GETPOS, 0, 0) + r.lo;
				m_SetsPP.*(r.pField) = v;
				SetDlgItemTextW(r.nEdit, std::to_wstring(v).c_str());
				m_pVideoRenderer->SetSettings(m_SetsPP);
				SetDirty();
				return (LRESULT)1;
			}
		}
	}

	// Let the parent class handle the message.
	return CBasePropertyPage::OnReceiveMessage(hwnd, uMsg, wParam, lParam);
}

HRESULT CVRMainPPage::OnApplyChanges()
{
	BOOL translated = FALSE;
	int displayMaxNits = GetDlgItemInt(IDC_EDIT_DISPLAYMAX, &translated, FALSE);
	if (!translated) {
		MessageBoxW(L"Invalid HDR Brightness. Please enter a valid number from 100 to 10000.", L"Error", MB_OK | MB_ICONERROR);
	}
	else if (displayMaxNits <= HDR_NITS_MIN || displayMaxNits > HDR_NITS_MAX) {
		MessageBoxW(L"Invalid HDR Brightness. Please enter a valid number from 100 to 10000.", L"Error", MB_OK | MB_ICONERROR);
	}
	else {
		m_SetsPP.iHdrDisplayMaxNits = displayMaxNits;
	}

	// Zion ML manual values
	auto readFloat = [](HWND hWnd, int idEdit, float& dst, float def, float lo, float hi) {
		WCHAR buffer[32];
		::GetDlgItemTextW(hWnd, idEdit, buffer, _countof(buffer));
		if (buffer[0]) {
			dst = discard<float>((float)_wtof(buffer), def, lo, hi);
		}
	};
	readFloat(m_hWnd, IDC_EDIT3, m_SetsPP.fMLGamma, 1.0f, 0.5f, 1.5f);
	readFloat(m_hWnd, IDC_EDIT4, m_SetsPP.fMLShadow, 2.0f, 0.0f, 5.0f);
	readFloat(m_hWnd, IDC_EDIT5, m_SetsPP.fMLHighlight, 0.0f, -0.3f, 0.4f);

	m_pVideoRenderer->SetSettings(m_SetsPP);
	m_pVideoRenderer->SaveSettings();

	m_oldSDRDisplayNits = m_SetsPP.iSDRDisplayNits;

	return S_OK;
}

HWND CVRMainPPage::CreateHintWindow(HWND parent, int timePop, int timeInit, int timeReshow)
{
	HWND hhint = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr,
		WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, parent, nullptr, nullptr, nullptr);

	SetWindowPos(hhint, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	SendMessageW(hhint, TTM_SETDELAYTIME, TTDT_AUTOPOP, MAKELONG(timePop, 0));
	SendMessageW(hhint, TTM_SETDELAYTIME, TTDT_INITIAL, MAKELONG(timeInit, 0));
	SendMessageW(hhint, TTM_SETDELAYTIME, TTDT_RESHOW, MAKELONG(timeReshow, 0));
	SendMessageW(hhint, TTM_SETMAXTIPWIDTH, 0, 470);
	return hhint;
}

void CVRMainPPage::AddHint(int id, const LPCWSTR text)
{
	if (!m_hHint) {
		m_hHint = CreateHintWindow(m_Dlg, 15000);
	}
	TOOLINFOW ti;
	ti.cbSize = sizeof(TOOLINFOW);
	ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
	ti.hwnd = m_Dlg;
	ti.uId = (LPARAM)GetDlgItem(id).m_hWnd;
	ti.lpszText = const_cast<LPWSTR>(text);
	SendMessageW(m_hHint, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

// CVRInfoPPage

CVRInfoPPage::CVRInfoPPage(LPUNKNOWN lpunk, HRESULT* phr) :
	CBasePropertyPage(L"InfoProp", lpunk, IDD_INFOPROPPAGE, IDS_INFOPROPPAGE_TITLE)
{
	DLog(L"CVRInfoPPage()");
}

CVRInfoPPage::~CVRInfoPPage()
{
	DLog(L"~CVRInfoPPage()");

	if (m_hMonoFont) {
		DeleteObject(m_hMonoFont);
		m_hMonoFont = 0;
	}
}

HRESULT CVRInfoPPage::OnConnect(IUnknown *pUnk)
{
	if (pUnk == nullptr) return E_POINTER;

	m_pVideoRenderer = pUnk;
	if (!m_pVideoRenderer) {
		return E_NOINTERFACE;
	}

	return S_OK;
}

HRESULT CVRInfoPPage::OnDisconnect()
{
	if (m_pVideoRenderer == nullptr) {
		return E_UNEXPECTED;
	}

	m_pVideoRenderer.Release();

	return S_OK;
}

HWND GetParentOwner(HWND hwnd)
{
	HWND hWndParent = hwnd;
	HWND hWndT;
	while ((::GetWindowLongPtrW(hWndParent, GWL_STYLE) & WS_CHILD) &&
		(hWndT = ::GetParent(hWndParent)) != NULL) {
		hWndParent = hWndT;
	}

	return hWndParent;
}

static WNDPROC OldControlProc;
static LRESULT CALLBACK ControlProc(HWND control, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_KEYDOWN && LOWORD(wParam) == VK_ESCAPE) {
		// fixed Esc handling when EDITTEXT control has ES_MULTILINE property and is in focus
		HWND parentOwner = GetParentOwner(control);
		if (parentOwner) {
			::PostMessageW(parentOwner, WM_COMMAND, IDCANCEL, 0);
		}
		return TRUE;
	}

	return CallWindowProcW(OldControlProc, control, message, wParam, lParam); // call edit control's own windowproc
}

HRESULT CVRInfoPPage::OnActivate()
{
	// set m_hWnd for CWindow
	m_hWnd = m_hwnd;

	SetDlgItemTextW(IDC_EDIT2, GetNameAndVersion());

	// init monospace font
	LOGFONTW lf = {};
	HDC hdc = GetWindowDC();
	lf.lfHeight = -MulDiv(9, GetDeviceCaps(hdc, LOGPIXELSY), 72);
	ReleaseDC(hdc);
	lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
	wcscpy_s(lf.lfFaceName, L"Consolas");
	m_hMonoFont = CreateFontIndirectW(&lf);

	GetDlgItem(IDC_EDIT1).SetFont(m_hMonoFont);
	ASSERT(m_pVideoRenderer);

	if (!m_pVideoRenderer->GetActive()) {
		SetDlgItemTextW(IDC_EDIT1, L"filter is not active");
		return S_OK;
	}

	std::wstring strInfo(L"Windows ");
	strInfo.append(GetWindowsVersion());
	strInfo.append(L"\r\n");

	std::wstring strVP;
	if (S_OK == m_pVideoRenderer->GetVideoProcessorInfo(strVP)) {
		str_replace(strVP, L"\n", L"\r\n");
		strInfo.append(strVP);
	}

#ifdef _DEBUG
	{
		std::vector<DisplayConfig_t> displayConfigs;

		bool ret = GetDisplayConfigs(displayConfigs);

		strInfo.append(L"\r\n");

		for (const auto& dc : displayConfigs) {
			double freq = (double)dc.refreshRate.Numerator / (double)dc.refreshRate.Denominator;
			strInfo += std::format(L"\r\n{} - {:.3f} Hz", dc.displayName, freq);

			if (dc.bitsPerChannel) { // if bitsPerChannel is not set then colorEncoding and other values are invalid
				const wchar_t* colenc = ColorEncodingToString(dc.colorEncoding);
				if (colenc) {
					strInfo += std::format(L" {}", colenc);
				}
				strInfo += std::format(L" {}-bit", dc.bitsPerChannel);
			}

			const wchar_t* output = OutputTechnologyToString(dc.outputTechnology);
			if (output) {
				strInfo += std::format(L" {}", output);
			}
		}
	}
#endif

	SetDlgItemTextW(IDC_EDIT1, strInfo.c_str());

	OldControlProc = (WNDPROC)::SetWindowLongPtrW(::GetDlgItem(m_hWnd, IDC_EDIT1), GWLP_WNDPROC, (LONG_PTR)ControlProc);

	return S_OK;
}
