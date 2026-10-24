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

#pragma once

#include "IVideoRenderer.h"

// CVRMainPPage

// New GUID, same reason as the renderer's: these two property pages are registered
// by the same AMovieDllRegisterServer2 pass over g_Templates, so keeping upstream's
// {DA46D181-...} would let a legacy MPCVR registration answer the CLSID and open
// ITS dialog on top of OUR filter -- a settings UI reading the wrong registry key,
// which looks like lost settings rather than a wrong object.
class __declspec(uuid("57EA578F-1187-48F1-8E93-86713589221B"))
	CVRMainPPage : public CBasePropertyPage, public CWindow
{
	CComQIPtr<IVideoRenderer> m_pVideoRenderer;

	Settings_t m_SetsPP;

	int m_oldSDRDisplayNits = SDR_NITS_DEF;

	bool m_bActivated = false;
	HWND m_hHint = nullptr;

public:
	CVRMainPPage(LPUNKNOWN lpunk, HRESULT* phr);
	~CVRMainPPage();

private:
	void SetControls();
	void EnableControls();

	HRESULT OnConnect(IUnknown* pUnknown) override;
	HRESULT OnDisconnect() override;
	HRESULT OnActivate() override;
	void SetDirty() {
		if (m_bActivated && !m_bDirty) {
			m_bDirty = TRUE;
			if (m_pPageSite) {
				m_pPageSite->OnStatusChange(PROPPAGESTATUS_DIRTY);
			}
		}
	}
	INT_PTR OnReceiveMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) override;
	HRESULT OnApplyChanges() override;

	HWND CreateHintWindow(HWND parent, int timePop = 1700, int timeInit = 70, int timeReshow = 7);
	void AddHint(int id, const LPCWSTR text);
};

// CVRHdrPPage — the HDR tab: HDR delivery / Trims / GT7 Adapt.
// Owns the curve-parameter fields it shows; every write into the filter is
// field-grafted (read current settings, copy own fields, write) so this page
// can never clobber a field belonging to the Settings or Zion tab — the
// frame applies pages in order and a full-struct write from a stale page
// snapshot would silently undo another tab's live edits.

class __declspec(uuid("370C7CDB-131B-48F7-8DB5-928531C69BE0"))
	CVRHdrPPage : public CBasePropertyPage, public CWindow
{
	CComQIPtr<IVideoRenderer> m_pVideoRenderer;

	Settings_t m_SetsPP;
	bool m_bActivated = false;
	HWND m_hHint = nullptr;

	void SetControls();
	void EnableControls();
	void InitSliders();
	void SetSliders();
	void ApplyOwn();            // graft own fields into the filter's settings
	void Graft(Settings_t& dst) const;

	HRESULT OnConnect(IUnknown* pUnknown) override;
	HRESULT OnDisconnect() override;
	HRESULT OnActivate() override;
	void SetDirty() {
		if (m_bActivated && !m_bDirty) {
			m_bDirty = TRUE;
			if (m_pPageSite) {
				m_pPageSite->OnStatusChange(PROPPAGESTATUS_DIRTY);
			}
		}
	}
	INT_PTR OnReceiveMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) override;
	HRESULT OnApplyChanges() override;

	HWND CreateHintWindow(HWND parent, int timePop = 1700, int timeInit = 70, int timeReshow = 7);
	void AddHint(int id, const LPCWSTR text);

public:
	CVRHdrPPage(LPUNKNOWN lpunk, HRESULT* phr);
	~CVRHdrPPage();
};

// CVRZionPPage — the Zion tab: tone-engine selector, ML grade, Grade,
// Micro contrast, Bars & segments, debug rows, live stats box.

class __declspec(uuid("25C394F5-94F1-4A54-AFB3-C4169CAFBA11"))
	CVRZionPPage : public CBasePropertyPage, public CWindow
{
	CComQIPtr<IVideoRenderer> m_pVideoRenderer;

	Settings_t m_SetsPP;
	bool m_bActivated = false;
	HWND m_hHint = nullptr;

	void SetControls();
	void EnableControls();
	void InitSliders();
	void SetSliders();
	void ApplyOwn();
	void Graft(Settings_t& dst) const;

	HRESULT OnConnect(IUnknown* pUnknown) override;
	HRESULT OnDisconnect() override;
	HRESULT OnActivate() override;
	void SetDirty() {
		if (m_bActivated && !m_bDirty) {
			m_bDirty = TRUE;
			if (m_pPageSite) {
				m_pPageSite->OnStatusChange(PROPPAGESTATUS_DIRTY);
			}
		}
	}
	INT_PTR OnReceiveMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) override;
	HRESULT OnApplyChanges() override;

	HWND CreateHintWindow(HWND parent, int timePop = 1700, int timeInit = 70, int timeReshow = 7);
	void AddHint(int id, const LPCWSTR text);

public:
	CVRZionPPage(LPUNKNOWN lpunk, HRESULT* phr);
	~CVRZionPPage();
};

// CVRInfoPPage

class __declspec(uuid("43183E16-B1BA-4170-BBF7-6FA989BC1CB9"))
	CVRInfoPPage : public CBasePropertyPage, public CWindow
{
	HFONT m_hMonoFont = nullptr;
	CComQIPtr<IVideoRenderer> m_pVideoRenderer;

public:
	CVRInfoPPage(LPUNKNOWN lpunk, HRESULT* phr);
	~CVRInfoPPage();

private:
	HRESULT OnConnect(IUnknown* pUnknown) override;
	HRESULT OnDisconnect() override;
	HRESULT OnActivate() override;
};
