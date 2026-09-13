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

#include <dxva2api.h>

enum :int {
	TEXFMT_AUTOINT = 0,
	TEXFMT_8INT = 8,
	TEXFMT_10INT = 10,
	TEXFMT_16FLOAT = 16,
};

enum :int {
	DEINT_Disable = 0,
	DEINT_Enable = 1,
	DEINT_HackFutureFrames = 2,
};

enum :int {
	SUPERRES_Disable = 0,
	SUPERRES_SD,
	SUPERRES_720p,
	SUPERRES_1080p,
	SUPERRES_1440p,
	SUPERRES_COUNT
};

enum :int {
	CHROMA_Nearest = 0,
	CHROMA_Bilinear,
	CHROMA_CatmullRom,
	CHROMA_Krig, // Shiandow KrigBilateral (DX11 only; DX9 falls back to Catmull-Rom)
	CHROMA_COUNT
};

enum :int {
	UPSCALE_Nearest = 0,
	UPSCALE_Mitchell,
	UPSCALE_CatmullRom,
	UPSCALE_Lanczos2,
	UPSCALE_Lanczos3,
	UPSCALE_Jinc2,
	UPSCALE_COUNT
};

enum :int {
	DOWNSCALE_Box = 0,
	DOWNSCALE_Bilinear,
	DOWNSCALE_Hamming,
	DOWNSCALE_Bicubic,
	DOWNSCALE_BicubicSharp,
	DOWNSCALE_Lanczos,
	DOWNSCALE_COUNT
};

enum :int {
	SWAPEFFECT_Discard = 0,
	SWAPEFFECT_Flip,
	SWAPEFFECT_COUNT
};

enum :int {
	HDRTD_Disabled = 0,
	HDRTD_On_Fullscreen,
	HDRTD_On,
	HDRTD_OnOff_Fullscreen,
	HDRTD_OnOff
};

#define SDR_NITS_DEF 50
#define SDR_NITS_MIN  25
#define SDR_NITS_MAX 400
#define SDR_NITS_STEP  5

constexpr inline auto HDR_NITS_DEF = 1000;
constexpr inline auto HDR_NITS_MIN = 100;
constexpr inline auto HDR_NITS_MAX = 10000;

struct VPEnableFormats_t {
	bool bNV12;
	bool bP01x;
	bool bYUY2;
	bool bOther;
};

enum : int {
	ML_Disable = 0,
	ML_Manual,
	ML_Auto
};

// Input colour-space override for live HDMI capture (DeckLink). Auto = derive
// from the media type / resolution as today; an explicit value forces the
// video input to be graded as that signal (the source filter does not stamp
// VIH2 colourimetry for DeckLink streams).
enum : int {
	ICS_Auto = 0,
	ICS_BT601,       // Rec.601 / SDR
	ICS_BT709,       // Rec.709 / SDR
	ICS_BT2020_SDR,  // BT.2020 primaries, SDR transfer (DV-on-HDMI heuristic)
	ICS_BT2020_PQ,   // BT.2020 + PQ  (HDR10)
	ICS_BT2020_HLG,  // BT.2020 + HLG
};

struct Settings_t {
	bool bUseD3D11;
	bool bShowStats;
	int  iResizeStats;
	int  iTexFormat;
	VPEnableFormats_t VPFmts;
	int  iVPDeinterlacing;
	bool bDeintDouble;
	bool bVPScaling;
	int iVPSuperRes;
	bool bVPRTXVideoHDR;
	int  iChromaScaling;
	int  iUpscaling;
	int  iDownscaling;
	bool bInterpolateAt50pct;
	bool bUseDither;
	bool bDeintBlend;
	int  iSwapEffect;
	bool bExclusiveFS;
	bool bVBlankBeforePresent;
	bool bAdjustPresentTime;
	bool bReinitByDisplay;
	bool bHdrPreferDoVi;
	bool bHdrPassthrough;
	int  iHdrToggleDisplay;
	int  iHdrOsdBrightness;
	bool bConvertToSdr;
	int  iSDRDisplayNits;
	bool bHdrLocalToneMapping;
	int  iHdrLocalToneMappingType; // 1=ACES 2=Reinhard 3=Hable 4=Mobius 5=BT.2390 6=...
	int iHdrDisplayMaxNits;
	bool bHDRPassthroughRaw = false; // skip tone map + ML: show the raw HDR/DV signal on SDR (demo)
	int iInputColorSpace = ICS_Auto;  // forced input colour space for live HDMI capture (ICS_*)
	// HTPC cadence: when the input is 23.976/24p, auto-switch the display
	// refresh to a clean multiple of the source rate (2x = 48 Hz, 4x = 96 Hz,
	// 5x = 120 Hz ...) so every source frame maps to an integer number of
	// refreshes (no judder on pans). Prefers the lowest multiple the display
	// actually offers; if none is supported it falls back to the display's own
	// configured refresh. Restores the original refresh when the input stops
	// being 24p or the renderer tears down. 0 = off, 1 = on.
	int iRefreshSwitch24p = 0;

