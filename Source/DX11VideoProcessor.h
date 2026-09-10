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

	CComPtr<ID3D11Buffer> m_pCorrectionConstants;
	CComPtr<ID3D11PixelShader> m_pPSCorrection;
	const wchar_t* m_strCorrection = nullptr;

	// HDR tonemapping
	struct HDRParamsConstantBuffer_t {
		float MasteringMinLuminanceNits;
		float MasteringMaxLuminanceNits;
		float maxCLL;
		float maxFALL;
		float displayMaxNits;
		UINT selection; // 1 = ACES, 2 = Reinhard, 3 = Habel, 4 = Möbius, 5 = BT2390, 6 = ST 2094-10
		float padding[2];
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
	};
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

	// Zion ML debug (registry: MLDebugLog / MLDebugPassthrough)
	bool m_bMLDebug = false;
	bool m_bMLDebugCopy = false; // bind ps_simple to the grade steps (bisect test)
	bool m_bMLShadowDebugVis = false;
	bool m_bMLContrastDebugVis = false;
	float m_fMLShadowZoneCeil = 0.20f;
	float m_fMLShadowSkinProtect = 0.65f;
	float m_fMLShadowRatioCap = 1.40f;
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
	CComPtr<ID3D11Texture2D>          m_SkyMask;      // 640x360 R8_UNORM (pad-cropped content)
	CComPtr<ID3D11ShaderResourceView> m_SkyMaskSRV;
	CComPtr<ID3D11SamplerState>       m_pSamplerSky;  // bilinear + clamp
	CComPtr<ID3D11Texture2D>          m_SegDownsample; // 640x360 BGRA8 SDR-ized seg preview
	CComPtr<ID3D11Texture2D>          m_SegStaging;    // CPU-readable copy
	HRESULT CreateSkyMaskTexture();
	HRESULT CreateSkySegTextures();
	float   m_SkyFrac = 0.0f;      // last decode: fraction of valid pixels == sky
	float   m_SkyLuma = 0.0f;      // last decode: mean gamma-2.2 luma of sky pixels
	uint64_t m_nSkyFrame = 0;      // render-frame counter for cadence
	bool    m_bSkyEnabled = true;  // run segmentation (+ overlay)
	bool    m_bSkyDebugVis = false; // cyan overlay of the sky mask (validation)
	float   m_fMLSkyStrength = 0.6f; // sky tone-leg blend amount [0, 1]
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

	void SetHDR10ShaderParams(float, float, float, float, float, int);
	void SetDolbyVisionDynamicParams();

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
