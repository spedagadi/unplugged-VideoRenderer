/*
 * (C) 2020-2026 see Authors.txt
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

#include <evr9.h>
#include "DisplayConfig.h"
#include "FrameStats.h"
#include "IVideoRenderer.h"
#include "SubPic/ISubPic.h"

enum : int {
	VP_DX9 = 9,
	VP_DX11 = 11
};

enum : int {
	STEREO3D_AsIs = 0,
	STEREO3D_HalfOverUnder_to_Interlace,
};

class CMpcVideoRenderer;

class CVideoProcessor
	: public IMFVideoProcessor
	, public IMFVideoMixerBitmap
{
protected:
	long m_nRefCount = 1;
	CMpcVideoRenderer* m_pFilter = nullptr;

	// Settings
	bool m_bShowStats                      = false;
	int  m_iResizeStats                    = 0;
	int  m_iTexFormat                      = TEXFMT_AUTOINT;
	VPEnableFormats_t m_VPFormats          = {true, true, true, true};
	int  m_iVPDeinterlacing                = DEINT_Enable;
	bool m_bDeintDouble                    = true;
	bool m_bVPScaling                      = true;
	int  m_iChromaScaling                  = CHROMA_Bilinear;
	int  m_iUpscaling                      = UPSCALE_CatmullRom; // interpolation
	int  m_iDownscaling                    = DOWNSCALE_Hamming;  // convolution
	bool m_bInterpolateAt50pct             = true;
	bool m_bUseDither                      = true;
	bool m_bDeintBlend                     = false;
	int  m_iSwapEffect                     = SWAPEFFECT_Flip;
	bool m_bVBlankBeforePresent            = false;
	bool m_bAdjustPresentTime              = true;
	bool m_bHdrPreferDoVi                  = false;
	bool m_bHdrPassthrough                 = true;
	bool m_bHdrLocalToneMapping            = false;
	int  m_iHdrLocalToneMappingType        = 0;
	int  m_iHdrDisplayMaxNits              = 1000;
	float m_fHdrDisplayMinNits             = 0.0f;   // display black, nits (0 = assume ideal black)
	int   m_iOutputGamut                   = 0;      // 0 = BT.709 SDR out, 1 = keep BT.2020
	// Content peak, nits — the top of the range the tone curves map from. Distinct
	// from m_iHdrDisplayMaxNits (the display's peak). Falls back to the HDR10
	// nominal 1000 when the source carries no metadata, which is always on the
	// DeckLink capture path.
	float m_fHdrTitlePeakNits              = 1000.0f;
	// Hable knee: display-relative light below which Hable is replaced by identity.
	// 0 = pure Hable. See the full rationale in IVideoRenderer.h.
	float m_fHdrHableKnee                  = 0.0f;
	bool m_bHDRPassthroughRaw             = false; // demo: show raw HDR/DV on SDR

	// Zion ML grade
	bool  m_bML                           = false;
	int   m_iMLMode                       = ML_Disable;
	float m_fMLGamma                      = 1.0f;
	float m_fMLShadow                     = 0.0f;
	float m_fMLHighlight                  = 0.0f;
	int  m_iHdrToggleDisplay               = HDRTD_Disabled;
	int  m_iHdrOsdBrightness               = 0;
	bool m_bConvertToSdr                   = true;
	int  m_iSDRDisplayNits                 = SDR_NITS_DEF;

	bool m_bVPScalingUseShaders = false;

	CopyFrameDataFn m_pCopyPlaneFn = CopyPlaneAsIs;
	CopyFrameDataFn m_pCopyGpuFn   = CopyPlaneAsIs;

	// Input parameters
	FmtConvParams_t m_srcParams = GetFmtConvParams(CF_NONE);
	UINT  m_srcWidth        = 0;
	UINT  m_srcHeight       = 0;
	UINT  m_srcRectWidth    = 0;
	UINT  m_srcRectHeight   = 0;
	int   m_srcPitch        = 0;
	UINT  m_srcLines        = 0;
	DWORD m_srcAspectRatioX = 0;
	DWORD m_srcAspectRatioY = 0;
	bool  m_srcAnamorphic   = false;
	CRect m_srcRect;
	DXVA2_ExtendedFormat m_decExFmt = {};
	DXVA2_ExtendedFormat m_srcExFmt = {};
	// Content-side ST 2086 snapshot, display-independent. Maintained in
	// CopySample from the decoded side data whenever a value is non-zero, and
	// reported in the pipeline dump (cll=/fall=/mmax=/mmin=). Distinct from
	// m_lastHdr10, which is only written when the swapchain is an HDR10
	// DXGI_SWAP_CHAIN4 — on an SDR display that never happens and it stays
	// zero, which silently blanked the panel's MaxCLL/Mastering rows
	// (2026-09-23: sdata=7 proved the side data arrived; the dump read the
	// display-applied copy instead of the stream copy). Plain ints on purpose:
	// this base header must not pull DXGI HDR types.
	UINT m_hdr10Cll  = 0;   // MaxContentLightLevel, cd/m²
	UINT m_hdr10Fall = 0;   // MaxFrameAverageLightLevel, cd/m²
	UINT m_hdr10Mmax = 0;   // MaxMasteringLuminance, cd/m²
	UINT m_hdr10Mmin = 0;   // MinMasteringLuminance, 0.0001 cd/m² (DXGI_HDR_METADATA scale)
	// RAW CLL blob as LAV delivered it (diagnostic): -1 = GUID absent, else the
	// bytes/size exactly as read. Reported as rcll=/rfall=/cllsz= in the pipeline
	// dump so a 0 MaxCLL row can be told apart from "LAV sent zeros", "read
	// skipped (size mismatch)" and "GUID never arrived".
	int m_rawCll = -1;
	int m_rawFall = -1;
	int m_rawCllSize = -1;
	// Side-data presence probe (bitmap): which IMediaSideData GUIDs the decoded
	// samples actually carry. bit0=IID_MediaSideDataHDR (static mastering),
	// bit1=...HDRContentLightLevel (CLL), bit2=...HDR10Plus (RPU),
	// bit3=...DOVIMetadataV2. Logged in the pipeline dump (sdata=N) so a blank
	// MaxCLL/MaxFALL/Mastering row can be told apart from "decoder sent nothing".
	int m_sideData = 0;
	bool  m_bInterlaced = false;
	REFERENCE_TIME m_rtAvgTimePerFrame = 0;

	CRect m_videoRect;
	CRect m_windowRect;
	CRect m_renderRect;

	int  m_iRotation   = 0;
	bool m_bFlip       = false;
	int  m_iStereo3dTransform = 0;
	bool m_bFinalPass  = false;
	bool m_bDitherUsed = false;

	DXVA2_ValueRange m_DXVA2ProcAmpRanges[4] = {};
	DXVA2_ProcAmpValues m_DXVA2ProcAmpValues = {};

	struct DOVIMetadata {
		MediaSideDataDOVIMetadata msd = {};
		bool bValid = false;
		bool bHasMMR = false;
	} m_Dovi;

	bool CheckDoviMetadata(const MediaSideDataDOVIMetadata* pDOVIMetadata, const uint8_t maxReshapeMethon);

	HWND m_hWnd = nullptr;
	UINT m_nCurrentAdapter = {}; // redefine explicitly in subclasses
	DWORD m_VendorId = 0;
	std::wstring m_strAdapterDescription;

	REFERENCE_TIME m_rtStart = 0;
	int m_FieldDrawn = 0;
	bool m_bDoubleFrames = false;

	UINT32 m_uHalfRefreshPeriodMs = 0;

	bool m_bAllowDeepColorBitmaps = false;

	// AlphaBitmap
	bool m_bAlphaBitmapEnable = false;
	RECT m_AlphaBitmapRectSrc = {};
	MFVideoNormalizedRect m_AlphaBitmapNRectDest = {};

	// Statistics
	CRenderStats m_RenderStats;
	std::wstring m_strStatsHeader;
	std::wstring m_strStatsInputFmt;
	std::wstring m_strStatsVProc;
	std::wstring m_strStatsDispInfo;
	//std::wstring m_strStatsPostProc;
	std::wstring m_strStatsHDR;
	std::wstring m_strStatsPresent;
	int m_iSrcFromGPU = 0;
	const wchar_t* m_strShaderX = nullptr;
	const wchar_t* m_strShaderY = nullptr;
	int m_StatsFontH = 14;
	RECT m_StatsRect = { 10, 10, 10 + 5 + 63*8 + 3, 10 + 5 + 18*17 + 3 };
	const POINT m_StatsTextPoint = { 10 + 5, 10 + 5};

	// Graph of a function
	CMovingAverage<int> m_Syncs = CMovingAverage<int>(120);
#if SYNC_OFFSET_EX
	CMovingAverage<int> m_SyncDevs = CMovingAverage<int>(m_Syncs.Size()-1);
#endif
	int m_Xstep  = 4;
	int m_Yscale = 2;
	RECT m_GraphRect = {};
	int m_Yaxis  = 0;

	int m_nStereoSubtitlesOffsetInPixels = 4;

	UINT m_MaxDisplayLuminance = 0;

	CVideoProcessor(CMpcVideoRenderer* pFilter) : m_pFilter(pFilter) {}

public:
	virtual ~CVideoProcessor() = default;

	virtual int Type() = 0;

	virtual HRESULT Init(const HWND hwnd, bool displayHdrChanged, bool* pChangeDevice = nullptr) = 0;

	virtual IDirect3DDeviceManager9* GetDeviceManager9() { return nullptr; }
	UINT GetCurrentAdapter() { return m_nCurrentAdapter; }

	virtual BOOL VerifyMediaType(const CMediaType* pmt) = 0;
	virtual BOOL InitMediaType(const CMediaType* pmt) = 0;

	virtual BOOL GetAlignmentSize(const CMediaType& mt, SIZE& Size) = 0;

	virtual HRESULT ProcessSample(IMediaSample* pSample) = 0;
	virtual HRESULT Render(int field, const REFERENCE_TIME frameStartTime) = 0;
	virtual HRESULT FillBlack() = 0;

	void Start() { m_rtStart = 0; }
	virtual void Flush() = 0;
	virtual HRESULT Reset(bool bDisplayModeChange) = 0;

	virtual bool IsInit() const { return false; }

	ColorFormat_t GetColorFormat() { return m_srcParams.cformat; }

	void GetSourceRect(CRect& sourceRect) { sourceRect = m_srcRect; }
	void GetVideoRect(CRect& videoRect) { videoRect = m_videoRect; }
	virtual void SetVideoRect(const CRect& videoRect) = 0;
	virtual HRESULT SetWindowRect(const CRect& windowRect) = 0;

	void GetVideoSize(long& width, long& height);
	void GetAspectRatio(long& aspectX, long& aspectY);

	// Settings
	void SetShowStats(bool value);
	virtual void Configure(const Settings_t& config) = 0;

	int GetRotation() { return m_iRotation; }
	virtual void SetRotation(int value) = 0;
	bool GetFlip() { return m_bFlip; }
	void SetFlip(bool value) { m_bFlip = value; }
	virtual void SetStereo3dTransform(int value) {};
	void SetAllowDeepColorBitmaps(bool value) { m_bAllowDeepColorBitmaps = value; }

	virtual void ClearPreScaleShaders() {};
	virtual void ClearPostScaleShaders() {};

	virtual HRESULT AddPreScaleShader(const std::wstring& name, const std::string& srcCode) { return E_NOTIMPL; };
	virtual HRESULT AddPostScaleShader(const std::wstring& name, const std::string& srcCode) { return E_NOTIMPL; };

	// Display calibration 3D LUT (.cube). Only DX11 has the final-pass stage to
	// host it; DX9 reports E_NOTIMPL so the command is a harmless no-op there.
	virtual HRESULT LoadCubeLut(const char* utf8Path) { return E_NOTIMPL; };
	virtual void SetCubeLutMode(int mode) {};

	virtual HRESULT GetCurentImage(long *pDIBImage) = 0;
	virtual HRESULT GetDisplayedImage(BYTE **ppDib, unsigned *pSize) = 0;
	virtual HRESULT GetVPInfo(std::wstring& str) = 0;

	void UpdateStatsByWindow();
	void UpdateStatsByDisplay();
	bool CheckGraphPlacement();
	void CalcGraphParams();
	virtual void CalcStatsParams() = 0;

	void SetDisplayInfo(const DisplayConfig_t& dc, const bool primary, const bool exclusiveScreen);

	virtual void GetMLStats(std::wstring& str) { str.clear(); }

	// Renderer frame-accounting counters (LIVE panel / PERFORMANCE). IVideoRenderer
	// exposes a ::RenderStats_t; the base returns zeros here and the concrete
	// processors fill real numbers.
	virtual void GetRenderStats(::RenderStats_t& stats)
	{
		stats = ::RenderStats_t{};
	}

	bool GetDoubleRate() { return m_bDoubleFrames; }

	virtual ISubPicAllocator* GetSubPicAllocator() { return nullptr; }

	virtual void SwitchFullScreen(bool set) {};

protected:
	inline bool SourceIsHDR10orHLG() {
		return (m_decExFmt.VideoTransferFunction == MFVideoTransFunc_2084 && m_decExFmt.VideoTransferMatrix != DXVA2_VideoTransferMatrix_Unknown)
			|| m_decExFmt.VideoTransferFunction == MFVideoTransFunc_HLG;
	}
	// source is PQ, HLG or Dolby Vision
	inline bool SourceIsHDR() {
		return SourceIsHDR10orHLG() || m_Dovi.bValid;
	}

	// Check if source is Dolby Vision Full Enhancement Layer
	inline bool SourceIsDoviFel(const MediaSideDataDOVIMetadata* pDOVIMetadata) {
		if (pDOVIMetadata->Header.disable_residual_flag)
			return false;

		if (pDOVIMetadata->Mapping.nlq_method_idc == 0) {
			for (uint8_t i = 0; i < 3; i++) {
				if (pDOVIMetadata->Mapping.nlq[i].nlq_offset != 0 ||
					pDOVIMetadata->Mapping.nlq[i].vdr_in_max != (1ULL << pDOVIMetadata->Header.coef_log2_denom) ||
					pDOVIMetadata->Mapping.nlq[i].linear_deadzone_slope != 0 ||
					pDOVIMetadata->Mapping.nlq[i].linear_deadzone_threshold != 0)
					return true;
			}
		}

		return false;
	}

	void UpdateStatsInputFmt();

	CRefTime m_streamTime;
	void SyncFrameToStreamTime(const REFERENCE_TIME frameStartTime);

public:
	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void **ppv);
	STDMETHODIMP_(ULONG) AddRef();
	STDMETHODIMP_(ULONG) Release();

	// IMFVideoProcessor
	STDMETHODIMP GetAvailableVideoProcessorModes(UINT *lpdwNumProcessingModes, GUID **ppVideoProcessingModes) { return E_NOTIMPL; }
	STDMETHODIMP GetVideoProcessorCaps(LPGUID lpVideoProcessorMode, DXVA2_VideoProcessorCaps *lpVideoProcessorCaps) { return E_NOTIMPL; }
	STDMETHODIMP GetVideoProcessorMode(LPGUID lpMode) { return E_NOTIMPL; }
	STDMETHODIMP SetVideoProcessorMode(LPGUID lpMode) { return E_NOTIMPL; }
	STDMETHODIMP GetProcAmpRange(DWORD dwProperty, DXVA2_ValueRange *pPropRange);
	STDMETHODIMP GetProcAmpValues(DWORD dwFlags, DXVA2_ProcAmpValues *Values);
	STDMETHODIMP GetFilteringRange(DWORD dwProperty, DXVA2_ValueRange *pPropRange) { return E_NOTIMPL; }
	STDMETHODIMP GetFilteringValue(DWORD dwProperty, DXVA2_Fixed32 *pValue) { return E_NOTIMPL; }
	STDMETHODIMP SetFilteringValue(DWORD dwProperty, DXVA2_Fixed32 *pValue) { return E_NOTIMPL; }
	STDMETHODIMP GetBackgroundColor(COLORREF *lpClrBkg);
	STDMETHODIMP SetBackgroundColor(COLORREF ClrBkg) { return E_NOTIMPL; }

	// IMFVideoMixerBitmap
	STDMETHODIMP ClearAlphaBitmap() override;
	STDMETHODIMP GetAlphaBitmapParameters(MFVideoAlphaBitmapParams *pBmpParms) override;

private:
	virtual void UpdateStatsStatic() = 0;
};