	// Zion ML grade
	bool  bMLEnabled = false;
	int   iMLMode = ML_Disable; // ML_Disable / ML_Manual / ML_Auto
	bool  bMLToneCurveEnabled = true; // "Tone curve" group master
	bool  bMLGammaEnabled = true;
	float fMLGamma = 1.0f;       // manual gamma [0.5, 1.5]
	bool  bMLShadowEnabled = true;
	float fMLShadow = 2.0f;      // shadow strength: 0=Off 1=Low 2=Med 3=High 4=VHi 5=Saturate
	float fMLShadowDehaze = 0.0f; // 0..1 scale DOWN the model's shadow-toe lift (lifted-black haze control); 0 = model as-is
	float fMLHighlightRolloffCtrl = 0.0f; // model-override: signed offset added to the model's rolloff in Auto (rim highlight softening); 0 = model as-is
	bool  bMLHighlightEnabled = true;
	float fMLHighlight = 0.0f;   // manual highlight roll-off [-0.3, 0.4]
	bool  bMLDebugCopy = false;  // grade steps use the plain copy shader (shader-vs-plumbing bisect)
	bool  bMLDebugLog = false;   // per-frame diagnostic CSV next to the filter
	bool  bMLShadowDebugVis = false; // heatmap overlay showing shadow enhancement zones
	float fMLShadowZoneCeil = 0.20f;
	float fMLShadowSkinProtect = 1.0f;
	float fMLShadowRatioCap = 1.40f;
	bool  bMLContrastEnabled = true;
	float fMLContrast = 0.15f; // micro-contrast clarity strength [0, 0.5]
	float fMLContrastZoneLo = 0.15f;
	float fMLContrastSkinProtect = 0.65f; // MC damping on person-skin (stage 4)
	float fMLContrastRatioCap = 1.40f;
	bool  bMLContrastDebugVis = false;
	// Sky semantic segmentation (yolo26n-sem; tone-leg). Default OFF — the sky
	// tone leg produced mask-region artifacts on first live runs; kept for later tuning.
	bool  bMLSkyEnabled = false;
	bool  bMLSkyDebugVis = false;   // cyan mask overlay for validation
	float fMLSkyStrength = 0.6f;    // sky tone-leg blend amount [0, 1]
	// Person skin protection (stage 4; class 11|12 of the same segmentation)
	bool  bMLPersonEnabled = false; // default OFF (pick up stage-5 work later)
	float fMLPersonStrength = 1.0f; // LUT-only-inside-people gate [0,1]; 0 = legacy
	bool  bMLPersonDebugVis = false; // magenta person-mask overlay (validation)
	float fMLSkinPreserve = 0.0f;    // stage-5 skin tone: source-structure keep-scale [0,1] (default OFF)
	bool  bTrayIcon = true;      // system tray icon when the renderer is active (opens the property page)
	// Phase 1b — chroma vector tuning / hunt compensation
	bool  bMLHuntEnabled = true;         // default preset: enabled
	float fMLHunt = 0.80f;  // libplacebo default (ml_render.h)
	bool  bMLChromaEnabled = true;       // master pass gate (also off under DoVi)
	bool  bMLChromaNeutralEnabled = true;
	float fMLChromaNeutral = 1.20f;
	bool  bMLChromaFireEnabled = true;
	float fMLChromaFire = 1.35f;
	bool  bMLChromaKneeEnabled = true;
	float fMLChromaKnee = 0.55f;
	bool  bMLChromaSkinEnabled = true;
	float fMLChromaSkinProtect = 0.95f;
	// Face detection
	bool  bMLFaceProtectEnabled = true;
	float fMLFaceProtect = 0.80f;   // face gamma protection strength [0,1]
	float fMLFaceFeather = 0.04f;   // SDF feather width in UV
	float fMLFaceExpand = 1.50f;    // box expansion factor
	bool  bMLFaceDebugVis = false;  // red rectangle overlay on detected faces
	// P0 black bars
	bool  bMLBars = true;           // repaint detected letterbox bars
	bool  bMLBarsHighlight = false; // red debug overlay
	// diagnostic: monotonic config-change counter shared by s3_shell and MPCVR
	uint32_t configSeq = 0;

	Settings_t() {
		SetDefault();
	}

