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

#include <DXGI1_2.h>
#include <dxva2api.h>
#include <dxgi1_5.h>
#include <strmif.h>
#include <map>
#include "IVideoRenderer.h"
#include "DX11Helper.h"
#include "D3D11VP.h"
#include "D3DUtil/D3D11Font.h"
#include "D3DUtil/D3D11Geometry.h"
#include "VideoProcessor.h"
#include "SubPic/DX11SubPic.h"
#include "ml_model.h"
#include "ml_features.h"
#include "face_detect.h"
#include "sky_seg.h"
#include "person_seg.h"
#include "BYTETracker.h"

#include <atomic>
#include <memory>

#define TEST_SHADER 0

class CVideoRendererInputPin;

class CDX11VideoProcessor
	: public CVideoProcessor
{
private:
	friend class CVideoRendererInputPin;

	// Direct3D 11
	CComPtr<ID3D11Device1>        m_pDevice;
	CComPtr<ID3D11DeviceContext1> m_pDeviceContext;
	CComPtr<ID3D11SamplerState>   m_pSamplerPoint;
	CComPtr<ID3D11SamplerState>   m_pSamplerLinear;
	CComPtr<ID3D11SamplerState>   m_pSamplerDither;
	CComPtr<ID3D11BlendState>     m_pAlphaBlendState;
	CComPtr<ID3D11VertexShader>   m_pVS_Simple;
	CComPtr<ID3D11PixelShader>    m_pPS_Simple;
	CComPtr<ID3D11PixelShader>    m_pPS_BitmapToFrame;
	CComPtr<ID3D11InputLayout>    m_pVSimpleInputLayout;
	CComPtr<ID3D11Buffer>         m_pVertexBuffer;
	CComPtr<ID3D11Buffer>         m_pResizeShaderConstantBuffer;
	CComPtr<ID3D11Buffer>         m_pHalfOUtoInterlaceConstantBuffer;
	CComPtr<ID3D11Buffer>         m_pFinalPassConstantBuffer;

	DXGI_SWAP_EFFECT              m_UsedSwapEffect = DXGI_SWAP_EFFECT_DISCARD;

#if TEST_SHADER
	CComPtr<ID3D11PixelShader>    m_pPS_TEST;
#endif

	Tex11Video_t m_TexSrcVideo; // for copy of frame
	Tex2D_t m_TexConvertOutput;
	Tex2D_t m_TexResize;        // for intermediate result of two-pass resize
	CTex2DRing m_TexsPostScale;
	Tex2D_t m_TexDither;

	// for GetAlignmentSize()
	struct Alignment_t {
		Tex11Video_t texture;
		ColorFormat_t cformat = {};
		LONG cx = {};
	} m_Alignment;

	// D3D11 Video Processor
	CD3D11VP m_D3D11VP;

	// Display calibration 3D LUT (see LoadCubeLut). Bound to t2/s2 of the final pass.
	CComPtr<ID3D11Texture3D>          m_pCubeLut;
	CComPtr<ID3D11ShaderResourceView> m_pCubeLutSRV;
	CComPtr<ID3D11SamplerState>       m_pCubeLutSampler;
	int  m_iCubeLutSize    = 0;      // N of LUT_3D_SIZE N; 0 == no LUT loaded
	bool m_bCubeLutEnabled = false;
	int  m_iCubeLutMode    = 3;      // default full 3D (the historical behaviour)
	// The final pass is normally gated on dithering. A loaded LUT must run there
	// regardless (it is the only display-referred stage), so the pass is forced on
	// and the dither sample is skipped in the shader when it is not wanted.
	bool m_bDitherActive   = false;

	CComPtr<ID3D11Buffer> m_pCorrectionConstants;
	CComPtr<ID3D11PixelShader> m_pPSCorrection;
	const wchar_t* m_strCorrection = nullptr;

	// [CBPROBE] sdrNits live-re-push investigation. The [SHADER-CB] log proves the
	// CPU wrote new constants into m_pCorrectionConstants; the user says the picture
	// does not change. The only way those two facts coexist is if the ACTIVE
	// correction draw reads a DIFFERENT buffer than the one being updated (or the
	// draw does not run). So: on every settings push arm a few frames, and on those
	// frames read back what b0 the live draw actually had bound — pointer identity
	// plus the float values (via a staging copy; DEFAULT buffers cannot be Map'd).
	// No [CBPROBE] line after arming = the draw site never ran. That is also an answer.
	CComPtr<ID3D11Buffer> m_pCbProbeStaging;
	LONG m_iCbProbeTicks = 0;
	UINT64 m_uCorrectionDraws = 0; // liveness: [CBPROBE] alive line every 300 draws

	// Experimental tone mapping (libplacebo port — see namespace exptm in the .cpp).
	// A 1D tone curve built on the CPU from the same per-frame scene statistics the
	// Zion grade already extracts, sampled by the converter instead of evaluating a
	// fixed curve per pixel. Bound to t1/s1 of the correction pass; 1D because the
	// curve is applied per channel, matching the existing ToneMappingHable.
	//
	// Selecting none of these leaves the shipped tone mapping bit-identical: the
	// shader branches on toneLutEnable, and the constant is 0 unless a mapper is on.
	CComPtr<ID3D11Texture2D>          m_pToneLut;
	CComPtr<ID3D11ShaderResourceView> m_pToneLutSRV;
	CComPtr<ID3D11SamplerState>       m_pToneLutSampler;
	int   m_iExperimentalToneMapper = 0;    // 0 = off (shipped path), 1 = spline
	float m_fToneLutContrast = 0.50f;       // pl_tone_map_spline "Contrast", 0.00 .. 1.50
	float m_fToneLutPivotScale = 1.0f;      // scales the scene avg feeding the spline pivot

	// Everything the curve is built from. Keeping the whole input set in one place is
	// deliberate: the rebuild test used to name fields one at a time, which stops
	// rebuilding the day a field is added — and the symptom is a control that does
	// nothing. The comparison itself lives in UpdateToneLut() because the two
	// scene statistics get a deadband and the configuration fields do not.
	struct ToneLutKey {
		float pqMin = 0.0f;        // index axis, PQ code values
		float pqMax = 0.0f;
		float pqAvg = 0.0f;        // scene average, PQ — drives the pivot
		float contrast = -1.0f;
		float pivotScale = -1.0f;  // scene-avg scale into the pivot (1 = libplacebo)
		// Display white in NITS. Named for what it is because the obvious
		// alternative — 10000/this — is LuminanceScale, and swapping the two
		// silently scaled the whole curve by dispNits/(10000/dispNits).
		float dispNits = 0.0f;
		float blackNits = 0.0f;    // display black, nits
	};
	ToneLutKey m_ToneLutKey;
	float m_MLToneMaxSmooth = -1.0f;        // IIR state, PQ
	float m_MLToneAvgSmooth = -1.0f;
	bool  m_bToneLutReady = false;
	// How many times the curve has actually been regenerated. Reported in the ML
	// stats line: "the mapper is selected" and "the mapper is producing a curve"
	// are different claims, and the rebuild counter is what separates them.
	unsigned long long m_nToneLutBuilds = 0;
	unsigned long long m_nToneLutFrame = 0;   // rate-limit tick, see UpdateToneLut()

	// HDR tonemapping
	struct HDRParamsConstantBuffer_t {
		float MasteringMinLuminanceNits;
		float MasteringMaxLuminanceNits;
		float maxCLL;
		float maxFALL;
		float displayMaxNits;
		UINT selection; // 1 = ACES, 2 = Reinhard, 3 = Habel, 4 = Möbius, 5 = BT2390, 6 = ST 2094-10
		// Destination BLACK, nits. Consumes one of the two former padding floats,
		// so the cbuffer size is unchanged and the .cso layout still matches.
		// 0.0 = the mapping assumes an ideal black (previous behaviour).
		float displayMinNits;
		float padding[1];
	};
	struct DoViDynamicConstantsBuffer_t {
		float trim_chroma_weight;
		float trim_saturation_gain;
		float trim_slope;
		float trim_offset;
		float trim_power;
		UINT enabled;
		float padding[2];
	};
	HDRParamsConstantBuffer_t m_lastHDRParamsConstantBuffer = {};
	DoViDynamicConstantsBuffer_t m_lastDoViDynamicConstantsBuffer = {};
	CComPtr<ID3D11Buffer> m_pHDR10ToneMappingConstants;
	CComPtr<ID3D11Buffer> m_pDoViDynamicConstants;
	CComPtr<ID3D11PixelShader> m_pPSHDR10ToneMapping;

	// Zion ML grade (shared cbuffer layout for ps_ml_*.hlsl)
	struct MLParams_t {
		float MLGamma;            // [0.5, 1.5], 1.0 = neutral
		float MLShadowToe;        // [0.0, 0.4]
		float MLShadowKnee;       // 0.25
		float MLShadowStrength;   // DoG detail multiplier (0=off, from strength tier)
		float MLHighlightRolloff; // [-0.3, 0.4]
		float MLHighlightKnee;    // adaptive on gamma
		float MLGuard;            // highlight guard, 0.85 default
		float MLHunt;             // hunt compensation
		float MLL1Max;            // scene peak in PQ
		float MLHiDesat;          // highlight desaturation
		float IsPQ;               // 1 = PQ input, 0 = gamma-2.2 (HDR->SDR)
		float Padding;
		// Phase 1b — chroma vector tuning (Spectrum Shift)
		float MLChromaNeutral;     // chroma gain at/below the knee (1.0 == off)
		float MLChromaFire;        // chroma gain above the knee
		float MLChromaKnee;        // luma knee where the boost transitions
		float MLChromaSkinProtect; // 0..1 how strongly P(skin) pulls the boost to 1.0
		float MLSkinLUTCR0;        // skin LUT grid bounds
		float MLSkinLUTCR1;
		float MLSkinLUTCB0;
		float MLSkinLUTCB1;
		float MLChromaGate;        // 1.0 = chroma pass active
		float MLHuntGate;          // 1.0 = hunt active
		// NOT free, despite the name: these six carry the shadow/contrast/sky debug
		// flags and the zone parameters that ps_ml_shadow reads. A new field goes at
		// the END of the struct instead.
		float _reserved[6];
		// P0 — black bars repaint
		float uBarTop;
		float uBarBottom;
		float uBarV0;
		float uBarSpan;
		float uBarFeather;
		float uBarBlack;
		float uBarHighlight;
		float _padBar;             // align to 16-byte boundary (D3D11 cbuffer requirement)
		// Contrast recovery (dual-band detail enhancement)
		float MCStrength;          // contrast band gain (0 = off)
		float MCZoneLo;            // zone floor where contrast fades in
		float MCSkinProtect;       // skin protection for contrast band
		float MCRatioCap;          // ratio cap for contrast band
		// Face detection results
		float FaceProtect;         // protection strength (0 = off, 1 = full)
		float FaceFeather;         // feather width in UV space
		float FaceExpand;          // box expansion factor (1.5 = 50% larger)
		float FaceDebugVis;        // 1.0 = draw red debug rectangles
		int   NumFaces;            // 0-4
		float _padFace[3];         // align FaceBox to 16-byte boundary
		float FaceBox[16];         // 4 x (x, y, w, h) in UV [0,1]
		float FaceTrackId[4];      // ByteTrack persistent ID per face slot
		// Sky tone leg (stage 3) — appended tail, offsets of all prior fields unchanged
		float MLSkyStrength;       // 0..1 blend of the sky-relaxed curve leg
		float MLSkyGuard;          // higher guard used inside sky (gentler crush)
		float MLSkyLuma;           // sky-region mean luma (anchor, informational)
		float MLSkyUVOffX;         // mask-UV remap: content sits at uvOff..(uvOff+uvSc)
		float MLSkyUVOffY;         // within the window the shader samples
		float MLSkyUVScX;
		float MLSkyUVScY;
		float _padSky;             // keep sizeof multiple of 16 (D3D11 cbuffer rule)
		// Person skin protection (stage 4) — appended tail (offsets above unchanged).
		// Effective skin weight: LUT weight gated to people. Strength 0 = legacy.
		float MLSkinPersonStrength; // 0..1 gate; 0 = bit-identical legacy LUT path
		float MLSkinPersonDebugVis; // 0/1 magenta person-mask overlay
		float _padPerson0;          // keep the block 16-byte aligned
		float _padPerson1;
		// Skin tone-preservation (stage 5): source-anchored I remap on skin pixels.
		float MLSkinAnchor;   // skin-set mean source luma (IIR, normalized gamma-2.2)
		float MLSkinPreserve; // 0..1 keep-scale of the source's internal skin structure
		float _padSkin0;
		float _padSkin1;
		// Gamma top-up — the user's signed offset on the model's gamma, passed
		// SEPARATELY rather than folded into MLGamma on the CPU. The gamma shader
		// has to be able to gate it by the trained skin LUT, and it cannot do that
		// to a term it cannot see. Appended tail: every offset above is unchanged,
		// and only ps_ml_gamma.hlsl declares this far.
		float MLGammaTopUp;
		// Shadow PEDESTAL: a true offset in the toe, independent of x. Takes a
		// padding slot rather than growing the struct — the layout is shared by every
		// ps_ml_*.hlsl and its size is locked to a 16-byte boundary. Only ps_ml_gamma
		// declares this far, so no other shader's offsets move. 0 = off, and off is
		// byte-identical to the previous picture.
		float MLShadowPedestal;
		float _padGT1;
		float _padGT2;
	};
	static_assert(sizeof(MLParams_t) == 352, "MLParams_t cbuffer size drift");
	MLParams_t m_lastMLParams = {};
	CComPtr<ID3D11Buffer> m_pMLParams;
	CComPtr<ID3D11PixelShader> m_pPSMLGamma;
	CComPtr<ID3D11PixelShader> m_pPSMLShadow;
	CComPtr<ID3D11PixelShader> m_pPSMLHighlight;
	CComPtr<ID3D11PixelShader> m_pPSMLChroma;
	CComPtr<ID3D11PixelShader> m_pPSMLBars;

	// P0 black-bars temporal state
	bool m_bMLBarsEnabled = true;      // repaint bars to display black (registry: MLBars)
	bool m_bMLBarsHighlight = false;   // red debug fill (registry: MLBarsHighlight)
	MLBarBounds m_MLBarDetected = {};
	uint16_t m_MLCropTop = 0;
	uint16_t m_MLCropBot = ML_DOWNSAMPLE_H - 1;
	uint32_t m_MLCropAge = 0;
	float m_fMLBarTop = 0.0f;
	float m_fMLBarBot = 1.0f;

	// Zion ML feature extraction (Phase 1: AUTO gamma)
	CComPtr<ID3D11Texture2D> m_MLDownsample;   // 256x144 R16_UNORM luma cache
	CComPtr<ID3D11Texture2D> m_MLStaging;      // CPU-readable copy
	CComPtr<ID3D11PixelShader> m_pPSMLDownsample;

	// DELIVERED-frame instrument (debug-gated, see BuildRenderedLumaStats). The
	// models measure the SOURCE and act on the render without ever seeing it; these
	// two caches hold what the tone curve produced and what the grade produced,
	// downsampled at draw time and reduced at scene rate into [DELIVERED] lines.
	CComPtr<ID3D11Texture2D> m_MLDeliveredCurveOut;  // post-correction, pre-grade
	CComPtr<ID3D11Texture2D> m_MLDeliveredPostGrade; // post-grade, pre-cal/dither
	MLRenderedLumaStats m_DeliveredCurveOut;
	MLRenderedLumaStats m_DeliveredPostGrade;
	bool m_bDeliveredA = false;   // cache A holds this frame's correction output
	bool m_bDeliveredB = false;   // cache B holds this frame's post-grade frame
	// The instrument's OWN throttle — deliberately not the peak anchor's cut/lock
	// state. See ReadDeliveredStats' call site in Render().
	uint64_t m_uDeliveredLastFrame = 0;
	// Content RECT in the DELIVERED (window-sized) caches — pillarbox columns from
	// m_videoRect plus the film's own letterbox rows mapped through it. Logged with
	// every line so the crop is auditable rather than assumed.
	int m_DeliveredTopRow = 0;
	int m_DeliveredBotRow = ML_DOWNSAMPLE_H - 1;
	int m_DeliveredLeftCol = 0;
	int m_DeliveredRightCol = ML_DOWNSAMPLE_W - 1;

	// Zion ML debug (registry: MLDebugLog / MLDebugPassthrough)
	bool m_bMLDebug = false;
	int   m_mlSessionId = 0;      // playback session counter (media-type changes) for log triage
	bool m_bMLDebugCopy = false; // bind ps_simple to the grade steps (bisect test)
	bool m_bMLShadowDebugVis = false;
	bool m_bMLContrastDebugVis = false;
	float m_fMLShadowZoneCeil = 0.20f;
	float m_fMLShadowSkinProtect = 0.65f;
	float m_fMLShadowRatioCap = 1.40f;
	float m_fMLShadowDehaze = 0.0f;   // 0..1 scale-down of the model's shadow-toe lift (haze control)
	float m_MLShadowDehazeApplied = 0.0f; // what was actually applied (telemetry)
	float m_fMLHighlightRolloffCtrl = 0.0f; // signed offset on the model's rolloff in Auto (rim soften)
	float m_MLRolloffCtrlApplied = 0.0f;   // what was actually applied (telemetry)
	float m_fMLGammaTopUp = 0.0f;          // signed offset ADDED to the model's gamma in Auto (midtone lift)
	// Shadow pedestal: a true OFFSET in the toe, independent of the pixel's value.
	// The toe (m_fMLShadow / MLShadowToe) is a gain — lift proportional to x — so it
	// cannot raise a crushed shadow at all (measured: crushFrac unchanged across the
	// whole grade). This is the term that can. 0 = off.
	float m_fMLShadowPedestal = 0.0f;
	bool  m_bMLContrastEnabled = true;
	float m_fMLContrast = 0.15f; // micro-contrast clarity strength (exponent) — overwritten by config
	float m_fMLContrastZoneLo = 0.15f;
	float m_fMLContrastSkinProtect = 0.65f;
	float m_fMLContrastRatioCap = 1.40f;
	// Face detection (YuNet via ONNX Runtime)
	std::unique_ptr<CFaceDetect> m_pFaceDetect;
	CComPtr<ID3D11Texture2D>     m_FaceDownsample;   // 480x288 BGRA8
	CComPtr<ID3D11Texture2D>     m_FaceStaging;      // CPU-readable copy
	CComPtr<ID3D11PixelShader>   m_pPSFaceDown;      // color downsample shader
	CComPtr<ID3D11Buffer>        m_pFaceDownParams;  // tiny cbuffer: IsPQ flag for PQ→SDR
	bool  m_bFaceProtectEnabled = true;
	float m_fFaceProtect = 0.8f;
	float m_fFaceFeather = 0.12f;
	float m_fFaceExpand = 2.0f;
	bool  m_bFaceDebugVis = false;
	// Sky semantic segmentation (yolo26n-sem via ONNX Runtime + DirectML)
	std::unique_ptr<CSkySeg>          m_pSkySeg;
	std::unique_ptr<CPersonSeg>       m_pPersonSeg;
	CComPtr<ID3D11Texture2D>          m_SkyMask;      // 640x360 R8G8_UNORM (R=sky, G=person)
	CComPtr<ID3D11ShaderResourceView> m_SkyMaskSRV;
	CComPtr<ID3D11SamplerState>       m_pSamplerSky;  // bilinear + clamp
	CComPtr<ID3D11Texture2D>          m_SkinGate;     // 320x180 R8_UNORM Poisson-relaxed person×skin gate (t4)
	CComPtr<ID3D11ShaderResourceView> m_SkinGateSRV;
	CComPtr<ID3D11SamplerState>       m_pSamplerGate; // bilinear + clamp
	CComPtr<ID3D11Texture2D>          m_SegDownsample; // 640x360 BGRA8 SDR-ized seg preview
	CComPtr<ID3D11Texture2D>          m_SegStaging;    // CPU-readable copy
	CComPtr<ID3D11Texture2D>          m_MaskProbe;     // CPU staging twin of m_SkyMask (diag readback)
	CComPtr<ID3D11Texture2D>          m_MetDown;       // 640x360 BGRA graded-output grab (skin metric)
	CComPtr<ID3D11Texture2D>          m_MetStaging;
	HRESULT CreateSkyMaskTexture();
	HRESULT CreateSkySegTextures();
	void    CreateSkinGateTexture();
	void    LogMaskProbe();
	void    LogSkinMetric(ID3D11Texture2D* pRT, const CRect& rect);
	void    UpdateSkinAnchor(const uint8_t* bgra, UINT stride);
	void    UpdateSkinGate(const uint8_t* bgra, UINT stride);   // Poisson-relaxed person×skin gate (t4)
	float   m_SkyFrac = 0.0f;      // last decode: fraction of valid pixels == sky
	float   m_PersonFrac = 0.0f;   // last decode: fraction of valid pixels == person/rider
	std::vector<uint8_t> m_LastSkyMap;     // CPU cache of last sky map (640x360) — R channel persists between refreshes
	std::vector<uint8_t> m_LastPersonMap;  // CPU cache of last person map (640x360) — refreshed every frame
	std::vector<uint8_t> m_PersonMaskSm;   // temporally smoothed twin (flicker-stable gate)
	float   m_SkyLuma = 0.0f;      // last decode: mean gamma-2.2 luma of sky pixels
	uint64_t m_nSkyFrame = 0;      // render-frame counter for cadence (incremented per frame)
	uint64_t m_nSkyLastFrame = 0;  // last frame a mask refresh was uploaded
	uint64_t m_nPersonLastFrame = 0; // last frame the person inference + gate ran (every 2)
	bool    m_bSkyEnabled = false; // sky segmentation + tone leg — default OFF
	bool    m_bSkyDebugVis = false; // cyan overlay of the sky mask (validation)
	float   m_fMLSkyStrength = 0.6f; // sky tone-leg blend amount [0, 1]
	// Person skin protection (stage 4) — class 11|12 of the same seg inference
	bool    m_bPersonEnabled = true;   // gate person skin protection (not the inference)
	float   m_fPersonStrength = 1.0f;  // LUT-only-inside-people gate [0,1]
	bool    m_bPersonDebugVis = false; // magenta person-mask overlay (validation)
	float   m_fMLSkinPreserve = 0.0f;  // stage-5 skin tone keep-scale (default OFF)
	double  m_skinAnchorSmooth = 0.0f; // IIR skin-set mean source luma (gamma-2.2)
	bool    m_skinAnchorValid = false;
	float   m_maskTrust = 1.0f;     // scene-confidence scalar (0..1): falls fast on mask
	                                // mismatch (cut/pan), recovers slowly on stability —
	                                // MLSkinPreserve *= env so the tone blend fades in with
	                                // a smooth curve after a scene change (Bay montage safe).
	float   m_maskEnv = 1.0f;       // = smoothstep(0.25, 0.80, m_maskTrust), applied to preserve
	int     m_framesSinceGood = 0;  // consecutive mismatch frames (whip-pan vs cut disambig)
	std::vector<uint8_t> m_LastInstMap; // per-pixel person-instance label (0=bg, else 1..8)
	float   m_instSkinSm[9] = {0};  // per-instance IIR of skin fraction σ_j (label-indexed)
	float   m_instAmp[9] = {0};     // per-instance amplitude = smoothstep(0.03,0.12,σ) skin gate
	bool    m_instSmInit = false;   // first-frame / cut force-reset of m_instSkinSm
	// ByteTrack multi-object tracker (Kalman + Hungarian)
	std::unique_ptr<BYTETracker> m_pByteTracker;
	static constexpr int FACE_DETECT_W = 480;
	static constexpr int FACE_DETECT_H = 288;
	CFaceDetect::FaceBox m_syncFaces[4] = {};
	int m_nSyncFaces = 0;
	static constexpr int SCENE_CUT_FEATS[] = {0, 1, 3, 5, 7}; // maxscl, avg, p25, p75, p99
	static constexpr int SCENE_CUT_N = 5;
	static constexpr float SCENE_CUT_THRESH = 0.12f;
	float m_fPrevSceneFeat[SCENE_CUT_N] = {-1, -1, -1, -1, -1};
	FILE* m_pMLDebugFile = nullptr;
	uint64_t m_MLDebugFrame = 0;
	uint64_t m_MLDebugStepCount = 0;
	uint16_t m_MLDebugLastMin = 0, m_MLDebugLastMax = 0;
	float m_MLDebugLastAvg = 0.0f, m_MLDebugLastClipF = 0.0f, m_MLDebugLastEdgeF = 0.0f;
	// Phase 1b feature-state placeholders (wired to Settings_t in task 5)
	bool  m_bMLToneCurveEnabled = true;
	bool  m_bMLGammaEnabled = true;
	bool  m_bMLShadowEnabled = true;
	bool  m_bMLHighlightEnabled = true;
	bool  m_bMLHuntEnabled = true;
	float m_fMLHunt = 0.80f;
	bool  m_bMLChromaEnabled = true;
	bool  m_bMLChromaNeutralEnabled = true;
	float m_fMLChromaNeutral = 1.20f;
	bool  m_bMLChromaFireEnabled = true;
	float m_fMLChromaFire = 1.35f;
	bool  m_bMLChromaKneeEnabled = true;
	float m_fMLChromaKnee = 0.55f;
	bool  m_bMLChromaSkinEnabled = true;
	float m_fMLChromaSkinProtect = 0.95f;
	float m_fSkinLUTCR0 = -0.2009f, m_fSkinLUTCR1 = 0.3295f;
	float m_fSkinLUTCB0 = -0.2494f, m_fSkinLUTCB1 = 0.1967f;
	CComPtr<ID3D11Texture2D> m_SkinLUT;
	CComPtr<ID3D11ShaderResourceView> m_SkinLUTSRV;
	std::vector<unsigned char> m_SkinLutCpu; // 96x96 raster cache (skin-metric CPU sampling)
	int  m_SkinLutN = 0;
	CComPtr<ID3D11SamplerState> m_pSamplerSkin;
	HRESULT LoadSkinLUT();

	int m_MLDebugWinW = 0, m_MLDebugWinH = 0;
	int m_MLDebugFeatReady = 0;
	bool m_bMLGammaPredictOk = false;   // last auto-frame gamma Predict() result
	float m_MLGammaRawPred = 1.0f;      // raw model output before range clamping
	int m_MLDebugStale = 0, m_MLDebugConvertSeen = 0, m_MLDebugTexConv = 0;
	float m_MLDebugFeat0 = 0.0f, m_MLDebugFeat1 = 0.0f;
	float m_MLDebugPred = 0.0f;
	bool m_bMLActive = false;
	bool m_bMLFeaturesReady = false;
	bool m_bMLFeaturesStale = false;
	bool m_bMLConvertSeen = false; // a previous frame's convert output exists
	bool m_bMLModelLoadTried = false;
	bool m_bMLResourcesReady = false;   // full ML compute set preloaded (no runtime load/unload)
	bool m_bMLGammaModelLoaded = false;
	bool m_bMLShadowModelLoaded = false;
	bool m_bMLHighlightModelLoaded = false;
	float m_MLFeatures[ML_FEATURE_DIM] = {};
	CMLModel m_MLGammaModel;
	CMLModel m_MLShadowModel;
	CMLModel m_MLHighlightModel;
	float m_MLIIRGamma = 1.0f;
	float m_MLPrevL1Max = -1.0f;
	float m_MLRawTexture = 0.0f;    // BuildMLFeatures out-param (high-freq edge fraction)
	float m_MLTextureSmooth = 0.0f; // IIR-filtered texture energy feeding the MC gate

	// Per-scene source peak for the Hable curve, in nits.
	//
	// Measured from m_MLFeatures[0] — the per-scene PQ peak of the PRE-tone-map
	// signal — then IIR-smoothed so the curve does not re-normalise every shot,
	// and snapped on a scene cut so it does not ramp across one. Negative means
	// "no reading yet"; the shader falls back to the static maxCLL until then.
	// The p99.995 peak from the last extraction, in PQ. This rather than
	// m_MLFeatures[0] is what the Hable per-scene normalisation uses: features[0]
	// is a MAX and a max is set by one bright speck, which makes the whole picture
	// shimmer. See BuildMLFeatures.
	float m_MLPeakPq995 = -1.0f;
	float m_MLActiveFrac = 1.0f;   // non-black CELL fraction (libplacebo's bias term); was a pixel fraction, which pinned the scene-change hysteresis at 0
	// libplacebo's peak-detect state (sh_color_map_obj.peak): the IIR- and
	// hysteresis-filtered average and peak, in PQ. These persist across frames —
	// the filter IS the state, and clearing them restarts the detection.
	float m_PeakAvgPq = 0.0f;
	float m_PeakMaxPq = 0.0f;
	// Scene-lock state (snap/settle/lock with asymmetric drift — see the block at
	// UpdatePeak). The anchor is locked within a scene so the Hable normaliser
	// cannot pump; it only drifts downward (slow brighten) once locked.
	bool m_SceneLocked = false;
	int  m_SceneFrames = 0;
	// Spike test (2026-09-22): a cut is a DISCONTINUITY — the frame's delta
	// dominates its recent 4-frame baseline, so occlusion and camera reframes
	// (a ramp) cannot trip it, however far p99.5 wanders. (The slow re-anchor
	// "glide" that used to live here was deleted: it re-anchored on sustained
	// content — a character standing in frame for a second — and became the very
	// dimming/brightening it was meant to prevent. The anchor is now a constant
	// until the next cut, nothing else moves it.)
	float m_DeltaHistory[4] = {};
	int   m_DeltaIdx        = 0;
	// Zonal coherence (2026-09-22). A cut is SPATIALLY GLOBAL: it replaces the
	// frame, so most of the 5x5 zones' maxes (m_MLFeatures[52..76]) move. An
	// occlusion or reframe is local — only the zones it touches move. A cut must
	// ALSO clear the zone-majority test, not just the global peak spike.
	float m_PrevZoneMax[25] = {};
	bool  m_ZoneSeen        = false;
	float m_ZoneChangeFrac  = 0.0f;   // fraction of zones moved this frame (CSV)
	float m_HableScenePeakNits = -1.0f;
	float m_HablePrevPeakPq = -1.0f;        // previous raw PQ peak, for the cut test
	// Soft-plus pre-compressor: display-relative; knee <= 0 disables.
	float m_fSoftKnee      = 0.60f;
	float m_fSoftKneeRatio = 2.48f;
	float m_fSoftKneeWidth = 0.42f;
	float m_fHdrRefWhiteNits = 203.0f;   // the Hable curve's 1.0 in nits; 0 = sdrNits
	float m_fHdrHighlightNeutral = 0.9f; // highlight neutral roll, 0..1 (0 = off)
	// STAGE 1 (delivery only): configured chroma compensation exponent. Stored +
	// reported as clE= in the pipeline dump; NOT applied in any shader yet.
	float m_fHdrChromaExp = 1.35f;
	// GT7 operator (TONE_CURVE_GT7). Defaults are Polyphony's own; see
	// Shaders/convert/gt7_tonemap.hlsl for what each one does to the picture.
	float m_fHdrGt7Blend     = 0.6f;
	float m_fHdrGt7FadeStart = 0.98f;
	float m_fHdrGt7FadeEnd   = 1.16f;
	// GT7 Adapt (TONE_CURVE_GT7_ADAPT): weight of the measured scene peak in the
	// peak this curve maps FROM. 0 = plain GT7. cbuffer[24].
	float m_fHdrGt7Adapt     = 0.25f;
	float m_HableScenePeakApplied = -1.0f;  // value last pushed to the cbuffer

	// The gamma top-up actually added on top of the model's prediction this frame
	// (model-override in AUTO, mirrors m_MLRolloffCtrlApplied). Recorded so the
	// debug log can show what was applied rather than only what was asked for.
	float m_MLGammaTopUpApplied = 0.0f;

	// D3D11 Shader Video Processor
	CComPtr<ID3D11PixelShader> m_pPSConvertColor;
	CComPtr<ID3D11PixelShader> m_pPSConvertColorDeint;
	struct {
		bool bEnable = false;
		ID3D11Buffer* pVertexBuffer = nullptr;
		ID3D11Buffer* pConstants = nullptr;
		void Release() {
			bEnable = false;
			SAFE_RELEASE(pVertexBuffer);
			SAFE_RELEASE(pConstants);
		}
	} m_PSConvColorData;

	CComPtr<ID3D11Buffer> m_pDoviCurvesConstantBuffer;

	CComPtr<ID3D11PixelShader> m_pShaderUpscaleX;
	CComPtr<ID3D11PixelShader> m_pShaderUpscaleY;
	CComPtr<ID3D11PixelShader> m_pShaderDownscaleX;
	CComPtr<ID3D11PixelShader> m_pShaderDownscaleY;

	std::vector<ExternalPixelShader11_t> m_pPreScaleShaders;
	std::vector<ExternalPixelShader11_t> m_pPostScaleShaders;
	CComPtr<ID3D11Buffer> m_pPostScaleConstants;
	CComPtr<ID3D11PixelShader> m_pPSHalfOUtoInterlace;
	CComPtr<ID3D11PixelShader> m_pPSFinalPass;

	CComPtr<IDXGIFactory2>   m_pDXGIFactory2;
	CComPtr<IDXGISwapChain1> m_pDXGISwapChain1;
	CComPtr<IDXGISwapChain4> m_pDXGISwapChain4;
	CComPtr<IDXGIOutput>    m_pDXGIOutput;
	DXGI_COLOR_SPACE_TYPE m_currentSwapChainColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

	// Input parameters
	DXGI_FORMAT m_srcDXGIFormat = DXGI_FORMAT_UNKNOWN;

	// D3D11 VP texture format
	DXGI_FORMAT m_D3D11OutputFmt = DXGI_FORMAT_UNKNOWN;

	// intermediate texture format
	DXGI_FORMAT m_InternalTexFmt = DXGI_FORMAT_B8G8R8A8_UNORM;

	// swap chain format
	DXGI_FORMAT m_SwapChainFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
	UINT32 m_DisplayBitsPerChannel = 8;

	D3D11_VIDEO_FRAME_FORMAT m_SampleFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;

	CComPtr<IDXGIFactory1> m_pDXGIFactory1;

	bool m_bSubPicWasRendered = false;

	// AlphaBitmap
	Tex2D_t m_TexAlphaBitmap;
	CComPtr<ID3D11Buffer> m_pAlphaBitmapVertex;

	// Statistics
	CD3D11Rectangle m_StatsBackground;
	CD3D11Font      m_Font3D;
	CD3D11Rectangle m_Rect3D;
	CD3D11Rectangle m_Underlay;
	CD3D11Lines     m_Lines;
	CD3D11Polyline  m_SyncLine;

	bool m_bExclusiveScreen = false;
	bool m_bFullScreen = false;

	int m_iVPSuperRes = SUPERRES_Disable;
	bool m_bVPUseSuperRes = false; // but it is not exactly

	bool m_bVPRTXVideoHDR = false;
	bool m_bVPUseRTXVideoHDR = false;

	bool m_bHdrPassthroughSupport             = false;
	std::atomic_bool m_bHdrDisplaySwitching   = false; // switching HDR display in progress
	bool m_bHdrDisplayModeEnabled             = false;
	bool m_bHdrAllowSwitchDisplay             = true;
	bool m_bACMEnabled                        = false;

	UINT m_srcVideoTransferFunction = 0; // need a description or rename

	// Forced input colour space for live HDMI capture (ICS_* enum in
	// IVideoRenderer.h). Auto = current behaviour (media type / resolution).
	int m_InputColorSpaceOverride = ICS_Auto;
	void ApplyInputColorSpaceOverride(); // post-SpecifyExtendedFormat override

	std::map<std::wstring, bool> m_hdrModeSavedState;
	std::map<std::wstring, bool, std::less<>> m_hdrModeStartState;

	struct HDRMetadata {
		DXGI_HDR_METADATA_HDR10 hdr10 = {};
		bool bValid = false;
	};
	HDRMetadata m_hdr10 = {};
	HDRMetadata m_lastHdr10 = {};

	UINT m_DoviMaxMasteringLuminance = 0;
	UINT m_DoviMinMasteringLuminance = 0;
	UINT m_DoviMaxContentLightLevel = 0;
	UINT m_DoviMaxFrameAverageLightLevel = 0;

	struct DoviExtensionMetadata_t {
		struct L1_t {
			uint16_t min_pq = 0;
			uint16_t max_pq = 0;
			uint16_t avg_pq = 0;
			bool present = false;

			bool operator==(const L1_t& other) const {
				return min_pq == other.min_pq && max_pq == other.max_pq && avg_pq == other.avg_pq;
			}
			bool operator!=(const L1_t& other) const {
				return !(*this == other);
			}
		};
		L1_t L1;
		L1_t L1Cached;

		struct L2_t {
			float trim_slope = 0.f;
			float trim_offset = 0.f;
			float trim_power = 0.f;
			float trim_saturation_gain = 0.f;
			float trim_chroma_weight = 0.f;
			bool present = false;
		};
		L2_t L2;
	} m_DoviExtensionMetadata;

	HMONITOR m_lastFullscreenHMonitor = nullptr;

	D3DCOLOR m_dwStatsTextColor = D3DCOLOR_XRGB(255, 255, 255);

	// SubPic
	CComPtr<CDX11SubPicAllocator> m_pSubPicAllocator;
	bool m_bCallbackDeviceIsSet = false;
	void SetCallbackDevice();
	void UpdateSubPic();

	std::atomic_bool m_bDisplayModeChangeAfterHDRToggle = false;
	CCritSec m_HDRToggleLock;

	// Present-latency gauge (HTPC): how far the on-screen frame lags capture.
	// Anchored at the first presented frame: lag = (wallNow - wallFirst) -
	// (streamTimeNow - streamTimeFirst) — valid because the decklink stream
	// time advances 1:1 with real capture time.
	REFERENCE_TIME m_rtFirstPresent = -1;
	long long m_wallFirstPresent100ns = 0;   // GetPreciseTick() converted to 100 ns
	double m_presentLatencyMs = 0.0;

	bool m_bHDRModeChangeOutside = false;

	void FillDisplayParams();

public:
	CDX11VideoProcessor(CMpcVideoRenderer* pFilter, const Settings_t& config, HRESULT& hr);
	~CDX11VideoProcessor() override;

	int Type() override { return VP_DX11; }

	HRESULT Init(const HWND hwnd, const bool displayHdrChanged, bool* pChangeDevice = nullptr) override;
	bool Initialized();

private:
	void ReleaseVP();
	void ReleaseDevice();
	void ReleaseSwapChain();

	UINT GetPostScaleSteps();

	HRESULT CreatePShaderFromResource(ID3D11PixelShader** ppPixelShader, UINT resid);
	void SetShaderConvertColorParams();
	void SetShaderLuminanceParams();

	// Display calibration 3D LUT (.cube, ArgyllCMS/DisplayCAL/Resolve format).
	// Loaded into a TEXTURE3D and sampled in the final pass — the last stage
	// before quantization, which is the only correct place for a display
	// correction: after tone mapping, after the ML grade, after scaling.
	// An empty path unloads and returns S_OK; a missing/corrupt file returns E_FAIL
	// so the caller can tell "cleared" from "failed to load".
	HRESULT LoadCubeLut(const char* utf8Path) override;
	// 0 = off, 1 = gamma only, 2 = greyscale, 3 = full 3D. Separate from the load
	// so the user can A/B the modes without re-parsing the file.
	void SetCubeLutMode(int mode) override { m_iCubeLutMode = mode; }

	void SetHDR10ShaderParams(float, float, float, float, float, int);
	void SetDolbyVisionDynamicParams();

	// Picks and creates the correction (converter) shader from the CURRENT source
	// format and settings. Extracted from InitMediaType so a live settings change
	// can re-run just the selection: bConvertToSdr decides WHICH shader is built
	// rather than being a constant it feeds, so it cannot be pushed as a uniform.
	void UpdateCorrectionShader();

	// Rebuilds m_pToneLut from the current scene statistics. Cheap (1024 entries,
	// one 4 KB upload) and only runs when the statistics actually moved, so it is
	// safe to call every frame from SetMLParams().
	void UpdateToneLut();
	// True when the experimental mapper is selected AND has parameters ready, i.e.
	// the converter must take the LUT branch. Single source of truth for the
	// C++-side enable, so the cbuffer flag and the shader branch cannot disagree.
	// The correction this frame selects: PQ -> SDR. ONE predicate, because it now
	// decides the extractor's luma domain, the ML passes' input domain, and which
	// shader SetShaderLuminanceParams feeds.
	bool IsPqToSdrSelected() const;

	// Which luma domain each consumer's texture is actually in. TWO predicates,
	// deliberately — they read DIFFERENT textures, and one flag cannot be right
	// for both.
	//
	//   MLFeatureDomainIsPq(): ExtractMLFeatures reads m_TexConvertOutput, the
	//     PRE-correction texture, so its domain depends only on the SOURCE. Keying
	//     it off m_bConvertToSdr was a bug: with Convert to SDR off the texture is
	//     still PQ but the flag said otherwise, and the extractor applied
	//     SdrGammaToPQ on top of values that were already PQ. Measured on a static
	//     frame it reported feat0 = 0.3161 where SdrGammaToPQ(0.3855) = 0.31575 —
	//     the double remap, to 0.1%. The Auto model then predicted gamma 1.4816
	//     against 1.4446 for the identical frame, so the grade ran wrong numbers.
	//
	//   MLInputIsPq(): the ML passes read pInputTexture, the POST-correction
	//     texture, so PQ survives only when the correction did not convert to SDR.
	bool MLFeatureDomainIsPq() const;
	bool MLInputIsPq() const;

	bool ToneLutActive() const {
		// The REFERENCE and GT7 curves outrank the experimental mapper in the shader,
		// so the LUT must not be built (or uploaded, or flagged in the cbuffer) while
		// either is selected — otherwise the shader and this predicate disagree about
		// which branch is live, and the "toneMap: active=" readout lies. GT7 was added
		// to this test for the same reason REFERENCE was: an explicit curve selection
		// must not be silently replaced by a different axis.
		return m_iHdrLocalToneMappingType != TONE_CURVE_REFERENCE
			&& m_iHdrLocalToneMappingType != TONE_CURVE_GT7
			&& m_iHdrLocalToneMappingType != TONE_CURVE_GT7_ADAPT
			&& m_iExperimentalToneMapper != 0 && m_bToneLutReady && m_pToneLutSRV != nullptr;
	}

	// True when the per-frame scene statistics are needed — by the Zion grade, by
	// AUTO mode warming up, or by the experimental tone mapper.
	//
	// This is deliberately ONE predicate rather than a condition repeated at each
	// site. It used to be three separate conditions that had to agree, and they
	// did not: the latch recording "a convert output exists" was gated on the Zion
	// grade alone, so selecting the tone mapper with the grade off left the
	// extractor never running, the features at zero, and the mapper permanently
	// inactive — a setting that is applied, reported back, and does nothing.
	// The per-frame scene statistics (maxscl / avg → the measured peak) are now
	// computed UNCONDITIONALLY, not only when the Zion grade or the spline wants
	// them. The measured peak/avg feed GT7 and any future consumer regardless of
	// which tone curve is selected; gating them on the consumer left them at 0
	// exactly when they were most needed (e.g. metadata-less DV, or GT7 with the
	// grade off). The CONSUMERS stay separately gated: the grade passes on
	// m_bMLActive, the spline LUT build on expTm (UpdateToneLut self-gates).
	// Cost: one GPU->CPU readback per frame, always.
	bool MLStatsNeeded() const {
		return true;
	}

	void UpdateMLActive();
	void SetMLParams();
	void ExtractMLFeatures();
	void UpdateMLBarsFromDetected();
	void CreateMLTextures();
	void InitMLResources();
	std::wstring GetMLModelPath(const wchar_t* fileName);
	void InitMLDebug();
	void LogMLFrame();
	void LogMLFrameStats(const Tex2D_t& src, const wchar_t* tag);
	std::wstring GetMLModuleDir();

	HRESULT SetShaderDoviCurvesPoly();
	HRESULT SetShaderDoviCurves();

	void UpdateTexParams(int cdepth);
	void UpdateRenderRect();
	void UpdateScalingStrings();

	void CalcStatsParams() override;

	HRESULT MemCopyToTexSrcVideo(const BYTE* srcData, const int srcPitch);

	bool Preferred10BitOutput() {
		return m_DisplayBitsPerChannel >= 10 && (m_InternalTexFmt == DXGI_FORMAT_R10G10B10A2_UNORM || m_InternalTexFmt == DXGI_FORMAT_R16G16B16A16_FLOAT);
	}

	bool HandleHDRToggle();
	bool ToggleHDR(const DisplayConfig_t& displayConfig, const bool bEnableAdvancedColor);

public:
	HRESULT SetDevice(ID3D11Device *pDevice, ID3D11DeviceContext *pContext);
	HRESULT InitSwapChain(bool bWindowChanged);

	BOOL VerifyMediaType(const CMediaType* pmt) override;
	BOOL InitMediaType(const CMediaType* pmt) override;

	HRESULT InitializeD3D11VP(const FmtConvParams_t& params, const UINT width, const UINT height, const CMediaType* pmt);
	HRESULT InitializeTexVP(const FmtConvParams_t& params, const UINT width, const UINT height);
	void UpdatFrameProperties(); // use this after receiving modified frame from hardware decoder

	BOOL GetAlignmentSize(const CMediaType& mt, SIZE& Size) override;

	HRESULT ProcessSample(IMediaSample* pSample) override;
	HRESULT CopySample(IMediaSample* pSample);
	// Render: 1 - render first fied or progressive frame, 2 - render second fied, 0 or other - forced repeat of render.
	HRESULT Render(int field, const REFERENCE_TIME frameStartTime) override;
	HRESULT FillBlack() override;

	void SetVideoRect(const CRect& videoRect)      override;
	HRESULT SetWindowRect(const CRect& windowRect) override;
	HRESULT Reset(bool bDisplayModeChange) override;
	bool IsInit() const override { return m_bHdrDisplaySwitching; }

	HRESULT GetCurentImage(long *pDIBImage) override;
	HRESULT GetDisplayedImage(BYTE **ppDib, unsigned* pSize) override;
	HRESULT GetVPInfo(std::wstring& str) override;

	// Settings
	void Configure(const Settings_t& config) override;

	void SetRotation(int value) override;
	void SetStereo3dTransform(int value) override;
	void GetMLStats(std::wstring& str) override;
	void GetRenderStats(::RenderStats_t& stats) override;

	void Flush() override;

	void ClearPreScaleShaders() override;
	void ClearPostScaleShaders() override;

	HRESULT AddPreScaleShader(const std::wstring& name, const std::string& srcCode) override;
	HRESULT AddPostScaleShader(const std::wstring& name, const std::string& srcCode) override;

	ISubPicAllocator* GetSubPicAllocator() override;

	void SwitchFullScreen(bool set) override;

private:
	void UpdateTexures();
	void UpdatePostScaleTexures();
	void UpdateUpscalingShaders();
	void UpdateDownscalingShaders();
	HRESULT UpdateConvertColorShader();
	void UpdateBitmapShader();

	HRESULT D3D11VPPass(ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect, const bool second);
	HRESULT ConvertColorPass(ID3D11Texture2D* pRenderTarget);
	HRESULT ResizeShaderPass(const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect, const int rotation);
	HRESULT FinalPass(const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect);

	void DrawSubtitles(ID3D11Texture2D* pRenderTarget);
	HRESULT Process(ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect, const bool second);

	HRESULT AlphaBlt(ID3D11ShaderResourceView* pShaderResource, ID3D11Texture2D* pRenderTarget,
					 ID3D11Buffer* pVertexBuffer, D3D11_VIEWPORT* pViewPort,
					 ID3D11SamplerState* pSampler);
	HRESULT TextureCopyRect(const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget,
							const CRect& srcRect, const CRect& destRect,
							ID3D11PixelShader* pPixelShader, ID3D11Buffer* pConstantBuffer,
							const int iRotation, const bool bFlip);

	// DELIVERED-frame instrument: downsample a frame boundary into its 256x144 cache
	// (draw time, debug-gated), and reduce both caches into [DELIVERED] log lines
	// (scene rate). See BuildRenderedLumaStats.
	void DownsampleDelivered(const Tex2D_t& src, ID3D11Texture2D* dst);
	void ReadDeliveredStats(unsigned long long frame);

	HRESULT TextureResizeShader(const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget,
								const CRect& srcRect, const CRect& destRect,
								ID3D11PixelShader* pPixelShader,
								const int iRotation, const bool bFlip);

	void UpdateStatsPresent();
	void UpdateStatsStatic() override;
	//void UpdateStatsPostProc();
	HRESULT DrawStats(ID3D11Texture2D* pRenderTarget);

public:
	// IMFVideoProcessor
	STDMETHODIMP SetProcAmpValues(DWORD dwFlags, DXVA2_ProcAmpValues *pValues) override;

	// IMFVideoMixerBitmap
	STDMETHODIMP SetAlphaBitmap(const MFVideoAlphaBitmap *pBmpParms) override;
	STDMETHODIMP UpdateAlphaBitmapParameters(const MFVideoAlphaBitmapParams *pBmpParms) override;
};