	void SetDefault() {
		if (IsWindows8OrGreater()) {
			bUseD3D11                   = true;
		} else {
			bUseD3D11                   = false;
		}
		bShowStats                      = false;
		iResizeStats                    = 0;
		iTexFormat                      = TEXFMT_AUTOINT;
		VPFmts.bNV12                    = true;
		VPFmts.bP01x                    = true;
		VPFmts.bYUY2                    = true;
		VPFmts.bOther                   = true;
		iVPDeinterlacing                = DEINT_Enable;
		bDeintDouble                    = true;
		bVPScaling                      = true;
		iVPSuperRes                     = SUPERRES_Disable;
		bVPRTXVideoHDR                  = false;
		iChromaScaling                  = CHROMA_Bilinear;
		iUpscaling                      = UPSCALE_CatmullRom;
		iDownscaling                    = DOWNSCALE_Hamming;
		bInterpolateAt50pct             = true;
		bUseDither                      = true;
		bDeintBlend                     = false;
		iSwapEffect                     = SWAPEFFECT_Flip;
		bExclusiveFS                    = false;
		bVBlankBeforePresent            = false;
		bAdjustPresentTime              = true;
		bReinitByDisplay                = false;
		bHdrPreferDoVi                  = false;
		if (IsWindows10OrGreater()) {
			bHdrPassthrough             = true;
			bHdrLocalToneMapping        = false;
			iHdrLocalToneMappingType    = 1;
			iHdrDisplayMaxNits          = 1000;
		} else {
			bHdrPassthrough             = false;
			bHdrLocalToneMapping        = false;
			iHdrLocalToneMappingType    = 1;
			iHdrDisplayMaxNits          = 1000;
		}
		iHdrToggleDisplay               = HDRTD_Disabled;
		bConvertToSdr                   = true;
		iHdrOsdBrightness               = 0;
		iSDRDisplayNits                 = SDR_NITS_DEF;
		iHdrLocalToneMappingType        = 5;   // BT.2390 default
		bHDRPassthroughRaw              = false;
		iInputColorSpace                = ICS_Auto;
		iRefreshSwitch24p               = 0;
		bMLEnabled                      = false;
		iMLMode                         = ML_Disable;
		bMLToneCurveEnabled             = true;
		bMLGammaEnabled                 = true;
		fMLGamma                        = 1.0f;
		bMLShadowEnabled                = true;
		fMLShadow                       = 2.0f;
		fMLShadowDehaze                 = 0.0f;
		fMLHighlightRolloffCtrl         = 0.0f;
		bMLHighlightEnabled             = true;
		fMLHighlight                    = 0.0f;
		bMLDebugCopy                    = false;
		bMLDebugLog                     = false;
		bMLShadowDebugVis               = false;
		fMLShadowZoneCeil               = 0.20f;
		fMLShadowSkinProtect            = 1.0f;
		fMLShadowRatioCap               = 1.40f;
		bMLContrastEnabled              = true;
		fMLContrast                     = 0.15f; // micro-contrast clarity strength [0, 0.5]
		fMLContrastZoneLo               = 0.15f;
		fMLContrastSkinProtect          = 0.65f;
		fMLContrastRatioCap             = 1.40f;
		bMLContrastDebugVis             = false;
		bMLSkyEnabled                   = false;
		bMLSkyDebugVis                  = false;
		fMLSkyStrength                  = 0.6f;
		bMLPersonEnabled                = false;
		fMLPersonStrength               = 1.0f;
		bMLPersonDebugVis               = false;
		bMLHuntEnabled                  = true;
		fMLHunt                         = 0.80f;
		bMLChromaEnabled                = true;
		bMLChromaNeutralEnabled         = true;
		fMLChromaNeutral                = 1.20f;
		bMLChromaFireEnabled            = true;
		fMLChromaFire                   = 1.35f;
		bMLChromaKneeEnabled            = true;
		fMLChromaKnee                   = 0.55f;
		bMLChromaSkinEnabled            = true;
		fMLChromaSkinProtect            = 0.95f;
		bMLFaceProtectEnabled           = true;
		fMLFaceProtect                  = 0.80f;
		fMLFaceFeather                  = 0.04f;
		fMLFaceExpand                   = 1.50f;
		bMLFaceDebugVis                 = false;
		bMLBars                         = true;
		bMLBarsHighlight                = false;
		configSeq                       = 0;
	}
};

// Renderer frame accounting (LIVE panel / PERFORMANCE): receive-side frame
// counts + dropped/skipped/failed tallies from the draw pipeline.
struct RenderStats_t {
	unsigned framesReceived = 0;   // total input frames seen by the renderer
	unsigned framesDrawn = 0;      // frames actually presented
	unsigned dropped = 0;          // receive-side drops (m_DrawStats.m_dropped)
	unsigned skipped = 0;          // double-rate skip path (m_RenderStats.dropped2)
	unsigned failed = 0;           // render failures (m_RenderStats.failed)
	double   drawFps = 0.0;        // presented frames per second (measured)
};

interface __declspec(uuid("1AB00F10-5F55-42AC-B53F-38649F11BE3E"))
IVideoRenderer : public IUnknown {
	STDMETHOD(GetVideoProcessorInfo) (std::wstring& str) PURE;
	STDMETHOD_(bool, GetActive()) PURE;

	STDMETHOD_(void, GetSettings(Settings_t& setings)) PURE;
	STDMETHOD_(void, SetSettings(const Settings_t& setings)) PURE;

	STDMETHOD(SaveSettings()) PURE;

	STDMETHOD_(void, GetMLStats)(std::wstring& str) PURE;
	STDMETHOD_(void, GetRenderStats)(::RenderStats_t& stats) PURE;
};
