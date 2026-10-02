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
#include <uuids.h>
#include <Mferror.h>
#include <Mfidl.h>
#include <optional>
#include <limits>
#include <algorithm>
#include "Helper.h"
#include "Times.h"
#include "resource.h"
#include "VideoRenderer.h"
#include "../Include/Version.h"
#include "DX11VideoProcessor.h"
#include "../Include/ID3DVideoMemoryConfiguration.h"
#include "Shaders.h"
#include "Utils/CPUInfo.h"

#include <dxgi1_6.h>

#include "../external/minhook/include/MinHook.h"

bool g_bPresent = false;
bool g_bCreateSwapChain = false;
HWND g_hWnd = nullptr;

typedef BOOL(WINAPI* pSetWindowPos)(
	_In_ HWND hWnd,
	_In_opt_ HWND hWndInsertAfter,
	_In_ int X,
	_In_ int Y,
	_In_ int cx,
	_In_ int cy,
	_In_ UINT uFlags);

pSetWindowPos pOrigSetWindowPosDX11 = nullptr;
static BOOL WINAPI pNewSetWindowPosDX11(
	_In_ HWND hWnd,
	_In_opt_ HWND hWndInsertAfter,
	_In_ int X,
	_In_ int Y,
	_In_ int cx,
	_In_ int cy,
	_In_ UINT uFlags)
{
	if (g_bPresent && g_hWnd == hWnd) {
		DLog(L"call SetWindowPos() function during Present()");
		uFlags |= SWP_ASYNCWINDOWPOS;
	}
	return pOrigSetWindowPosDX11(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
}

typedef LONG(WINAPI* pSetWindowLongA)(
	_In_ HWND hWnd,
	_In_ int nIndex,
	_In_ LONG dwNewLong);

pSetWindowLongA pOrigSetWindowLongADX11 = nullptr;
static LONG WINAPI pNewSetWindowLongADX11(
	_In_ HWND hWnd,
	_In_ int nIndex,
	_In_ LONG dwNewLong)
{
	if (g_bCreateSwapChain && g_hWnd == hWnd) {
		DLog(L"Blocking call SetWindowLongA() function during create fullscreen swap chain");
		return 0L;
	}
	return pOrigSetWindowLongADX11(hWnd, nIndex, dwNewLong);
}

template <typename T>
inline bool HookFunc(T** ppSystemFunction, PVOID pHookFunction)
{
	return MH_CreateHook(*ppSystemFunction, pHookFunction, reinterpret_cast<LPVOID*>(ppSystemFunction)) == MH_OK;
}

static const ScalingShaderResId s_Upscaling11ResIDs[UPSCALE_COUNT] = {
	{0,                            0,                            L"Nearest-neighbor"  },
	{IDF_PS_11_INTERP_MITCHELL4_X, IDF_PS_11_INTERP_MITCHELL4_Y, L"Mitchell-Netravali"},
	{IDF_PS_11_INTERP_CATMULL4_X,  IDF_PS_11_INTERP_CATMULL4_Y,  L"Catmull-Rom"       },
	{IDF_PS_11_INTERP_LANCZOS2_X,  IDF_PS_11_INTERP_LANCZOS2_Y,  L"Lanczos2"          },
	{IDF_PS_11_INTERP_LANCZOS3_X,  IDF_PS_11_INTERP_LANCZOS3_Y,  L"Lanczos3"          },
	{IDF_PS_11_INTERP_JINC2,       IDF_PS_11_INTERP_JINC2,       L"Jinc2m"            },
};

static const ScalingShaderResId s_Downscaling11ResIDs[DOWNSCALE_COUNT] = {
	{IDF_PS_11_CONVOL_BOX_X,       IDF_PS_11_CONVOL_BOX_Y,       L"Box"          },
	{IDF_PS_11_CONVOL_BILINEAR_X,  IDF_PS_11_CONVOL_BILINEAR_Y,  L"Bilinear"     },
	{IDF_PS_11_CONVOL_HAMMING_X,   IDF_PS_11_CONVOL_HAMMING_Y,   L"Hamming"      },
	{IDF_PS_11_CONVOL_BICUBIC05_X, IDF_PS_11_CONVOL_BICUBIC05_Y, L"Bicubic"      },
	{IDF_PS_11_CONVOL_BICUBIC15_X, IDF_PS_11_CONVOL_BICUBIC15_Y, L"Bicubic sharp"},
	{IDF_PS_11_CONVOL_LANCZOS_X,   IDF_PS_11_CONVOL_LANCZOS_Y,   L"Lanczos"      }
};

const UINT dither_size = 32;

struct VERTEX {
	DirectX::XMFLOAT3 Pos;
	DirectX::XMFLOAT2 TexCoord;
};

struct PS_EXTSHADER_CONSTANTS {
	DirectX::XMFLOAT2 pxy; // pixel size in normalized coordinates
	DirectX::XMFLOAT2 wh;  // width and height of texture
	uint32_t counter;      // rendered frame counter
	float clock;           // some time in seconds
	float reserved1;
	float reserved2;
};

static_assert(sizeof(PS_EXTSHADER_CONSTANTS) % 16 == 0);

static void FillVertices(VERTEX (&Vertices)[4], const UINT srcW, const UINT srcH, const RECT& srcRect,
	const int iRotation, const bool bFlip)
{
	const float src_dx = 1.0f / srcW;
	const float src_dy = 1.0f / srcH;
	float src_l = src_dx * srcRect.left;
	float src_r = src_dx * srcRect.right;
	const float src_t = src_dy * srcRect.top;
	const float src_b = src_dy * srcRect.bottom;

	POINT points[4];
	switch (iRotation) {
	case 90:
		points[0] = { -1, +1 };
		points[1] = { +1, +1 };
		points[2] = { -1, -1 };
		points[3] = { +1, -1 };
		break;
	case 180:
		points[0] = { +1, +1 };
		points[1] = { +1, -1 };
		points[2] = { -1, +1 };
		points[3] = { -1, -1 };
		break;
	case 270:
		points[0] = { +1, -1 };
		points[1] = { -1, -1 };
		points[2] = { +1, +1 };
		points[3] = { -1, +1 };
		break;
	default:
		points[0] = { -1, -1 };
		points[1] = { -1, +1 };
		points[2] = { +1, -1 };
		points[3] = { +1, +1 };
	}

	if (bFlip) {
		std::swap(src_l, src_r);
	}

	// Vertices for drawing whole texture
	// 2 ___4
	//  |\ |
	// 1|_\|3
	Vertices[0] = { {(float)points[0].x, (float)points[0].y, 0}, {src_l, src_b} };
	Vertices[1] = { {(float)points[1].x, (float)points[1].y, 0}, {src_l, src_t} };
	Vertices[2] = { {(float)points[2].x, (float)points[2].y, 0}, {src_r, src_b} };
	Vertices[3] = { {(float)points[3].x, (float)points[3].y, 0}, {src_r, src_t} };
}

static HRESULT CreateVertexBuffer(ID3D11Device* pDevice, ID3D11Buffer** ppVertexBuffer,
	const UINT srcW, const UINT srcH, const RECT& srcRect,
	const int iRotation, const bool bFlip)
{
	ASSERT(ppVertexBuffer);
	ASSERT(*ppVertexBuffer == nullptr);

	VERTEX Vertices[4];
	FillVertices(Vertices, srcW, srcH, srcRect, iRotation, bFlip);

	D3D11_BUFFER_DESC BufferDesc = { sizeof(Vertices), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
	D3D11_SUBRESOURCE_DATA InitData = { Vertices, 0, 0 };

	HRESULT hr = pDevice->CreateBuffer(&BufferDesc, &InitData, ppVertexBuffer);
	DLogIf(FAILED(hr), L"CreateVertexBuffer() : CreateBuffer() failed with error {}", HR2Str(hr));

	return hr;
}

static HRESULT FillVertexBuffer(ID3D11DeviceContext* pDeviceContext, ID3D11Buffer* pVertexBuffer,
	const UINT srcW, const UINT srcH, const RECT& srcRect,
	const int iRotation, const bool bFlip)
{
	ASSERT(pVertexBuffer);

	VERTEX Vertices[4];
	FillVertices(Vertices, srcW, srcH, srcRect, iRotation, bFlip);

	D3D11_MAPPED_SUBRESOURCE mr;
	HRESULT hr = pDeviceContext->Map(pVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
	if (FAILED(hr)) {
		DLog(L"FillVertexBuffer() : Map() failed with error {}", HR2Str(hr));
		return hr;
	}

	memcpy(mr.pData, &Vertices, sizeof(Vertices));
	pDeviceContext->Unmap(pVertexBuffer, 0);

	return hr;
}

static void TextureBlt11(
	ID3D11DeviceContext* pDeviceContext,
	ID3D11RenderTargetView* pRenderTargetView, D3D11_VIEWPORT& viewport,
	ID3D11InputLayout* pInputLayout,
	ID3D11VertexShader* pVertexShader,
	ID3D11PixelShader* pPixelShader,
	ID3D11ShaderResourceView* pShaderResourceViews,
	ID3D11SamplerState* pSampler,
	ID3D11Buffer* pConstantBuffer,
	ID3D11Buffer* pVertexBuffer)
{
	ASSERT(pDeviceContext);
	ASSERT(pRenderTargetView);
	ASSERT(pShaderResourceViews);

	const UINT Stride = sizeof(VERTEX);
	const UINT Offset = 0;

	// Set resources
	pDeviceContext->IASetInputLayout(pInputLayout);
	pDeviceContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);
	pDeviceContext->RSSetViewports(1, &viewport);
	pDeviceContext->OMSetBlendState(nullptr, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
	pDeviceContext->VSSetShader(pVertexShader, nullptr, 0);
	pDeviceContext->PSSetShader(pPixelShader, nullptr, 0);
	pDeviceContext->PSSetShaderResources(0, 1, &pShaderResourceViews);
	pDeviceContext->PSSetSamplers(0, 1, &pSampler);
	pDeviceContext->PSSetConstantBuffers(0, 1, &pConstantBuffer);
	pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	pDeviceContext->IASetVertexBuffers(0, 1, &pVertexBuffer, &Stride, &Offset);

	// Draw textured quad onto render target
	pDeviceContext->Draw(4, 0);

	ID3D11ShaderResourceView* views[1] = {};
	pDeviceContext->PSSetShaderResources(0, 1, views);
}

//
// CDX11VideoProcessor
//

HRESULT CDX11VideoProcessor::AlphaBlt(
	ID3D11ShaderResourceView* pShaderResource,
	ID3D11Texture2D* pRenderTarget,
	ID3D11Buffer* pVertexBuffer,
	D3D11_VIEWPORT* pViewPort,
	ID3D11SamplerState* pSampler)
{
	ID3D11RenderTargetView* pRenderTargetView;
	HRESULT hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);

	if (S_OK == hr) {
		UINT Stride = sizeof(VERTEX);
		UINT Offset = 0;

		// Set resources
		m_pDeviceContext->IASetInputLayout(m_pVSimpleInputLayout);
		m_pDeviceContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);
		m_pDeviceContext->RSSetViewports(1, pViewPort);
		m_pDeviceContext->OMSetBlendState(m_pAlphaBlendState, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
		m_pDeviceContext->VSSetShader(m_pVS_Simple, nullptr, 0);
		m_pDeviceContext->PSSetShader(m_pPS_BitmapToFrame, nullptr, 0);
		m_pDeviceContext->PSSetShaderResources(0, 1, &pShaderResource);
		m_pDeviceContext->PSSetSamplers(0, 1, &pSampler);
		m_pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		m_pDeviceContext->IASetVertexBuffers(0, 1, &pVertexBuffer, &Stride, &Offset);

		// Draw textured quad onto render target
		m_pDeviceContext->Draw(4, 0);

		pRenderTargetView->Release();
	}
	DLogIf(FAILED(hr), L"AlphaBlt() : CreateRenderTargetView() failed with error {}", HR2Str(hr));

	return hr;
}

HRESULT CDX11VideoProcessor::TextureCopyRect(
	const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget,
	const CRect& srcRect, const CRect& destRect,
	ID3D11PixelShader* pPixelShader, ID3D11Buffer* pConstantBuffer,
	const int iRotation, const bool bFlip)
{
	CComPtr<ID3D11RenderTargetView> pRenderTargetView;

	HRESULT hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
	if (FAILED(hr)) {
		DLog(L"TextureCopyRect() : CreateRenderTargetView() failed with error {}", HR2Str(hr));
		return hr;
	}

	hr = FillVertexBuffer(m_pDeviceContext, m_pVertexBuffer, Tex.desc.Width, Tex.desc.Height, srcRect, iRotation, bFlip);
	if (FAILED(hr)) {
		return hr;
	}

	D3D11_VIEWPORT VP;
	VP.TopLeftX = (FLOAT)destRect.left;
	VP.TopLeftY = (FLOAT)destRect.top;
	VP.Width    = (FLOAT)destRect.Width();
	VP.Height   = (FLOAT)destRect.Height();
	VP.MinDepth = 0.0f;
	VP.MaxDepth = 1.0f;

	TextureBlt11(m_pDeviceContext, pRenderTargetView, VP, m_pVSimpleInputLayout, m_pVS_Simple, pPixelShader, Tex.pShaderResource, m_pSamplerPoint, pConstantBuffer, m_pVertexBuffer);

	return hr;
}

HRESULT CDX11VideoProcessor::TextureResizeShader(
	const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget,
	const CRect& srcRect, const CRect& dstRect,
	ID3D11PixelShader* pPixelShader,
	const int iRotation, const bool bFlip)
{
	CComPtr<ID3D11RenderTargetView> pRenderTargetView;

	HRESULT hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::TextureResizeShader() : CreateRenderTargetView() failed with error {}", HR2Str(hr));
		return hr;
	}

	hr = FillVertexBuffer(m_pDeviceContext, m_pVertexBuffer, Tex.desc.Width, Tex.desc.Height, srcRect, iRotation, bFlip);
	if (FAILED(hr)) {
		return hr;
	}

	const FLOAT constants[][4] = {
		{(float)Tex.desc.Width, (float)Tex.desc.Height, 1.0f / Tex.desc.Width, 1.0f / Tex.desc.Height},
		{(float)srcRect.Width() / dstRect.Width(), (float)srcRect.Height() / dstRect.Height(), 0, 0}
	};

	D3D11_MAPPED_SUBRESOURCE mr;
	hr = m_pDeviceContext->Map(m_pResizeShaderConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::TextureResizeShader() : Map() failed with error {}", HR2Str(hr));
		return hr;
	}

	memcpy(mr.pData, &constants, sizeof(constants));
	m_pDeviceContext->Unmap(m_pResizeShaderConstantBuffer, 0);

	D3D11_VIEWPORT VP;
	VP.TopLeftX = (FLOAT)dstRect.left;
	VP.TopLeftY = (FLOAT)dstRect.top;
	VP.Width = (FLOAT)dstRect.Width();
	VP.Height = (FLOAT)dstRect.Height();
	VP.MinDepth = 0.0f;
	VP.MaxDepth = 1.0f;

	TextureBlt11(m_pDeviceContext, pRenderTargetView, VP, m_pVSimpleInputLayout, m_pVS_Simple, pPixelShader, Tex.pShaderResource, m_pSamplerPoint, m_pResizeShaderConstantBuffer, m_pVertexBuffer);

	return hr;
}

// CDX11VideoProcessor

CDX11VideoProcessor::CDX11VideoProcessor(CMpcVideoRenderer* pFilter, const Settings_t& config, HRESULT& hr)
	: CVideoProcessor(pFilter)
{
	m_bShowStats           = config.bShowStats;
	m_iResizeStats         = config.iResizeStats;
	m_iTexFormat           = config.iTexFormat;
	m_VPFormats            = config.VPFmts;
	m_iVPDeinterlacing     = config.iVPDeinterlacing;
	m_bDeintDouble         = config.bDeintDouble;
	m_bVPScaling           = config.bVPScaling;
	m_iVPSuperRes          = config.iVPSuperRes;
	m_bVPRTXVideoHDR       = config.bVPRTXVideoHDR;
	m_iChromaScaling       = config.iChromaScaling;
	m_iUpscaling           = config.iUpscaling;
	m_iDownscaling         = config.iDownscaling;
	m_bInterpolateAt50pct  = config.bInterpolateAt50pct;
	m_bUseDither           = config.bUseDither;
	m_bDeintBlend          = config.bDeintBlend;
	m_iSwapEffect          = config.iSwapEffect;
	m_bVBlankBeforePresent = config.bVBlankBeforePresent;
	m_bAdjustPresentTime   = config.bAdjustPresentTime;
	m_bHdrPreferDoVi       = config.bHdrPreferDoVi;
	m_bHdrPassthrough      = config.bHdrPassthrough;
	m_bHdrLocalToneMapping = config.bHdrLocalToneMapping;
	m_iHdrLocalToneMappingType = config.iHdrLocalToneMappingType;
	m_iHdrDisplayMaxNits   = config.iHdrDisplayMaxNits;
	m_bHDRPassthroughRaw   = config.bHDRPassthroughRaw;
	m_iHdrToggleDisplay    = config.iHdrToggleDisplay;
	m_iHdrOsdBrightness    = config.iHdrOsdBrightness;
	m_bConvertToSdr        = config.bConvertToSdr;
	m_iSDRDisplayNits      = config.iSDRDisplayNits;
	m_fHdrDisplayMinNits   = config.fHdrDisplayMinNits;
	m_fHdrTitlePeakNits    = config.fHdrTitlePeakNits;
	m_fHdrHableKnee        = config.fHdrHableKnee;

	m_bML                   = config.bMLEnabled;
	m_iMLMode               = config.iMLMode;
	m_fMLGamma              = config.fMLGamma;
	m_fMLShadow             = config.fMLShadow;
	m_fMLShadowDehaze       = config.fMLShadowDehaze;
	m_fMLHighlight          = config.fMLHighlight;
	m_fMLHighlightRolloffCtrl = config.fMLHighlightRolloffCtrl;
	m_fMLGammaTopUp         = config.fMLGammaTopUp;
	m_bMLDebugCopy          = config.bMLDebugCopy;
	m_bMLDebug              = config.bMLDebugLog;
	if (m_pFaceDetect) m_pFaceDetect->SetLogEnabled(m_bMLDebug);   // per-frame face lines follow the flag
	if (m_pSkySeg)     m_pSkySeg->SetLogEnabled(m_bMLDebug);       // .. and sky seg
	m_bMLShadowDebugVis    = config.bMLShadowDebugVis;
	m_bMLContrastDebugVis  = config.bMLContrastDebugVis;
	m_bSkyEnabled          = config.bMLSkyEnabled;
	m_bSkyDebugVis         = config.bMLSkyDebugVis;
	m_fMLSkyStrength      = config.fMLSkyStrength;
	m_bPersonEnabled       = config.bMLPersonEnabled;
	m_fPersonStrength      = config.fMLPersonStrength;
	m_bPersonDebugVis      = config.bMLPersonDebugVis;
	m_fMLShadowZoneCeil    = config.fMLShadowZoneCeil;
	m_fMLShadowSkinProtect = config.fMLShadowSkinProtect;
	m_fMLShadowRatioCap    = config.fMLShadowRatioCap;
	m_bMLContrastEnabled   = config.bMLContrastEnabled;
	m_fMLContrast          = config.fMLContrast;
	m_fMLContrastZoneLo    = config.fMLContrastZoneLo;
	m_fMLContrastSkinProtect = config.fMLContrastSkinProtect;
	m_fMLContrastRatioCap  = config.fMLContrastRatioCap;
	m_bMLHuntEnabled        = config.bMLHuntEnabled;
	m_fMLHunt               = config.fMLHunt;
	m_bMLChromaEnabled      = config.bMLChromaEnabled;
	m_fMLChromaNeutral      = config.fMLChromaNeutral;
	m_fMLChromaFire         = config.fMLChromaFire;
	m_fMLChromaKnee         = config.fMLChromaKnee;
	m_fMLChromaSkinProtect  = config.fMLChromaSkinProtect;
	m_bMLBarsEnabled        = config.bMLBars;
	m_bMLBarsHighlight      = config.bMLBarsHighlight;
	DLog(L"CDX11VideoProcessor() BARS init: bMLBars=%d bMLBarsHighlight=%d sizeof(Settings_t)=%zu",
		(int)config.bMLBars, (int)config.bMLBarsHighlight, sizeof(Settings_t));

	m_nCurrentAdapter = -1;

	hr = CreateDXGIFactory1(IID_IDXGIFactory1, (void**)&m_pDXGIFactory1);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::CDX11VideoProcessor() : CreateDXGIFactory1() failed with error {}", HR2Str(hr));
		return;
	}

	// set default ProcAmp ranges and values
	SetDefaultDXVA2ProcAmpRanges(m_DXVA2ProcAmpRanges);
	SetDefaultDXVA2ProcAmpValues(m_DXVA2ProcAmpValues);

	pOrigSetWindowPosDX11 = SetWindowPos;
	auto ret = HookFunc(&pOrigSetWindowPosDX11, pNewSetWindowPosDX11);
	DLogIf(!ret, L"CDX11VideoProcessor::CDX11VideoProcessor() : hook for SetWindowPos() fail");

	pOrigSetWindowLongADX11 = SetWindowLongA;
	ret = HookFunc(&pOrigSetWindowLongADX11, pNewSetWindowLongADX11);
	DLogIf(!ret, L"CDX11VideoProcessor::CDX11VideoProcessor() : hook for SetWindowLongA() fail");

	MH_EnableHook(MH_ALL_HOOKS);

	// ML log file: always open at construction so every init step is captured.
	// The m_bMLDebug flag gates per-frame verbosity, not file existence.
	{
		wchar_t logPath[MAX_PATH] = {};
		swprintf_s(logPath, L"%s\\ml_debug.csv", GetMLModuleDir().c_str());
		if (_wfopen_s(&m_pMLDebugFile, logPath, L"a") != 0 || !m_pMLDebugFile) {
			wchar_t temp[MAX_PATH] = {};
			GetTempPathW(MAX_PATH, temp);
			swprintf_s(logPath, L"%smpcvr_ml_debug.csv", temp);
			_wfopen_s(&m_pMLDebugFile, logPath, L"a");
		}
		if (m_pMLDebugFile) {
			if (fseek(m_pMLDebugFile, 0, SEEK_END) == 0 && ftell(m_pMLDebugFile) == 0) {
				fwprintf(m_pMLDebugFile, L"# ml_debug.csv — MPCVR ML diagnostic log\n");
			}
			fwprintf(m_pMLDebugFile, L"# init (bMLEnabled=%d bMLDebug=%d)\n", (int)m_bML, (int)m_bMLDebug);
				fwprintf(m_pMLDebugFile, L"# BUILD ZION-PIPE tag=skyMC-decouple-v3 person=perFrame skyMaxCad=5 personModel=yolo26n-seg_op21\n");
				fflush(m_pMLDebugFile);
		}
	}

	// Deterministic ML init: load all models and face detection at construction
	// time. No lazy/deferred loading — the .ax pays the cost upfront so that
	// enabling ML grade or switching video processors incurs zero delay.
	m_bMLModelLoadTried = true;
	m_bMLGammaModelLoaded     = m_MLGammaModel.Load(GetMLModelPath(L"xgb_gamma_model.plxgb").c_str());
	m_bMLShadowModelLoaded    = m_MLShadowModel.Load(GetMLModelPath(L"xgb_shadow_recovery.plxgb").c_str());
	m_bMLHighlightModelLoaded = m_MLHighlightModel.Load(GetMLModelPath(L"xgb_highlight_rolloff.plxgb").c_str());
	if (m_pMLDebugFile) {
		fwprintf(m_pMLDebugFile, L"INIT_MODELS gamma=%s shadow=%s highlight=%s\n",
			(m_bMLGammaModelLoaded ? L"ok" : L"missing"),
			(m_bMLShadowModelLoaded ? L"ok" : L"missing"),
			(m_bMLHighlightModelLoaded ? L"ok" : L"missing"));
		fflush(m_pMLDebugFile);
	}
	EXECUTE_ASSERT(m_bMLGammaModelLoaded);
	EXECUTE_ASSERT(m_bMLShadowModelLoaded);
	EXECUTE_ASSERT(m_bMLHighlightModelLoaded);

	// Face detection (YuNet) DISABLED — person instance masks replace it. The
	// member stays NULL so every guarded block is inert; re-enable by restoring
	// these lines and un-#if-0'ing the detect block in ExtractMLFeatures.
#if 0
	m_pFaceDetect = std::make_unique<CFaceDetect>();
	m_pFaceDetect->SetLogFile(m_pMLDebugFile);
	HRESULT hrFace = m_pFaceDetect->Init(GetMLModelPath(L"face_detection_yunet.onnx"));
	if (m_pMLDebugFile) {
		fwprintf(m_pMLDebugFile, L"INIT_FACE hr=0x%08X ready=%d err=%s\n",
			(unsigned)hrFace, (int)m_pFaceDetect->IsReady(),
			m_pFaceDetect->GetLastError().c_str());
		fflush(m_pMLDebugFile);
	}
	EXECUTE_ASSERT(SUCCEEDED(hrFace) && m_pFaceDetect->IsReady());
#endif

	m_pSkySeg = std::make_unique<CSkySeg>();
	m_pSkySeg->SetLogFile(m_pMLDebugFile);
	m_pSkySeg->SetLogEnabled(m_bMLDebug);
	HRESULT hrSky = m_pSkySeg->Init(GetMLModelPath(L"yolo26n-sem.onnx"));
	if (m_pMLDebugFile) {
		fwprintf(m_pMLDebugFile, L"INIT_SKY hr=0x%08X ready=%d err=%s\n",
			(unsigned)hrSky, (int)m_pSkySeg->IsReady(),
			m_pSkySeg->GetLastError().c_str());
		fflush(m_pMLDebugFile);
	}
	// Sky seg is optional — do not hard-fail the filter if the model is missing.
	if (FAILED(hrSky) && m_pMLDebugFile) {
		fwprintf(m_pMLDebugFile, L"INIT_SKY disabled (model load failed)\n");
		fflush(m_pMLDebugFile);
	}

	// Person instance segmentation (yolo26n-seg, COCO class 0) — the continuity
	// layer for skin protection. Ships re-stamped to opset 21 (the raw export is
	// opset 22, rejected by the bundled ORT 1.21). Optional like sky.
	m_pPersonSeg = std::make_unique<CPersonSeg>();
	m_pPersonSeg->SetLogFile(m_pMLDebugFile);
	m_pPersonSeg->SetLogEnabled(m_bMLDebug);
	HRESULT hrPerson = m_pPersonSeg->Init(GetMLModelPath(L"yolo26n-seg_op21.onnx"));
	if (m_pMLDebugFile) {
		fwprintf(m_pMLDebugFile, L"INIT_PERSON hr=0x%08X ready=%d err=%s\n",
			(unsigned)hrPerson, (int)m_pPersonSeg->IsReady(),
			m_pPersonSeg->GetLastError().c_str());
		fflush(m_pMLDebugFile);
	}
	if (FAILED(hrPerson) && m_pMLDebugFile) {
		fwprintf(m_pMLDebugFile, L"INIT_PERSON disabled (model load failed)\n");
		fflush(m_pMLDebugFile);
	}

	CComPtr<IDXGIAdapter> pDXGIAdapter;
	for (UINT adapter = 0; m_pDXGIFactory1->EnumAdapters(adapter, &pDXGIAdapter) != DXGI_ERROR_NOT_FOUND; ++adapter) {
		CComPtr<IDXGIOutput> pDXGIOutput;
		for (UINT output = 0; pDXGIAdapter->EnumOutputs(output, &pDXGIOutput) != DXGI_ERROR_NOT_FOUND; ++output) {
			DXGI_OUTPUT_DESC desc{};
			if (SUCCEEDED(pDXGIOutput->GetDesc(&desc))) {
				DisplayConfig_t displayConfig = {};
				if (GetDisplayConfig(desc.DeviceName, displayConfig)) {
					m_hdrModeStartState[desc.DeviceName] = displayConfig.HDREnabled();
				}
			}

			pDXGIOutput.Release();
		}

		pDXGIAdapter.Release();
	}
}

CDX11VideoProcessor::~CDX11VideoProcessor()
{
	for (const auto& [displayName, state] : m_hdrModeSavedState) {
		DisplayConfig_t displayConfig = {};
		if (GetDisplayConfig(displayName.c_str(), displayConfig)) {
			if (displayConfig.HDRSupported() && displayConfig.HDREnabled() != state) {
				const auto ret = ToggleHDR(displayConfig, state);
				DLogIf(!ret, L"CDX11VideoProcessor::~CDX11VideoProcessor() : Toggle HDR {} for '{}' failed", state ? L"ON" : L"OFF", displayName);
			}
		}
	}

	m_pFilter->m_pSubPicQueue.Release();
	m_pSubPicAllocator.Release();

	ReleaseSwapChain();
	m_pDXGIFactory2.Release();

	ReleaseDevice();

	if (m_pFaceDetect) m_pFaceDetect->Shutdown();
	m_pFaceDetect.reset();

	if (m_pMLDebugFile) {
		fclose(m_pMLDebugFile);
		m_pMLDebugFile = nullptr;
	}

	m_pDXGIFactory1.Release();

	MH_RemoveHook(SetWindowPos);
	MH_RemoveHook(SetWindowLongA);
}

void CDX11VideoProcessor::FillDisplayParams()
{
	m_bHdrPassthroughSupport = false;
	m_bHdrDisplayModeEnabled = false;
	m_DisplayBitsPerChannel = 8;

	m_bACMEnabled = false;

	MONITORINFOEXW mi = { sizeof(mi) };
	GetMonitorInfoW(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTOPRIMARY), reinterpret_cast<LPMONITORINFO>(&mi));
	DisplayConfig_t displayConfig = {};

	if (GetDisplayConfig(mi.szDevice, displayConfig)) {
		m_bHdrDisplayModeEnabled = displayConfig.HDREnabled();
		m_bHdrPassthroughSupport = displayConfig.HDRSupported() && m_bHdrDisplayModeEnabled;
		m_DisplayBitsPerChannel = displayConfig.bitsPerChannel;

		m_bACMEnabled = !m_bHdrDisplayModeEnabled && displayConfig.ACMEnabled();

#ifdef _DEBUG
		auto displayDesc = std::format(L"\nDisplay: {} {}", mi.szDevice, DisplayConfigToString(displayConfig));
		if (displayConfig.bitsPerChannel) {
			auto colenc = ColorEncodingToString(displayConfig.colorEncoding);
			if (colenc) {
				displayDesc.append(std::format(L"\n  Color: {} {}-bit", colenc, displayConfig.bitsPerChannel));
				if (displayConfig.HDRSupported()) {
					displayDesc.append(L", HDR10: ");
					displayDesc.append(displayConfig.HDREnabled() ? L"on" : L"off");

					if (IsWindows11_24H2OrGreater()) {
						auto ColorModeToStr = [](DISPLAYCONFIG_ADVANCED_COLOR_MODE ColorMode) {
							std::wstring str;
#define UNPACK_VALUE(VALUE) case VALUE: str = L"" #VALUE; break;
							switch (ColorMode) {
								UNPACK_VALUE(DISPLAYCONFIG_ADVANCED_COLOR_MODE_SDR);
								UNPACK_VALUE(DISPLAYCONFIG_ADVANCED_COLOR_MODE_WCG);
								UNPACK_VALUE(DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR);
								default:
									str = std::to_wstring(static_cast<int>(ColorMode));
							};
#undef UNPACK_VALUE

							return str;
						};

						const auto& colors = displayConfig.windows1124H2Colors;
						displayDesc.append(std::format(L"\n         Advanced Color: Supported: {}, Active: {}, Limited by OS policy: {}, HDR is supported: {}",
													   colors.advancedColorSupported, colors.advancedColorActive,
													   colors.advancedColorLimitedByPolicy, colors.highDynamicRangeSupported));
						displayDesc.append(std::format(L"\n                         HDR enabled: {}, Wide supported: {}, Wide enabled: {}",
													   colors.highDynamicRangeUserEnabled, colors.wideColorSupported,
													   colors.wideColorUserEnabled));
						displayDesc.append(std::format(L"\n                         Display color mode: {}", ColorModeToStr(colors.activeColorMode)));
					} else {
						const auto& colors = displayConfig.advancedColor;
						displayDesc.append(std::format(L"\n         Advanced Color: Supported: {}, Enabled: {}, Wide forced: {}, Force disabled: {}",
													   colors.advancedColorSupported, colors.advancedColorEnabled,
													   colors.wideColorEnforced, colors.advancedColorForceDisabled));
					}
				}
			}
		}

		DLog(L"CDX11VideoProcessor::FillDisplayParams():{}", displayDesc);
#endif
	}
}

HRESULT CDX11VideoProcessor::Init(const HWND hwnd, const bool displayHdrChanged, bool* pChangeDevice/* = nullptr*/)
{
	DLog(L"CDX11VideoProcessor::Init()");

	const bool bWindowChanged = displayHdrChanged || (m_hWnd != hwnd);
	g_hWnd = m_hWnd = hwnd;

	FillDisplayParams();

	if (m_bExclusiveScreen != m_pFilter->m_bExclusiveScreen) {
		m_srcVideoTransferFunction = 0;
	}

	IDXGIAdapter* pDXGIAdapter = nullptr;
	const UINT currentAdapter = GetAdapter(hwnd, m_pDXGIFactory1, &pDXGIAdapter);
	CheckPointer(pDXGIAdapter, E_FAIL);
	if (m_nCurrentAdapter == currentAdapter) {
		SAFE_RELEASE(pDXGIAdapter);

		SetCallbackDevice();

		if (!m_pDXGISwapChain1 || m_bExclusiveScreen != m_pFilter->m_bExclusiveScreen || bWindowChanged) {
			InitSwapChain(bWindowChanged);
			UpdateStatsStatic();
			m_pFilter->OnDisplayModeChange();
		}

		return S_OK;
	}
	m_nCurrentAdapter = currentAdapter;

	ReleaseSwapChain();
	m_pDXGIFactory2.Release();
	ReleaseDevice();

	D3D_FEATURE_LEVEL featureLevels[] = {
		D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
	};
	D3D_FEATURE_LEVEL featurelevel;

	ID3D11Device *pDevice = nullptr;

	HRESULT hr = D3D11CreateDevice(
		pDXGIAdapter,
		D3D_DRIVER_TYPE_UNKNOWN,
		nullptr,
#ifdef _DEBUG
		D3D11_CREATE_DEVICE_DEBUG,
#else
		0,
#endif
		featureLevels,
		std::size(featureLevels),
		D3D11_SDK_VERSION,
		&pDevice,
		&featurelevel,
		nullptr);
#ifdef _DEBUG
	if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING || (hr == E_FAIL && !IsWindows8OrGreater())) {
		DLog(L"WARNING: D3D11 debugging messages will not be displayed");
		hr = D3D11CreateDevice(
			pDXGIAdapter,
			D3D_DRIVER_TYPE_UNKNOWN,
			nullptr,
			0,
			featureLevels,
			std::size(featureLevels),
			D3D11_SDK_VERSION,
			&pDevice,
			&featurelevel,
			nullptr);
	}
#endif
	SAFE_RELEASE(pDXGIAdapter);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::Init() : D3D11CreateDevice() failed with error {}", HR2Str(hr));
		return hr;
	}

	DLog(L"CDX11VideoProcessor::Init() : D3D11CreateDevice() successfully with feature level {}.{}", (featurelevel >> 12), (featurelevel >> 8) & 0xF);

	hr = SetDevice(pDevice, nullptr);
	pDevice->Release();

	if (S_OK == hr) {
		if (pChangeDevice) {
			*pChangeDevice = true;
		}
	}

	if (m_VendorId == PCIV_INTEL && CPUInfo::HaveSSE41()) {
		m_pCopyGpuFn = CopyGpuFrame_SSE41;
	} else {
		m_pCopyGpuFn = CopyPlaneAsIs;
	}

	return hr;
}

bool CDX11VideoProcessor::Initialized()
{
	return (m_pDevice.p != nullptr && m_pDeviceContext.p != nullptr);
}

void CDX11VideoProcessor::ReleaseVP()
{
	DLog(L"CDX11VideoProcessor::ReleaseVP()");

	m_pFilter->ResetStreamingTimes2();
	m_RenderStats.Reset();

	if (m_pDeviceContext) {
		m_pDeviceContext->ClearState();
	}

	m_TexSrcVideo.Release();
	m_TexConvertOutput.Release();
	m_TexResize.Release();
	m_TexsPostScale.Release();

	m_PSConvColorData.Release();
	m_pDoviCurvesConstantBuffer.Release();

	m_D3D11VP.ReleaseVideoProcessor();
	m_strCorrection = nullptr;

	m_srcParams      = {};
	m_srcDXGIFormat  = DXGI_FORMAT_UNKNOWN;
	m_pCopyPlaneFn   = CopyPlaneAsIs;
	m_srcWidth       = 0;
	m_srcHeight      = 0;
}

void CDX11VideoProcessor::ReleaseDevice()
{
	DLog(L"CDX11VideoProcessor::ReleaseDevice()");

	ReleaseVP();
	m_D3D11VP.ReleaseVideoDevice();

	m_StatsBackground.InvalidateDeviceObjects();
	m_Font3D.InvalidateDeviceObjects();
	m_Rect3D.InvalidateDeviceObjects();

	m_Underlay.InvalidateDeviceObjects();
	m_Lines.InvalidateDeviceObjects();
	m_SyncLine.InvalidateDeviceObjects();

	m_TexDither.Release();
	m_bAlphaBitmapEnable = false;
	m_pAlphaBitmapVertex.Release();
	m_TexAlphaBitmap.Release();

	ClearPreScaleShaders();
	ClearPostScaleShaders();
	m_pPSCorrection.Release();
	m_pPSConvertColor.Release();
	m_pPSConvertColorDeint.Release();

	m_pPSHDR10ToneMapping.Release();

	m_pShaderUpscaleX.Release();
	m_pShaderUpscaleY.Release();
	m_pShaderDownscaleX.Release();
	m_pShaderDownscaleY.Release();
	m_strShaderX = nullptr;
	m_strShaderY = nullptr;
	m_pPSFinalPass.Release();

	m_pCorrectionConstants.Release();
	m_pPostScaleConstants.Release();

	m_pCubeLutSRV.Release();
	m_pCubeLut.Release();
	m_pCubeLutSampler.Release();
	m_iCubeLutSize    = 0;
	m_bCubeLutEnabled = false;

	// The experimental tone curve belongs to the device too: these are D3D11
	// objects, and leaving them alive across a device reset would hand the new
	// device textures it never created. Rebuilt on the next frame from the
	// statistics, so nothing is lost by releasing them here.
	m_pToneLutSRV.Release();
	m_pToneLut.Release();
	m_pToneLutSampler.Release();
	m_bToneLutReady = false;
	m_MLToneMaxSmooth = -1.0f;

	m_pHDR10ToneMappingConstants.Release();
	m_pDoViDynamicConstants.Release();

	m_pPSMLGamma.Release();
	m_pPSMLShadow.Release();
	m_pPSMLHighlight.Release();
	m_pPSMLChroma.Release();
	m_pPSMLBars.Release();
	m_SkinLUT.Release();
	m_SkinLUTSRV.Release();
	m_pSamplerSkin.Release();
	m_SkyMask.Release();
	m_SkyMaskSRV.Release();
	m_pSamplerSky.Release();
	m_SkinGate.Release();
	m_SkinGateSRV.Release();
	m_pSamplerGate.Release();
	m_SegDownsample.Release();
	m_SegStaging.Release();
	m_MaskProbe.Release();
	m_pPSMLDownsample.Release();
	m_pPSFaceDown.Release();
	m_pFaceDownParams.Release();
	m_MLDownsample.Release();
	m_MLStaging.Release();
	m_MLDeliveredCurveOut.Release();
	m_MLDeliveredPostGrade.Release();
	m_FaceDownsample.Release();
	m_FaceStaging.Release();
	m_pMLParams.Release();
	m_lastMLParams = {};
	m_bMLActive = false;
	m_bMLResourcesReady = false;
	m_bMLFeaturesReady = false;
	m_bMLFeaturesStale = false;
	m_bMLConvertSeen = false;
	m_MLIIRGamma = 1.0f;
	m_MLPrevL1Max = -1.0f;
	m_MLRawTexture = 0.0f;
	m_MLTextureSmooth = 0.0f;

#if TEST_SHADER
	m_pPS_TEST.Release();
#endif

	m_pVSimpleInputLayout.Release();
	m_pVS_Simple.Release();
	m_pPS_Simple.Release();
	m_pPS_BitmapToFrame.Release();
	m_pSamplerPoint.Release();
	m_pSamplerLinear.Release();
	m_pSamplerDither.Release();
	m_pAlphaBlendState.Release();

	m_Alignment.texture.Release();
	m_Alignment.cformat = {};
	m_Alignment.cx = {};

	m_pVertexBuffer.Release();
	m_pResizeShaderConstantBuffer.Release();
	m_pHalfOUtoInterlaceConstantBuffer.Release();
	m_pFinalPassConstantBuffer.Release();

	if (m_pDeviceContext) {
		// need ClearState() (see ReleaseVP()) and Flush() for ID3D11DeviceContext when using DXGI_SWAP_EFFECT_DISCARD in Windows 8/8.1
		m_pDeviceContext->Flush();
	}
	m_pDeviceContext.Release();
	m_bCallbackDeviceIsSet = false;

#if (1 && _DEBUG)
	if (m_pDevice) {
		ID3D11Debug* pDebugDevice = nullptr;
		HRESULT hr2 = m_pDevice->QueryInterface(IID_PPV_ARGS(&pDebugDevice));
		if (S_OK == hr2) {
			hr2 = pDebugDevice->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL);
			ASSERT(S_OK == hr2);
		}
		SAFE_RELEASE(pDebugDevice);
	}
#endif

	m_pDevice.Release();
}

void CDX11VideoProcessor::ReleaseSwapChain()
{
	m_pFilter->LogLine("renderer: ReleaseSwapChain — tearing the swap chain down%s",
		m_pDXGISwapChain1 ? "" : " (nothing to release)");
	if (m_pDXGISwapChain1) {
		m_pDXGISwapChain1->SetFullscreenState(FALSE, nullptr);
	}
	m_pDXGIOutput.Release();
	m_pDXGISwapChain4.Release();
	m_pDXGISwapChain1.Release();

	m_MaxDisplayLuminance = 0;
}

UINT CDX11VideoProcessor::GetPostScaleSteps()
{
	UINT nSteps = m_pPostScaleShaders.size();
	if (m_pPSCorrection) {
		nSteps++;
	}
	if (m_pPSHDR10ToneMapping) {
		nSteps++;
	}
	if (m_bMLActive && m_pPSMLGamma) {
		nSteps += 3; // shadow -> highlight -> gamma
	}
	if (m_bMLActive && m_pPSMLChroma) {
		nSteps++; // chroma vector tuning (skin-guarded)
	}
	if (m_bMLActive && m_pPSMLBars && m_bMLBarsEnabled && !m_bMLDebugCopy && !m_bHDRPassthroughRaw) {
		nSteps++; // P0 bar repaint
	}
	if (m_pPSHalfOUtoInterlace) {
		nSteps++;
	}
	if (m_bFinalPass) {
		nSteps++;
	}
	return nSteps;
}

HRESULT CDX11VideoProcessor::CreatePShaderFromResource(ID3D11PixelShader** ppPixelShader, UINT resid)
{
	if (!m_pDevice || !ppPixelShader) {
		return E_POINTER;
	}

	LPVOID data;
	DWORD size;
	HRESULT hr = GetDataFromResource(data, size, resid);
	if (FAILED(hr)) {
		return hr;
	}

	return m_pDevice->CreatePixelShader(data, size, nullptr, ppPixelShader);
}

void CDX11VideoProcessor::SetShaderConvertColorParams()
{
	mp_cmat cmatrix;

	if (m_Dovi.bValid) {
		const float brightness = DXVA2FixedToFloat(m_DXVA2ProcAmpValues.Brightness) / 255;
		const float contrast   = DXVA2FixedToFloat(m_DXVA2ProcAmpValues.Contrast);

		for (int i = 0; i < 3; i++) {
			cmatrix.m[i][0] = (float)m_Dovi.msd.ColorMetadata.ycc_to_rgb_matrix[i * 3 + 0] * contrast;
			cmatrix.m[i][1] = (float)m_Dovi.msd.ColorMetadata.ycc_to_rgb_matrix[i * 3 + 1] * contrast;
			cmatrix.m[i][2] = (float)m_Dovi.msd.ColorMetadata.ycc_to_rgb_matrix[i * 3 + 2] * contrast;
		}

		for (int i = 0; i < 3; i++) {
			cmatrix.c[i] = brightness;
			for (int j = 0; j < 3; j++) {
				cmatrix.c[i] -= cmatrix.m[i][j] * m_Dovi.msd.ColorMetadata.ycc_to_rgb_offset[j];
			}
		}

		m_PSConvColorData.bEnable = true;
	}
	else {
		mp_csp_params csp_params;
		set_colorspace(m_srcExFmt, csp_params.color);
		csp_params.brightness = DXVA2FixedToFloat(m_DXVA2ProcAmpValues.Brightness) / 255;
		csp_params.contrast   = DXVA2FixedToFloat(m_DXVA2ProcAmpValues.Contrast);
		csp_params.hue        = DXVA2FixedToFloat(m_DXVA2ProcAmpValues.Hue) / 180 * acos(-1);
		csp_params.saturation = DXVA2FixedToFloat(m_DXVA2ProcAmpValues.Saturation);
		csp_params.gray       = m_srcParams.CSType == CS_GRAY;

		csp_params.input_bits = csp_params.texture_bits = m_srcParams.CDepth;

		mp_get_csp_matrix(&csp_params, &cmatrix);

		m_PSConvColorData.bEnable =
			m_srcParams.CSType == CS_YUV ||
			m_srcParams.cformat == CF_GBRP8 || m_srcParams.cformat == CF_GBRP10 || m_srcParams.cformat == CF_GBRP16 ||
			csp_params.gray ||
			fabs(csp_params.brightness) > 1e-4f || fabs(csp_params.contrast - 1.0f) > 1e-4f;
	}

	PS_COLOR_TRANSFORM cbuffer = {
		{cmatrix.m[0][0], cmatrix.m[0][1], cmatrix.m[0][2], 0},
		{cmatrix.m[1][0], cmatrix.m[1][1], cmatrix.m[1][2], 0},
		{cmatrix.m[2][0], cmatrix.m[2][1], cmatrix.m[2][2], 0},
		{cmatrix.c[0],    cmatrix.c[1],    cmatrix.c[2],    0},
	};

	if (m_srcParams.cformat == CF_GBRP8 || m_srcParams.cformat == CF_GBRP10 || m_srcParams.cformat == CF_GBRP16) {
		std::swap(cbuffer.cm_r.x, cbuffer.cm_r.y); std::swap(cbuffer.cm_r.y, cbuffer.cm_r.z);
		std::swap(cbuffer.cm_g.x, cbuffer.cm_g.y); std::swap(cbuffer.cm_g.y, cbuffer.cm_g.z);
		std::swap(cbuffer.cm_b.x, cbuffer.cm_b.y); std::swap(cbuffer.cm_b.y, cbuffer.cm_b.z);
	}
	else if (m_srcParams.CSType == CS_GRAY) {
		cbuffer.cm_g.x = cbuffer.cm_g.y;
		cbuffer.cm_g.y = 0;
		cbuffer.cm_b.x = cbuffer.cm_b.z;
		cbuffer.cm_b.z = 0;
	}

	if (m_PSConvColorData.pConstants) {
		m_pDeviceContext->UpdateSubresource(m_PSConvColorData.pConstants, 0, nullptr, &cbuffer, 0, 0);
	}
	else {
		D3D11_BUFFER_DESC BufferDesc = {
			.ByteWidth = sizeof(cbuffer),
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER
		};
		D3D11_SUBRESOURCE_DATA InitData = { &cbuffer, 0, 0 };
		EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, &InitData, &m_PSConvColorData.pConstants));
	}
}

// ---------------------------------------------------------------------------
// Experimental tone mapping — ported from libplacebo src/tone_mapping.c
//
// libplacebo builds a 1D tone-curve LUT on the CPU and the shader SAMPLES it,
// instead of evaluating a curve per pixel. That is both cheaper and better:
// complex curves (splines, ST.2094) become practical, and each curve carries a
// tunable parameter rather than being one fixed shape.
//
// Only what this port needs is here, and it lives entirely apart from the
// shipped tone mapping — selecting none of these leaves that path bit-identical.
// Where this and upstream disagree, UPSTREAM IS RIGHT: it is the reference.
//
// The scene statistics it needs are already computed per frame for the Zion
// grade (ml_features.cpp, itself a port of libplacebo's extract_features_cpu):
// features[0] = maxscl, features[1] = average. No new readback, no second
// statistics path to keep in step.
// ---------------------------------------------------------------------------
// Entries in the experimental tone curve. libplacebo defaults to 256 and allows
// up to 4096; 1024 keeps the 4 KB upload trivial while leaving the interpolated
// curve indistinguishable from the closed form. Must match TONE_LUT_SIZE in the
// converter shaders — the shader takes it from the constant buffer so the two
// cannot silently disagree.
static constexpr int kToneLutSize = 1024;

namespace exptm {

// PL_MIX / fclampf / pl_smoothstep, verbatim from libplacebo src/common.h. The
// smoothstep matters: pl_smoothstep returns x >= edge0 (not a clamped ramp) when
// the edges coincide, and st2094_pick_knee can hand it equal edges.
static inline float mixf(float a, float b, float t) { return t * b + (1.0f - t) * a; }
static inline float clampf(float x, float lo, float hi) {
	return x < lo ? lo : (x > hi ? hi : x);
}
static inline float smoothstepf(float e0, float e1, float x) {
	if (e0 == e1) {
		return x >= e0 ? 1.0f : 0.0f;
	}
	x = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

// ST.2084, matching pl_hdr_rescale() against PL_HDR_NITS (libplacebo
// src/colorspace.c). Used only at the EDGES of this block: the params and the
// table are PQ, because that is the domain libplacebo runs the spline in.
static float PqToLinearNits(float x) {
	const float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 4096.0f * 128.0f;
	const float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 4096.0f * 32.0f, c3 = 2392.0f / 4096.0f * 32.0f;
	x = powf(fmaxf(x, 0.0f), 1.0f / m2);
	x = fmaxf(x - c1, 0.0f) / (c2 - c3 * x);
	return powf(fmaxf(x, 0.0f), 1.0f / m1) * 10000.0f;
}
static float LinearNitsToPq(float y) {
	const float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 4096.0f * 128.0f;
	const float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 4096.0f * 32.0f, c3 = 2392.0f / 4096.0f * 32.0f;
	y = powf(fmaxf(y / 10000.0f, 0.0f), m1);
	return powf((c1 + c2 * y) / (1.0f + c3 * y), m2);
}

// PL_HDR_PQ code values — NOT nits. This is load-bearing: the spline's
// `scaling` is PL_HDR_PQ and libplacebo's own colour-mapping shader sets
// `input_scaling`/`output_scaling` to PL_HDR_PQ too (src/shaders/colorspace.c),
// so the table's index axis, its values, and every param below live in PQ.
// Its ML feature extractor does the same (src/ml_render.c feeds the spline
// `features[0]`/`features[1]` — the same two numbers this renderer's extractor
// produces — with no rescale at all).
//
// Running the same math in nits instead looks equivalent and is not: the pivot
// and the knee are picked by interpolation along the index axis, so the axis
// domain decides where the knee actually lands.
struct Params {
	// Defaults are PQ code values for the HDR10 reference case: 0.05 .. 1000
	// nits input, 0 .. 203 nits output (PL_COLOR_SDR_WHITE).
	float input_min = 0.0000f, input_max = 0.7518f, input_avg = 0.5081f;
	float output_min = 0.0000f, output_max = 0.5806f;
	// PL_TONE_MAP_CONSTANTS defaults (libplacebo tone_mapping.h), NOT guessed.
	// slope_tuning/slope_offset are the two that shape the spline's knee slope.
	float knee_minimum = 0.1f, knee_maximum = 0.8f, knee_default = 0.4f, knee_adaptation = 0.4f;
	float slope_tuning = 1.5f, slope_offset = 0.2f;
	// pl_tone_map_spline "Contrast", 0.00 .. 1.50, default 0.50
	float spline_contrast = 0.50f;
};

// fix_constants() + the params_infer clamp, from pl_tone_map_params_infer().
// The clamp is not cosmetic: without it a dark scene (20-nit peak) builds a
// curve whose entire input range sits below the display's own white, so the
// spline's pivot lands outside the data and the result is a near-linear ramp.
static void InferParams(Params& p) {
	const float eps = 1e-6f;
	p.knee_adaptation = clampf(p.knee_adaptation, 0.0f, 1.0f);
	p.knee_minimum    = clampf(p.knee_minimum, eps, 0.5f - eps);
	p.knee_maximum    = clampf(p.knee_maximum, 0.5f + eps, 1.0f - eps);
	p.knee_default    = clampf(p.knee_default, p.knee_minimum, p.knee_maximum);
	p.slope_tuning    = clampf(p.slope_tuning, 0.0f, 10.0f);
	p.slope_offset    = clampf(p.slope_offset, 0.0f, 1.0f);
	p.spline_contrast = clampf(p.spline_contrast, 0.0f, 1.5f);

	// Constrain the input peak to be no less than min(output_max, 100 nits).
	// input_scaling == output_scaling here, so libplacebo's rescale of output_max
	// between them is the identity and only the 100-nit floor does any work.
	const float sdr = fminf(p.output_max, LinearNitsToPq(100.0f));
	p.input_max = fmaxf(p.input_max, sdr);
}

// libplacebo st2094_pick_knee(): the pivot is chosen from SOURCE SCENE
// BRIGHTNESS, which is exactly features[1] from the ML extractor.
static void St2094PickKnee(float* out_src, float* out_dst, const Params& p) {
	const float src_min = p.input_min;
	const float src_max = p.input_max;
	const float src_avg = p.input_avg;
	const float dst_min = p.output_min;
	const float dst_max = p.output_max;

	const float src_knee_min = mixf(src_min, src_max, p.knee_minimum);
	const float src_knee_max = mixf(src_min, src_max, p.knee_maximum);
	const float dst_knee_min = mixf(dst_min, dst_max, p.knee_minimum);
	const float dst_knee_max = mixf(dst_min, dst_max, p.knee_maximum);

	// PL_DEF: fall back to the default knee when there is no average.
	float src_knee = src_avg > 0.0f ? src_avg : mixf(src_min, src_max, p.knee_default);
	src_knee = clampf(src_knee, src_knee_min, src_knee_max);

	const float target = (src_knee - src_min) / fmaxf(src_max - src_min, 1e-6f);
	const float adapted = mixf(dst_min, dst_max, target);

	const float tuning = 1.0f - smoothstepf(p.knee_maximum, p.knee_default, target) *
	                             smoothstepf(p.knee_minimum, p.knee_default, target);
	const float adaptation = mixf(p.knee_adaptation, 1.0f, tuning);
	float dst_knee = mixf(src_knee, adapted, adaptation);
	dst_knee = clampf(dst_knee, dst_knee_min, dst_knee_max);

	*out_src = src_knee;
	*out_dst = dst_knee;
}

// libplacebo spline(): the function's `map`, operating IN PLACE on a table of PQ
// values. Single-pivot polynomial — quadratic below the pivot, cubic above,
// solved so the slope matches through the knee. Do not "simplify" the PQ away:
// it is the domain libplacebo solves in, and the curve shape depends on it.
static void SplineMap(float* lut, int n, const Params& p) {
	float src_pivot = 0.0f, dst_pivot = 0.0f;
	St2094PickKnee(&src_pivot, &dst_pivot, p);

	// Solve for the linear knee (Pa = 0)
	float slope = (dst_pivot - p.output_min) / fmaxf(src_pivot - p.input_min, 1e-6f);

	// Tune the slope at the knee point slightly: raise it to a user-provided
	// gamma exponent, multiplied by an extra tuning coefficient designed to make
	// the slope closer to 1.0 when the difference in peaks is low, and closer to
	// linear when the difference between peaks is high.
	// libplacebo computes this peak RATIO in LINEAR NITS (input_max/output_max of the
	// actual luminance range). Here input_max/output_max are PQ CODES (the index axis),
	// and PQ is perceptual: 1000/85 nits is a ratio of ~11.8 but their PQ codes ratio to
	// ~0.24. Feeding the PQ ratio collapsed slope_tuning's exponent to ~0.08, so
	// slope = pow(linear_slope, ~0.08) ≈ 1 — a near-1:1 PQ map that LIFTS the shadows
	// (the high black floor), and left both `contrast` and the pivot dial with almost no
	// leverage (their effect rides on that exponent). Convert to nits for the ratio only;
	// the index axis stays PQ (that is what the knee interpolation needs).
	const float ratio_raw = PqToLinearNits(p.input_max)
	                      / fmaxf(PqToLinearNits(p.output_max), 1e-6f) - 1.0f;
	const float ratio = clampf(p.slope_tuning * ratio_raw, p.slope_offset, 1.0f + p.slope_offset);
	slope = powf(fmaxf(slope, 1e-6f), (1.0f - p.spline_contrast) * ratio);

	// Normalise everything to the pivot to make the math easier
	const float in_min  = p.input_min - src_pivot;
	const float in_max  = p.input_max - src_pivot;
	const float out_min = p.output_min - dst_pivot;
	const float out_max = p.output_max - dst_pivot;

	// Solve P of order 2 for: P(in_min) = out_min, P'(0) = slope, P(0) = 0
	const float Pa = (out_min - slope * in_min) / fmaxf(in_min * in_min, 1e-9f);
	const float Pb = slope;

	// Solve Q of order 3 for: Q(in_max) = out_max, Q''(in_max) = 0, Q(0) = 0, Q'(0) = slope
	const float t = 2.0f * in_max * in_max;
	const float Qa = (slope * in_max - out_max) / fmaxf(in_max * t, 1e-9f);
	const float Qb = -3.0f * (slope * in_max - out_max) / fmaxf(t, 1e-9f);
	const float Qc = slope;

	for (int i = 0; i < n; ++i) {
		float x = lut[i] - src_pivot;
		x = x > 0.0f ? ((Qa * x + Qb) * x + Qc) * x : (Pa * x + Pb) * x;
		lut[i] = x + dst_pivot;
	}
}

// pl_tone_map_generate() for the spline. The index axis is uniform in PQ over
// [input_min, input_max] and each entry is the output in PQ — exactly what
// libplacebo builds when input_scaling/output_scaling are PL_HDR_PQ, which is
// how both its colour-mapping shader and its ML extractor call it.
//
// `out` is filled with the output in DISPLAY-RELATIVE linear light: 0.0 is the
// display's measured black, 1.0 its measured white. That is the domain the
// shader's plain `pow(v, 1/2.2)` encodes, and it has to be this rather than
// absolute light for a panel that has a black offset.
//
// A projector's response is L = Lb + (Lw - Lb)*v^gamma — the offset is IN the
// panel, which is what "black output offset 100%" in a calibration report means.
// Handing the shader absolute light (nits/Lw) makes the encode add that offset a
// second time: source black came out at code 15 instead of code 0, i.e. exactly
// 2*Lb, which is visible as blacks sitting at grey. Normalising by the display's
// own RANGE instead of its white is what removes the double count.
//
// The PQ decode back to linear happens here, once per entry, rather than per
// pixel per channel.
static void SplineLut(float* out, int n, Params p) {
	InferParams(p);

	for (int i = 0; i < n; ++i) {
		out[i] = mixf(p.input_min, p.input_max, (float)i / (float)(n - 1));
	}

	SplineMap(out, n, p);

	// Sanitize outputs, adapt back out of the function's scaling, and convert to
	// display-relative. The subtraction uses the SAME clamp bounds the sanitize
	// does, so entry 0 lands on exactly 0.0 and the last entry on exactly 1.0 —
	// the two codes that must be right for black and white to land where the
	// calibration measured them.
	const float outMinNits = PqToLinearNits(p.output_min);
	const float outMaxNits = PqToLinearNits(p.output_max);
	const float range = fmaxf(outMaxNits - outMinNits, 1e-6f);
	for (int i = 0; i < n; ++i) {
		out[i] = (PqToLinearNits(clampf(out[i], p.output_min, p.output_max)) - outMinNits) / range;
	}
}

} // namespace exptm

bool CDX11VideoProcessor::IsPqToSdrSelected() const
{
	return (m_srcExFmt.VideoTransferFunction == MFVideoTransFunc_2084)
		&& !(m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping))
		&& m_bConvertToSdr;
}

bool CDX11VideoProcessor::MLFeatureDomainIsPq() const
{
	// The extractor reads the PRE-correction texture (m_TexConvertOutput), so the
	// domain follows the SOURCE. m_bConvertToSdr has no business here, and having
	// it here is what produced the double remap documented in the header.
	return (m_currentSwapChainColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
		|| (m_srcExFmt.VideoTransferFunction == MFVideoTransFunc_2084);
}

bool CDX11VideoProcessor::MLInputIsPq() const
{
	// The ML passes read the POST-correction texture (pInputTexture), so PQ
	// survives only if the correction left it alone. On the DeckLink path with
	// Convert to SDR OFF that correction is "Fix BT.2020", which passes PQ
	// through while the swap chain stays SDR — so the old swap-chain test said
	// "gamma 2.2" and ps_ml_gamma decoded PQ with pow(x, 2.2).
	return (m_currentSwapChainColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
		|| ((m_srcExFmt.VideoTransferFunction == MFVideoTransFunc_2084) && !IsPqToSdrSelected());
}

void CDX11VideoProcessor::UpdateCorrectionShader()
{
	m_pCorrectionConstants.Release();
	if (!m_D3D11VP.IsReady()) {
		// Nothing to correct through: the Tex VP path builds its converter instead
		// (see UpdateConvertColorShader), and the correction pass would be skipped.
		m_pPSCorrection.Release();
		m_strCorrection = nullptr;
		return;
	}

	UINT resId = 0;
	const bool bTransFunc22 = m_srcExFmt.VideoTransferFunction == DXVA2_VideoTransFunc_22
							|| m_srcExFmt.VideoTransferFunction == DXVA2_VideoTransFunc_709
							|| m_srcExFmt.VideoTransferFunction == DXVA2_VideoTransFunc_240M;

	if (m_srcExFmt.VideoTransferFunction == MFVideoTransFunc_2084 && !(m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping)) && m_bConvertToSdr) {
		resId = m_D3D11VP.IsPqSupported() ? IDF_PS_11_CONVERT_PQ_TO_SDR : IDF_PS_11_FIXCONVERT_PQ_TO_SDR;
		m_strCorrection = L"PQ to SDR";
	}
	else if (m_srcExFmt.VideoTransferFunction == MFVideoTransFunc_HLG) {
		if (m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping)) {
			resId = IDF_PS_11_CONVERT_HLG_TO_PQ;
			m_strCorrection = L"HLG to PQ";
		}
		else if (m_bConvertToSdr) {
			resId = IDF_PS_11_FIXCONVERT_HLG_TO_SDR;
			m_strCorrection = L"HLG to SDR";
		}
		else if (m_srcExFmt.VideoPrimaries == MFVideoPrimaries_BT2020) {
			// HLG compatible with SDR
			resId = IDF_PS_11_FIX_BT2020;
			m_strCorrection = L"Fix BT.2020";
		}
	}
	else if ((bTransFunc22 || !m_bHdrPassthroughSupport) && m_srcExFmt.VideoPrimaries == MFVideoPrimaries_BT2020) {
		resId = IDF_PS_11_FIX_BT2020;
		m_strCorrection = L"Fix BT.2020";
	}

	// A change of selection must drop the old shader, or turning Convert to SDR off
	// would leave the PQ->SDR converter bound and the control would appear dead.
	m_pPSCorrection.Release();
	if (resId) {
		EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSCorrection, resId));
		DLogIf(m_pPSCorrection, L"CDX11VideoProcessor::UpdateCorrectionShader() m_pPSCorrection('{}') created", m_strCorrection);
		SetShaderLuminanceParams();
	} else {
		m_strCorrection = nullptr;
	}
}

void CDX11VideoProcessor::SetShaderLuminanceParams()
{
	// [0] LuminanceScale — PQ decode scale, 10000/sdrNits, so 1.0 == sdrNits nits
	// [1] displayMinNits — the display's measured black. Was an unused zero; the
	//     PQ->SDR converter now builds its curve over [floor, 1] and puts the
	//     floor back on the output, so the shadow end lands in the range the
	//     panel can actually show instead of inside a band it renders as one black.
	// [2] outputGamut — 0 converts BT.2020->BT.709, 1 keeps BT.2020 so a wider-than-
	//     Rec.709 display can show the source's real colour. Read only by the
	//     PQ/HLG->SDR converters; the other correction shaders ignore the slot.
	// [3] toneLutEnable — 0 = the shipped tone mapping, 1 = sample the experimental
	//     1D curve. Read only by the PQ/HLG->SDR converters.
	// [4] toneLutPqMin — the curve's index axis, in PQ code values
	// [5] toneLutPqMax
	// [6] toneLutSize — entries in m_pToneLut. The shader samples texel CENTRES,
	//     so it needs N to map [0,1] onto them; passing it keeps the two sides
	//     from drifting if the texture size is ever changed.
	// [7] unused
	//
	// Slots 3..6 are new. The other correction shaders declare only three floats,
	// so their reads are unaffected by the larger buffer.
	//
	// [8] toneCurveSelection — the "Tone curve" choice (iHdrLocalToneMappingType,
	//     1..6). This is the value the panel has always set; the converter used to
	//     ignore it and call ToneMappingHable unconditionally, so the row changed
	//     nothing on an SDR display. 0 and 3 both mean the legacy Hable.
	// [9..12] HDR10 static metadata for the curves that derive a knee from it.
	//     Same fallbacks SetHDR10ShaderParams applies, so the HDR-output path and
	//     the SDR path see the same numbers for the same content. All zero on the
	//     DeckLink capture path — the card's ST.2086 frame metadata is unsupported
	//     on HDMI — so in practice the fallback peak (1000 nits) is what is used.
	// [13] per-scene Hable peak   [14] Hable knee   [15] spare
	// [16..18] mobius coefficients for the curve that takes over above the knee
	// [19] padding — cbuffers must stay 16-byte aligned
	// [19] the Hable curve's 1.0 in nits (decoupled reference white).
		// [20] the nonlinear chroma compensation exponent (hableChromaExp).
		// [21..23] the GT7 operator's blend + chroma-fade window (selection 8).
		// [24]     the GT7 Adapt scene-peak weight (selection 10).
		// cbuffer ByteWidth MUST stay a multiple of 16 bytes (84 vs 96 is the
		// difference between a valid and a FAILED CreateBuffer — observed as a black
		// picture whenever the buffer grew to 21 floats). The array has grown 24 -> 28
		// -> 32 floats (96 -> 112 -> 128 bytes), each time landing exactly on a 16-byte
		// multiple as the old comment prescribed — never on 25/26/27 or 29/30/31.
		FLOAT cbuffer[36] = { 10000.0f / m_iSDRDisplayNits, m_fHdrDisplayMinNits,
	                     (float)m_iOutputGamut,
	                     ToneLutActive() ? 1.0f : 0.0f,
	                     m_ToneLutKey.pqMin, m_ToneLutKey.pqMax,
	                     (float)kToneLutSize, 0,
	                     // 7 = REFERENCE, 8 = GT7, 9 = SPLINE, 10 = GT7 ADAPT. Upper bound
	                     // must cover ALL of them or discard() quietly rewrites the
	                     // selection to 3 (Hable), i.e. the row would do nothing.
	                     (float)discard<int>(m_iHdrLocalToneMappingType, TONE_CURVE_HABLE, 0, TONE_CURVE_GT7_ADAPT),
	                     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

	// The metadata, guarded exactly as SetHDR10ShaderParams guards it.
	{
		// The content peak comes from real metadata when there is any, and from the
		// user's Title peak otherwise. On the DeckLink capture path there never is:
		// the card's ST.2086 frame metadata is unsupported on HDMI and the values
		// arrive as media SIDE data, which a captured frame does not carry — so
		// m_lastHdr10 stays zeroed and the control is what the curves actually get.
		//
		// FILE content has the opposite problem, measured 2026-09-26: on an SDR
		// display m_lastHdr10 is NEVER written (it only lands when an HDR10 swapchain
		// is live — see the side-data snapshot comment ~line 5025), yet the sample's
		// metadata DOES arrive and is snapshotted into the display-independent
		// m_hdr10Cll/Fall/Mmax/Mmin (the very numbers the pipeline echo prints as
		// rcll=991 etc). The code below used to fall straight to
		// m_lastHdr10(=0) -> titlePeak(=100), so GT7's adapted gain divided by 100
		// instead of 991 and "every source pixel above ~101 nits hit the clamp" —
		// the blown picture persisted no matter what the dials said.
		const float titlePeak = std::clamp(m_fHdrTitlePeakNits, 100.0f, 4000.0f);
		// Display-independent snapshots first; m_lastHdr10 next (an HDR10 swapchain
		// that is actually live); titlePeak last. m_hdr10Cll etc are only written
		// when the sample carries the field, so zero means "absent" unambiguously.
		const UINT cll_  = m_hdr10Cll  ? m_hdr10Cll  : m_lastHdr10.hdr10.MaxContentLightLevel;
		const UINT fall_ = m_hdr10Fall ? m_hdr10Fall : m_lastHdr10.hdr10.MaxFrameAverageLightLevel;
		const UINT mmax_ = m_hdr10Mmax ? m_hdr10Mmax : m_lastHdr10.hdr10.MaxMasteringLuminance;
		const UINT mmin_ = m_hdr10Mmin ? m_hdr10Mmin : m_lastHdr10.hdr10.MinMasteringLuminance;
		float mMin = (float)mmin_;
		float mMax = (float)mmax_;
		float cll  = (float)cll_;
		float fall = (float)fall_;
		if (mMin <= 0.0f)  mMin = 0.0f;
		if (mMax <= 10.0f) mMax = titlePeak;
		if (cll <= 10.0f)  cll = mMax;
		if (fall <= 1.0f)  fall = cll;
		cbuffer[9]  = cll;
		// The same number the curve divides by, published for the CPU-side
		// prediction that drives the relative mid-tone target.
		m_fCurvePeakNits = cll;
		cbuffer[10] = fall;
		cbuffer[11] = mMax;
		cbuffer[12] = mMin;

		// cbuffer[13]: the IIR-smoothed per-scene source peak the Hable branch
		// normalises by. Negative until the extractor has a reading, in which case
		// the shader falls back to maxCLL.
		//
		// Deliberately NOT folded into cbuffer[9] (maxCLL): that is what BT.2390
		// and ST.2094-10 derive their knee from, and this change needs to be
		// attributable to Hable alone. Promoting it is a separate decision.
		cbuffer[13] = m_HableScenePeakNits;

		// cbuffer[14]: the Hable knee, in display-relative light (1.0 = the
		// display's white). cbuffer[16..18]: the mobius coefficients for the curve
		// that takes over ABOVE it.
		//
		// With the knee OFF (0) Hable applies unchanged — exactly the behaviour it
		// has always had, and the reason the curve is worth keeping: it protects
		// highlights. With the knee ON, the curve above it is a SOLVED rational:
		//
		//   M(x) = s*(x + a)/(x + b),  with M(j) = j, M(peak) = 1, M'(j) = 1
		//
		// so the joint at the knee is smooth BY CONSTRUCTION rather than by a blend
		// band. That matters because a blend band shows its mismatch, and it would
		// show it most when the knee MOVES — which is the whole point of a knee.
		//
		// Hable itself cannot be joined this way. It is concave everywhere, so
		// identity (slope 1) meets it only at the origin and no positive knee can be
		// both value- and slope-continuous. A rational with a free (a, b) can, which
		// is why libplacebo solves mobius this way and why its spline family is
		// joinable while Hable is not. (Solve from libplacebo tone_mapping.c.)
		//
		// Three divides, run when the knee or the measured peak moves — never per
		// pixel.
		// cbuffer[14]: the Hable reserved knee slot (kept, unused since the mobius
		// knee was removed). cbuffer[15..17]: the soft-plus pre-compressor.
		const float kJ = std::clamp(m_fHdrHableKnee, 0.0f, 0.90f);
		cbuffer[14] = kJ;
		cbuffer[15] = m_fSoftKnee;           // <= 0 disables
		cbuffer[16] = m_fSoftKneeRatio;      // 1.0 = identity above the knee
		cbuffer[17] = m_fSoftKneeWidth;      // softplus transition width
		cbuffer[18] = m_fHdrHighlightNeutral;   // highlight neutral roll, 0..1 (0 = off)
		// cbuffer[19]: the curve's 1.0 in nits (decoupled reference white). With auto
		// exposure on and a GT7-family curve selected, this is the SOLVED value, not
		// the dial: the dial becomes the override. Hable keeps the dial, because its
		// normalisation is different and inheriting a per-scene exposure would make it
		// breathe.
		m_fEffRefWhite = (m_bAutoRefWhite && m_fRefWhiteAuto > 0.0f &&
		                  (m_iHdrLocalToneMappingType == TONE_CURVE_GT7 ||
		                   m_iHdrLocalToneMappingType == TONE_CURVE_GT7_ADAPT))
		               ? m_fRefWhiteAuto : m_fHdrRefWhiteNits;
		cbuffer[19] = m_fEffRefWhite;
		// [20] the nonlinear chroma compensation exponent (1.0 = none).
		cbuffer[20] = m_fHdrChromaExp;

		// [21..23] the GT7 operator's two knobs (selection 8). These were the three
		// pad slots, so the buffer is STILL 24 floats / 96 bytes — no growth, and no
		// repeat of the 21-float (84-byte) CreateBuffer failure that produced a black
		// picture with nothing on screen to explain it. The NEXT field added here has
		// to take the array to 28, never 25/26/27 — which is what happened here.
		cbuffer[21] = m_fHdrGt7Blend;
		cbuffer[22] = m_fHdrGt7FadeStart;
		cbuffer[23] = m_fHdrGt7FadeEnd;
		// [24] GT7 Adapt (selection 10): weight of the measured scene peak in the
		// peak the curve maps FROM. Harmless for every other selection — only the
		// GT7 branch reads it, and only when the selection is 10.
		cbuffer[24] = m_fHdrGt7Adapt;
		// [25] the shoulder's reach (GT7_ALPHA). Takes a padding slot, so the buffer
		// stays 28 floats / 112 bytes — no alignment risk.
		cbuffer[25] = m_fHdrGt7Alpha;
		cbuffer[26] = m_fHdrGt7Desat;   // luminance-linked chroma compression
		cbuffer[27] = m_fGamutCap;      // 709 capacity target for the gamut mapping
		// [28] who applies the mapping. The chroma pass is the last stage that can
		// change chroma, so when it runs the constraint belongs there; when the grade
		// is down, nothing downstream touches chroma and this shader is the right
		// place. Applying it in both would double-compress. 28..31 keeps the buffer at
		// 32 floats / 128 bytes, the next 16-byte multiple.
		cbuffer[28] = (m_bMLActive && m_pPSMLChroma) ? 0.0f : 1.0f;
		// [32] GT7 debug view (0 = off). 32..35 keeps the buffer at 36 floats / 144
		// bytes, the next 16-byte multiple.
		cbuffer[32] = m_fHdrGt7Dbg;
	}

	if (m_pCorrectionConstants) {
		// Live proof the ACTIVE converter's constants are being rewritten: the
		// constant buffer contents follow every settings push (not per-frame).
		// curve= and gtB= are here so "I selected GT7 and nothing changed" is
		// answerable from one line: the clamped selection that reached the buffer,
		// not the one the UI asked for.
		// ad= is GT7 Adapt's scene-peak weight and sPk= the measured peak it blends
		// toward, so the anchor the shader actually used is one line's arithmetic:
		// peak = cll*(1-ad) + sPk*ad.
		// refW is the EFFECTIVE reference white (the solved value when auto exposure
		// is on) and avgN the scene mean in nits it was solved from, so "is auto
		// exposure doing anything" is answerable from one log line.
		DLog(L"renderer: [SHADER-CB] push buf=0x{:x} LuminanceScale={:.4f} sdrNits={} hlN={:.2f} clE={:.2f} curve={} gtB={:.2f} ad={:.2f} refW={:.1f} sPk={:.1f} avgN={:.1f}",
			(uintptr_t)m_pCorrectionConstants.p, 10000.0f / m_iSDRDisplayNits, m_iSDRDisplayNits,
			m_fHdrHighlightNeutral, m_fHdrChromaExp,
			discard<int>(m_iHdrLocalToneMappingType, TONE_CURVE_HABLE, 0, TONE_CURVE_GT7_ADAPT),
			m_fHdrGt7Blend, m_fHdrGt7Adapt,
			m_fEffRefWhite, m_HableScenePeakNits, m_MeanNitsSmooth);
		m_pDeviceContext->UpdateSubresource(m_pCorrectionConstants, 0, nullptr, &cbuffer, 0, 0);
		// Arm the read-back: the next few correction draws report the buffer they
		// ACTUALLY had bound at b0 and its float values. [CBPROBE] absent after this
		// line = the correction draw is not running at all.
		InterlockedExchange(&m_iCbProbeTicks, 5);
	}
	else {
		D3D11_BUFFER_DESC BufferDesc = {
			.ByteWidth = sizeof(cbuffer),
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER
		};
		D3D11_SUBRESOURCE_DATA InitData = { &cbuffer, 0, 0 };
		EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, &InitData, &m_pCorrectionConstants));
	}
}

// Display calibration LUT. Text format per the .cube spec (Adobe/Resolve/Argyll):
//   LUT_3D_SIZE N            N^3 entries, red varying fastest
//   DOMAIN_MIN / DOMAIN_MAX  (assumed 0..1 — the only range DisplayCAL emits)
//   then N^3 "r g b" float rows
// A 1D shaper (LUT_1D_SIZE) is ignored in v1; DisplayCAL's 3D-only exports have none.
HRESULT CDX11VideoProcessor::LoadCubeLut(const char* utf8Path)
{
	// Unload first: a failed load must never leave a half-applied LUT on screen.
	m_bCubeLutEnabled = false;
	m_pCubeLutSRV.Release();
	m_pCubeLut.Release();
	m_iCubeLutSize = 0;

	if (!utf8Path || !utf8Path[0]) {
		DLog(L"CDX11VideoProcessor::LoadCubeLut() : cleared (no path)");
		// S_OK: the requested end state — no LUT — is exactly what we now have.
		// S_FALSE here would be indistinguishable from a bad file to the caller.
		return S_OK;
	}
	if (!m_pDevice) {
		return E_FAIL;
	}

	wchar_t wpath[MAX_PATH] = {};
	MultiByteToWideChar(CP_UTF8, 0, utf8Path, -1, wpath, MAX_PATH);

	FILE* f = nullptr;
	if (_wfopen_s(&f, wpath, L"rb") != 0 || !f) {
		DLog(L"CDX11VideoProcessor::LoadCubeLut() : cannot open the file");
		return E_FAIL;
	}

	int  size = 0;
	std::vector<float> lut;
	char buf[512];
	while (fgets(buf, sizeof(buf), f)) {
		std::string s(buf);
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
		size_t b = s.find_first_not_of(" \t");
		if (b == std::string::npos) continue;
		s.erase(0, b);
		if (s[0] == '#') continue;

		if (s.rfind("LUT_3D_SIZE", 0) == 0) { size = atoi(s.c_str() + 12); continue; }
		if (s.rfind("LUT_1D_SIZE", 0) == 0 || s.rfind("DOMAIN_", 0) == 0
			|| s.rfind("TITLE", 0) == 0) {
			continue;
		}
		float r = 0, g = 0, bl = 0;
		if (sscanf_s(s.c_str(), "%f %f %f", &r, &g, &bl) == 3) {
			lut.push_back(r); lut.push_back(g); lut.push_back(bl);
		}
	}
	fclose(f);

	const size_t want = (size_t)size * size * size;
	if (size < 2 || size > 128 || lut.size() != want * 3) {
		DLog(L"CDX11VideoProcessor::LoadCubeLut() : bad LUT (size={}, floats={})", size, lut.size());
		return E_FAIL;
	}

	// R32G32B32A32 avoids a float->half pass; 65^3 is 4.4 MB, uploaded once.
	std::vector<float> rgba(want * 4);
	for (size_t i = 0; i < want; ++i) {
		rgba[i * 4 + 0] = lut[i * 3 + 0];
		rgba[i * 4 + 1] = lut[i * 3 + 1];
		rgba[i * 4 + 2] = lut[i * 3 + 2];
		rgba[i * 4 + 3] = 1.0f;
	}

	D3D11_TEXTURE3D_DESC desc = {};
	desc.Width      = (UINT)size;
	desc.Height     = (UINT)size;
	desc.Depth      = (UINT)size;
	desc.MipLevels  = 1;
	desc.Format     = DXGI_FORMAT_R32G32B32A32_FLOAT;
	desc.Usage      = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags  = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA init = {};
	init.pSysMem          = rgba.data();
	init.SysMemPitch      = (UINT)size * 4 * sizeof(float);
	init.SysMemSlicePitch = (UINT)size * (UINT)size * 4 * sizeof(float);

	HRESULT hr = m_pDevice->CreateTexture3D(&desc, &init, &m_pCubeLut);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::LoadCubeLut() : CreateTexture3D failed {:#x}", hr);
		return hr;
	}
	hr = m_pDevice->CreateShaderResourceView(m_pCubeLut, nullptr, &m_pCubeLutSRV);
	if (FAILED(hr)) {
		m_pCubeLut.Release();
		return hr;
	}

	if (!m_pCubeLutSampler) {
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;   // hardware trilinear over the cube
		sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sd.MaxLOD         = D3D11_FLOAT32_MAX;
		hr = m_pDevice->CreateSamplerState(&sd, &m_pCubeLutSampler);
		if (FAILED(hr)) {
			m_pCubeLutSRV.Release();
			m_pCubeLut.Release();
			return hr;
		}
	}

	m_iCubeLutSize    = size;
	m_bCubeLutEnabled = true;
	DLog(L"CDX11VideoProcessor::LoadCubeLut() : loaded {}^3 LUT", size);
	return S_OK;
}

// The experimental tone curve. libplacebo builds its tone LUT on the CPU once
// per parameter change and lets the shader sample it — complex curves become
// practical, and each curve carries a tunable parameter instead of being one
// fixed shape. The scene statistics that drive it (features[0] = maxscl,
// features[1] = average, both in PQ) are the ones the Zion grade already
// extracts every frame, so this adds no readback of its own.
//
// The two IIR-smoothed inputs are the curve's dynamic part: the spline picks its
// pivot from the scene average, so an unsmoothed average makes the curve flicker
// on noise. The smoothing anchor is the same scene-jump test the gamma filter
// uses (a change in maxscl beyond 0.15 PQ is a cut, not a drift) — one policy
// for "has the scene changed", not two that can disagree.
void CDX11VideoProcessor::UpdateToneLut()
{
	if (m_iExperimentalToneMapper == 0 || !m_pDevice || !m_bMLFeaturesReady) {
		return;
	}

	const float featMax = m_MLFeatures[0];
	const float featAvg = m_MLFeatures[1];

	const bool sceneJumped = m_MLToneMaxSmooth < 0.0f ||
	                         fabsf(featMax - m_MLToneMaxSmooth) > 0.15f;
	if (sceneJumped) {
		m_MLToneMaxSmooth = featMax;
		m_MLToneAvgSmooth = featAvg;
	} else {
		m_MLToneMaxSmooth = 0.10f * featMax + 0.90f * m_MLToneMaxSmooth;
		m_MLToneAvgSmooth = 0.10f * featAvg + 0.90f * m_MLToneAvgSmooth;
	}

	// The display's measured range. This IS the curve's output range — that is the
	// whole point of feeding the mapper display black and white.
	//
	// `iSDRDisplayNits` is the white in NITS. Do not compute 10000/sdrNits here:
	// that is LuminanceScale, the PQ decode scale the shader uses, and it is a
	// different quantity. Conflating them is what made this curve render 28% dark
	// at an 85-nit target (the table came out normalised to 117.6 instead of 85),
	// and it is invisible except by reading the output range back.
	const float displayNits = fmaxf((float)m_iSDRDisplayNits, 1.0f);

	ToneLutKey key;
	key.pqMin     = 0.0f;   // source nominal black: PQ code 0. DeckLink HDR10 carries
	                        // no usable min-luminance, so this is the honest default.
	// input_max is the SIGNAL's nominal peak, not the current frame's measured peak.
	//
	// This is the one place where following the measured statistics would be
	// wrong, and it is worth being explicit about why. libplacebo takes input_max
	// from the source colour space's nominal luma — the declared peak (MaxCLL, or
	// the mastering display peak) — and takes only input_avg from dynamic
	// per-scene metadata. Feeding the MEASURED peak in instead stretches every
	// scene to fill the display: a 40-nit night scene and a 1000-nit daylight
	// scene would come out the same brightness, which is auto-gain, not tone
	// mapping. It would also fight the Zion grade and the calibration LUT, both of
	// which assume the signal's own levels are preserved.
	//
	// 1000 nits is libplacebo's own fallback for HDR10 with no usable metadata
	// (see pl_color_space_nominal_luma and the SetHDR10ShaderParams guard below,
	// which substitutes 1000 for anything <= 10). The Title peak control replaces
	// it when the user knows better — and on this capture path they may be the only
	// source of the value, since the card cannot supply MaxCLL.
	key.pqMax     = exptm::LinearNitsToPq(std::clamp(m_fHdrTitlePeakNits, 100.0f, 4000.0f));
	key.pqAvg     = m_MLToneAvgSmooth;   // measured: this is the pivot's input
	key.contrast  = m_fToneLutContrast;
	key.pivotScale = std::clamp(m_fToneLutPivotScale, 0.0f, 1.0f);
	key.dispNits  = displayNits;
	key.blackNits = m_fHdrDisplayMinNits;

	// Rebuild only when something the curve is built from actually moved.
	//
	// The configuration fields are compared EXACTLY: a change to those is a user
	// action and must take effect on the next frame, not when the scene drifts.
	//
	// The rate limit has to be on TIME rather than on the size of the change: the
	// IIR steps ~2/1000 PQ per frame, so any deadband small enough to be invisible
	// is also small enough to be cleared every frame — measured at 24 rebuilds/s
	// with a 2/1000 deadband, i.e. no throttling at all. One rebuild every 6th
	// frame, plus an immediate one on a cut or a config change, keeps the curve
	// responsive for a quarter of the cost.
	//
	// Only pqAvg can move at runtime; pqMax is a configuration value now (the Title
	// peak), so it is compared with the configuration fields rather than deadbanded.
	const bool statsMoved = fabsf(key.pqAvg - m_ToneLutKey.pqAvg) > 2e-3f;
	const bool configChanged = key.pqMin != m_ToneLutKey.pqMin ||
	                           key.pqMax != m_ToneLutKey.pqMax ||
	                           key.contrast != m_ToneLutKey.contrast ||
	                           key.pivotScale != m_ToneLutKey.pivotScale ||
	                           key.dispNits != m_ToneLutKey.dispNits ||
	                           key.blackNits != m_ToneLutKey.blackNits;
	m_nToneLutFrame++;
	const bool rateOk = sceneJumped || (m_nToneLutFrame % 6 == 0);
	if (m_bToneLutReady) {
		if (!statsMoved && !configChanged) return;  // nothing to do
		if (!configChanged && !rateOk) return;      // moved, but rate-limited
	}

	exptm::Params p;
	p.input_min  = exptm::LinearNitsToPq(key.pqMin);
	p.input_max  = key.pqMax;
	// The scene average drives ONLY the spline's knee/pivot (St2094PickKnee), and a
	// bright average is what lifts the shadow band into a high black floor. Scaling
	// it here is the litmus dial: lower = pivot sits lower = deeper blacks, with the
	// highlight shoulder untouched. 1.0 = libplacebo as-is.
	p.input_avg  = key.pqAvg * key.pivotScale;
	p.output_min = exptm::LinearNitsToPq(key.blackNits);
	p.output_max = exptm::LinearNitsToPq(key.dispNits);
	p.spline_contrast = key.contrast;

	exptm::InferParams(p);
	// InferParams can lift input_max, so publish the INFERRED axis — the shader
	// indexes with these, and an axis that disagrees with the table mis-maps the
	// whole curve while still looking like a plausible picture.
	key.pqMax = p.input_max;

	std::vector<float> table(kToneLutSize);
	// Display-relative (0 = the display's black, 1 = its white), which is what the
	// shader's plain pow(v, 1/2.2) encodes — see SplineLut().
	exptm::SplineLut(table.data(), kToneLutSize, p);

	if (!m_pToneLut) {
		// R32_FLOAT, 1D as a 1024x1 2D texture: D3D11 has no 1D textures, and a
		// 2D one with height 1 is what every other 1D LUT here does too.
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width      = (UINT)kToneLutSize;
		desc.Height     = 1;
		desc.MipLevels  = 1;
		desc.ArraySize  = 1;
		desc.Format     = DXGI_FORMAT_R32_FLOAT;
		desc.SampleDesc = { 1, 0 };
		desc.Usage      = D3D11_USAGE_DEFAULT;   // updated per scene
		desc.BindFlags  = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SUBRESOURCE_DATA init = {};
		init.pSysMem     = table.data();
		init.SysMemPitch = (UINT)kToneLutSize * sizeof(float);

		HRESULT hr = m_pDevice->CreateTexture2D(&desc, &init, &m_pToneLut);
		if (FAILED(hr)) {
			DLog(L"CDX11VideoProcessor::UpdateToneLut() : CreateTexture2D failed {:#x}", hr);
			m_bToneLutReady = false;
			return;
		}
		hr = m_pDevice->CreateShaderResourceView(m_pToneLut, nullptr, &m_pToneLutSRV);
		if (FAILED(hr)) {
			m_pToneLut.Release();
			m_bToneLutReady = false;
			return;
		}
		if (!m_pToneLutSampler) {
			D3D11_SAMPLER_DESC sd = {};
			sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;   // linear between entries
			sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;       // above the peak -> display white
			sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
			sd.MaxLOD         = D3D11_FLOAT32_MAX;
			if (FAILED(m_pDevice->CreateSamplerState(&sd, &m_pToneLutSampler))) {
				m_pToneLutSRV.Release();
				m_pToneLut.Release();
				m_bToneLutReady = false;
				return;
			}
		}
	} else {
		m_pDeviceContext->UpdateSubresource(m_pToneLut, 0, nullptr, table.data(),
		                                           (UINT)kToneLutSize * sizeof(float), 0);
	}

	m_bToneLutReady = true;
	m_ToneLutKey = key;
	m_nToneLutBuilds++;
	// The curve's axis lives in the correction cbuffer, which is only re-uploaded
	// by SetShaderLuminanceParams — push it so a scene change takes effect on this
	// frame rather than at the next unrelated settings change.
	SetShaderLuminanceParams();

	DLog(L"CDX11VideoProcessor::UpdateToneLut() : mapper {} rebuilt, PQ {:.4f}..{:.4f} avg {:.4f} contrast {:.2f}",
	     m_iExperimentalToneMapper, key.pqMin, key.pqMax, key.pqAvg, key.contrast);
}

void CDX11VideoProcessor::SetHDR10ShaderParams(float masteringMinLuminanceNits, float masteringMaxLuminanceNits,
											   float maxCLL, float maxFALL,
											   float displayMaxNits, int toneMappingType)
{
	if (masteringMinLuminanceNits <= 0.f) masteringMinLuminanceNits = 0.f;
	if (masteringMaxLuminanceNits <= 10.f) masteringMaxLuminanceNits = 1000.f;
	if (maxCLL <= 10.f) maxCLL = masteringMaxLuminanceNits;
	if (maxFALL <= 1.f) maxFALL = maxCLL;
	if (displayMaxNits < 100.f || displayMaxNits > 10000.f) displayMaxNits = 1000.f;
	if (toneMappingType < 1 || toneMappingType > 6) toneMappingType = 1;

	const HDRParamsConstantBuffer_t cbuffer = {
		masteringMinLuminanceNits, masteringMaxLuminanceNits,
		maxCLL, maxFALL,
		displayMaxNits, static_cast<UINT>(toneMappingType),
		m_fHdrDisplayMinNits
	};

	if (!m_pPSHDR10ToneMapping) {
		EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSHDR10ToneMapping, IDF_PS_11_HDR10_TONEMAP));
		DLogIf(m_pPSHDR10ToneMapping, L"CDX11VideoProcessor::SetHDR10ShaderParams() : m_pPSHDR10ToneMapping({}) created", m_iHdrLocalToneMappingType);

		UpdatePostScaleTexures();
	}

	if (m_pHDR10ToneMappingConstants) {
		if (memcmp(&m_lastHDRParamsConstantBuffer, &cbuffer, sizeof(cbuffer)) == 0) {
			return;
		}

		m_pDeviceContext->UpdateSubresource(m_pHDR10ToneMappingConstants, 0, nullptr, &cbuffer, 0, 0);
	} else {
		D3D11_BUFFER_DESC BufferDesc = {
			.ByteWidth = sizeof(cbuffer),
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
		};
		D3D11_SUBRESOURCE_DATA InitData = { &cbuffer, 0, 0 };
		HRESULT result = m_pDevice->CreateBuffer(&BufferDesc, &InitData, &m_pHDR10ToneMappingConstants);
		if (FAILED(result)) {
			DLog(L"SetHDR10ShaderLuminanceParams() failed to create m_pHDR10ToneMappingConstants. Error: {}", result);
		}
		EXECUTE_ASSERT(S_OK == result);
	}

	m_lastHDRParamsConstantBuffer = cbuffer;
}

void CDX11VideoProcessor::SetDolbyVisionDynamicParams()
{
	const DoViDynamicConstantsBuffer_t cbuffer = {
		m_DoviExtensionMetadata.L2.trim_chroma_weight - 0.5f, m_DoviExtensionMetadata.L2.trim_saturation_gain - 0.5f,
		m_DoviExtensionMetadata.L2.trim_slope + 0.5f, m_DoviExtensionMetadata.L2.trim_offset - 0.5f, m_DoviExtensionMetadata.L2.trim_power + 0.5f,
		static_cast<UINT>(m_DoviExtensionMetadata.L2.present)
	};

	if (m_pDoViDynamicConstants) {
		if (memcmp(&m_lastDoViDynamicConstantsBuffer, &cbuffer, sizeof(cbuffer)) == 0) {
			return;
		}

		m_pDeviceContext->UpdateSubresource(m_pDoViDynamicConstants, 0, nullptr, &cbuffer, 0, 0);
	} else {
		D3D11_BUFFER_DESC BufferDesc = {
			.ByteWidth = sizeof(cbuffer),
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
		};
		D3D11_SUBRESOURCE_DATA InitData = { &cbuffer, 0, 0 };
		HRESULT result = m_pDevice->CreateBuffer(&BufferDesc, &InitData, &m_pDoViDynamicConstants);
		if (FAILED(result))
		{
			DLog(L"SetDolbyVisionDynamicParams() failed to create m_pDoViDynamicConstants. Error: {}", result);
		}
		EXECUTE_ASSERT(S_OK == result);
	}

	m_lastDoViDynamicConstantsBuffer = cbuffer;

#ifndef NDEBUG
	UpdateStatsStatic();
#endif
}

void CDX11VideoProcessor::InitMLResources()
{
	if (m_bMLResourcesReady || !m_pDevice) return;

	DLog(L"CDX11VideoProcessor::InitMLResources() creating shaders...");
	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSMLGamma, IDF_PS_11_ML_GAMMA));
	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSMLShadow, IDF_PS_11_ML_SHADOW));
	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSMLHighlight, IDF_PS_11_ML_HIGHLIGHT));
	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSMLDownsample, IDF_PS_11_ML_DOWNSAMPLE));
	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSMLBars, IDF_PS_11_ML_BARS));
	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSFaceDown, IDF_PS_11_ML_FACE_DOWN));
	{
		struct { float IsPQ; float pad[3]; } faceDownInit = {};
		D3D11_BUFFER_DESC bd = { sizeof(faceDownInit), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };
		D3D11_SUBRESOURCE_DATA sd = { &faceDownInit };
		m_pDevice->CreateBuffer(&bd, &sd, &m_pFaceDownParams);
	}
	DLog(L"CDX11VideoProcessor::InitMLResources() shaders OK");

	if (SUCCEEDED(LoadSkinLUT())) {
		EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSMLChroma, IDF_PS_11_ML_CHROMA));
	} else {
		DLog(L"CDX11VideoProcessor::InitMLResources() skin LUT missing; chroma tuning disabled");
	}

	DLog(L"CDX11VideoProcessor::InitMLResources() creating textures...");
	CreateMLTextures();
	if (FAILED(CreateSkyMaskTexture())) {
		DLog(L"CDX11VideoProcessor::InitMLResources() sky mask texture failed; overlay disabled");
	}
	CreateSkinGateTexture();   // Poisson skin gate (t4); created fresh even on device reset
	if (FAILED(CreateSkySegTextures())) {
		DLog(L"CDX11VideoProcessor::InitMLResources() sky seg preview textures failed; seg disabled");
	}

	if (!m_pMLParams) {
		D3D11_BUFFER_DESC BufferDesc = {
			.ByteWidth = sizeof(MLParams_t),
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
		};
		EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, nullptr, &m_pMLParams));
	}

	m_bMLResourcesReady = true;
	DLog(L"CDX11VideoProcessor::InitMLResources() all ML GPU resources ready");
}

void CDX11VideoProcessor::UpdateMLActive()
{
	// Models and GPU resources are loaded at construction / SetDevice time.
	// This method only toggles the active flag based on source and settings.
	InitMLDebug();

	const bool bAutoReady = (m_iMLMode == ML_Auto) && m_bMLGammaModelLoaded;
	// ML grade applies to both HDR and SDR sources: the grade shaders already
	// select IsPQ=1/0 from the swapchain (never from SourceIsHDR), so native SDR
	// (Blu-ray Rec.709/BT.1886) takes the gamma-2.2 SDR path identically to an
	// HDR file after tone-mapping. Gate on enable + mode only.
	const bool bActive = m_bML && (m_iMLMode == ML_Manual || bAutoReady);

	// Only the ACTIVE flag toggles; the preloaded resources persist.
	if (bActive == m_bMLActive) {
		return;
	}
	m_bMLActive = bActive;

	// Transition: reset per-activation state, not resources.
	m_lastMLParams = {};
	m_bMLFeaturesReady = false;
	m_bMLFeaturesStale = false;
	m_bMLConvertSeen = false;
	m_MLIIRGamma = 1.0f;
	m_MLPrevL1Max = -1.0f;
	m_MLRawTexture = 0.0f;
	m_MLTextureSmooth = 0.0f;
	m_MLBarDetected = {};
	m_MLCropTop = 0;
	m_MLCropBot = ML_DOWNSAMPLE_H - 1;
	m_MLCropAge = 0;
	m_fMLBarTop = 0.0f;
	m_fMLBarBot = 1.0f;
	DLogIf(m_bMLActive, L"CDX11VideoProcessor::UpdateMLActive() ML grade active (mode {})", m_iMLMode);
	UpdatePostScaleTexures();
}

// CPU-side equivalents of the HLSL intrinsics the mirrored chain uses. Named to match
// the shader so the two implementations read side by side and stay in step.
static inline float satf(float v) { return fminf(1.0f, fmaxf(0.0f, v)); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float smoothstepf2(float e0, float e1, float x)
{
	const float t = satf((x - e0) / fmaxf(e1 - e0, 1e-6f));
	return t * t * (3.0f - 2.0f * t);
}

// The grade chain in I_norm space, mirrored from ps_ml_gamma.hlsl applyToneCurve with
// sky = 0 (the global path) and the legacy multiplicative toe removed (the controller
// owns the shadow band through the additive pedestal instead, because a gain cannot
// lift a pixel that is already at zero).
//   w  = linear fraction of white delivered by the tone curve
//   x  = PQ(I) / 0.5893       (I_norm: the grade's ACTUAL domain — a normalised PQ
//                              code, NOT a linear fraction. ps_ml_gamma divides
//                              ictcp.x by SDR_I_PEAK_PQ = 0.5893, the PQ code of 203
//                              nits. Mirroring this in linear space made the solver
//                              invert a curve the shader does not implement, and it
//                              DARKENED a dark scene instead of lifting it.)
//   0. toe slope: x  = knee * (x/knee)^S          (S<1 expands shadows, 0 and knee fixed)
//   1. gamma:     x  = lerp(x, x^(1/G), fade(x) * (1 - hi_g(x)))
//   2. rolloff:   x += R * t^2 * (1-x), t = (x - kneeHi) / (1 - kneeHi)
// Returns the delivered linear fraction of white for one source anchor.
//
// The shadow actuator is the SLOPE, not a pedestal: an additive offset lifts black to
// grey and veils the picture (measured by eye as "a grey layer on top" of a dark tent
// interior), because it adds the same amount to a pixel at 0 and to one at 0.01 and so
// cannot reveal detail that sits near the floor. A power below the knee keeps 0 at 0
// and the knee fixed while expanding everything between in log — the code span dark
// detail occupies moves up out of the invisible floor with its contrast intact.
static float GradeChainDelivered(float w, float S, float G, float R,
                                 float kneeShadow, float guard, float kneeHi, float sdrNits)
{
	const float kIPeak = 0.5893f;                 // SDR_I_PEAK_PQ, ictcp.hlsl
	float x = exptm::LinearNitsToPq(w * sdrNits) / kIPeak;

	if (S < 0.999f && x > 0.0f && x < kneeShadow) {
		x = kneeShadow * powf(x / kneeShadow, S);
	}

	if (G != 1.0f && x > 0.001f && x < 0.999f) {
		const float fade = smoothstepf2(0.08f, 0.45f, x);
		const float hi_g = smoothstepf2(guard, 0.98f, x);
		x = lerpf(x, powf(x, 1.0f / G), fade * (1.0f - hi_g));
	}

	if (fabsf(R) > 0.005f && x > kneeHi && x < 0.995f) {
		const float span = fmaxf(1.0f - kneeHi, 0.01f);
		const float t = (x - kneeHi) / span;
		x = satf(x + R * t * t * (1.0f - x));
	}

	return exptm::PqToLinearNits(x * kIPeak) / sdrNits;
}

// Joint solve: three band targets (delivered linear fractions of white) in, the three
// actuator values out. Coordinate-descent Newton on the analytic chain — each actuator
// is monotone in its own band, so per-anchor secant steps converge in a handful of
// iterations.
//
// This is the point of the controller. The three grade stages compose sequentially in
// the shader, but every model behind them was trained as if it acted ALONE on the
// source. Stacking three solo-trained corrections is not the sum of their intents:
// a power applied after an additive lift rescales that lift, and neither knows.
// Solving the composition jointly means the gamma the solver returns already accounts
// for the pedestal it is also returning.
static void SolveGrade(const float wAnchor[3], const float wTarget[3],
                       float kneeShadow, float guard, float kneeHi, float sdrNits,
                       float& outS, float& outG, float& outR)
{
	float S = outS, G = outG, R = outR;

	auto delivered = [&](int a, float s, float g, float r) {
		return GradeChainDelivered(wAnchor[a], s, g, r, kneeShadow, guard, kneeHi, sdrNits);
	};

	for (int iter = 0; iter < 6; iter++) {
		// (param, anchor, step, lo, hi) — shadow slope->p25, gamma->p50, rolloff->p95.
		// The slope's range stops at 1.0 (identity): it may expand shadows, never
		// crush them, because crushing is what the source or the curve already did
		// and the grade's job here is to undo it.
		struct Axis { float* p; int a; float step; float lo; float hi; };
		Axis axes[3] = {
			{ &S, 0, 0.02f,  0.50f, 1.00f },
			{ &G, 1, 0.05f,  0.60f, 1.60f },
			{ &R, 2, 0.02f, -0.30f, 0.40f },
		};
		for (auto& ax : axes) {
			const float f0 = delivered(ax.a, S, G, R) - wTarget[ax.a];
			const float saved = *ax.p;
			// Probe in a direction that has ROOM. The slope axis starts at its upper
			// bound (1.0 = identity), so an upward probe clamps back onto the saved
			// value, the secant sees a zero derivative and skips the axis forever --
			// the solver then returns identity and the grade is a passthrough. That
			// was exactly the reported symptom.
			const float dir = (saved + ax.step <= ax.hi) ? ax.step : -ax.step;
			const float bumped = fminf(fmaxf(saved + dir, ax.lo), ax.hi);
			if (bumped == saved) {
				continue;
			}
			*ax.p = bumped;
			const float f1 = delivered(ax.a, S, G, R) - wTarget[ax.a];
			*ax.p = saved;
			const float d = (f1 - f0) / (bumped - saved);
			if (fabsf(d) > 1e-6f) {
				*ax.p = fminf(fmaxf(saved - f0 / d, ax.lo), ax.hi);
			}
		}
	}

	outS = S; outG = G; outR = R;
}

// CPU mirror of the GT7 path in ps_convert_pq_to_sdr.hlsl / convert/gt7_tonemap.hlsl:
// for one SOURCE luminance in nits, return the ENCODED (gamma-2.2, 0..1) value the
// correction pass will hand to the grade. This is what lets the grade know what the
// curve is about to deliver without measuring anything — the curve is closed-form
// and every input to it is already on this side of the API.
//
// The constants MUST stay in step with the shader. If GT7_ALPHA, GT7_LINEAR_SECTION
// or kGT7RangeFit changes there, this stops predicting and the mid-tone target goes
// quietly wrong, so they are named here rather than folded into one expression.
static float PredictGt7RelLinear(float srcNits, float refWhiteNits, float peakNits, float ceilingNits,
                                 float alpha)
{
	if (srcNits <= 0.0f || peakNits <= 0.0f || ceilingNits <= 0.0f) {
		return 0.0f;
	}
	const float kGT7RangeFit   = 2.2f;    // ps_convert_pq_to_sdr.hlsl
	const float kGT7LinearSec  = 0.444f;  // GT7_LINEAR_SECTION, gt7_tonemap.hlsl
	// The user's reach, with the shader's own "0 means unset" fallback applied
	// identically. This MUST track gt7Alpha: the mid-tone target solves against this
	// prediction, so a prediction that ignores the curve's actual alpha computes a
	// gamma for a curve that is not running.
	const float kGT7Alpha      = (alpha > 0.001f) ? alpha : 0.25f;

	const float gain = kGT7RangeFit * refWhiteNits / peakNits;
	const float x    = srcNits * gain;                 // the operator's own units

	const float k    = (kGT7LinearSec - 1.0f) / (kGT7Alpha - 1.0f);
	const float kA   = ceilingNits * kGT7LinearSec + ceilingNits * k;
	const float kB   = -ceilingNits * k * expf(kGT7LinearSec / k);
	const float kC   = -1.0f / (k * ceilingNits);
	const float knee = kGT7LinearSec * ceilingNits;

	float out;
	if (x >= knee) {
		out = kA + kB * expf(x * kC);                  // the exponential shoulder
	} else {
		out = x;      // GT7_TOE_STRENGTH is 1.0 in this port, so the toe branch IS
		              // identity — which is why this prediction is simple.
	}
	out = fminf(out, ceilingNits);                     // min(blended, ceiling), as GT does
	return fmaxf(out / ceilingNits, 0.0f);             // linear fraction of white
}

// The same prediction, encoded (gamma 2.2) — what the panel/PNG comparisons use.
static float PredictGt7Encoded(float srcNits, float refWhiteNits, float peakNits, float ceilingNits,
                               float alpha)
{
	return powf(PredictGt7RelLinear(srcNits, refWhiteNits, peakNits, ceilingNits, alpha), 1.0f / 2.2f);
}

// Where GT7's shoulder lands on the ceiling, as a multiple of it — the CPU mirror of
// the algebra in gt7_tonemap.hlsl (solve shoulder(x) = peak for x/peak). 1.47 at
// alpha 0.25, 1.87 at 0.10, 2.67 at 0.02. This is the most exposure the curve can
// carry before highlights flatten, so it is the only honest upper bound on refWhite.
static float Gt7Reach(float alpha)
{
	const float LS = 0.444f;                       // GT7_LINEAR_SECTION
	const float a  = (alpha > 0.001f && alpha < 0.999f) ? alpha : 0.25f;
	const float k  = (LS - 1.0f) / (a - 1.0f);
	return LS + k * logf(k / (LS + k - 1.0f));
}

void CDX11VideoProcessor::SetMLParams()
{
	float gamma   = 1.0f;
	float shadow  = 0.0f;
	float rolloff = 0.0f;
	float l1max   = 0.5f;
	float gammaTopUp = 0.0f;   // user offset, passed to the shader SEPARATELY (see below)

	// Micro-contrast texture gate: IIR-filter the raw texture energy, snapping
	// on the same scene-jump anchor as the gamma filter (features[0] = maxscl).
	// Placed BEFORE the gamma branch so m_MLPrevL1Max still holds last frame.
	if (m_bMLFeaturesReady) {
		const float f0now = m_MLFeatures[0];
		if (m_MLPrevL1Max < 0.0f || fabsf(f0now - m_MLPrevL1Max) > 0.15f) {
			m_MLTextureSmooth = m_MLRawTexture;
		} else {
			m_MLTextureSmooth = 0.10f * m_MLRawTexture + 0.90f * m_MLTextureSmooth;
		}
	}

	if (m_iMLMode == ML_Manual) {
		gamma   = (m_bMLToneCurveEnabled && m_bMLGammaEnabled) ? discard<float>(m_fMLGamma, 1.0f, 0.5f, 1.5f) : 1.0f;
		shadow  = (m_bMLToneCurveEnabled && m_bMLShadowEnabled) ? discard<float>(m_fMLShadow, 0.0f, 0.0f, 0.4f) : 0.0f;
		rolloff = (m_bMLToneCurveEnabled && m_bMLHighlightEnabled) ? discard<float>(m_fMLHighlight, 0.0f, -0.3f, 0.4f) : 0.0f;
	}
	else if (m_iMLMode == ML_Auto && m_bMLFeaturesReady && m_bMLGammaModelLoaded) {
		float pred = 1.0f;
		m_bMLGammaPredictOk = (m_bMLToneCurveEnabled && m_bMLGammaEnabled)
			&& m_MLGammaModel.Predict(m_MLFeatures, ML_FEATURE_DIM, pred);
		if (m_bMLGammaPredictOk) {
			m_MLGammaRawPred = pred;
			pred = fminf(1.5f, fmaxf(0.5f, pred));

			const float l1now = m_MLFeatures[0];
			if (m_MLPrevL1Max < 0.0f || fabsf(l1now - m_MLPrevL1Max) > 0.15f) {
				m_MLIIRGamma = pred;
			} else {
				m_MLIIRGamma = 0.10f * pred + 0.90f * m_MLIIRGamma;
			}
			m_MLPrevL1Max = l1now;
			gamma = m_MLIIRGamma;

			// Gamma top-up (model-override, mirrors the highlight shoulder below):
			// a signed offset the user ADDS to the model's prediction IN AUTO. The
			// model keeps shaping the per-scene gamma; this moves it up or down
			// without freezing the scene adaptation.
			//
			// Additive on the EXPONENT, not multiplicative: a multiplier cannot lift
			// a scene the model judged neutral — predicted 1.0 is a fixed point —
			// which is precisely the case a top-up exists to cover.
			//
			// Clamped wider than the model's own 0.5-1.5 because an offset can leave
			// it, but bounded: the shader's gamma block guards BOTH ends itself
			// (fade over the shadows, hi_g over the highlights), so this stays a
			// midtone move and cannot reintroduce the highlight push the curve gain
			// had at 1.6.
			//
			// Signed, and added to the exponent — see fMLGammaTopUp in IVideoRenderer.h
			// for why it is additive rather than a multiplier.
			//
			// NOT folded into `gamma`. It is passed to the shader SEPARATELY as
			// MLGammaTopUp so the gamma pass can hold it back inside skin using the
			// trained skin LUT. Folded in, it would be inseparable from the model's
			// own judgement and there would be nothing to gate.
			// ---- RELATIVE MID-TONE TARGET -------------------------------------
			// The models read SOURCE statistics and emit an absolute value, so they
			// cannot see what the tone curve already did to the picture: change
			// refWhite or sdrNits and the input vector is identical while the delivered
			// mid-tone moves a long way. That is why the top-up has to be ridden by
			// hand — it has been absorbing a delivery error the model is blind to.
			//
			// The curve is closed-form, so its effect on the source's average can be
			// COMPUTED rather than measured. Solve for the exponent that lands the
			// delivered mid-tone on the user's target and the grade becomes insensitive
			// to the delivery: refWhite is then free to do its real job (setting how
			// hard the shoulder works, i.e. highlight safety) while luminosity is set
			// independently. Those two have been fighting all day because they were
			// the same control.
			//
			// pow(x, 1/g) = target  =>  g = ln(x) / ln(target), and the shader applies
			// g = MLGamma + MLGammaTopUp, so the top-up is the difference.
			//
			// m_fMLTargetMid == 0 disables all of this and leaves the manual top-up
			// exactly as it was, so it cannot change anyone's picture by surprise.
			float autoTopUp = 0.0f;
			if (m_fMLTargetMid > 0.001f && MLFeatureDomainIsPq() &&
			    (m_iHdrLocalToneMappingType == TONE_CURVE_GT7 ||
			     m_iHdrLocalToneMappingType == TONE_CURVE_GT7_ADAPT) &&
			    m_iSDRDisplayNits > 0 && m_fCurvePeakNits > 10.0f) {
				const float srcAvgNits = exptm::PqToLinearNits(m_MLFeatures[1]);
				const float cur = PredictGt7Encoded(srcAvgNits, m_fEffRefWhite,
				                                    m_fCurvePeakNits, (float)m_iSDRDisplayNits,
				                                    m_fHdrGt7Alpha);
				if (cur > 0.001f && cur < 0.999f) {
					autoTopUp = logf(cur) / logf(m_fMLTargetMid) - gamma;
				}
			}
			m_MLAutoTopUp = autoTopUp;
			gammaTopUp = fminf(0.50f, fmaxf(-0.50f, autoTopUp + m_fMLGammaTopUp));
			m_MLGammaTopUpApplied = gammaTopUp;
		}
		float highlightPred = 0.0f;
		if (m_bMLHighlightModelLoaded && m_MLHighlightModel.Predict(m_MLFeatures, ML_FEATURE_DIM, highlightPred)) {
			highlightPred = fmaxf(-0.30f, fminf(0.40f, highlightPred));
			// Highlight shoulder override (model-override, mirrors dehaze): a signed
			// offset the user adds to the model's rolloff IN AUTO — the model keeps
			// shaping the per-scene shoulder, the slider just pulls the rim highlight
			// back (negative = softer/earlier shoulder) without freezing per scene.
			const float ctrl = fminf(0.20f, fmaxf(-0.20f, m_fMLHighlightRolloffCtrl));
			highlightPred = fmaxf(-0.30f, fminf(0.40f, highlightPred + ctrl));
			m_MLRolloffCtrlApplied = ctrl;
		} else {
			m_MLRolloffCtrlApplied = 0.0f;
		}
		rolloff = (m_bMLToneCurveEnabled && m_bMLHighlightEnabled) ? highlightPred : 0.0f;

		m_MLDebugFeat0 = m_MLFeatures[0];
		m_MLDebugFeat1 = m_MLFeatures[1];
		m_MLDebugPred = gamma;
		l1max = fmaxf(0.05f, fminf(0.90f, m_MLFeatures[0]));
	}

	// Shadow model: feeds both the unified tone curve toe (MLShadowToe) and the
	// DoG detail gate. Runs when either the tone curve or DoG shadow is active.
	const bool bNeedShadowPred = (m_iMLMode == ML_Auto && m_bMLToneCurveEnabled)
	                           || m_bMLShadowEnabled;
	if (bNeedShadowPred && m_bMLShadowModelLoaded && m_bMLFeaturesReady) {
		float shadowPred = 0.0f;
		if (m_MLShadowModel.Predict(m_MLFeatures, ML_FEATURE_DIM, shadowPred)) {
			shadow = fmaxf(0.0f, fminf(0.5f, shadowPred));
			// Shadow dehaze: 0..1 scale DOWN the model's shadow-toe lift. The model
			// predicts a toe that lifts the dark end (x += toe·x·t²) — at high values
			// this reads as lifted-black haze. dehaze multiplies the toe target by
			// (1−dehaze): 0 = model as-is; 1 = model fully suppressed (toe→0, the
			// shader's >0.005 gate then skips the whole step → bit-identical no-lift).
			const float dehaze = fminf(1.0f, fmaxf(0.0f, m_fMLShadowDehaze));
			shadow *= (1.0f - dehaze);
			m_MLShadowDehazeApplied = dehaze;
		}
	} else if (m_bMLShadowEnabled) {
		m_MLShadowDehazeApplied = 0.0f;
	}

	// Adaptive highlight guard from the scene peak (same formula as ml_render.c).
	const float guard = (l1max > 0.6f) ? fmaxf(0.50f, 0.85f - (l1max - 0.6f) * 0.75f) : 0.85f;

	const float hunt = m_bMLHuntEnabled
		? discard<float>(m_fMLHunt, 0.80f, 0.0f, 1.0f) : 0.0f;
	// all Zion features apply regardless of DoVi/HDR10 source (user decision)
	const bool bChromaActive = m_bMLChromaEnabled;

	const float shadowStrength = 0.0f;
	// Micro-contrast strength = user slider (clarity exponent, cap 0.5) × the
	// automatic per-scene texture gate. The gate NORMALIZES the raw texture
	// energy into [0,1]: the raw fraction is ~0.006 on box-averaged luma and
	// crushed strength below the shader's 0.001 fast-path. Textured scenes
	// (raw ≥ kMCGateRef) get the full slider; flat scenes stay ~0.
	const float kMCGateRef = 0.02f;
	const float mcGate = m_bMLFeaturesReady
		? fminf(1.0f, m_MLTextureSmooth / kMCGateRef)
		: 0.0f;
	const float contrastStrength = m_bMLContrastEnabled
		? fmaxf(0.0f, fminf(m_fMLContrast, 0.50f) * mcGate)
		: 0.0f;
	if (m_bMLDebug && m_pMLDebugFile) {
		fwprintf(m_pMLDebugFile, L"MC_TRACE frame=%llu featsReady=%d enabled=%d cf=%.4f raw=%.4f smooth=%.4f strength=%.4f zoneLo=%.3f ratioCap=%.3f maxscl=%.4f debugVis=%d\n",
			(unsigned long long)m_MLDebugFrame, (int)m_bMLFeaturesReady, (int)m_bMLContrastEnabled,
			(double)m_fMLContrast, (double)m_MLRawTexture, (double)m_MLTextureSmooth,
			(double)contrastStrength, (double)m_fMLContrastZoneLo, (double)m_fMLContrastRatioCap,
			(double)m_MLFeatures[0], (int)m_bMLContrastDebugVis);
		fflush(m_pMLDebugFile);
	} else if (m_bMLDebug && m_pMLDebugFile && m_MLDebugFrame % 120 == 0) {
		// Throttled MC summary (frame-log on; keeps the file small at 2/sec).
		fwprintf(m_pMLDebugFile, L"MC_SUM frame=%llu featsReady=%d enabled=%d cf=%.4f raw=%.4f smooth=%.4f strength=%.4f zoneLo=%.3f ratioCap=%.3f\n",
			(unsigned long long)m_MLDebugFrame, (int)m_bMLFeaturesReady, (int)m_bMLContrastEnabled,
			(double)m_fMLContrast, (double)m_MLRawTexture, (double)m_MLTextureSmooth,
			(double)contrastStrength, (double)m_fMLContrastZoneLo, (double)m_fMLContrastRatioCap);
		fflush(m_pMLDebugFile);
	}

	// Face detection (YuNet + ByteTrack) DISABLED — person masks replace it.
#if 0
	float sceneDelta2 = 0.0f;
	bool hasPrev = m_fPrevSceneFeat[0] >= 0.0f;
	for (int i = 0; i < SCENE_CUT_N; i++) {
		const float cur = m_MLFeatures[SCENE_CUT_FEATS[i]];
		if (hasPrev) {
			const float d = cur - m_fPrevSceneFeat[i];
			sceneDelta2 += d * d;
		}
		m_fPrevSceneFeat[i] = cur;
	}
	if (hasPrev && sqrtf(sceneDelta2) > SCENE_CUT_THRESH) {
		m_pByteTracker = std::make_unique<BYTETracker>(0.5f, 30, 0.8f, 24);
		if (m_bMLDebug && m_pMLDebugFile) {
			fwprintf(m_pMLDebugFile, L"FACE_SCENE_CUT l2=%.4f thresh=%.4f feats=%.4f,%.4f,%.4f,%.4f,%.4f\n",
				(double)sqrtf(sceneDelta2), (double)SCENE_CUT_THRESH,
				(double)m_MLFeatures[0], (double)m_MLFeatures[1],
				(double)m_MLFeatures[3], (double)m_MLFeatures[5], (double)m_MLFeatures[7]);
			fflush(m_pMLDebugFile);
		}
	}

	// Feed synchronous face detections into ByteTracker (Kalman + Hungarian)
	if (!m_pByteTracker)
		m_pByteTracker = std::make_unique<BYTETracker>(0.5f, 30, 0.8f, 24);

	const int nRaw = m_nSyncFaces;
	Eigen::MatrixXf detMatrix(nRaw > 0 ? nRaw : 1, 5);
	if (nRaw > 0) {
		for (int i = 0; i < nRaw; i++) {
			detMatrix(i, 0) = m_syncFaces[i].x * FACE_DETECT_W;
			detMatrix(i, 1) = m_syncFaces[i].y * FACE_DETECT_H;
			detMatrix(i, 2) = m_syncFaces[i].w * FACE_DETECT_W;
			detMatrix(i, 3) = m_syncFaces[i].h * FACE_DETECT_H;
			detMatrix(i, 4) = m_syncFaces[i].score;
		}
	} else {
		detMatrix.setZero();
	}
	auto trackedTracks = m_pByteTracker->process_frame_detections(detMatrix);
	// Include recently-lost tracks (Kalman-predicted positions bridge detection gaps)
	auto lostTracks = m_pByteTracker->get_recent_lost_tracks(8);
	trackedTracks.insert(trackedTracks.end(), lostTracks.begin(), lostTracks.end());

	// Convert tracked results back to UV [0,1] for the shader
	CFaceDetect::FaceBox faceBoxes[4] = {};
	float faceTrackIds[4] = {};
	int numFaces = 0;
	const float invW = 1.0f / FACE_DETECT_W;
	const float invH = 1.0f / FACE_DETECT_H;
	for (const auto& trk : trackedTracks) {
		if (numFaces >= 4) break;
		auto box = trk.tlwh();
		faceTrackIds[numFaces] = (float)trk.get_track_id();
		faceBoxes[numFaces++] = {
			(float)(box[0] * invW), (float)(box[1] * invH),
			(float)(box[2] * invW), (float)(box[3] * invH),
			trk.get_score()
		};
	}
#endif   // end of face-detection disable
	// Face cbuffer stays zeroed (layout stable) — face path is inert.
	int numFaces = 0;
	CFaceDetect::FaceBox faceBoxes[4] = {};
	float faceTrackIds[4] = {};

	// ---- JOINT BAND CONTROLLER ---------------------------------------------
	// Solve pedestal/gamma/rolloff together so the delivered p25/p50/p95 land on the
	// house targets, inverting the grade's closed-form chain. Per frame while armed:
	// the inputs are this frame's source percentiles, so it tracks scenes with no
	// settle window and no per-title ritual; temporal smoothing below is noise
	// control, not convergence.
	float ctlS = 1.0f, ctlG = 1.0f, ctlR = 0.0f;
	// Gate bookkeeping, OUTSIDE the gate: a line inside `if (ctl)` cannot explain its
	// own absence. Four conditions, and three consecutive sessions in which the
	// controller appeared to do nothing.
	if (m_bMLDebug && m_pMLDebugFile && (m_MLDebugFrame % 30 == 0)) {
		fwprintf(m_pMLDebugFile,
			L"CTLSTATE on=%d feat=%d curve=%d peak=%.0f sdr=%d effRefW=%.1f\n",
			m_bMLController ? 1 : 0, m_bMLFeaturesReady ? 1 : 0,
			m_iHdrLocalToneMappingType, (double)m_fCurvePeakNits,
			m_iSDRDisplayNits, (double)m_fEffRefWhite);
		fflush(m_pMLDebugFile);
	}
	const bool ctl = m_bMLController && m_bMLFeaturesReady &&
	                 (m_iHdrLocalToneMappingType == TONE_CURVE_GT7 ||
	                  m_iHdrLocalToneMappingType == TONE_CURVE_GT7_ADAPT) &&
	                 m_iSDRDisplayNits > 0 && m_fCurvePeakNits > 10.0f;
	if (ctl) {
		const float sdrN = (float)m_iSDRDisplayNits;
		// features[3..8] = p25 p50 p75 p90 p95 p99, in source PQ.
		const int idx[3] = { 3, 4, 7 };
		float wA[3], wT[3];
		for (int i = 0; i < 3; i++) {
			const float nits = exptm::PqToLinearNits(m_MLFeatures[idx[i]]);
			wA[i] = PredictGt7RelLinear(nits, m_fEffRefWhite, m_fCurvePeakNits, sdrN, m_fHdrGt7Alpha);
			// SCENE-REFERRED target, not a constant. A constant delivered mid is
			// auto-gain by construction: it turns a night scene into noon, because it
			// says where the mid should land without asking how bright the scene is.
			// This law says instead: a source at W nits approaches display white, and
			// everything else follows monotonically — so 10 nits lands in the same
			// place whatever else is in the frame. Night stays night; the controller's
			// job narrows to making OUR delivery match the law, band by band.
			wT[i] = 1.0f - expf(-nits / fmaxf(m_fCtlWhiteNits, 10.0f));
		}

		// The highlight knee is derived from gamma, and gamma is what we are solving
		// for: one extra pass with the solved value keeps the two consistent.
		ctlS = m_ctlS; ctlG = m_ctlG; ctlR = m_ctlR;
		for (int pass = 0; pass < 2; pass++) {
			const float kneeHi = fmaxf(0.65f, fminf(0.85f, 0.90f - (1.0f / fmaxf(ctlG, 0.5f)) * 0.20f));
			SolveGrade(wA, wT, 0.25f, guard, kneeHi, sdrN, ctlS, ctlG, ctlR);
		}

		// Smooth, but snap on a large move: that is a cut, and a cut should arrive in
		// one frame rather than trail the picture.
		auto settle = [](float& held, float now, float jump) {
			if (fabsf(now - held) > jump) { held = now; }
			else { held += 0.20f * (now - held); }
		};
		settle(m_ctlS, ctlS, 0.10f);
		settle(m_ctlG, ctlG, 0.25f);
		settle(m_ctlR, ctlR, 0.10f);
		ctlS = m_ctlS; ctlG = m_ctlG; ctlR = m_ctlR;

		// The solver's own bookkeeping, because three consecutive iterations were
		// diagnosed from symptoms and each diagnosis was wrong. This prints the
		// anchors, the targets, the solved actuators and what the mirror says those
		// actuators deliver -- so "the controller does nothing" becomes a line that
		// says WHICH of the four stages (measure, target, solve, apply) is lying.
		if (m_bMLDebug && m_pMLDebugFile && (m_MLDebugFrame % 30 == 0)) {
			const float kneeHi = fmaxf(0.65f, fminf(0.85f, 0.90f - (1.0f / fmaxf(ctlG, 0.5f)) * 0.20f));
			fwprintf(m_pMLDebugFile,
				L"CTL frame=%llu wA=%.4f/%.4f/%.4f wT=%.4f/%.4f/%.4f S=%.3f G=%.3f R=%.3f "
				L"dA=%.4f/%.4f/%.4f refW=%.1f peak=%.0f sdr=%d\n",
				(unsigned long long)m_MLDebugFrame,
				(double)wA[0], (double)wA[1], (double)wA[2],
				(double)wT[0], (double)wT[1], (double)wT[2],
				(double)ctlS, (double)ctlG, (double)ctlR,
				(double)GradeChainDelivered(wA[0], ctlS, ctlG, ctlR, 0.25f, guard, kneeHi, sdrN),
				(double)GradeChainDelivered(wA[1], ctlS, ctlG, ctlR, 0.25f, guard, kneeHi, sdrN),
				(double)GradeChainDelivered(wA[2], ctlS, ctlG, ctlR, 0.25f, guard, kneeHi, sdrN),
				(double)m_fEffRefWhite, (double)m_fCurvePeakNits, m_iSDRDisplayNits);
			fflush(m_pMLDebugFile);
		}
	}

	MLParams_t params = {
		ctl ? ctlG : gamma,
		ctl ? 0.0f : shadow,        // controller owns the shadow band; legacy toe stands down
		0.25f,                                            // shadow knee
		shadowStrength,                                   // DoG detail multiplier
		ctl ? ctlR : rolloff,
		fmaxf(0.65f, fminf(0.85f, 0.90f - (1.0f / fmaxf(gamma, 0.5f)) * 0.20f)), // highlight knee
		guard,
		hunt,                                             // hunt compensation
		l1max,
		0.25f,                                            // highlight desaturation
		// The domain of the texture the ML passes actually read — NOT the swap
		// chain's. On an SDR display the swap chain is G22 whether or not the
		// correction ran, so the old test told ps_ml_gamma to decode PQ with
		// pow(x, 2.2) whenever Convert to SDR was off. See MLInputIsPq().
		MLInputIsPq() ? 1.0f : 0.0f,
		0.0f,
		// Phase 1b — chroma vector tuning (per-row enables)
		(bChromaActive && m_bMLChromaNeutralEnabled) ? discard<float>(m_fMLChromaNeutral, 1.08f, 1.0f, 1.3f) : 1.0f,
		(bChromaActive && m_bMLChromaFireEnabled) ? discard<float>(m_fMLChromaFire, 1.15f, 1.0f, 1.3f) : 1.0f,
		(bChromaActive && m_bMLChromaKneeEnabled) ? discard<float>(m_fMLChromaKnee, 0.45f, 0.1f, 0.9f) : 0.45f,
		(bChromaActive && m_bMLChromaSkinEnabled) ? discard<float>(m_fMLChromaSkinProtect, 0.95f, 0.5f, 1.0f) : 0.95f,
		m_fSkinLUTCR0, m_fSkinLUTCR1, m_fSkinLUTCB0, m_fSkinLUTCB1,
		bChromaActive ? 1.0f : 0.0f,
		m_bMLHuntEnabled ? 1.0f : 0.0f,
		m_bMLShadowDebugVis ? 1.0f : 0.0f, m_fMLShadowZoneCeil, m_fMLShadowSkinProtect, m_fMLShadowRatioCap, m_bMLContrastDebugVis ? 1.0f : 0.0f, m_bSkyDebugVis ? 1.0f : 0.0f,
		// P0 — black bars repaint
		(m_bMLFeaturesReady ? m_fMLBarTop : 0.0f),
		(m_bMLFeaturesReady ? m_fMLBarBot : 1.0f),
		(m_bMLFeaturesReady ? (float)m_videoRect.top / std::max((int)m_windowRect.Height(), 1) : 0.0f),
		(m_bMLFeaturesReady ? (float)m_videoRect.Height() / std::max((int)m_windowRect.Height(), 1) : 1.0f),
		0.0f,
		0.0f,                                     // uBarBlack (P4 display-model constant)
		(m_bMLFeaturesReady && m_bMLBarsHighlight) ? 1.0f : 0.0f,
		0.0f, // _padBar
		// Contrast recovery
		contrastStrength,
		m_fMLContrastZoneLo,
		m_fMLContrastSkinProtect,
		m_fMLContrastRatioCap,
		// Face detection
		m_bFaceProtectEnabled ? m_fFaceProtect : 0.0f,
		m_fFaceFeather,
		m_fFaceExpand,
		m_bFaceDebugVis ? 1.0f : 0.0f,
		numFaces,
		{}, // _padFace[3]
	};
	for (int i = 0; i < numFaces; i++) {
		params.FaceBox[i * 4 + 0] = faceBoxes[i].x;
		params.FaceBox[i * 4 + 1] = faceBoxes[i].y;
		params.FaceBox[i * 4 + 2] = faceBoxes[i].w;
		params.FaceBox[i * 4 + 3] = faceBoxes[i].h;
		params.FaceTrackId[i] = faceTrackIds[i];
	}

	if (m_bMLDebug && m_pMLDebugFile && m_MLDebugFrame % 60 == 0 && numFaces > 0) {
		fwprintf(m_pMLDebugFile, L"FACE_CBUF numFaces=%d protect=%.2f debugVis=%.0f feather=%.3f expand=%.2f srcTex=%ux%u\n",
			numFaces, (double)params.FaceProtect, (double)params.FaceDebugVis,
			(double)params.FaceFeather, (double)params.FaceExpand,
			m_TexConvertOutput.desc.Width, m_TexConvertOutput.desc.Height);
		for (int i = 0; i < numFaces; i++) {
			fwprintf(m_pMLDebugFile, L"  face[%d] tid=%d x=%.4f y=%.4f w=%.4f h=%.4f score=%.3f px=(%d,%d,%d,%d)\n",
				i, (int)faceTrackIds[i], (double)faceBoxes[i].x, (double)faceBoxes[i].y,
				(double)faceBoxes[i].w, (double)faceBoxes[i].h, (double)faceBoxes[i].score,
				(int)(faceBoxes[i].x * m_TexConvertOutput.desc.Width),
				(int)(faceBoxes[i].y * m_TexConvertOutput.desc.Height),
				(int)(faceBoxes[i].w * m_TexConvertOutput.desc.Width),
				(int)(faceBoxes[i].h * m_TexConvertOutput.desc.Height));
		}
		fflush(m_pMLDebugFile);
	}

	if (m_bMLDebug && m_pMLDebugFile) {
		fwprintf(m_pMLDebugFile,
			L"BARS_CBUF uBarTop=%.4f uBarBot=%.4f uBarV0=%.4f uBarSpan=%.4f uBarFeather=%.1f uBarBlack=%.4f uBarHL=%.0f  m_bMLBarsEnabled=%d m_bMLBarsHighlight=%d m_bMLFeaturesReady=%d\n",
			(double)params.uBarTop, (double)params.uBarBottom,
			(double)params.uBarV0, (double)params.uBarSpan,
			(double)params.uBarFeather, (double)params.uBarBlack,
			(double)params.uBarHighlight,
			(int)m_bMLBarsEnabled, (int)m_bMLBarsHighlight, (int)m_bMLFeaturesReady);
		fflush(m_pMLDebugFile);
	}

	// Sky tone leg (stage 3): relaxed-guard leg blended inside sky by the mask.
	params.MLSkyStrength = (m_bMLFeaturesReady && m_bSkyEnabled)
		? fminf(1.0f, fmaxf(0.0f, m_fMLSkyStrength)) : 0.0f;
	params.MLSkyGuard = m_bMLFeaturesReady
		? fminf(0.97f, fmaxf(guard + 0.08f, 0.88f))
		: guard - 1.0f; // effectively disabled before features are ready
	params.MLSkyLuma = m_SkyLuma;

	// Gamma top-up (see fMLGammaTopUp): carried SEPARATELY from MLGamma so the
	// gamma pass can gate it by the trained skin LUT. Stays 0 when the model did
	// not predict, which is also the "no top-up" default.
	// The controller solves gamma itself; a top-up on top would double-act on the
	// same band. Zeroed here rather than at the solve site because `ctl` is not in
	// scope there.
	params.MLGammaTopUp = ctl ? 0.0f : gammaTopUp;
	// Shadow pedestal — the offset the toe's gain cannot be. Bounded to 8% of white:
	// it is a fraction of I_norm, and a value large enough to matter for crushed
	// shadows is also large enough to grey out the letterbox matte if the shader's
	// in-content gate ever fails. 0 = the previous picture exactly.
	// The controller does NOT use the pedestal: an additive offset lifts black to grey
	// and veils the picture instead of revealing detail (measured by eye). It solves
	// the toe SLOPE instead, and leaves the pedestal at zero. The manual dial stays
	// available when the controller is off, one-sided as before.
	params.MLShadowPedestal = ctl ? 0.0f
	                              : std::clamp(m_fMLShadowPedestal, 0.0f, 0.08f);
	params.MLShadowSlope = ctl ? std::clamp(ctlS, 0.50f, 1.00f) : 1.0f;
	params._padS0 = 0.0f;
	params._padS1 = 0.0f;
	params._padS2 = 0.0f;
	params.MLGamutCap = m_fGamutCap;
	params.MLGamutMap709 = (m_iOutputGamut == 0) ? 1.0f : 0.0f;

	// Person skin protection (stage 4): LUT-only-inside-people gate. Strength 0
	// -> shaders branch to the plain legacy LUT/MC path (bit-identical).
	params.MLSkinPersonStrength = (m_bMLFeaturesReady && m_bPersonEnabled)
		? fminf(1.0f, fmaxf(0.0f, m_fPersonStrength)) : 0.0f;
	params.MLSkinPersonDebugVis = m_bPersonDebugVis ? 1.0f : 0.0f;
	// Skin tone-preservation (stage 5): CPU IIR skin-set mean source luma (from the
	// SKINMET readback cadence) anchors the shader's skin remap; strength 0 = inert.
	params.MLSkinAnchor   = m_skinAnchorValid ? (float)m_skinAnchorSmooth : 0.0f;
	params.MLSkinPreserve = fminf(1.0f, fmaxf(0.0f, m_fMLSkinPreserve)) * m_maskEnv;

	// Snapshot the FULL struct AFTER the appended tails (sky/person) are set.
	// Process() re-uploads m_lastMLParams every frame for the UV remap, so the
	// mirror must carry the tails or they hit the shader as zeros.
	m_lastMLParams = params;

	// Always upload: a zero-filled buffer (from a missed update) made the
	// gamma shader run at MLGamma=0 -> pow(y, g<1) -> washed-out image.
	if (m_pMLParams) {
		m_pDeviceContext->UpdateSubresource(m_pMLParams, 0, nullptr, &params, 0, 0);
	} else {
		D3D11_BUFFER_DESC BufferDesc = {
			.ByteWidth = sizeof(params),
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
		};
		D3D11_SUBRESOURCE_DATA InitData = { &params, 0, 0 };
		HRESULT result = m_pDevice->CreateBuffer(&BufferDesc, &InitData, &m_pMLParams);
		EXECUTE_ASSERT(S_OK == result);
	}
}

static void DumpSegPreviewBmp(const uint8_t* bgra, int w, int h, int stride, const wchar_t* path);

void CDX11VideoProcessor::ExtractMLFeatures()
{
	if (!m_pPSMLDownsample || !m_MLDownsample || !m_MLStaging || !m_TexConvertOutput.pTexture) {
		if (m_bMLDebug && m_pMLDebugFile) {
			fwprintf(m_pMLDebugFile, L"EXTRACT skip (ps=%d ds=%d stag=%d tex=%d)\n",
				(m_pPSMLDownsample ? 1 : 0), (m_MLDownsample ? 1 : 0),
				(m_MLStaging ? 1 : 0), (m_TexConvertOutput.pTexture ? 1 : 0));
			fflush(m_pMLDebugFile);
		}
		return;
	}

	// 1) downsample the convert output into the 256x144 luma cache.
	// Use the actual texture dimensions — m_srcRect can exceed the texture
	// when the convert output is display-sized, pushing UVs past 1.0.
	const CRect fullRect(0, 0, ML_DOWNSAMPLE_W, ML_DOWNSAMPLE_H);
	const CRect texRect(0, 0, m_TexConvertOutput.desc.Width, m_TexConvertOutput.desc.Height);
	HRESULT hr = TextureCopyRect(m_TexConvertOutput, m_MLDownsample, texRect, fullRect, m_pPSMLDownsample, nullptr, 0, false);
	if (FAILED(hr)) {
		return;
	}

	// 2) read back
	m_pDeviceContext->CopyResource(m_MLStaging, m_MLDownsample);

	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (FAILED(m_pDeviceContext->Map(m_MLStaging, 0, D3D11_MAP_READ, 0, &mr))) {
		return;
	}

	// WHICH COLOUR SPACE IS THE SAMPLED TEXTURE IN?
	//
	// Not the swap chain's. m_TexConvertOutput is written by the D3D11 video
	// processor, and for a 2084 source the VP emits PQ
	// (DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, D3D11VP.cpp:609-615): the tone
	// mapping happens later, inside the correction shader. On an SDR display the
	// swap chain stays G22, so keying the domain off the swap chain selected
	// SDRGamma22 and applied SdrGammaToPQ to values that were ALREADY PQ — a
	// double remap that corrupted every feature on exactly the path the tone
	// curve and the Zion grade run on.
	//
	// Key it off the correction actually selected instead: MLFeatureDomainIsPq().
	// The swap-chain test is kept as well, for the HDR-passthrough path where it is
	// the correct signal.
	//
	// The predicate itself was still wrong, and this frame is where it showed: it
	// ANDed in m_bConvertToSdr, so turning Convert to SDR OFF flipped the domain to
	// SDRGamma22 for a texture that is still PQ, and the identical double remap
	// came back on that path only. Caught from the log — feat0 read 0.3161 on the
	// no-conversion path against 0.3855 on the tone-mapped one for a static frame,
	// and SdrGammaToPQ(0.3855) = 0.31575. The extractor's texture is written by the
	// VP, so only the SOURCE decides its domain; the correction is downstream of it.
	const MLLumaDomain domain =
		MLFeatureDomainIsPq() ? MLLumaDomain::PQ : MLLumaDomain::SDRGamma22;
	BuildMLFeatures((const uint16_t*)mr.pData, (int)(mr.RowPitch / sizeof(uint16_t)), m_MLFeatures, domain, &m_MLBarDetected, &m_MLRawTexture, &m_MLPeakPq995, &m_MLActiveFrac, &m_MLMeanNitsRaw);
	m_pDeviceContext->Unmap(m_MLStaging, 0);

	if (m_bMLDebug && m_pMLDebugFile) {
		fwprintf(m_pMLDebugFile, L"EXTRACT frame=%llu raw_tex=%.4f avg=%.4f maxscl=%.4f\n",
			(unsigned long long)m_MLDebugFrame, (double)m_MLRawTexture,
			(double)m_MLFeatures[1], (double)m_MLFeatures[0]);
		fflush(m_pMLDebugFile);
	}

	m_bMLFeaturesReady = true;
	m_MLDebugFeat0 = m_MLFeatures[0];
	m_MLDebugFeat1 = m_MLFeatures[1];
	m_MLDebugFeatReady = 1;

	// Face detection DISABLED — person instance masks (yolo26n-seg) replace YuNet +
// ByteTrack. The face_down shader + its IsPQ cbuffer STAY (the sky/person seg
// preview uses them). Kept behind #if 0 for easy re-enable.
	{
		// Update IsPQ flag so face_down applies PQ→SDR for the seg preview.
		if (m_pPSFaceDown && m_pFaceDownParams) {
			D3D11_MAPPED_SUBRESOURCE mr2 = {};
			if (SUCCEEDED(m_pDeviceContext->Map(m_pFaceDownParams, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr2))) {
				struct { float IsPQ; float pad[3]; } fd = {
					MLInputIsPq() ? 1.0f : 0.0f
				};
				memcpy(mr2.pData, &fd, sizeof(fd));
				m_pDeviceContext->Unmap(m_pFaceDownParams, 0);
			}
		}
#if 0
		if (m_FaceDownsample && m_FaceStaging && m_pFaceDetect && m_pPSFaceDown) {
			const CRect faceRect(0, 0, 480, 288);
			hr = TextureCopyRect(m_TexConvertOutput, m_FaceDownsample, texRect, faceRect, m_pPSFaceDown, m_pFaceDownParams, 0, false);
			if (SUCCEEDED(hr)) {
				m_pDeviceContext->CopyResource(m_FaceStaging, m_FaceDownsample);
				D3D11_MAPPED_SUBRESOURCE faceMr = {};
				HRESULT hrMap = m_pDeviceContext->Map(m_FaceStaging, 0, D3D11_MAP_READ, 0, &faceMr);
				if (SUCCEEDED(hrMap)) {
					CFaceDetect::FaceBox syncOut[4] = {};
					m_nSyncFaces = m_pFaceDetect->DetectSync((const uint8_t*)faceMr.pData, 480, 288, faceMr.RowPitch, syncOut, 4);
					for (int i = 0; i < m_nSyncFaces; i++) m_syncFaces[i] = syncOut[i];
					m_pDeviceContext->Unmap(m_FaceStaging, 0);
				}
			}
		}
#endif
	}

	// Sky/person segmentation — throttled (~10 fps). Renders its OWN 640x360
	// SDR-ized preview: higher fidelity than the 480x288 face texture, so
	// building/sky boundaries survive the downscale. Pushes a 640x360 R8G8 mask
	// (R=sky, G=person) to the shader. Runs when either feature (or an overlay
	// that needs the mask) is wanted; cadence uses the render-thread frame
	// counter — m_MLDebugFrame is NOT a safe throttle (pinned at 0 without the
	// frame log, which made this run EVERY frame in normal playback).
	const bool wantPerson = m_bPersonEnabled || m_bPersonDebugVis;
	const bool wantSky    = m_bSkyEnabled || m_bSkyDebugVis;
	const bool wantMask   = wantPerson || wantSky;
	// Cadence (render-thread frame counter m_nSkyFrame; NOT m_MLDebugFrame which is
	// pinned at 0 without the frame log):
	//   - Person: every 2 frames (mask updates are EMA/snap-stabilized and the τ
	//     envelope riders out a couple stale frames; every-frame DirectML + a GPU
	//     readback per frame was the stutter — the mask does not need 60 fps).
	//   - Sky: every 5 frames.
	const bool doPerson = wantPerson && (m_nSkyFrame - m_nPersonLastFrame >= 2);
	const bool doSky    = wantSky && (m_nSkyFrame - m_nSkyLastFrame >= 5);
	if (wantMask && (m_pPersonSeg && m_pPersonSeg->IsReady() || m_pSkySeg && m_pSkySeg->IsReady()) &&
		m_SkyMaskSRV && m_SegDownsample && m_SegStaging && (doPerson || doSky)) {
		// Preview is rendered from the PRESENTED content rect in Process (the mask is
		// keyed to the presentation, so it stays aligned even under player crop /
		// zoom / pillarbox on any display aspect). Stage + detect here, 1 frame latent.
		m_pDeviceContext->CopyResource(m_SegStaging, m_SegDownsample);
			D3D11_MAPPED_SUBRESOURCE segMr = {};
			if (SUCCEEDED(m_pDeviceContext->Map(m_SegStaging, 0, D3D11_MAP_READ, 0, &segMr))) {
				// Diagnostic: save the staged preview every ~120 refreshes (frame log
				// on) so a mask-vs-screen offset can be eyeballed (esp. ultrawide /
				// DV-P5) — is the preview the correct presented region?
				if (m_bMLDebug && m_pMLDebugFile && segMr.pData && (m_nSkyFrame % 120 == 0)) {
					wchar_t dumpPath[MAX_PATH];
					swprintf_s(dumpPath, L"%s\\seg_preview_%04llu.bmp", GetMLModuleDir().c_str(),
						(unsigned long long)(m_nSkyFrame / 120));
					DumpSegPreviewBmp((const uint8_t*)segMr.pData, 640, 360, segMr.RowPitch, dumpPath);
					fwprintf(m_pMLDebugFile, L"SEGDUMP wrote %s\n", dumpPath);
					fflush(m_pMLDebugFile);
				}
				CSkySeg::Result sky;
				if (doSky) {
					if (m_pSkySeg->DetectSky((const uint8_t*)segMr.pData, 640, 360, segMr.RowPitch, sky)) {
						m_SkyFrac = sky.skyFrac;
						m_SkyLuma = sky.skyLuma;
						if (!sky.map.empty()) m_LastSkyMap = sky.map;
					}
					m_nSkyLastFrame = m_nSkyFrame;
				}
				// Person instance masks (yolo26n-seg, class 0) — every frame.
				CPersonSeg::Result person;
				if (wantPerson && m_pPersonSeg && m_pPersonSeg->IsReady() &&
					m_pPersonSeg->DetectPersons((const uint8_t*)segMr.pData, 640, 360, segMr.RowPitch, person)) {
					m_PersonFrac = person.personFrac;
					// Camera-cut detector: the new frame's person mask vs the previous
					// raw — a cut has ~zero overlap (different scene, unrelated people).
					// On a cut SNAP the smoothed mask to the new raw (no blend → the old
					// scene's silhouette cannot linger as a shadow). No tracker needed:
					// instance IDs exist in yolo26n-seg but a cut is a mismatch, not a
					// continuity failure — the mask itself is the discontinuity signal.
					float overlap = 1.0f;
					if (!m_LastPersonMap.empty() && m_LastPersonMap.size() == person.map.size()) {
						long long inter = 0, uni = 0;
						for (size_t _i = 0; _i < person.map.size(); _i++) {
							const bool a = person.map[_i] > 0, b = m_LastPersonMap[_i] > 0;
							if (a && b) inter++;
							if (a || b) uni++;
						}
						overlap = uni ? (float)inter / (float)uni : 1.0f;
					}
					const bool bCut = overlap < 0.20f;
					// Scene-confidence scalar (τ): falls fast on SUSTAINED mismatch
					// (a real cut / montage), recovers slowly on stability. A whip pan
					// dips for 1-3 frames then recovers → τ blips but the tone blend
					// barely moves; a real cut keeps τ low for a while → preserve fades
					// out and re-fades in smoothly after masks+anchor settle.
					// Decay on bad: -0.35/frame (reaches ~0 in ~3 bad frames).
					// Recover on good: +0.08/frame (~13 frames back to full).
					if (bCut) {
						m_maskTrust = fmaxf(0.0f, m_maskTrust - 0.35f);
						m_framesSinceGood = 0;
						// Labels are NOT consistent across a cut — reset the per-instance
						// skin-fraction IIR so re-emerging people start from a clean σ
						// (4ms of stale σ on a face would dip its gate for ~5 frames).
						m_instSmInit = false;
						for (int ji = 1; ji <= 8; ji++) m_instSkinSm[ji] = 0.0f;
					} else {
						// recovery only after the scene has been good for a couple frames
						// (a 1-frame dip = noise; don't chase it)
						if (m_framesSinceGood >= 2)
							m_maskTrust = fminf(1.0f, m_maskTrust + 0.08f);
						m_framesSinceGood++;
					}
					// env = smoothstep passes through ~0 below 0.25 trust, ~1 above 0.80
					const float t = (m_maskTrust - 0.25f) / (0.80f - 0.25f);
					m_maskEnv = t <= 0.0f ? 0.0f : (t >= 1.0f ? 1.0f : t * t * (3.0f - 2.0f * t));
					// Update the raw FIRST so the blend below always targets the current
					// frame (the old ordering blended against the previous frame's raw —
					// a permanent 1-frame-late mask).
					if (!person.map.empty()) m_LastPersonMap = person.map;
					if (!person.instMap.empty()) m_LastInstMap = person.instMap;
					if (m_PersonMaskSm.size() != 640 * 360 || bCut) {
						m_PersonMaskSm = m_LastPersonMap;          // snap (first frame / cut)
					} else {
						// Temporal stability: 62% old / 38% new — damps detection flicker
						// (mirrors, weak silhouettes) within a shot without ghost/trail.
						for (size_t _i = 0; _i < m_LastPersonMap.size(); _i++)
							m_PersonMaskSm[_i] = (uint8_t)((5 * (int)m_PersonMaskSm[_i] + 3 * (int)m_LastPersonMap[_i]) >> 3);
					}
					if (m_bMLDebug && m_pMLDebugFile && m_MLDebugFrame % 30 == 0) {
						fwprintf(m_pMLDebugFile, L"MASK_WATCH overlap=%.3f cut=%d trust=%.3f env=%.3f pfrac=%.4f\n",
							(double)overlap, (int)bCut, (double)m_maskTrust, (double)m_maskEnv, (double)m_PersonFrac);
						fflush(m_pMLDebugFile);
					}
				} else {
					// No person this frame (e.g. camera cut to a person-less scene): the
					// masks MUST clear — leaving them made the old scene's silhouette
					// persist as a shadow on the new scene indefinitely. τ also decays
					// (scene changed); masks cleared → gate is zero → preserve can't act
					// anyway, but the fade-in state should reset for the next scene.
					m_PersonFrac = 0.0f;
					m_maskTrust = fmaxf(0.0f, m_maskTrust - 0.35f);
					m_framesSinceGood = 0;
					const float t = (m_maskTrust - 0.25f) / (0.80f - 0.25f);
					m_maskEnv = t <= 0.0f ? 0.0f : (t >= 1.0f ? 1.0f : t * t * (3.0f - 2.0f * t));
					m_instSmInit = false;
					for (int ji = 1; ji <= 8; ji++) m_instSkinSm[ji] = 0.0f;
					if (!m_LastPersonMap.empty()) std::fill(m_LastPersonMap.begin(), m_LastPersonMap.end(), 0);
					if (!m_PersonMaskSm.empty())  std::fill(m_PersonMaskSm.begin(), m_PersonMaskSm.end(), 0);
					if (!m_LastInstMap.empty())   std::fill(m_LastInstMap.begin(), m_LastInstMap.end(), 0);
				}
				if (doPerson) m_nPersonLastFrame = m_nSkyFrame;   // cadence reset after the attempt
				// R=sky (persists between refreshes), G=person (fresh every frame).
				// Independent channels: zero-fill whichever model is unavailable.
				const bool haveSky = !m_LastSkyMap.empty();
				const std::vector<uint8_t>& gmap = m_PersonMaskSm.empty() ? m_LastPersonMap : m_PersonMaskSm;
				const bool haveP = !gmap.empty();
				if (haveSky || haveP) {
					const size_t nPix = 640 * 360;
					std::vector<uint8_t> packed(nPix * 2, 0);
					for (size_t i = 0; i < nPix; i++) {
						if (haveSky) packed[i * 2 + 0] = m_LastSkyMap[i];
						if (haveP)   packed[i * 2 + 1] = gmap[i];
					}
					m_pDeviceContext->UpdateSubresource(m_SkyMask, 0, nullptr, packed.data(), (UINT)640 * 2, 0);
					if (m_bMLDebug && m_pMLDebugFile && doSky) {
						fwprintf(m_pMLDebugFile,
							L"SKY_REFRESH frame=%llu srcW=%ux%u sfrac=%.4f pfrac=%.4f luma=%.3f skyOK=%d personOK=%d stagingPitch=%u\n",
							(unsigned long long)m_MLDebugFrame,
							m_TexConvertOutput.desc.Width, m_TexConvertOutput.desc.Height,
							(double)m_SkyFrac, (double)m_PersonFrac, (double)m_SkyLuma,
							(int)haveSky, (int)haveP, (unsigned)segMr.RowPitch);
						fflush(m_pMLDebugFile);
					}
				}
				// Skin-tone anchor + Poisson gate (production — no frame log needed):
		// anchor = source skin-set mean luma; gate = relaxed person×skin field (gamma t4).
		// The gate (t4) is ONLY consumed by the shader's skin leg, which is gated on
		// MLSkinPreserve — so with preserve=0 the whole 160x90 build (luma, Sobel,
		// per-instance σ, 100-iter Jacobi, upload) is PURE WASTE and was the
		// per-frame stutter. Skip both when the leg is off: bit-identical result,
		// the gate texture simply keeps its last nonzero value (irrelevant at 0).
		if (wantPerson && segMr.pData && m_bMLFeaturesReady && m_fMLSkinPreserve > 0.001f) {
			UpdateSkinAnchor((const uint8_t*)segMr.pData, segMr.RowPitch);
			UpdateSkinGate((const uint8_t*)segMr.pData, segMr.RowPitch);
		}
		m_pDeviceContext->Unmap(m_SegStaging, 0);
			}
	}

	// Mask auto-diagnostic: read back the ACTUAL R8G8 GPU texture and compare
	// the G (person) channel against the CPU personFrac. If gpuG stays ~0 while
	// cpu pfrac is live, the upload/pack is broken — log catches it in the field.
	if (m_bMLDebug && m_pMLDebugFile && (m_nSkyFrame % 36 == 0)) {
		LogMaskProbe();
	}

	UpdateMLBarsFromDetected();

	if (m_bMLDebug && m_pMLDebugFile) {
		fwprintf(m_pMLDebugFile, L"EXTRACT ok feat0=%.4f feat1=%.4f crop=[%d,%d] -> fTop=%.4f fBot=%.4f\n",
			(double)m_MLFeatures[0], (double)m_MLFeatures[1],
			m_MLBarDetected.contentTop, m_MLBarDetected.contentBottom,
			(double)m_fMLBarTop, (double)m_fMLBarBot);
		fflush(m_pMLDebugFile);
	}
}

void CDX11VideoProcessor::UpdateMLBarsFromDetected()
{
	constexpr uint32_t kCropWindow = 90;
	constexpr int kArmRows = 2;
	const int h = ML_DOWNSAMPLE_H;
	const int cTop = m_MLBarDetected.contentTop;
	const int cBot = m_MLBarDetected.contentBottom;
	const bool blank = (cBot < 0);

	if (!blank) {
		if (m_MLCropAge == 0) {
			m_MLCropTop = (uint16_t)cTop;
			m_MLCropBot = (uint16_t)cBot;
		} else {
			m_MLCropTop = std::min(m_MLCropTop, (uint16_t)cTop);
			m_MLCropBot = std::max(m_MLCropBot, (uint16_t)cBot);
		}
	}

	m_MLCropAge++;
	if (m_MLCropAge >= kCropWindow)
		m_MLCropAge = 0;

	const int barTop = (int)m_MLCropTop;
	const int barBot = (h - 1) - (int)m_MLCropBot;
	const bool armed = (barTop >= kArmRows || barBot >= kArmRows);

	if (armed) {
		m_fMLBarTop = (float)barTop / h;
		m_fMLBarBot = 1.0f - (float)std::max(barBot, 0) / h;
	} else {
		m_fMLBarTop = 0.0f;
		m_fMLBarBot = 1.0f;
	}

	if (m_bMLDebug && m_pMLDebugFile) {
		fwprintf(m_pMLDebugFile,
			L"BARS_UNION frame=[cTop=%d cBot=%d blank=%d] union=[cropTop=%d cropBot=%d] age=%u/%u barTop=%d barBot=%d armed=%d -> fTop=%.4f fBot=%.4f\n",
			cTop, cBot, (int)blank,
			(int)m_MLCropTop, (int)m_MLCropBot, m_MLCropAge, kCropWindow,
			barTop, barBot, (int)armed,
			(double)m_fMLBarTop, (double)m_fMLBarBot);
		fflush(m_pMLDebugFile);
	}
}

void CDX11VideoProcessor::GetMLStats(std::wstring& str)
{
	const wchar_t* mode = (m_iMLMode == ML_Auto) ? L"Auto" : (m_iMLMode == ML_Manual ? L"Manual" : L"Off");
	const wchar_t* domain = (m_currentSwapChainColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) ? L"PQ" : L"SDR-remap";
	// input colour space (RENDERER IN / LIVE panel): what we're grading the
	// signal as. ICS_Auto is derived from the media type + resolution; an
	// explicit override is pushed by the DeckLink monitor.
	const wchar_t* icsName = L"auto";
	switch (m_InputColorSpaceOverride) {
	case ICS_BT601:      icsName = L"601"; break;
	case ICS_BT709:      icsName = L"709"; break;
	case ICS_BT2020_SDR: icsName = L"2020-SDR"; break;
	case ICS_BT2020_PQ:  icsName = L"2020-PQ"; break;
	case ICS_BT2020_HLG: icsName = L"2020-HLG"; break;
	default: break;
	}
	const wchar_t* inFmt = m_srcParams.str ? m_srcParams.str : L"-";
	str = std::format(
		L"mode: {}   domain: {}   active: {}\n"
		L"gamma={:.4f}   shadow={:.3f}   highlight={:.3f}\n"
		L"gates: tone={} gammaEnable={} gammaModel={} features={} gammaPredict={}\n"
		L"scene: peak={:.4f} avg={:.4f}   pred={:.4f} raw={:.4f}   feat={}\n"
		L"hunt={:.3f}{}   chroma={:.2f}/{:.2f} knee={:.2f} skin={:.2f}{}\n"
		L"mask: sky={:.3f} person={:.3f}  pDbg={} pStr={:.3f} cad={}/{}\n"
		L"bars: top={} bot={}{}\n"
		L"toneMap: mapper={} active={} builds={} axis={:.4f}..{:.4f} avg={:.4f} contrast={:.2f} out={:.4f}..{:.4f} outNits={:.3f}..{:.1f}\n"
		// WHICH converter is live, and the flags that decide it. `m_strCorrection`
		// is the name chosen in InitMediaType ("PQ to SDR", "Fix BT.2020", ...), so
		// this line answers "did the control change the pipeline" directly instead
		// of by inference. The four flags next to it are the ones that could make
		// Convert to SDR inert on their own, so a reader can tell WHICH reason
		// applies: no HDR display support, passthrough armed, DV suppressing the
		// re-init, or the VP not ready.
		L"pipeline: correction={} convertSdr={} passthrough={} passthroughSupport={} localToneMap={} dovi={} vpReady={} hdrRaw={} tf={} prim={} texVP={} cfmt={} rtxHdr={} toneCurve={} titlePeak={:.0f} cll={} fall={} mmax={} mmin={} sdata={} rcll={} rfall={} cllsz={} hlN={:.2f} clE={:.2f}\n"
		L"rendererIn: fmt={} {}x{}  ics={}",
		mode, domain, (m_bMLActive ? L"yes" : L"no"),
		m_lastMLParams.MLGamma, m_lastMLParams.MLShadowToe, m_lastMLParams.MLHighlightRolloff,
		(m_bMLToneCurveEnabled ? L"1" : L"0"), (m_bMLGammaEnabled ? L"1" : L"0"),
		(m_bMLGammaModelLoaded ? L"ok" : L"no"), (m_bMLFeaturesReady ? L"ready" : L"n/a"),
		(m_bMLGammaPredictOk ? L"ok" : L"FAIL"),
		m_MLFeatures[0], m_MLFeatures[1], m_lastMLParams.MLGamma, m_MLGammaRawPred,
		(m_bMLFeaturesReady ? L"ready" : L"n/a"),
		m_lastMLParams.MLHunt, (m_lastMLParams.MLHuntGate > 0.5f ? L"" : L"(off)"),
		m_lastMLParams.MLChromaNeutral, m_lastMLParams.MLChromaFire,
		m_lastMLParams.MLChromaKnee, m_lastMLParams.MLChromaSkinProtect,
		(m_lastMLParams.MLChromaGate > 0.5f ? L"" : L"(off)"),
		m_SkyFrac, m_PersonFrac,
		(m_bPersonDebugVis ? L"1" : L"0"),
		(double)m_lastMLParams.MLSkinPersonStrength,
		(unsigned long long)m_nSkyFrame, (unsigned long long)m_nSkyLastFrame,
		(int)std::lround(m_fMLBarTop * m_windowRect.Height()),
		(int)std::lround((1.0f - m_fMLBarBot) * m_windowRect.Height()),
		(m_bMLBarsHighlight ? L" (hl)" : L""),
		m_iExperimentalToneMapper, (ToneLutActive() ? L"yes" : L"no"),
		(unsigned long long)m_nToneLutBuilds,
		m_ToneLutKey.pqMin, m_ToneLutKey.pqMax, m_ToneLutKey.pqAvg, m_fToneLutContrast,
		// The curve's OUTPUT range, in PQ and in nits. During calibration this is
		// the number that matters: it is the measured display range the mapper was
		// given, and seeing it here is what separates "I typed 85 and 0.17" from
		// "the curve is built over 85 and 0.17".
		exptm::LinearNitsToPq(m_ToneLutKey.blackNits), exptm::LinearNitsToPq(m_ToneLutKey.dispNits),
		m_ToneLutKey.blackNits, m_ToneLutKey.dispNits,
		(m_strCorrection ? m_strCorrection : L"-"),
		(m_bConvertToSdr ? 1 : 0), (m_bHdrPassthrough ? 1 : 0),
		(m_bHdrPassthroughSupport ? 1 : 0), (m_bHdrLocalToneMapping ? 1 : 0),
		(m_Dovi.bValid ? 1 : 0), (m_D3D11VP.IsReady() ? 1 : 0),
		(m_bHDRPassthroughRaw ? 1 : 0),
		// The source format the selection above actually saw, plus which converter
		// path is live. `m_strCorrection` being "-" is only meaningful alongside
		// these: it can be "-" because the transfer function is not PQ/HLG at all,
		// or because the D3D11 VP is unavailable and the Tex VP path is building its
		// converter instead (texVP), and those need opposite fixes.
		(int)m_srcExFmt.VideoTransferFunction, (int)m_srcExFmt.VideoPrimaries,
		(m_PSConvColorData.bEnable ? 1 : 0), (int)m_srcParams.cformat,
		(m_bVPUseRTXVideoHDR ? 1 : 0),
		// The curve the PQ->SDR converter is actually applying. 0/3 = the legacy
		// ToneMappingHable; 1/2/4/5/6 = the standard curves. This is the field that
		// answers "does the Tone curve row do anything" without reading the shader.
		m_iHdrLocalToneMappingType,
		// The content peak the curves are normalised by. Distinct from the display
		// peak: this is the top of the range being mapped FROM.
		m_fHdrTitlePeakNits,
		// The file's own ST 2086 as the renderer read it from the decoded side
		// data (m_lastHdr10.hdr10). MaxCLL/MaxFALL are in cd/m², MaxMasteringLuminance
		// in cd/m², MinMasteringLuminance scaled by 10000 (0.0001 cd/m²) — the
		// DXGI_HDR_METADATA_HDR10 layout. The engine parses these to feed the
		// panel's MaxCLL / MaxFALL / Mastering max / Mastering min rows on the
		// file path (the DeckLink card path feeds those same rows from dl.*).
		// Content-side ST 2086 (m_hdr10Cll… — display-independent copy of what
		// the decoded side data carried; m_lastHdr10 is display-applied and
		// stays zero on SDR swapchains).
		m_hdr10Cll,
		m_hdr10Fall,
		m_hdr10Mmax,
		m_hdr10Mmin,
		m_sideData,
		m_rawCll, m_rawFall, m_rawCllSize,
		m_fHdrHighlightNeutral,
		m_fHdrChromaExp,
		inFmt, (int)m_srcRectWidth, (int)m_srcRectHeight, icsName);
}

void CDX11VideoProcessor::GetRenderStats(::RenderStats_t& stats)
{
	stats = ::RenderStats_t{};
	stats.framesReceived = m_pFilter ? m_pFilter->m_FrameStats.GetFrames() : 0;
	stats.framesDrawn   = m_pFilter ? m_pFilter->m_DrawStats.GetFrames() : 0;
	stats.dropped       = m_pFilter ? m_pFilter->m_DrawStats.m_dropped : 0;
	stats.skipped       = m_RenderStats.dropped2;
	stats.failed        = m_RenderStats.failed;
	stats.presentLatencyMs = m_presentLatencyMs;
	if (m_pFilter) stats.drawFps = m_pFilter->m_DrawStats.GetAverageFps();
}

HRESULT CDX11VideoProcessor::CreateSkyMaskTexture()
{
	if (!m_pDevice) return E_FAIL;
	if (m_SkyMaskSRV) return S_OK; // already created

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = 640;           // content-res mask (pad-cropped), matches shader UV
	desc.Height = 360;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8_UNORM;   // R = sky, G = person (single semseg mask)
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	std::vector<uint8_t> zero(640 * 360 * 2, 0);
	D3D11_SUBRESOURCE_DATA init = { zero.data(), (UINT)1280, 0 };
	HRESULT hr = m_pDevice->CreateTexture2D(&desc, &init, &m_SkyMask);
	if (FAILED(hr)) return hr;
	hr = m_pDevice->CreateShaderResourceView(m_SkyMask, nullptr, &m_SkyMaskSRV);
	if (FAILED(hr)) return hr;

	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	hr = m_pDevice->CreateSamplerState(&sd, &m_pSamplerSky);
	if (FAILED(hr)) return hr;

	// CPU-readable twin for the mask auto-diagnostic (LogMaskProbe).
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	hr = m_pDevice->CreateTexture2D(&desc, nullptr, &m_MaskProbe);
	return hr;
}

void CDX11VideoProcessor::CreateSkinGateTexture()
{
	// 160x90 R8_UNORM — the quarter-res Poisson-relaxed person gate consumed on
	// gamma t4. CPU-written every person-cadence update; released + recreated on
	// device reset. Kept small: the field is smooth and bilinearly sampled, so
	// half-resolution of the old 320x180 grid is visually identical but ~4x
	// cheaper per solve iteration (kills per-frame stutter).
	if (!m_pDevice) return;
	m_SkinGate.Release();
	m_SkinGateSRV.Release();
	m_pSamplerGate.Release();

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = 160;
	desc.Height = 90;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	std::vector<uint8_t> zero(160 * 90, 0);
	D3D11_SUBRESOURCE_DATA init = { zero.data(), (UINT)160, 0 };
	if (FAILED(m_pDevice->CreateTexture2D(&desc, &init, &m_SkinGate))) return;
	if (FAILED(m_pDevice->CreateShaderResourceView(m_SkinGate, nullptr, &m_SkinGateSRV))) return;

	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	m_pDevice->CreateSamplerState(&sd, &m_pSamplerGate);
}

HRESULT CDX11VideoProcessor::CreateSkySegTextures()
{
	// 640x360 BGRA8 SDR-ized preview — higher fidelity than the 480x288 face
	// texture so building/sky boundaries survive the downscale for the model.
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = 640;
	desc.Height = 360;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	HRESULT hr = m_pDevice->CreateTexture2D(&desc, nullptr, &m_SegDownsample);
	if (FAILED(hr)) return hr;

	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	hr = m_pDevice->CreateTexture2D(&desc, nullptr, &m_SegStaging);
	if (FAILED(hr)) return hr;

	// Skin-metric grab pair: 640x360 BGRA downsampled copy of the graded output.
	desc = {};
	desc.Width = 640; desc.Height = 360; desc.MipLevels = 1; desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	hr = m_pDevice->CreateTexture2D(&desc, nullptr, &m_MetDown);
	if (FAILED(hr)) return hr;
	desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	hr = m_pDevice->CreateTexture2D(&desc, nullptr, &m_MetStaging);
	return hr;
}

void CDX11VideoProcessor::LogSkinMetric(ID3D11Texture2D* pRT, const CRect& rect)
{
	// Real-video skin-retention check: on a cadence, grab the GRADED output
	// (pRT content rect -> 640x360) and the SOURCE preview (m_SegStaging, the
	// SDR-ized PQ this frame) and log the source->output displacement in the skin
	// LUT's own space (gamma-2.2 luma + Cr/Cb), averaged over PERSON pixels and
	// the skin sub-set (person ∧ LUT>0.5). Small deltas = retained skin tone.
	if (!pRT || !m_MetDown || !m_MetStaging || !m_pDeviceContext || !m_pMLDebugFile) return;
	if (m_LastPersonMap.size() != 640 * 360) return;

	Tex2D_t pRTt = {}; pRTt.pTexture = pRT;
	D3D11_TEXTURE2D_DESC d{}; pRT->GetDesc(&d);
	pRTt.desc.Width = d.Width; pRTt.desc.Height = d.Height;
	HRESULT hr = TextureCopyRect(pRTt, m_MetDown, rect, CRect(0, 0, 640, 360), m_pPS_Simple.p, nullptr, 0, false);
	if (FAILED(hr)) return;
	m_pDeviceContext->CopyResource(m_MetStaging, m_MetDown);
	D3D11_MAPPED_SUBRESOURCE omr = {};
	if (FAILED(m_pDeviceContext->Map(m_MetStaging, 0, D3D11_MAP_READ, 0, &omr))) return;
	D3D11_MAPPED_SUBRESOURCE smr = {};
	if (FAILED(m_pDeviceContext->Map(m_SegStaging, 0, D3D11_MAP_READ, 0, &smr))) {
		m_pDeviceContext->Unmap(m_MetStaging, 0);
		return;
	}
	const uint8_t* outBgr = (const uint8_t*)omr.pData;
	const uint8_t* srcBgr = (const uint8_t*)smr.pData;
	const UINT op = omr.RowPitch, sp = smr.RowPitch;

	const float cr0 = m_fSkinLUTCR0, cr1 = m_fSkinLUTCR1, cb0 = m_fSkinLUTCB0, cb1 = m_fSkinLUTCB1;
	struct Acc { double y = 0, cr = 0, cb = 0; long long n = 0; } srcP, outP, srcS, outS;
	for (int y0 = 0; y0 < 360; y0 += 3) {
		for (int x0 = 0; x0 < 640; x0 += 3) {
			if (m_LastPersonMap[(size_t)y0 * 640 + x0] == 0) continue;
			auto rd = [](const uint8_t* p, size_t ox) {
				return (float)p[ox] / 255.0f;
			};
			auto ycb = [](float r, float g, float b, float& y, float& cr, float& cb) {
				y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
				cr = (r - y) / 1.5748f; cb = (b - y) / 1.8556f;
			};
			// source preview is gamma-2.2 SDR-ized (face_down); output may be PQ.
			float sr = rd(srcBgr, (size_t)y0 * sp + (size_t)x0 * 4 + 2);
			float sg = rd(srcBgr, (size_t)y0 * sp + (size_t)x0 * 4 + 1);
			float sb = rd(srcBgr, (size_t)y0 * sp + (size_t)x0 * 4 + 0);
			float orr = rd(outBgr, (size_t)y0 * op + (size_t)x0 * 4 + 2);
			float og = rd(outBgr, (size_t)y0 * op + (size_t)x0 * 4 + 1);
			float ob = rd(outBgr, (size_t)y0 * op + (size_t)x0 * 4 + 0);
			float sy, scr, scb, oy, ocr, ocb;
			ycb(sr, sg, sb, sy, scr, scb);
			ycb(orr, og, ob, oy, ocr, ocb);
			srcP.y += sy; srcP.cr += scr; srcP.cb += scb; srcP.n++;
			outP.y += oy; outP.cr += ocr; outP.cb += ocb;
			// skin selector: person ∧ skinLUT(source cr/cb) > 0.5 (bilinear, like the shader)
			const float u = (scr - cr0) / (cr1 - cr0);
			const float v = (scb - cb0) / (cb1 - cb0);
			float lutW = 0.0f;
			if (m_SkinLutN > 0 && u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
				const float fu = u * (m_SkinLutN - 1), fv = v * (m_SkinLutN - 1);
				const int iu = (int)fu, iv = (int)fv;
				const int iu1 = std::min(m_SkinLutN - 1, iu + 1), iv1 = std::min(m_SkinLutN - 1, iv + 1);
				const float wu = fu - iu, wv = fv - iv;
				const auto L = [&](int ci, int ri) {
					return (float)m_SkinLutCpu[(size_t)ri * m_SkinLutN + ci] / 255.0f;
				};
				lutW = L(iu, iv) * (1 - wu) * (1 - wv) + L(iu1, iv) * wu * (1 - wv)
					 + L(iu, iv1) * (1 - wu) * wv + L(iu1, iv1) * wu * wv;
			}
			if (lutW > 0.5f) {
				srcS.y += sy; srcS.cr += scr; srcS.cb += scb; srcS.n++;
				outS.y += oy; outS.cr += ocr; outS.cb += ocb;
			}
		}
	}
	m_pDeviceContext->Unmap(m_SegStaging, 0);
	m_pDeviceContext->Unmap(m_MetStaging, 0);

	// Log source->output deltas over person + skin sub-sets (gamma-2.2 Y/Cr/Cb).
	// (The stage-5 anchor is updated by UpdateSkinAnchor in the seg block, not here.)
	auto avg = [](const Acc& a) { return a.n ? (double)(a.y / a.n) : 0.0; };
	auto avgc = [](const Acc& a) { return a.n ? a.cr / a.n : 0.0; };
	auto avgq = [](const Acc& a) { return a.n ? a.cb / a.n : 0.0; };
	fwprintf(m_pMLDebugFile,
		L"SKINMET frame=%llu nP=%lld nS=%lld  dYp=%.4f dCrp=%.4f dCbp=%.4f  dYs=%.4f dCrs=%.4f dCbs=%.4f anch=%.4f\n",
		(unsigned long long)m_MLDebugFrame, srcP.n, srcS.n,
		(double)fabs(avg(outP) - avg(srcP)), (double)fabs(avgc(outP) - avgc(srcP)), (double)fabs(avgq(outP) - avgq(srcP)),
		(double)fabs(avg(outS) - avg(srcS)), (double)fabs(avgc(outS) - avgc(srcS)), (double)fabs(avgq(outS) - avgq(srcS)),
		(double)(m_skinAnchorValid ? m_skinAnchorSmooth : -1.0));
	fflush(m_pMLDebugFile);
}

void CDX11VideoProcessor::UpdateSkinAnchor(const uint8_t* bgra, UINT stride)
{
	// Skin-tone anchor: IIR of the skin-set mean SOURCE luma (gamma-2.2, in the
	// same SDR-ized space the shader samples via t3) — runs in the seg block so
	// it's live in production (no frame log needed). MLSkinAnchor for stage 5.
	if (!bgra || m_SkinLutN <= 0) return;
	const std::vector<uint8_t>& pm = m_PersonMaskSm.empty() ? m_LastPersonMap : m_PersonMaskSm;
	if (pm.size() != 640 * 360) return;
	const int N = m_SkinLutN;
	const float cr0 = m_fSkinLUTCR0, cr1 = m_fSkinLUTCR1, cb0 = m_fSkinLUTCB0, cb1 = m_fSkinLUTCB1;
	long long n = 0; double sy = 0;
	for (int y = 0; y < 360; y += 3)
		for (int x = 0; x < 640; x += 3) {
			if (pm[(size_t)y * 640 + x] == 0) continue;
			const uint8_t* p = bgra + (size_t)y * stride + (size_t)x * 4;
			const float r = p[2] / 255.0f, g = p[1] / 255.0f, b = p[0] / 255.0f;
			const float Y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
			const float cr = (r - Y) / 1.5748f, cb = (b - Y) / 1.8556f;
			const float u = (cr - cr0) / (cr1 - cr0), v = (cb - cb0) / (cb1 - cb0);
			if (u < 0 || u > 1 || v < 0 || v > 1) continue;
			const float fu = u * (N - 1), fv = v * (N - 1);
			const int iu = (int)fu, iv = (int)fv;
			const int iu1 = std::min(N - 1, iu + 1), iv1 = std::min(N - 1, iv + 1);
			const float wu = fu - iu, wv = fv - iv;
			const auto L = [&](int ci, int ri) { return (float)m_SkinLutCpu[(size_t)ri * N + ci] / 255.0f; };
			const float w = L(iu, iv) * (1 - wu) * (1 - wv) + L(iu1, iv) * wu * (1 - wv)
				+ L(iu, iv1) * (1 - wu) * wv + L(iu1, iv1) * wu * wv;
			if (w > 0.5f) { sy += Y; n++; }
		}
	if (n > 8) {
		const double m = sy / (double)n;
		if (!m_skinAnchorValid) { m_skinAnchorSmooth = m; m_skinAnchorValid = true; }
		else                     { m_skinAnchorSmooth = 0.88 * m_skinAnchorSmooth + 0.12 * m; }
	}
}

void CDX11VideoProcessor::UpdateSkinGate(const uint8_t* bgra, UINT stride)
{
	// Edge-pinned Poisson gate for the gamma skin-tone leg (t4) — the boundary is
	// attached to the CURRENT-FRAME source luma, not to the (late under motion)
	// semantic mask. This fixes both residual artifacts at once:
	//   - under-coverage shimmer: when two hugging people separate, the mask edge
	//     lands INSIDE the face (mask = late). No mask erode/smooth can fix that;
	//     the source luma edge of the CURRENT frame is always right — so the tail
	//     is held by anisotropic diffusion that BLOCKS on strong luma contours:
	//     u stays ≈1 across the face up to the real silhouette, then falls. The
	//     edge map is per-frame → no temporal wobble (no shimmer).
	//   - rim glow on flat background: the plateau is eroded deep inside, the
	//     zero-pin sits beyond a DILATED mask (+3 texels, so it is safely outside
	//     the true boundary even when the mask lags), and conductivity is ~0
	//     across strong edges → the whole brightness turn happens exactly at the
	//     person's real contour, not on the background.
	//
	// Pins: u=1 where eroded-mask solid (er≥0.80); u=0 where dilated-mask clear
	// (gDil≤0.05). Annulus between: ANISOTROPIC Jacobi, weights c=1/(1+(E/κ)²)
	// where E = smoothed Sobel gradient of the source luma at gate resolution.
	// Runs every frame for person (bgra = the 640x360 SDR-ized seg preview).
	if (!m_SkinGate || !bgra) return;
	const std::vector<uint8_t>& pm = m_LastPersonMap.empty() ? m_PersonMaskSm : m_LastPersonMap;
	if (pm.size() != 640 * 360) return;

	constexpr int GW = 160, GH = 90;   // quarter-res gate grid (4x fewer cells than
									// 320x180 — the gate is a smooth field sampled
									// bilinearly, so this is visually identical but
									// ~4x cheaper per Jacobi iteration)
	constexpr float lw_r = 0.2126f, lw_g = 0.7152f, lw_b = 0.0722f;
	static UINT64 s_gateCalls = 0;      // timing cadence
	const bool bTiming = (++s_gateCalls % 3 == 0);   // time every 3rd call
	const auto gqpc = []() -> double {
		static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
		LARGE_INTEGER c; QueryPerformanceCounter(&c);
		return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
	};
	double tkA = bTiming ? gqpc() : 0.0;   // frame start
	double tkB = 0, tkC = 0;

	// 1a) Source luma at gate resolution (4x4 average of the BGRA preview).
	std::vector<float> lum(GW * GH, 0.0f);
	for (int y = 0; y < 360; y += 4) {
		const int gy = y / 4;
		const uint8_t* p = bgra + (size_t)y * stride;
		for (int x = 0; x < 640; x += 4) {
			float s = 0; const int gx = x / 4;
			for (int dy = 0; dy < 4; dy++) {
				const uint8_t* q = p + (size_t)dy * stride;
				for (int dx = 0; dx < 4; dx++) {
					const uint8_t* c = q + (size_t)(x + dx) * 4;
					s += lw_r * (c[2] / 255.0f) + lw_g * (c[1] / 255.0f) + lw_b * (c[0] / 255.0f);
				}
			}
			lum[(size_t)gy * GW + gx] = s * (1.0f / 16.0f);
		}
	}
	// 1b) Smoothed Sobel gradient magnitude -> edge conductivity.
	std::vector<float> E(GW * GH, 0.0f);
	for (int y = 1; y < GH - 1; y++)
		for (int x = 1; x < GW - 1; x++) {
			const size_t o = (size_t)y * GW + x;
			const float gx = (lum[o - GW - 1] + 2 * lum[o - 1] + lum[o + GW - 1])
			               - (lum[o - GW + 1] + 2 * lum[o + 1] + lum[o + GW + 1]);
			const float gy = (lum[o - GW - 1] + 2 * lum[o - GW] + lum[o - GW + 1])
			               - (lum[o + GW - 1] + 2 * lum[o + GW] + lum[o + GW + 1]);
			E[o] = sqrtf(gx * gx + gy * gy);
		}
	{                                           // 3x3 box smooth of E to de-speckle
		std::vector<float> Es = E;
		for (int y = 1; y < GH - 1; y++)
			for (int x = 1; x < GW - 1; x++) {
				const size_t o = (size_t)y * GW + x;
				E[o] = (Es[o - GW - 1] + Es[o - GW] + Es[o - GW + 1]
				      + Es[o - 1]      + Es[o]      + Es[o + 1]
				      + Es[o + GW - 1] + Es[o + GW] + Es[o + GW + 1]) * (1.0f / 9.0f);
			}
	}
	if (bTiming) tkB = gqpc();    // ← luma/edge phase end

	// 1) Half-res gate seed = 4x4 max of the RAW per-frame person mask.
	std::vector<float> g(GW * GH, 0.0f);
	std::vector<float> instAmpAt(GW * GH, 1.0f);  // plateau amplitude per cell (skin-presence gate)
	bool anyPerson = false;
	// Per-instance skin fraction σ_j = (person ∩ skin-LUT) / (person) per label,
	// sampled at 640x360 (stride 3 to bound cost). IIR'd per label (memory) and
	// shaped through smoothstep(0.03, 0.12, σ) → a per-instance amplitude A_j:
	//   person A (face, σ≈1)  → A≈1  → full tone preservation
	//   person B (waist, σ≈0) → A≈0  → gate off for that person only
	// NO global pooling — each person self-gates, so A can't be penalized by B.
	{
		const int N = m_SkinLutN;
		const float cr0 = m_fSkinLUTCR0, cr1 = m_fSkinLUTCR1, cb0 = m_fSkinLUTCB0, cb1 = m_fSkinLUTCB1;
		long long pTot[9] = {}, sTot[9] = {};    // [0..8], label 0 unused
		for (int y = 0; y < 360; y += 3)
			for (int x = 0; x < 640; x += 3) {
				const size_t o = (size_t)y * 640 + x;
				const uint8_t lab = (m_LastInstMap.size() == 640 * 360) ? m_LastInstMap[o] : 1;
				if (lab < 1 || lab > 8) continue;      // background
				if (pm[o] == 0) continue;              // inside its instance?
				pTot[lab]++;
				const uint8_t* p = bgra + (size_t)y * stride + (size_t)x * 4;
				const float r = p[2] / 255.0f, gr = p[1] / 255.0f, b = p[0] / 255.0f;
				const float Y = 0.2126f * r + 0.7152f * gr + 0.0722f * b;
				const float cr = (r - Y) / 1.5748f, cb = (b - Y) / 1.8556f;
				const float uu = (cr - cr0) / (cr1 - cr0), vv = (cb - cb0) / (cb1 - cb0);
				float lutW = 0.0f;
				if (uu >= 0.0f && uu <= 1.0f && vv >= 0.0f && vv <= 1.0f && N > 0) {
					const float fu = uu * (N - 1), fv = vv * (N - 1);
					const int iu = (int)fu, iv = (int)fv, iu1 = std::min(N - 1, iu + 1), iv1 = std::min(N - 1, iv + 1);
					const float wu = fu - iu, wv = fv - iv;
					const auto L = [&](int ci, int ri) { return (float)m_SkinLutCpu[(size_t)ri * N + ci] / 255.0f; };
					lutW = L(iu, iv) * (1 - wu) * (1 - wv) + L(iu1, iv) * wu * (1 - wv)
						 + L(iu, iv1) * (1 - wu) * wv + L(iu1, iv1) * wu * wv;
				}
				if (lutW > 0.5f) sTot[lab]++;
			}
		for (int j = 1; j <= 8; j++) {
			const float sig = (pTot[j] > 0) ? (float)sTot[j] / (float)pTot[j] : 0.0f;
			// IIR per label; on cut/no-person the seg block resets m_instSmInit
			// so re-emerging people start clean.
			if (!m_instSmInit) m_instSkinSm[j] = sig;
			else               m_instSkinSm[j] = 0.85f * m_instSkinSm[j] + 0.15f * sig;
			// shaped amplitude: needs a decisive skin share to engage
			const float t = (m_instSkinSm[j] - 0.03f) / (0.12f - 0.03f);
			m_instAmp[j] = t <= 0.0f ? 0.0f : (t >= 1.0f ? 1.0f : t * t * (3.0f - 2.0f * t));
		}
		m_instSmInit = true;
	}
	if (bTiming) tkC = gqpc();    // ← σ/inst phase end
	for (int y = 0; y < 360; y += 4) {
		for (int x = 0; x < 640; x += 4) {
			float mx = 0.0f;
			uint8_t labBest = 0;
			for (int dy = 0; dy < 4 && (y + dy) < 360; dy++)
				for (int dx = 0; dx < 4 && (x + dx) < 640; dx++) {
					const size_t o = (size_t)(y + dy) * 640 + (x + dx);
					const float v = pm[o] / 255.0f;
					if (v > mx) { mx = v; labBest = (m_LastInstMap.size() == 640 * 360) ? m_LastInstMap[o] : 1; }
				}
			g[(size_t)(y / 4) * GW + (x / 4)] = mx;
			instAmpAt[(size_t)(y / 4) * GW + (x / 4)] = m_instAmp[labBest > 0 && labBest <= 8 ? labBest : 1];
			if (mx > 0.01f) anyPerson = true;
		}
	}

	// 1z) No person on screen (empty scene / cut-to-no-one): skip the solve, leave
	//     a zero gate — nothing to blend and no stutter spent solving it.
	if (!anyPerson) {
		std::vector<uint8_t> out(GW * GH, 0);
		D3D11_BOX box = { 0, 0, 0, GW, GH, 1 };
		m_pDeviceContext->UpdateSubresource(m_SkinGate, 0, &box, out.data(), GW, 0);
		return;
	}

	// 2) Eroded plateau pin: 3x3-min x ER — u=1 lives DEEP INSIDE the silhouette, so
	//    the brightness tail spends itself on the face interior. ER=3 at 160x90
	//    (= 12 src px, matching the 320x180 ER=5 footprint of ~10 px, so the visual
	//    behavior is preserved at the coarser grid).
	std::vector<float> er = g, er2 = g;
	constexpr int ER = 3;
	for (int it = 0; it < ER; it++) {
		for (int y = 1; y < GH - 1; y++)
			for (int x = 1; x < GW - 1; x++) {
				const size_t o = (size_t)y * GW + x;
				er2[o] = std::min(std::min(er[o - GW], er[o + GW]),
				                  std::min(std::min(er[o - 1], er[o + 1]), er[o]));
			}
		er.swap(er2);
	}
	// 2b) Dilated zero-pin: 3x3-max x DIL — u=0 sits BEYOND the true boundary, so
	//     under motion the gate covers the whole face instead of the stale mask.
	std::vector<float> gDil = g, gD2 = g;
	constexpr int DIL = 2;
	for (int it = 0; it < DIL; it++) {
		for (int y = 1; y < GH - 1; y++)
			for (int x = 1; x < GW - 1; x++) {
				const size_t o = (size_t)y * GW + x;
				gD2[o] = std::max(std::max(gDil[o - GW], gDil[o + GW]),
				                  std::max(std::max(gDil[o - 1], gDil[o + 1]), gDil[o]));
			}
		gDil.swap(gD2);
	}

	// 2c) Edge-conductivity at the 4 neighbor faces (c = 1/(1+(E/κ)²); blocked
	//     across strong luma contours → the tail CLAMPS to the person boundary).
	//     Cells are 4 src px wide now (was 2), so a soft edge's Sobel reading is
	//     ~2x — κ scaled 0.45→0.90 to keep the SAME physical clamping (the "good"
	//     behavior verified at 320x180) at the coarser grid. Clamps tightest where
	//     the person's real contour is; softer contours in shadow still block.
	const float kappa = 0.90f;
	constexpr int cmin = 6;             // low-conductivity clamp (×1/6) — prevents
	const float cLow = 1.0f / (float)cmin;   // numerically dead neighbors
	auto cond = [&](size_t a, size_t b) {
		float e = std::max(E[a], E[b]);
		float c = 1.0f / (1.0f + (e / kappa) * (e / kappa));
		return std::max(c, cLow);
	};

	// 3) Anisotropic Jacobi solve (100 iters at 160x90 = the same physical tail reach
//    as 200 at 320x180 — each coarse cell diffuses 2x the distance, so half the
//    iterations dissolve the same residual mask-edge pattern). Pins read the FIXED
//    seeds (er/gDil at the cell), never the live field → interior magnitude stays
//    raw-mask-driven.
	std::vector<float> u = g;
	const int TW = GW + 2, TH = GH + 2;
	std::vector<float> pad(TW * TH, 0.0f), pad2(TW * TH, 0.0f);
	for (int i = 0; i < GW * GH; i++) pad[(size_t)(i / GW + 1) * TW + (i % GW) + 1] = u[i];
	for (int it = 0; it < 100; it++) {
		pad2 = pad;
		for (int y = 1; y <= GH; y++) {
			for (int x = 1; x <= GW; x++) {
				const size_t o = (size_t)y * TW + x;
				const size_t gix = (size_t)(y - 1) * GW + (x - 1);
				// Interior pin = per-instance skin-presence amplitude (person A face
				// ~1, person B waist ~0 → that plateau collapses) so the harmonic tail
				// falls from A_j to 0 exactly as it did from 1.0 — indistinguishable
				// for full-face people, fully off for clothes-only people.
				if (er[gix] >= 0.80f) { pad2[o] = instAmpAt[gix]; continue; }   // solid interior
				if (gDil[gix] <= 0.05f) { pad2[o] = 0.0f; continue; } // clear exterior
				const float cn = cond(gix, gix - GW), cs = cond(gix, gix + GW);
				const float ce = cond(gix, gix + 1),  cw = cond(gix, gix - 1);
				const float den = cn + cs + ce + cw;
				pad2[o] = (cn * pad[o - TW] + cs * pad[o + TW]
				         + ce * pad[o + 1] + cw * pad[o - 1]) / den;
			}
		}
		pad.swap(pad2);
	}
	for (int i = 0; i < GW * GH; i++) u[i] = pad[(size_t)(i / GW + 1) * TW + (i % GW) + 1];

	// 4) Upload R8 to m_SkinGate (gamma t4): u in [0,1].
	std::vector<uint8_t> out(GW * GH);
	for (int i = 0; i < GW * GH; i++) {
		float uv = u[i] * 255.0f;
		out[i] = (uint8_t)(uv < 0.0f ? 0 : (uv > 255.0f ? 255 : (unsigned)uv));
	}
	D3D11_BOX box = { 0, 0, 0, GW, GH, 1 };
	m_pDeviceContext->UpdateSubresource(m_SkinGate, 0, &box, out.data(), GW, 0);

	if (m_bMLDebug && m_pMLDebugFile && m_MLDebugFrame % 30 == 0) {
		float sum = 0; long long cnt = 0, eCnt = 0; float eSum = 0;
		for (int i = 0; i < GW * GH; i++) { sum += u[i]; if (u[i] > 0.01f) cnt++; eSum += E[i]; if (E[i] > kappa) eCnt++; }
		// per-instance amplitudes: A≈1 (face) / A≈0 (clothes-only) is the gate's
		// decision — person B cannot penalize person A when both are on screen.
		fwprintf(m_pMLDebugFile, L"GATE u_mean=%.4f u_posFrac=%.4f e_mean=%.4f e_strFrac=%.4f  amp1=%.3f amp2=%.3f amp3=%.3f sig1=%.3f sig2=%.3f",
			(float)(sum / (GW * GH)), (float)((double)cnt / (GW * GH)),
			(float)(eSum / (GW * GH)), (float)((double)eCnt / (GW * GH)),
			(double)m_instAmp[1], (double)m_instAmp[2], (double)m_instAmp[3],
			(double)m_instSkinSm[1], (double)m_instSkinSm[2]);
		if (bTiming && tkC > 0) {   // per-phase ms split: luma+edge / σ+inst / solve+upload
			const double e2 = gqpc();
			fwprintf(m_pMLDebugFile, L"  [lum=%.2fms σ=%.2fms solve=%.2fms tot≈%.2fms]",
				tkB - tkA, tkC - tkB, e2 - tkC, e2 - tkA);
		}
		fwprintf(m_pMLDebugFile, L"\n");
		fflush(m_pMLDebugFile);
	}
}

// Diagnostic: dump the staged BGRA seg preview to an 8-bit BMP next to the log
// (frame-log gated) so mask-vs-screen alignment can be eyeballed (esp. non-16:9
// windows / DV-P5 where the ring layout can diverge from the nominal rect).
static void DumpSegPreviewBmp(const uint8_t* bgra, int w, int h, int stride, const wchar_t* path)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return;
    const int rowSize = w * 3 + ((4 - (w * 3) % 4) % 4);
    const int dataSize = rowSize * h;
    BITMAPFILEHEADER bf = {}; bf.bfType = 0x4D42; bf.bfSize = 54 + dataSize; bf.bfOffBits = 54;
    BITMAPINFOHEADER bi = {}; bi.biSize = 40; bi.biWidth = w; bi.biHeight = h;
    bi.biPlanes = 1; bi.biBitCount = 24; bi.biSizeImage = dataSize;
    fwrite(&bf, 1, 14, f); fwrite(&bi, 1, 40, f);
    std::vector<uint8_t> row(rowSize, 0);
    for (int y = h - 1; y >= 0; y--) {            // BMP is bottom-up
        const uint8_t* src = bgra + (size_t)y * stride;
        for (int x = 0; x < w; x++) {              // BGRA -> BGR
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        fwrite(row.data(), 1, rowSize, f);
    }
    fclose(f);
}

void CDX11VideoProcessor::LogMaskProbe()
{
	if (!m_MaskProbe || !m_SkyMask || !m_pDeviceContext || !m_pMLDebugFile) return;
	m_pDeviceContext->CopyResource(m_MaskProbe, m_SkyMask);
	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (FAILED(m_pDeviceContext->Map(m_MaskProbe, 0, D3D11_MAP_READ, 0, &mr))) return;
	const uint8_t* p = (const uint8_t*)mr.pData;
	const uint32_t pitch = mr.RowPitch;
	long long nR = 0, nG = 0, n = 0;
	for (int y = 0; y < 360; y += 6)
		for (int x = 0; x < 640; x += 6) {
			const uint8_t* t = p + (size_t)y * pitch + (size_t)x * 2;
			if (t[0] > 128) nR++;
			if (t[1] > 128) nG++;
			n++;
		}
	const uint8_t* cp = p + 180 * (size_t)pitch + 320 * 2;
	fwprintf(m_pMLDebugFile,
		L"MASKPROBE cpuP=%.4f gpuR=%.3f gpuG=%.3f centerRG=(%u,%u) dbg=%.0f str=%.3f\n",
		(double)m_PersonFrac, (double)((double)nR / n), (double)((double)nG / n),
		(unsigned)cp[0], (unsigned)cp[1],
		(double)m_lastMLParams.MLSkinPersonDebugVis, (double)m_lastMLParams.MLSkinPersonStrength);
	fflush(m_pMLDebugFile);
	m_pDeviceContext->Unmap(m_MaskProbe, 0);
}

HRESULT CDX11VideoProcessor::LoadSkinLUT()
{
	LPVOID data = nullptr;
	DWORD size = 0;
	HRESULT hr = GetDataFromResource(data, size, IDF_SKIN_LUT);
	if (FAILED(hr) || !data || size < 20) {
		return E_FAIL;
	}

	// header: 4x float cr0,cr1,cb0,cb1 + 2x u16 N,N, then N*N u8 P(skin)
	const float* f = (const float*)data;
	const uint16_t* n = (const uint16_t*)((const BYTE*)data + 16);
	const UINT N = n[0];
	if (N == 0 || n[1] != N || (size_t)(size - 20) < (size_t)N * N) {
		return E_FAIL;
	}

	m_fSkinLUTCR0 = f[0]; m_fSkinLUTCR1 = f[1];
	m_fSkinLUTCB0 = f[2]; m_fSkinLUTCB1 = f[3];

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = N;
	desc.Height = N;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA init = { (const BYTE*)data + 20, (UINT)N, 0 };
	hr = m_pDevice->CreateTexture2D(&desc, &init, &m_SkinLUT);
	if (FAILED(hr)) {
		return hr;
	}
	// CPU rast dense cache for the skin-metric (spatial-delta) logging.
	m_SkinLutN = (int)N;
	m_SkinLutCpu.assign((const BYTE*)data + 20, (const BYTE*)data + 20 + (size_t)N * N);
	hr = m_pDevice->CreateShaderResourceView(m_SkinLUT, nullptr, &m_SkinLUTSRV);
	if (FAILED(hr)) {
		return hr;
	}

	if (!m_pSamplerSkin) {
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		hr = m_pDevice->CreateSamplerState(&sd, &m_pSamplerSkin);
	}

	DLog(L"CDX11VideoProcessor::LoadSkinLUT() {0}x{0} cr[{1:.4f},{2:.4f}] cb[{3:.4f},{4:.4f}]",
		N, m_fSkinLUTCR0, m_fSkinLUTCR1, m_fSkinLUTCB0, m_fSkinLUTCB1);
	return hr;
}

// DELIVERED-frame instrument ---------------------------------------------------
//
// The Zion models consume SOURCE-PQ statistics and act on the rendered frame
// without ever seeing it (the extractor reads the pre-correction texture, the
// grade passes read the post-correction one). So a change to the tone curve, to
// refWhite or to the calibration LUT silently invalidates every prediction the
// models make, and the only thing that notices is the viewer. These two caches
// are the missing measurement: what the tone curve produced, and what the grade
// produced, in the delivered frame's own encoded codes (0..255, comparable to a
// screen grab).
//
// Both downsamples run at DRAW time into caches this class owns, so neither
// depends on a post-scale chain texture outliving the frame. Both are
// debug-gated: with the instrument off this is exactly zero extra work.
void CDX11VideoProcessor::DownsampleDelivered(const Tex2D_t& src, ID3D11Texture2D* dst)
{
	if (!m_pPSMLDownsample || !dst || !src.pShaderResource) {
		return;
	}
	const CRect fullRect(0, 0, ML_DOWNSAMPLE_W, ML_DOWNSAMPLE_H);
	const CRect texRect(0, 0, src.desc.Width, src.desc.Height);
	TextureCopyRect(src, dst, texRect, fullRect, m_pPSMLDownsample, nullptr, 0, false);
}

void CDX11VideoProcessor::ReadDeliveredStats(unsigned long long frame)
{
	if (!m_bMLDebug || !m_pMLDebugFile || !m_MLStaging) {
		return;
	}

	// The content RECT in the delivered frame's own space, in readback pixels.
	//
	// Two mattes stack here and both have to come out:
	//   1. the PILLARBOX the player adds — m_videoRect is the destination rect, so
	//      everything outside it is black. On an ultrawide display this is the big
	//      one: 3840x2160 fitted into a 3440x1440 window is 2560x1440, i.e. 25.6% of
	//      the frame is black COLUMNS, which no row crop can touch.
	//   2. the film's own LETTERBOX, which lives INSIDE m_videoRect. The bar detector
	//      finds it on the source readback (rows of ML_DOWNSAMPLE_H), so it has to be
	//      mapped through the video rect before it means anything in window space.
	// Cropping only (2) left the pillarbox in; cropping only (1) threw the film's
	// matte away and kept its rows. Both produced a crushFrac that looked like
	// content and was geometry.
	m_DeliveredTopRow = 0;
	m_DeliveredBotRow = ML_DOWNSAMPLE_H - 1;
	m_DeliveredLeftCol = 0;
	m_DeliveredRightCol = ML_DOWNSAMPLE_W - 1;
	{
		const int winW = (int)m_windowRect.Width();
		const int winH = (int)m_windowRect.Height();
		const int vidW = (int)m_videoRect.Width();
		const int vidH = (int)m_videoRect.Height();
		if (winW > 0 && winH > 0 && vidW > 0 && vidH > 0) {
			// Pillarbox: the video rect's own horizontal extent.
			m_DeliveredLeftCol  = (int)floorf((float)m_videoRect.left / winW * ML_DOWNSAMPLE_W);
			m_DeliveredRightCol = (int)ceilf((float)(m_videoRect.left + vidW) / winW * ML_DOWNSAMPLE_W) - 1;

			// Letterbox inside the video rect, mapped from source-readback rows.
			// contentBottom < 0 means the detector has not produced a band yet —
			// fall back to the full video rect rather than a zero-height crop.
			const bool haveBars = (m_MLBarDetected.contentBottom >= m_MLBarDetected.contentTop);
			const float barTop = haveBars ? (float)m_MLBarDetected.contentTop / ML_DOWNSAMPLE_H : 0.0f;
			const float barBot = haveBars ? (float)(m_MLBarDetected.contentBottom + 1) / ML_DOWNSAMPLE_H : 1.0f;
			m_DeliveredTopRow = (int)floorf(((float)m_videoRect.top + barTop * vidH) / winH * ML_DOWNSAMPLE_H);
			m_DeliveredBotRow = (int)ceilf (((float)m_videoRect.top + barBot * vidH) / winH * ML_DOWNSAMPLE_H) - 1;
		}
	}

	// One Map per boundary, sequentially, into the shared staging. Scene rate only:
	// a read-back is the one genuinely expensive thing here, and a scene is the
	// unit the statistics mean anything at.
	auto readOne = [&](ID3D11Texture2D* cache, bool have, MLRenderedLumaStats& out) {
		out = MLRenderedLumaStats();
		if (!cache || !have) {
			return;
		}
		m_pDeviceContext->CopyResource(m_MLStaging, cache);
		D3D11_MAPPED_SUBRESOURCE mr = {};
		if (FAILED(m_pDeviceContext->Map(m_MLStaging, 0, D3D11_MAP_READ, 0, &mr))) {
			return;
		}
		// Content rows, mapped into the space THIS cache was captured in.
		//
		// Not m_MLBarDetected: those rows are measured on the convert output, which
		// is VIDEO-sized, while these caches come off the post-scale chain, which is
		// WINDOW-sized. Reusing them crops the wrong rows — a 2.39:1 film has the same
		// bar FRACTION in both spaces, so the result looks plausible and is still
		// wrong, which is why crush sat at a constant 0.258 on every scene no matter
		// what was playing. Map the content rect through m_videoRect/m_windowRect,
		// exactly as the bar cbuffer's uBarV0/uBarSpan do.
		BuildRenderedLumaStats((const uint16_t*)mr.pData,
		                       (int)(mr.RowPitch / sizeof(uint16_t)), out,
		                       m_DeliveredTopRow, m_DeliveredBotRow,
		                       m_DeliveredLeftCol, m_DeliveredRightCol);
		m_pDeviceContext->Unmap(m_MLStaging, 0);
	};
	readOne(m_MLDeliveredCurveOut.p,  m_bDeliveredA, m_DeliveredCurveOut);
	readOne(m_MLDeliveredPostGrade.p, m_bDeliveredB, m_DeliveredPostGrade);
	m_bDeliveredA = m_bDeliveredB = false;

	auto fmt = [](const MLRenderedLumaStats& s) -> std::wstring {
		if (!s.valid) {
			return L"n/a";
		}
		wchar_t b[192];
		swprintf_s(b, L"p01=%.0f p05=%.0f p25=%.0f p50=%.0f p75=%.0f p95=%.0f p99=%.0f mean=%.1f crush=%.3f shMean=%.0f shN=%d",
		           (double)s.p01, (double)s.p05, (double)s.p25, (double)s.p50,
		           (double)s.p75, (double)s.p95, (double)s.p99, (double)s.mean,
		           (double)s.crushFrac, (double)s.shadowMean, s.shadowCount);
		return b;
	};
	// crop= is printed so every number on the line is auditable: if the band is wrong
	// (or the source's own bars differ from the window's), that shows here instead of
	// silently becoming "shadow crush".
	fwprintf(m_pMLDebugFile,
		L"DELIVERED frame=%llu crop=[%d..%d]x[%d..%d] of %dx%d win=%dx%d vid=%dx%d (%.1f%% of frame) | curveOut: %s | postGrade: %s\n",
		frame, m_DeliveredTopRow, m_DeliveredBotRow, m_DeliveredLeftCol, m_DeliveredRightCol,
		ML_DOWNSAMPLE_H, ML_DOWNSAMPLE_W,
		(int)m_windowRect.Width(), (int)m_windowRect.Height(),
		(int)m_videoRect.Width(), (int)m_videoRect.Height(),
		100.0 * (m_DeliveredBotRow - m_DeliveredTopRow + 1) * (m_DeliveredRightCol - m_DeliveredLeftCol + 1)
		      / (ML_DOWNSAMPLE_H * ML_DOWNSAMPLE_W),
		fmt(m_DeliveredCurveOut).c_str(), fmt(m_DeliveredPostGrade).c_str());
	fflush(m_pMLDebugFile);
}

void CDX11VideoProcessor::CreateMLTextures()
{
	if (m_MLDownsample || !m_pDevice) {
		return;
	}

	D3D11_TEXTURE2D_DESC cache = {};
	cache.Width = ML_DOWNSAMPLE_W;
	cache.Height = ML_DOWNSAMPLE_H;
	cache.MipLevels = 1;
	cache.ArraySize = 1;
	cache.Format = DXGI_FORMAT_R16_UNORM;
	cache.SampleDesc.Count = 1;
	cache.Usage = D3D11_USAGE_DEFAULT;
	cache.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	HRESULT hr = m_pDevice->CreateTexture2D(&cache, nullptr, &m_MLDownsample);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::CreateMLTextures() failed to create the luma cache: {}", hr);
		return;
	}

	D3D11_TEXTURE2D_DESC staging = cache;
	staging.Usage = D3D11_USAGE_STAGING;
	staging.BindFlags = 0;
	staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	hr = m_pDevice->CreateTexture2D(&staging, nullptr, &m_MLStaging);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::CreateMLTextures() failed to create the staging texture: {}", hr);
		m_MLDownsample.Release();
	}

	// DELIVERED-frame instrument: two more caches of the SAME shape. They share
	// m_MLStaging at read-back time (one Map each, scene rate, sequential — the same
	// thread owns both), so this costs two textures and no extra staging. A failed
	// create just disables that boundary's measurement; the log says so.
	hr = m_pDevice->CreateTexture2D(&cache, nullptr, &m_MLDeliveredCurveOut);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::CreateMLTextures(): delivered cache A unavailable ({})", hr);
		m_MLDeliveredCurveOut.Release();
	}
	hr = m_pDevice->CreateTexture2D(&cache, nullptr, &m_MLDeliveredPostGrade);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::CreateMLTextures(): delivered cache B unavailable ({})", hr);
		m_MLDeliveredPostGrade.Release();
	}

	// Face detection: 480x288 BGRA8 downsample + staging
	D3D11_TEXTURE2D_DESC faceDesc = {};
	faceDesc.Width = 480;
	faceDesc.Height = 288;
	faceDesc.MipLevels = 1;
	faceDesc.ArraySize = 1;
	faceDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	faceDesc.SampleDesc.Count = 1;
	faceDesc.Usage = D3D11_USAGE_DEFAULT;
	faceDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
	hr = m_pDevice->CreateTexture2D(&faceDesc, nullptr, &m_FaceDownsample);
	if (SUCCEEDED(hr)) {
		D3D11_TEXTURE2D_DESC faceStg = faceDesc;
		faceStg.Usage = D3D11_USAGE_STAGING;
		faceStg.BindFlags = 0;
		faceStg.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		hr = m_pDevice->CreateTexture2D(&faceStg, nullptr, &m_FaceStaging);
		if (FAILED(hr)) {
			m_FaceDownsample.Release();
		}
	}
}

// Anchor used by GetModuleHandleExW to resolve this DLL's own module handle
// (model files ship next to the filter, like the install cmds).
static void MLModulePathAnchor() {}

std::wstring CDX11VideoProcessor::GetMLModuleDir()
{
	wchar_t filename[MAX_PATH] = {};
	HMODULE hModule = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&MLModulePathAnchor, &hModule) && hModule
		&& GetModuleFileNameW(hModule, filename, MAX_PATH) != 0 && filename[0]) {
		wchar_t* p = wcsrchr(filename, L'\\');
		if (p) {
			*p = 0;
		}
		return filename;
	}
	return L".";
}

std::wstring CDX11VideoProcessor::GetMLModelPath(const wchar_t* fileName)
{
	return GetMLModuleDir() + L"\\" + fileName;
}

void CDX11VideoProcessor::InitMLDebug()
{
	// The log file is opened unconditionally in the constructor.
	// This method only writes the per-frame CSV header when debug is toggled on.
	if (m_bMLDebug && m_pMLDebugFile && m_MLDebugFrame == 0) {
		fwprintf(m_pMLDebugFile,
			L"frame,time_100ns,active,mode,gamma,shadow,highlight,"
			L"ispq,swapcs,swfmt,steps,tmactive,winW,winH,srcW,srcH,vidL,vidT,vidR,vidB,"
			L"mlmin,mlmax,mlavg,clipfrac,edgefrac,logCOPY,"
			L"featReady,stale,convSeen,texConv,feat0,feat1,pred,dehaze,rolloffCtl,"
			// Appended 2026-09-20. The swap chain cannot identify the pipeline —
			// it is G22P709 in BOTH "tone mapped" and "not tone mapped" on an SDR
			// display — so a capture with the two could not be told apart, and the
			// comparison that mattered was unreadable. `correction` names the
			// converter actually selected and `mlIsPQ` is the domain the Zion grade
			// was TOLD its input is; those two must agree.
			L"convertSdr,correction,sdrNits,mlIsPQ,"
			// Which CURVE the converter is applying, and whether the experimental
			// mapper is armed. Added with the four above for the same reason: without
			// them a capture cannot be attributed to a setting, and "the row did
			// nothing" cannot be told apart from "the row was never read".
			L"toneCurve,expTm,toneLutActive\n");
		fwprintf(m_pMLDebugFile, L"# debug on (ML enabled=%d, copy=%d)\n", (m_bML ? 1 : 0), (m_bMLDebugCopy ? 1 : 0));
		fflush(m_pMLDebugFile);
	}
}

void CDX11VideoProcessor::LogMLFrame()
{
	if (!m_bMLDebug || !m_pMLDebugFile) {
		return;
	}
	m_MLDebugFrame++;
	m_MLDebugFeatReady = m_bMLFeaturesReady ? 1 : 0;

	const float gamma = m_lastMLParams.MLGamma;
	const float shadow = m_lastMLParams.MLShadowToe;
	const float rolloff = m_lastMLParams.MLHighlightRolloff;
	const bool bIsPQ = (m_currentSwapChainColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
	const wchar_t* pColor = L"?";
	switch (m_currentSwapChainColorSpace) {
	case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: pColor = L"G22P709"; break;
	case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: pColor = L"G2084P2020"; break;
	case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709: pColor = L"G10P709"; break;
	default: break;
	}

	fwprintf(m_pMLDebugFile,
		L"%llu,%lld,%d,%d,%.4f,%.4f,%.4f,%d,%s,%d,%u,%d,%d,%d,%u,%u,%d,%d,%d,%d,"
		L"%u,%u,%.4f,%.4f,%.4f,%d,%d,%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,"
		L"%d,%s,%d,%d,%d,%d,%d\n",
		m_MLDebugFrame,
		(long long)m_rtStart,
		(m_bMLActive ? 1 : 0),
		m_iMLMode,
		(double)gamma, (double)shadow, (double)rolloff,
		(bIsPQ ? 1 : 0),
		pColor,
		(int)m_SwapChainFmt,
		(unsigned)m_MLDebugStepCount,
		(m_pPSHDR10ToneMapping ? 1 : 0),
		m_MLDebugWinW, m_MLDebugWinH,
		(UINT)m_srcRectWidth, (UINT)m_srcRectHeight,
		m_videoRect.left, m_videoRect.top, m_videoRect.right, m_videoRect.bottom,
		m_MLDebugLastMin, m_MLDebugLastMax,
		(double)m_MLDebugLastAvg, (double)m_MLDebugLastClipF, (double)m_MLDebugLastEdgeF,
		(m_bMLDebugCopy ? 1 : 0),
		m_MLDebugFeatReady, m_MLDebugStale, m_MLDebugConvertSeen, m_MLDebugTexConv,
		(double)m_MLDebugFeat0, (double)m_MLDebugFeat1, (double)m_MLDebugPred, (double)m_MLShadowDehazeApplied, (double)m_MLRolloffCtrlApplied,
		// Four specifiers, four arguments — keep them in step. A specifier added
		// without its argument corrupts the file this log exists to be read from.
		(int)m_bConvertToSdr,
		(m_strCorrection ? m_strCorrection : L"-"),
		m_iSDRDisplayNits,
		(m_lastMLParams.IsPQ > 0.5f ? 1 : 0),
		m_iHdrLocalToneMappingType,
		m_iExperimentalToneMapper,
		(ToneLutActive() ? 1 : 0));
	fflush(m_pMLDebugFile);
}

// Downscales the given ML-output stage into the 256x144 cache and logs luma
// stats (min/max/avg, fraction clipped >0.98, mean-abs-gradient edge metric)
// so a corrupted frame can be diagnosed from the log alone. ~every 8 frames.
void CDX11VideoProcessor::LogMLFrameStats(const Tex2D_t& src, const wchar_t* tag)
{
	if (!m_bMLDebug || !m_pMLDebugFile || !m_pPSMLDownsample || !m_MLDownsample || !m_MLStaging) {
		return;
	}

	const CRect full(0, 0, ML_DOWNSAMPLE_W, ML_DOWNSAMPLE_H);
	const CRect srcRect(0, 0, (int)src.desc.Width, (int)src.desc.Height);
	if (FAILED(TextureCopyRect(src, m_MLDownsample, srcRect, full, m_pPSMLDownsample, nullptr, 0, false))) {
		return;
	}
	m_pDeviceContext->CopyResource(m_MLStaging, m_MLDownsample);

	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (FAILED(m_pDeviceContext->Map(m_MLStaging, 0, D3D11_MAP_READ, 0, &mr))) {
		return;
	}
	const uint16_t* p = (const uint16_t*)mr.pData;
	const int rowPitch = (int)(mr.RowPitch / 2);
	const int w = ML_DOWNSAMPLE_W, h = ML_DOWNSAMPLE_H;
	uint64_t sum = 0, over = 0, edge = 0;
	uint32_t mn = 65535, mx = 0;
	for (int y = 0; y < h; y++) {
		const uint16_t* row = p + (size_t)y * rowPitch;
		for (int x = 0; x < w; x++) {
			const uint32_t v = row[x];
			sum += v;
			if (v < mn) mn = v;
			if (v > mx) mx = v;
			if (v > 65535u * 98 / 100) over++;
			if (x > 0) {
				const uint32_t d = (v > row[x - 1]) ? (v - row[x - 1]) : (row[x - 1] - v);
				if (d > 3276) edge++; // ~>5% step between neighbours
			}
		}
	}
	m_pDeviceContext->Unmap(m_MLStaging, 0);

	m_MLDebugLastMin = (uint16_t)mn;
	m_MLDebugLastMax = (uint16_t)mx;
	m_MLDebugLastAvg = (float)((double)sum / (w * h * 65535.0));
	m_MLDebugLastClipF = (float)((double)over / (w * h));
	m_MLDebugLastEdgeF = (float)((double)edge / (w * (h - 1) + (h - 1) * (w - 1)));

	if (m_pMLDebugFile) {
		fwprintf(m_pMLDebugFile, L"STATS,%s,%u,%u,%.4f,%.4f,%.4f\n",
			tag, (unsigned)mn, (unsigned)mx, (double)m_MLDebugLastAvg,
			(double)m_MLDebugLastClipF, (double)m_MLDebugLastEdgeF);
		fflush(m_pMLDebugFile);
	}
}

HRESULT CDX11VideoProcessor::SetShaderDoviCurvesPoly()
{
	ASSERT(m_Dovi.bValid);

	PS_DOVI_POLY_CURVE polyCurves[3] = {};

	const float scale = 1.0f / ((1 << m_Dovi.msd.Header.bl_bit_depth) - 1);
	const float scale_coef = 1.0f / (1u << m_Dovi.msd.Header.coef_log2_denom);

	for (int c = 0; c < 3; c++) {
		const auto& curve = m_Dovi.msd.Mapping.curves[c];
		auto& out = polyCurves[c];

		const int num_coef = curve.num_pivots - 1;
		bool has_poly = false;
		bool has_mmr = false;

		for (int i = 0; i < num_coef; i++) {
			switch (curve.mapping_idc[i]) {
			case 0: // polynomial
				has_poly = true;
				out.coeffs_data[i].x = scale_coef * curve.poly_coef[i][0];
				out.coeffs_data[i].y = (curve.poly_order[i] >= 1) ? scale_coef * curve.poly_coef[i][1] : 0.0f;
				out.coeffs_data[i].z = (curve.poly_order[i] >= 2) ? scale_coef * curve.poly_coef[i][2] : 0.0f;
				out.coeffs_data[i].w = 0.0f; // order=0 signals polynomial
				break;
			case 1: // mmr
				has_mmr = true;
				// not supported, leave as is
				out.coeffs_data[i].x = 0.0f;
				out.coeffs_data[i].y = 1.0f;
				out.coeffs_data[i].z = 0.0f;
				out.coeffs_data[i].w = 0.0f;
				break;
			}
		}

		const int n = curve.num_pivots - 2;
		for (int i = 0; i < n; i++) {
			out.pivots_data[i].x = scale * curve.pivots[i + 1];
		}
		for (int i = n; i < 7; i++) {
			out.pivots_data[i].x = 1e9f;
		}
	}

	HRESULT hr;

	if (m_pDoviCurvesConstantBuffer) {
		D3D11_MAPPED_SUBRESOURCE mr;
		hr = m_pDeviceContext->Map(m_pDoviCurvesConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
		if (SUCCEEDED(hr)) {
			memcpy(mr.pData, &polyCurves, sizeof(polyCurves));
			m_pDeviceContext->Unmap(m_pDoviCurvesConstantBuffer, 0);
		}
	}
	else {
		D3D11_BUFFER_DESC BufferDesc = { sizeof(polyCurves), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
		D3D11_SUBRESOURCE_DATA InitData = { &polyCurves, 0, 0 };
		hr = m_pDevice->CreateBuffer(&BufferDesc, &InitData, &m_pDoviCurvesConstantBuffer);
	}

	return hr;
}

HRESULT CDX11VideoProcessor::SetShaderDoviCurves()
{
	ASSERT(m_Dovi.bValid);

	PS_DOVI_CURVE cbuffer[3] = {};

	for (int c = 0; c < 3; c++) {
		const auto& curve = m_Dovi.msd.Mapping.curves[c];
		auto& out = cbuffer[c];

		bool has_poly = false, has_mmr = false, mmr_single = true;
		uint32_t mmr_idx = 0, min_order = 3, max_order = 1;

		const float scale_coef = 1.0f / (1 << m_Dovi.msd.Header.coef_log2_denom);
		const int num_coef = curve.num_pivots - 1;
		for (int i = 0; i < num_coef; i++) {
			switch (curve.mapping_idc[i]) {
			case 0: // polynomial
				has_poly = true;
				out.coeffs_data[i].x = scale_coef * curve.poly_coef[i][0];
				out.coeffs_data[i].y = (curve.poly_order[i] >= 1) ? scale_coef * curve.poly_coef[i][1] : 0.0f;
				out.coeffs_data[i].z = (curve.poly_order[i] >= 2) ? scale_coef * curve.poly_coef[i][2] : 0.0f;
				out.coeffs_data[i].w = 0.0f; // order=0 signals polynomial
				break;
			case 1: // mmr
				min_order = std::min<int>(min_order, curve.mmr_order[i]);
				max_order = std::max<int>(max_order, curve.mmr_order[i]);
				mmr_single = !has_mmr;
				has_mmr = true;
				out.coeffs_data[i].x = scale_coef * curve.mmr_constant[i];
				out.coeffs_data[i].y = static_cast<float>(mmr_idx);
				out.coeffs_data[i].w = static_cast<float>(curve.mmr_order[i]);
				for (int j = 0; j < curve.mmr_order[i]; j++) {
					// store weights per order as two packed float4s
					out.mmr_data[mmr_idx].x = scale_coef * curve.mmr_coef[i][j][0];
					out.mmr_data[mmr_idx].y = scale_coef * curve.mmr_coef[i][j][1];
					out.mmr_data[mmr_idx].z = scale_coef * curve.mmr_coef[i][j][2];
					out.mmr_data[mmr_idx].w = 0.0f; // unused
					mmr_idx++;
					out.mmr_data[mmr_idx].x = scale_coef * curve.mmr_coef[i][j][3];
					out.mmr_data[mmr_idx].y = scale_coef * curve.mmr_coef[i][j][4];
					out.mmr_data[mmr_idx].z = scale_coef * curve.mmr_coef[i][j][5];
					out.mmr_data[mmr_idx].w = scale_coef * curve.mmr_coef[i][j][6];
					mmr_idx++;
				}
				break;
			}
		}

		const float scale = 1.0f / ((1 << m_Dovi.msd.Header.bl_bit_depth) - 1);
		const int n = curve.num_pivots - 2;
		for (int i = 0; i < n; i++) {
			out.pivots_data[i].x = scale * curve.pivots[i + 1];
		}
		for (int i = n; i < 7; i++) {
			out.pivots_data[i].x = 1e9f;
		}

		if (has_poly) {
			out.params.methods = PS_RESHAPE_POLY;
		}
		if (has_mmr) {
			out.params.methods |= PS_RESHAPE_MMR;
			out.params.mmr_single = mmr_single;
			out.params.min_order = min_order;
			out.params.max_order = max_order;
		}
	}

	HRESULT hr;

	if (m_pDoviCurvesConstantBuffer) {
		D3D11_MAPPED_SUBRESOURCE mr;
		hr = m_pDeviceContext->Map(m_pDoviCurvesConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
		if (SUCCEEDED(hr)) {
			memcpy(mr.pData, &cbuffer, sizeof(cbuffer));
			m_pDeviceContext->Unmap(m_pDoviCurvesConstantBuffer, 0);
		}
	}
	else {
		D3D11_BUFFER_DESC BufferDesc = { sizeof(cbuffer), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
		D3D11_SUBRESOURCE_DATA InitData = { &cbuffer, 0, 0 };
		hr = m_pDevice->CreateBuffer(&BufferDesc, &InitData, &m_pDoviCurvesConstantBuffer);
	}

	return hr;
}

void CDX11VideoProcessor::UpdateTexParams(int cdepth)
{
	switch (m_iTexFormat) {
	case TEXFMT_AUTOINT:
		m_InternalTexFmt = (cdepth > 8 || m_bVPUseRTXVideoHDR) ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
		break;
	case TEXFMT_8INT:    m_InternalTexFmt = DXGI_FORMAT_B8G8R8A8_UNORM;     break;
	case TEXFMT_10INT:   m_InternalTexFmt = DXGI_FORMAT_R10G10B10A2_UNORM;  break;
	case TEXFMT_16FLOAT: m_InternalTexFmt = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
	default:
		ASSERT(FALSE);
	}
}

void CDX11VideoProcessor::UpdateRenderRect()
{
	m_renderRect.IntersectRect(m_videoRect, m_windowRect);
	UpdateScalingStrings();
}

void CDX11VideoProcessor::UpdateScalingStrings()
{
	const int w2 = m_videoRect.Width();
	const int h2 = m_videoRect.Height();
	const int k = m_bInterpolateAt50pct ? 2 : 1;
	int w1, h1;
	if (m_iRotation == 90 || m_iRotation == 270) {
		w1 = m_srcRectHeight;
		h1 = m_srcRectWidth;
	} else {
		w1 = m_srcRectWidth;
		h1 = m_srcRectHeight;
	}
	m_strShaderX = (w1 == w2) ? nullptr
		: (w1 > k * w2)
		? s_Downscaling11ResIDs[m_iDownscaling].description
		: s_Upscaling11ResIDs[m_iUpscaling].description;
	m_strShaderY = (h1 == h2) ? nullptr
		: (h1 > k * h2)
		? s_Downscaling11ResIDs[m_iDownscaling].description
		: s_Upscaling11ResIDs[m_iUpscaling].description;
}

void CDX11VideoProcessor::CalcStatsParams()
{
	if (m_pDeviceContext && !m_windowRect.IsRectEmpty()) {
		SIZE rtSize = m_windowRect.Size();

		if (S_OK == m_Font3D.CreateFontBitmap(L"Consolas", m_StatsFontH, 0)) {
			SIZE charSize = m_Font3D.GetMaxCharMetric();
			m_StatsRect.right  = m_StatsRect.left + 61 * charSize.cx + 5 + 3;
			m_StatsRect.bottom = m_StatsRect.top + 19 * charSize.cy + 5 + 3;
		}
		m_StatsBackground.Set(m_StatsRect, rtSize, D3DCOLOR_ARGB(80, 0, 0, 0));

		CalcGraphParams();
		m_Underlay.Set(m_GraphRect, rtSize, D3DCOLOR_ARGB(80, 0, 0, 0));

		m_Lines.ClearPoints(rtSize);
		POINT points[2];
		const int linestep = 20 * m_Yscale;
		for (int y = m_GraphRect.top + (m_Yaxis - m_GraphRect.top) % (linestep); y < m_GraphRect.bottom; y += linestep) {
			points[0] = { m_GraphRect.left,  y };
			points[1] = { m_GraphRect.right, y };
			m_Lines.AddPoints(points, std::size(points), (y == m_Yaxis) ? D3DCOLOR_XRGB(150, 150, 255) : D3DCOLOR_XRGB(100, 100, 255));
		}
		m_Lines.UpdateVertexBuffer();
	}
}

HRESULT CDX11VideoProcessor::MemCopyToTexSrcVideo(const BYTE* srcData, const int srcPitch)
{
	HRESULT hr = S_FALSE;
	D3D11_MAPPED_SUBRESOURCE mappedResource = {};

	if (m_TexSrcVideo.pTexture2) {
		hr = m_pDeviceContext->Map(m_TexSrcVideo.pTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
		if (SUCCEEDED(hr)) {
			m_pCopyPlaneFn(m_srcHeight, (BYTE*)mappedResource.pData, mappedResource.RowPitch, srcData, srcPitch);
			m_pDeviceContext->Unmap(m_TexSrcVideo.pTexture, 0);

			hr = m_pDeviceContext->Map(m_TexSrcVideo.pTexture2, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
			if (SUCCEEDED(hr)) {
				const UINT cromaH = m_srcHeight / m_srcParams.pDX11Planes->div_chroma_h;
				const int cromaPitch = (m_TexSrcVideo.pTexture3) ? srcPitch / m_srcParams.pDX11Planes->div_chroma_w : srcPitch;
				srcData += srcPitch * m_srcHeight;
				m_pCopyPlaneFn(cromaH, (BYTE*)mappedResource.pData, mappedResource.RowPitch, srcData, cromaPitch);
				m_pDeviceContext->Unmap(m_TexSrcVideo.pTexture2, 0);

				if (m_TexSrcVideo.pTexture3) {
					hr = m_pDeviceContext->Map(m_TexSrcVideo.pTexture3, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
					if (SUCCEEDED(hr)) {
						srcData += cromaPitch * cromaH;
						m_pCopyPlaneFn(cromaH, (BYTE*)mappedResource.pData, mappedResource.RowPitch, srcData, cromaPitch);
						m_pDeviceContext->Unmap(m_TexSrcVideo.pTexture3, 0);
					}
				}
			}
		}
	} else {
		hr = m_pDeviceContext->Map(m_TexSrcVideo.pTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
		if (SUCCEEDED(hr)) {
			const BYTE* src = (srcPitch < 0) ? srcData + srcPitch * (1 - (int)m_srcLines) : srcData;
			m_pCopyPlaneFn(m_srcLines, (BYTE*)mappedResource.pData, mappedResource.RowPitch, src, srcPitch);
			m_pDeviceContext->Unmap(m_TexSrcVideo.pTexture, 0);
		}
	}

	return hr;
}

HRESULT CDX11VideoProcessor::SetDevice(ID3D11Device *pDevice, ID3D11DeviceContext *pContext)
{
	DLog(L"CDX11VideoProcessor::SetDevice()");

	ReleaseSwapChain();
	m_pDXGIFactory2.Release();
	ReleaseDevice();

	CheckPointer(pDevice, E_POINTER);

	HRESULT hr = pDevice->QueryInterface(IID_PPV_ARGS(&m_pDevice));
	if (FAILED(hr)) {
		return hr;
	}
	if (pContext) {
		hr = pContext->QueryInterface(IID_PPV_ARGS(&m_pDeviceContext));
		if (FAILED(hr)) {
			return hr;
		}
	} else {
		m_pDevice->GetImmediateContext1(&m_pDeviceContext);
	}

	// for d3d11 subtitles
	CComQIPtr<ID3D10Multithread> pMultithread(m_pDeviceContext);
	pMultithread->SetMultithreadProtected(TRUE);

	CComPtr<IDXGIDevice> pDXGIDevice;
	hr = m_pDevice->QueryInterface(IID_PPV_ARGS(&pDXGIDevice));
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::SetDevice() : QueryInterface(IDXGIDevice) failed with error {}", HR2Str(hr));
		ReleaseDevice();
		return hr;
	}

	CComPtr<IDXGIAdapter> pDXGIAdapter;
	hr = pDXGIDevice->GetAdapter(&pDXGIAdapter);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::SetDevice() : GetAdapter(IDXGIAdapter) failed with error {}", HR2Str(hr));
		ReleaseDevice();
		return hr;
	}

	DXGI_ADAPTER_DESC dxgiAdapterDesc = {};
	hr = pDXGIAdapter->GetDesc(&dxgiAdapterDesc);
	if (SUCCEEDED(hr)) {
		m_VendorId = dxgiAdapterDesc.VendorId;
		m_strAdapterDescription = std::format(L"{} ({:04X}:{:04X})", dxgiAdapterDesc.Description, dxgiAdapterDesc.VendorId, dxgiAdapterDesc.DeviceId);
		DLog(L"Graphics DXGI adapter: {}", m_strAdapterDescription);
	}

	HRESULT hr2 = m_D3D11VP.InitVideoDevice(m_pDevice, m_pDeviceContext, m_VendorId);
	DLogIf(FAILED(hr2), L"CDX11VideoProcessor::SetDevice() : InitVideoDevice failed with error {}", HR2Str(hr2));

	D3D11_SAMPLER_DESC SampDesc = {};
	SampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	SampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	SampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	SampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	SampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	SampDesc.MinLOD = 0;
	SampDesc.MaxLOD = D3D11_FLOAT32_MAX;
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateSamplerState(&SampDesc, &m_pSamplerPoint));

	SampDesc.Filter = D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT; // linear interpolation for magnification
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateSamplerState(&SampDesc, &m_pSamplerLinear));

	SampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	SampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	SampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateSamplerState(&SampDesc, &m_pSamplerDither));

	D3D11_BLEND_DESC bdesc = {};
	bdesc.RenderTarget[0].BlendEnable = TRUE;
	bdesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	bdesc.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_ALPHA;
	bdesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	bdesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	bdesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	bdesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	bdesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateBlendState(&bdesc, &m_pAlphaBlendState));

	LPVOID data;
	DWORD size;
	EXECUTE_ASSERT(S_OK == GetDataFromResource(data, size, IDF_VS_11_SIMPLE));
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateVertexShader(data, size, nullptr, &m_pVS_Simple));

	D3D11_INPUT_ELEMENT_DESC Layout[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
	};
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateInputLayout(Layout, std::size(Layout), data, size, &m_pVSimpleInputLayout));

	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPS_Simple, IDF_PS_11_SIMPLE));

#if TEST_SHADER
	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPS_TEST, IDF_PS_11_TEST));
#endif

	D3D11_BUFFER_DESC BufferDesc = { sizeof(VERTEX) * 4, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, nullptr, &m_pVertexBuffer));

	BufferDesc = { sizeof(FLOAT) * 4 * 2, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, nullptr, &m_pResizeShaderConstantBuffer));

	BufferDesc = { sizeof(FLOAT) * 4, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, nullptr, &m_pHalfOUtoInterlaceConstantBuffer));

	// 8 floats: [0..1] dither coord scale, [2] cube LUT enabled, [3] cube LUT size,
	// [4] dither active. Grew from 4 when the display LUT moved into this pass.
	BufferDesc = { sizeof(FLOAT) * 8, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, nullptr, &m_pFinalPassConstantBuffer));

	BufferDesc = { sizeof(PS_EXTSHADER_CONSTANTS), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, nullptr, &m_pPostScaleConstants));

	CComPtr<IDXGIFactory1> pDXGIFactory1;
	hr = pDXGIAdapter->GetParent(IID_PPV_ARGS(&pDXGIFactory1));
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::SetDevice() : GetParent(IDXGIFactory1) failed with error {}", HR2Str(hr));
		ReleaseDevice();
		return hr;
	}

	hr = pDXGIFactory1->QueryInterface(IID_PPV_ARGS(&m_pDXGIFactory2));
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::SetDevice() : QueryInterface(IDXGIFactory2) failed with error {}", HR2Str(hr));
		ReleaseDevice();
		return hr;
	}

	HRESULT hr3 = m_Font3D.InitDeviceObjects(m_pDevice, m_pDeviceContext);
	DLogIf(FAILED(hr3), L"m_Font3D.InitDeviceObjects() failed with error {}", HR2Str(hr3));
	if (SUCCEEDED(hr3)) {
		hr3 = m_StatsBackground.InitDeviceObjects(m_pDevice, m_pDeviceContext);
		hr3 = m_Rect3D.InitDeviceObjects(m_pDevice, m_pDeviceContext);
		hr3 = m_Underlay.InitDeviceObjects(m_pDevice, m_pDeviceContext);
		hr3 = m_Lines.InitDeviceObjects(m_pDevice, m_pDeviceContext);
		hr3 = m_SyncLine.InitDeviceObjects(m_pDevice, m_pDeviceContext);
		DLogIf(FAILED(hr3), L"Geometric primitives InitDeviceObjects() failed with error {}", HR2Str(hr3));
	}
	ASSERT(S_OK == hr3);

	if (m_pFilter->m_inputMT.IsValid()) {
		if (!InitMediaType(&m_pFilter->m_inputMT)) {
			ReleaseDevice();
			return E_FAIL;
		}
	}

	if (m_hWnd) {
		hr = InitSwapChain(false);
		if (FAILED(hr)) {
			ReleaseDevice();
			return hr;
		}
	}

	SetCallbackDevice();
	UpdateSubPic();

	SetStereo3dTransform(m_iStereo3dTransform);

	HRESULT hr4 = m_TexDither.Create(m_pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, dither_size, dither_size, Tex2D_DynamicShaderWrite);
	if (S_OK == hr4) {
		hr4 = GetDataFromResource(data, size, IDF_DITHER_32X32_FLOAT16);
		if (S_OK == hr4) {
			D3D11_MAPPED_SUBRESOURCE mappedResource;
			hr4 = m_pDeviceContext->Map(m_TexDither.pTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
			if (S_OK == hr4) {
				uint16_t* src = (uint16_t*)data;
				BYTE* dst = (BYTE*)mappedResource.pData;
				for (UINT y = 0; y < dither_size; y++) {
					uint16_t* pUInt16 = reinterpret_cast<uint16_t*>(dst);
					for (UINT x = 0; x < dither_size; x++) {
						*pUInt16++ = src[x];
						*pUInt16++ = src[x];
						*pUInt16++ = src[x];
						*pUInt16++ = src[x];
					}
					src += dither_size;
					dst += mappedResource.RowPitch;
				}
				m_pDeviceContext->Unmap(m_TexDither.pTexture, 0);
			}
		}
		if (FAILED(hr4)) {
			m_TexDither.Release();
		}
	}

	m_pFilter->OnDisplayModeChange();
	UpdateStatsStatic();
	UpdateStatsByWindow();
	UpdateStatsByDisplay();

	InitMLResources();

	return hr;
}

HRESULT CDX11VideoProcessor::InitSwapChain(bool bWindowChanged)
{
	DLog(L"CDX11VideoProcessor::InitSwapChain() - {}", m_pFilter->m_bExclusiveScreen ? L"fullscreen" : L"window");
	m_pFilter->LogLine("renderer: InitSwapChain ENTER %s windowChanged=%d",
		m_pFilter->m_bExclusiveScreen ? "fullscreen" : "window", bWindowChanged ? 1 : 0);
	CheckPointer(m_pDXGIFactory2, E_FAIL);

	ReleaseSwapChain();

	auto bFullscreenChange = m_bExclusiveScreen != m_pFilter->m_bExclusiveScreen;
	m_bExclusiveScreen = m_pFilter->m_bExclusiveScreen;

	if (bFullscreenChange || bWindowChanged) {
		HandleHDRToggle();
		UpdateBitmapShader();

		if ((m_bHdrPassthrough || m_bHdrLocalToneMapping) && SourceIsHDR10orHLG()) {
			m_bHdrAllowSwitchDisplay = false;
			InitMediaType(&m_pFilter->m_inputMT);
			m_bHdrAllowSwitchDisplay = true;

			if (m_pDXGISwapChain1) {
				DLog(L"CDX11VideoProcessor::InitSwapChain() - SwapChain was created during the call to InitMediaType(), exit");
				return S_OK;
			}
		}
	}

	const auto bHdrOutput = m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping) && (SourceIsHDR() || m_bVPUseRTXVideoHDR);
	const auto b10BitOutput = bHdrOutput || Preferred10BitOutput();
	m_SwapChainFmt = b10BitOutput ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;

	HRESULT hr = S_OK;
	DXGI_SWAP_CHAIN_DESC1 desc1 = {};

	if (m_bExclusiveScreen) {
		MONITORINFOEXW mi = { sizeof(mi) };
		GetMonitorInfoW(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST), &mi);
		const CRect rc(mi.rcMonitor);

		desc1.Width = rc.Width();
		desc1.Height = rc.Height();
		desc1.Format = m_SwapChainFmt;
		desc1.SampleDesc.Count = 1;
		desc1.SampleDesc.Quality = 0;
		desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		if ((m_iSwapEffect == SWAPEFFECT_Flip && IsWindows8OrGreater()) || bHdrOutput) {
			desc1.BufferCount = bHdrOutput ? 6 : 2;
			desc1.Scaling = DXGI_SCALING_NONE;
			desc1.SwapEffect = IsWindows10OrGreater() ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		} else { // SWAPEFFECT_Discard or Windows 7
			desc1.BufferCount = 1;
			desc1.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		}
		desc1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

		DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc = {};
		fullscreenDesc.RefreshRate.Numerator = 0;
		fullscreenDesc.RefreshRate.Denominator = 1;
		fullscreenDesc.Windowed = FALSE;

		g_bCreateSwapChain = true;
		hr = m_pDXGIFactory2->CreateSwapChainForHwnd(m_pDevice, m_hWnd, &desc1, &fullscreenDesc, nullptr, &m_pDXGISwapChain1);
		g_bCreateSwapChain = false;
		DLogIf(FAILED(hr), L"CDX11VideoProcessor::InitSwapChain() : CreateSwapChainForHwnd(fullscreen) failed with error {}", HR2Str(hr));

		m_lastFullscreenHMonitor = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTOPRIMARY);
	}
	else {
		desc1.Width = std::max(8, m_windowRect.Width());
		desc1.Height = std::max(8, m_windowRect.Height());
		desc1.Format = m_SwapChainFmt;
		desc1.SampleDesc.Count = 1;
		desc1.SampleDesc.Quality = 0;
		desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		if ((m_iSwapEffect == SWAPEFFECT_Flip && IsWindows8OrGreater()) || bHdrOutput) {
			desc1.BufferCount = bHdrOutput ? 6 : 2;
			desc1.Scaling = DXGI_SCALING_NONE;
			desc1.SwapEffect = IsWindows10OrGreater() ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		} else { // SWAPEFFECT_Discard or Windows 7
			desc1.BufferCount = 1;
			desc1.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		}
		desc1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

		DLogIf(m_windowRect.Width() < 8 || m_windowRect.Height() < 8,
			L"CDX11VideoProcessor::InitSwapChain() : Invalid window size {}x{}, use {}x{}",
			m_windowRect.Width(), m_windowRect.Height(), desc1.Width, desc1.Height);

		hr = m_pDXGIFactory2->CreateSwapChainForHwnd(m_pDevice, m_hWnd, &desc1, nullptr, nullptr, &m_pDXGISwapChain1);
		DLogIf(FAILED(hr), L"CDX11VideoProcessor::InitSwapChain() : CreateSwapChainForHwnd() failed with error {}", HR2Str(hr));

		m_lastFullscreenHMonitor = nullptr;
	}

	if (m_pDXGISwapChain1) {
		m_UsedSwapEffect = desc1.SwapEffect;

		HRESULT hr2 = m_pDXGISwapChain1->GetContainingOutput(&m_pDXGIOutput);

		m_currentSwapChainColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
		if (bHdrOutput) {
			hr2 = m_pDXGISwapChain1->QueryInterface(IID_PPV_ARGS(&m_pDXGISwapChain4));

			if (m_pDXGIOutput) {
				CComPtr<IDXGIOutput6> pDXGIOutput6;
				if (SUCCEEDED(m_pDXGIOutput->QueryInterface(IID_PPV_ARGS(&pDXGIOutput6)))) {
					DXGI_OUTPUT_DESC1 desc;
					if (SUCCEEDED(pDXGIOutput6->GetDesc1(&desc))) {
						m_MaxDisplayLuminance = static_cast<UINT>(desc.MaxLuminance);

						m_pFilter->UpdateDisplayInfo();
					}
				}
			}
		}
	}

	return hr;
}

BOOL CDX11VideoProcessor::VerifyMediaType(const CMediaType* pmt)
{
	const auto& FmtParams = GetFmtConvParams(pmt);
	if (FmtParams.VP11Format == DXGI_FORMAT_UNKNOWN && FmtParams.DX11Format == DXGI_FORMAT_UNKNOWN) {
		return FALSE;
	}

	const BITMAPINFOHEADER* pBIH = GetBIHfromVIHs(pmt);
	if (!pBIH) {
		return FALSE;
	}

	if (pBIH->biWidth <= 0 || !pBIH->biHeight) {
		return FALSE;
	}

	return TRUE;
}

bool CDX11VideoProcessor::HandleHDRToggle()
{
	CAutoLock cRendererLock(&m_HDRToggleLock);

	m_bHdrDisplaySwitching = true;
	bool bRet = false;
	bool bEnableHDR = false;

	if (!m_bHDRModeChangeOutside) {
		if ((m_bHdrPassthrough || m_bHdrLocalToneMapping) && SourceIsHDR()) {
			MONITORINFOEXW mi = { sizeof(mi) };
			GetMonitorInfoW(m_lastFullscreenHMonitor ? m_lastFullscreenHMonitor : MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO*)&mi);
			DisplayConfig_t displayConfig = {};

			if (GetDisplayConfig(mi.szDevice, displayConfig)) {
				if (displayConfig.HDRSupported() && m_iHdrToggleDisplay) {
					bool bHDREnabled = false;
					std::wstring deviceName(mi.szDevice);
					if (auto it = m_hdrModeStartState.find(deviceName); it != m_hdrModeStartState.end()) {
						bHDREnabled = it->second;
					}

					const bool bNeedToggleOn = !displayConfig.HDREnabled() &&
											   (m_iHdrToggleDisplay == HDRTD_On || m_iHdrToggleDisplay == HDRTD_OnOff
											   || m_bExclusiveScreen || m_bFullScreen && (m_iHdrToggleDisplay == HDRTD_On_Fullscreen || m_iHdrToggleDisplay == HDRTD_OnOff_Fullscreen));
					const bool bNeedToggleOff = displayConfig.HDREnabled() &&
												!bHDREnabled && (!m_bExclusiveScreen || !m_bFullScreen) && m_iHdrToggleDisplay == HDRTD_OnOff_Fullscreen;
					DLog(L"HandleHDRToggle() : {}, {}", bNeedToggleOn, bNeedToggleOff);
					if (bNeedToggleOn) {
						bRet = ToggleHDR(displayConfig, true);
						DLogIf(!bRet, L"CDX11VideoProcessor::HandleHDRToggle() : Toggle HDR ON failed");

						if (bRet) {
							bEnableHDR = true;
							m_hdrModeSavedState.try_emplace(std::move(deviceName), false);
						}
					} else if (bNeedToggleOff) {
						bRet = ToggleHDR(displayConfig, false);
						DLogIf(!bRet, L"CDX11VideoProcessor::HandleHDRToggle() : Toggle HDR OFF failed");

						if (bRet) {
							m_hdrModeSavedState.try_emplace(std::move(deviceName), true);
						}
					}
				}
			}
		} else if (m_iHdrToggleDisplay) {
			MONITORINFOEXW mi = { sizeof(mi) };
			GetMonitorInfoW(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO*)&mi);
			DisplayConfig_t displayConfig = {};

			if (GetDisplayConfig(mi.szDevice, displayConfig)) {
				// check if HDR was already enabled in Windows before starting
				bool bWindowsHDREnabled = false;
				std::wstring deviceName(mi.szDevice);
				if (auto it = m_hdrModeStartState.find(deviceName); it != m_hdrModeStartState.end()) {
					bWindowsHDREnabled = it->second;
				}

				if (displayConfig.HDRSupported() && displayConfig.HDREnabled() &&
						(!bWindowsHDREnabled || m_iHdrToggleDisplay == HDRTD_OnOff || (m_iHdrToggleDisplay == HDRTD_OnOff_Fullscreen && m_bExclusiveScreen))) {
					bRet = ToggleHDR(displayConfig, false);
					DLogIf(!bRet, L"CDX11VideoProcessor::HandleHDRToggle() : Toggle HDR OFF failed");

					if (bRet) {
						m_hdrModeSavedState.try_emplace(std::move(deviceName), true);
					}
				}
			}
		}
	}

	if (bRet) {
		Sleep(100);
		FillDisplayParams();

		for (int i = 0; i < 5 && bEnableHDR && !m_bHdrDisplayModeEnabled; i++) {
			Sleep(100);
			FillDisplayParams();
		}
	}

	m_bHdrDisplaySwitching = false;

	return bRet;
}

bool CDX11VideoProcessor::ToggleHDR(const DisplayConfig_t& displayConfig, const bool bEnableAdvancedColor)
{
	DLog(L"ToggleHDR() : {} for '{}'", bEnableAdvancedColor, displayConfig.displayName);

	auto GetCurrentDisplayMode = [](LPCWSTR lpszDeviceName) -> std::optional<DEVMODEW> {
		DEVMODEW devmode = {};
		devmode.dmSize = sizeof(DEVMODEW);
		auto ret = EnumDisplaySettingsW(lpszDeviceName, ENUM_CURRENT_SETTINGS, &devmode);
		if (ret) {
			return devmode;
		}

		return {};
	};

	auto beforeModeOpt = GetCurrentDisplayMode(displayConfig.displayName);

	LONG ret = 1;

	if (IsWindows11_24H2OrGreater()) {
		DISPLAYCONFIG_SET_HDR_STATE setHdrState = {};
		setHdrState.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE);
		setHdrState.header.size = sizeof(setHdrState);
		setHdrState.header.adapterId = displayConfig.modeTarget.adapterId;
		setHdrState.header.id = displayConfig.modeTarget.id;
		setHdrState.enableHdr = bEnableAdvancedColor ? 1 : 0;

		ret = DisplayConfigSetDeviceInfo(&setHdrState.header);
		DLogIf(ERROR_SUCCESS != ret, L"ToggleHDR() : DisplayConfigSetDeviceInfo(DISPLAYCONFIG_SET_HDR_STATE) with '{}' failed with error {}", bEnableAdvancedColor, HR2Str(HRESULT_FROM_WIN32(ret)));
	} else {
		DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE setColorState = {};
		setColorState.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
		setColorState.header.size = sizeof(setColorState);
		setColorState.header.adapterId = displayConfig.modeTarget.adapterId;
		setColorState.header.id = displayConfig.modeTarget.id;
		setColorState.enableAdvancedColor = bEnableAdvancedColor ? 1 : 0;

		ret = DisplayConfigSetDeviceInfo(&setColorState.header);
		DLogIf(ERROR_SUCCESS != ret, L"ToggleHDR() : DisplayConfigSetDeviceInfo(DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE) with '{}' failed with error {}", bEnableAdvancedColor, HR2Str(HRESULT_FROM_WIN32(ret)));
	}

	if (ret == ERROR_SUCCESS) {
		m_bDisplayModeChangeAfterHDRToggle = true;

		if (beforeModeOpt.has_value()) {
			auto afterModeOpt = GetCurrentDisplayMode(displayConfig.displayName);
			if (afterModeOpt.has_value()) {
				auto& beforeMode = *beforeModeOpt;
				auto& afterMode = *afterModeOpt;
				if (beforeMode.dmPelsWidth != afterMode.dmPelsWidth || beforeMode.dmPelsHeight != afterMode.dmPelsHeight
						|| beforeMode.dmBitsPerPel != afterMode.dmBitsPerPel || beforeMode.dmDisplayFrequency != afterMode.dmDisplayFrequency) {
					DLog(L"ToggleHDR() : Display mode changed from {}x{}@{} to {}x{}@{}, restoring",
						 beforeMode.dmPelsWidth, beforeMode.dmPelsHeight, beforeMode.dmDisplayFrequency,
						 afterMode.dmPelsWidth, afterMode.dmPelsHeight, afterMode.dmDisplayFrequency);

					auto ret = ChangeDisplaySettingsExW(displayConfig.displayName, &beforeMode, nullptr, CDS_FULLSCREEN, nullptr);
					DLogIf(DISP_CHANGE_SUCCESSFUL != ret, L"ToggleHDR() : ChangeDisplaySettingsExW() failed with error {}", HR2Str(HRESULT_FROM_WIN32(ret)));

					m_bDisplayModeChangeAfterHDRToggle = true;
				}
			}
		}
	}

	return ret == ERROR_SUCCESS;
}

void CDX11VideoProcessor::ApplyInputColorSpaceOverride()
{
	// Always-on trace (ml_debug.csv, opened at construction in release) so the
	// HTPC decklink run shows exactly which ICS value was applied per media
	// type / settings push.
	if (m_pMLDebugFile) {
		fwprintf(m_pMLDebugFile, L"ICS override=%d (mt w=%u h=%u fmt=%d)\n",
			m_InputColorSpaceOverride, (unsigned)m_srcRectWidth, (unsigned)m_srcRectHeight, (int)m_srcParams.cformat);
		fflush(m_pMLDebugFile);
	}

	if (m_InputColorSpaceOverride == ICS_Auto) {
		return;
	}

	// Override BOTH formats: m_srcExFmt drives the D3D11 VP / shader
	// conversion matrices, m_decExFmt drives SourceIsHDR10orHLG() (and the
	// PQ/HLG tone-map + swapchain decisions keyed off it). NominalRange is
	// left to auto-detection (16-235 fallback in SpecifyExtendedFormat) —
	// only the matrix / primaries / transfer are forced to the signal.
	DXVA2_ExtendedFormat& x = m_srcExFmt;
	switch (m_InputColorSpaceOverride) {
	case ICS_BT601:      x.VideoTransferMatrix = DXVA2_VideoTransferMatrix_BT601; break;
	case ICS_BT709:      x.VideoTransferMatrix = DXVA2_VideoTransferMatrix_BT709; break;
	case ICS_BT2020_SDR:
	case ICS_BT2020_PQ:
	case ICS_BT2020_HLG: x.VideoTransferMatrix = MFVideoTransferMatrix_BT2020_10; break;
	default:             break;
	}
	switch (m_InputColorSpaceOverride) {
	case ICS_BT2020_SDR: x.VideoPrimaries = MFVideoPrimaries_BT2020;
	                     x.VideoTransferFunction = DXVA2_VideoTransFunc_709; break;
	case ICS_BT2020_PQ:  x.VideoPrimaries = MFVideoPrimaries_BT2020;
	                     x.VideoTransferFunction = MFVideoTransFunc_2084; break;
	case ICS_BT2020_HLG: x.VideoPrimaries = MFVideoPrimaries_BT2020;
	                     x.VideoTransferFunction = MFVideoTransFunc_HLG; break;
	case ICS_BT601:
	case ICS_BT709:      x.VideoPrimaries = DXVA2_VideoPrimaries_BT709;
	                     x.VideoTransferFunction = DXVA2_VideoTransFunc_709; break;
	default:             break;
	}
	m_decExFmt.VideoTransferMatrix   = x.VideoTransferMatrix;
	m_decExFmt.VideoPrimaries        = x.VideoPrimaries;
	m_decExFmt.VideoTransferFunction = x.VideoTransferFunction;
}

BOOL CDX11VideoProcessor::InitMediaType(const CMediaType* pmt)
{
	if (m_pMLDebugFile) {
		fwprintf(m_pMLDebugFile, L"InitMediaType enter (override=%d pmtFmt=%d cformat=%d)\n",
			m_InputColorSpaceOverride,
			(pmt && pmt->formattype == FORMAT_VideoInfo2) ? 2 : (pmt && pmt->formattype == FORMAT_VideoInfo) ? 1 : 0,
			(pmt ? (int)GetFmtConvParams(pmt).cformat : -1));
		// Session geometry probe (diagnostic): mt dims, rcSource, aspect, DV flag.
		// Correlates the seg-mask UV remap against per-title presentation geometry
		// (e.g. Dolby Vision profile 5 crop windows).
		++m_mlSessionId;
		int mtw = 0, mth = 0;
		RECT src = {};
		if (pmt && pmt->formattype == FORMAT_VideoInfo2) {
			const VIDEOINFOHEADER2* v = (const VIDEOINFOHEADER2*)pmt->pbFormat;
			mtw = (int)v->bmiHeader.biWidth; mth = (int)v->bmiHeader.biHeight;
			if (mth < 0) mth = -mth;
			src = v->rcSource;
		} else if (pmt && pmt->formattype == FORMAT_VideoInfo) {
			const VIDEOINFOHEADER* v = (const VIDEOINFOHEADER*)pmt->pbFormat;
			mtw = (int)v->bmiHeader.biWidth; mth = (int)v->bmiHeader.biHeight;
			if (mth < 0) mth = -mth;
			src = v->rcSource;
		}
		fwprintf(m_pMLDebugFile,
			L"SESSION#%u doVi=%d mt=%dx%d srcRect=(%d,%d,%d,%d) aspect=%d:%d convert=%ux%u\n",
			(unsigned)m_mlSessionId, (int)m_Dovi.bValid,
			mtw, mth, (int)src.left, (int)src.top, (int)src.right, (int)src.bottom,
			(int)m_srcAspectRatioX, (int)m_srcAspectRatioY,
			(unsigned)m_TexConvertOutput.desc.Width, (unsigned)m_TexConvertOutput.desc.Height);
		fflush(m_pMLDebugFile);
	}
	DLog(L"CDX11VideoProcessor::InitMediaType()");

	// High-level: signal/format change arriving at the renderer. Paired with the
	// DONE line at the end of this function, so a renegotiation is bracketed and
	// a fault inside it is attributable. (The ml_debug.csv frame log is untouched
	// — that one is for image-quality work.)
	m_pFilter->LogLine("renderer: InitMediaType ENTER (media type change; dims reported on the DONE line)");

	if (!VerifyMediaType(pmt)) {
		m_pFilter->LogLine("renderer: InitMediaType REJECTED by VerifyMediaType");
		return FALSE;
	}

	ReleaseVP();

	auto FmtParams = GetFmtConvParams(pmt);

	const BITMAPINFOHEADER* pBIH = nullptr;
	m_decExFmt.value = 0;

	if (pmt->formattype == FORMAT_VideoInfo2) {
		const VIDEOINFOHEADER2* vih2 = (VIDEOINFOHEADER2*)pmt->pbFormat;
		pBIH = &vih2->bmiHeader;
		m_srcRect = vih2->rcSource;
		m_srcAspectRatioX = vih2->dwPictAspectRatioX;
		m_srcAspectRatioY = vih2->dwPictAspectRatioY;
		if (FmtParams.CSType == CS_YUV && (vih2->dwControlFlags & (AMCONTROL_USED | AMCONTROL_COLORINFO_PRESENT))) {
			m_decExFmt.value = vih2->dwControlFlags;
			m_decExFmt.SampleFormat = AMCONTROL_USED | AMCONTROL_COLORINFO_PRESENT; // ignore other flags
			if (m_pMLDebugFile) { // TEMP-DEBUG: does the media type carry HDR transfer?
				fwprintf(m_pMLDebugFile, L"MT_XFMT transfer=%lu matrix=%lu prim=%lu flags=0x%08lx\n",
					(unsigned long)m_decExFmt.VideoTransferFunction,
					(unsigned long)m_decExFmt.VideoTransferMatrix,
					(unsigned long)m_decExFmt.VideoPrimaries,
					(unsigned long)vih2->dwControlFlags);
				fflush(m_pMLDebugFile);
			}
		}
		m_bInterlaced = (vih2->dwInterlaceFlags & AMINTERLACE_IsInterlaced);
		m_rtAvgTimePerFrame = vih2->AvgTimePerFrame;
	}
	else if (pmt->formattype == FORMAT_VideoInfo) {
		const VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)pmt->pbFormat;
		pBIH = &vih->bmiHeader;
		m_srcRect = vih->rcSource;
		m_srcAspectRatioX = 0;
		m_srcAspectRatioY = 0;
		m_bInterlaced = 0;
		m_rtAvgTimePerFrame = vih->AvgTimePerFrame;
	}
	else {
		return FALSE;
	}

	m_pFilter->m_FrameStats.SetStartFrameDuration(m_rtAvgTimePerFrame);
	m_pFilter->m_bValidBuffer = false;

	UINT biWidth  = pBIH->biWidth;
	UINT biHeight = labs(pBIH->biHeight);

	m_srcLines = biHeight * FmtParams.PitchCoeff / 2;
	m_srcPitch = biWidth * FmtParams.Packsize;
	switch (FmtParams.cformat) {
	case CF_Y8:
	case CF_NV12:
	case CF_RGB24:
	case CF_BGR48:
		m_srcPitch = ALIGN(m_srcPitch, 4);
		break;
	case CF_V210:
		m_srcPitch = ALIGN((biWidth + 5) / 6 * 16, 128);
	}
	if (pBIH->biCompression == BI_RGB && pBIH->biHeight > 0) {
		m_srcPitch = -m_srcPitch;
	}

	UINT origW = biWidth;
	UINT origH = biHeight;
	if (pmt->FormatLength() == VR_EXRADATA_POS + sizeof(VR_Extradata)) {
		const VR_Extradata* vrextra = reinterpret_cast<VR_Extradata*>(pmt->pbFormat + VR_EXRADATA_POS);
		if (vrextra->QueryWidth == pBIH->biWidth && vrextra->QueryHeight == pBIH->biHeight && vrextra->Compression == pBIH->biCompression) {
			origW  = vrextra->FrameWidth;
			origH = abs(vrextra->FrameHeight);
		}
	}

	if (m_srcRect.IsRectNull()) {
		m_srcRect.SetRect(0, 0, origW, origH);
	}
	m_srcRectWidth  = m_srcRect.Width();
	m_srcRectHeight = m_srcRect.Height();

	m_srcExFmt = SpecifyExtendedFormat(m_decExFmt, FmtParams, m_srcRectWidth, m_srcRectHeight);
	ApplyInputColorSpaceOverride(); // forced ICS_* for live HDMI capture (no-op when Auto)

	bool disableD3D11VP = false;
	switch (FmtParams.cformat) {
	case CF_NV12: disableD3D11VP = !m_VPFormats.bNV12; break;
	case CF_P010:
	case CF_P016: disableD3D11VP = !m_VPFormats.bP01x;  break;
	case CF_YUY2: disableD3D11VP = !m_VPFormats.bYUY2;  break;
	default:      disableD3D11VP = !m_VPFormats.bOther; break;
	}
	if (m_srcExFmt.VideoTransferMatrix == VIDEOTRANSFERMATRIX_YCgCo || m_Dovi.bValid) {
		disableD3D11VP = true;
	}
	if (FmtParams.CSType == CS_RGB && m_VendorId == PCIV_NVIDIA) {
		// D3D11 VP does not work correctly if RGB32 with odd frame width (source or target) on Nvidia adapters
		disableD3D11VP = true;
	}
	if (disableD3D11VP) {
		FmtParams.VP11Format = DXGI_FORMAT_UNKNOWN;
	}

	const auto frm_gcd = std::gcd(m_srcRectWidth, m_srcRectHeight);
	const auto srcFrameARX = m_srcRectWidth / frm_gcd;
	const auto srcFrameARY = m_srcRectHeight / frm_gcd;

	if (!m_srcAspectRatioX || !m_srcAspectRatioY) {
		m_srcAspectRatioX = srcFrameARX;
		m_srcAspectRatioY = srcFrameARY;
		m_srcAnamorphic = false;
	}
	else {
		const auto ar_gcd = std::gcd(m_srcAspectRatioX, m_srcAspectRatioY);
		m_srcAspectRatioX /= ar_gcd;
		m_srcAspectRatioY /= ar_gcd;
		m_srcAnamorphic = (srcFrameARX != m_srcAspectRatioX || srcFrameARY != m_srcAspectRatioY);
	}

	UpdateUpscalingShaders();
	UpdateDownscalingShaders();

	m_pPSCorrection.Release();
	m_pPSConvertColor.Release();
	m_pPSConvertColorDeint.Release();
	m_PSConvColorData.bEnable = false;

	m_pPSHDR10ToneMapping.Release();
	m_pHDR10ToneMappingConstants.Release();
	m_pDoViDynamicConstants.Release();

	UpdateTexParams(FmtParams.CDepth);

	if (m_bHdrAllowSwitchDisplay && m_srcVideoTransferFunction != m_srcExFmt.VideoTransferFunction) {
		auto ret = HandleHDRToggle();
		if (!ret && ((m_bHdrPassthrough || m_bHdrLocalToneMapping) && m_bHdrPassthroughSupport && SourceIsHDR10orHLG() && !m_pDXGISwapChain4)) {
			ret = true;
		}
		if (ret) {
			ReleaseSwapChain();
			Init(m_hWnd, false);
		}
	}

	if (Preferred10BitOutput() && m_SwapChainFmt == DXGI_FORMAT_B8G8R8A8_UNORM) {
		ReleaseSwapChain();
		Init(m_hWnd, false);
	}

	m_srcVideoTransferFunction = m_srcExFmt.VideoTransferFunction;

	HRESULT hr = E_NOT_VALID_STATE;

	// D3D11 Video Processor
	if (FmtParams.VP11Format != DXGI_FORMAT_UNKNOWN) {
		hr = InitializeD3D11VP(FmtParams, origW, origH, pmt);
		if (SUCCEEDED(hr)) {
			UpdateCorrectionShader();
		}
		else {
			ReleaseVP();
		}
	}

	// Tex Video Processor
	if (FAILED(hr) && FmtParams.DX11Format != DXGI_FORMAT_UNKNOWN) {
		m_bVPUseRTXVideoHDR = false;
		hr = InitializeTexVP(FmtParams, origW, origH);
		if (SUCCEEDED(hr)) {
			SetShaderConvertColorParams();
			SetShaderLuminanceParams();
		}
	}

	if (SUCCEEDED(hr)) {
		UpdateBitmapShader();
		UpdateTexures();
		UpdatePostScaleTexures();
		UpdateStatsStatic();

		m_pFilter->m_inputMT = *pmt;
		UpdateMLActive();

		// High-level: the renderer's own account of the signal it just accepted.
		// This is the line that says WHICH format it rebuilt for, and it is what
		// makes a media-type change visible on the host's timeline rather than
		// inferred from the absence of frames.
		m_pFilter->LogLine("renderer: InitMediaType DONE %ux%u tf=%u prim=%u mtx=%u decFmt=0x%08X interlaced=%d rtAvg=%lld",
			(unsigned)m_srcWidth, (unsigned)m_srcHeight,
			(unsigned)m_srcVideoTransferFunction,
			(unsigned)m_decExFmt.VideoPrimaries, (unsigned)m_decExFmt.VideoTransferMatrix,
			(unsigned)m_decExFmt.value, m_bInterlaced ? 1 : 0, (long long)m_rtAvgTimePerFrame);

		return TRUE;
	}

	return FALSE;
}

HRESULT CDX11VideoProcessor::InitializeD3D11VP(const FmtConvParams_t& params, const UINT width, const UINT height, const CMediaType* pmt)
{
	if (!m_D3D11VP.IsVideoDeviceOk()) {
		return E_ABORT;
	}

	const auto& dxgiFormat = params.VP11Format;

	DLog(L"CDX11VideoProcessor::InitializeD3D11VP() started with input surface: {}, {} x {}", DXGIFormatToString(dxgiFormat), width, height);

	m_TexSrcVideo.Release();

	const bool bHdrPassthrough = m_bHdrDisplayModeEnabled && (SourceIsHDR10orHLG() || (m_bVPUseRTXVideoHDR && params.CDepth == 8));
	m_D3D11OutputFmt = m_InternalTexFmt;
	const int deinterlacing = m_bInterlaced ? m_iVPDeinterlacing : DEINT_Disable;
	HRESULT hr = m_D3D11VP.InitVideoProcessor(dxgiFormat, width, height, m_srcExFmt, deinterlacing, bHdrPassthrough, m_D3D11OutputFmt);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::InitializeD3D11VP() : InitVideoProcessor() failed with error {}", HR2Str(hr));
		return hr;
	}

	hr = m_D3D11VP.InitInputTextures(m_pDevice);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::InitializeD3D11VP() : InitInputTextures() failed with error {}", HR2Str(hr));
		return hr;
	}

	auto rtxHDR = m_bVPRTXVideoHDR && m_bHdrPassthroughSupport && m_bHdrPassthrough && m_iTexFormat != TEXFMT_8INT && !SourceIsHDR();
	m_bVPUseRTXVideoHDR = (m_D3D11VP.SetRTXVideoHDR(rtxHDR) == S_OK);

	auto superRes = (m_bVPScaling && (m_InternalTexFmt == DXGI_FORMAT_B8G8R8A8_UNORM || m_bVPUseRTXVideoHDR) && (params.CDepth == 8 || !m_bACMEnabled)) ? m_iVPSuperRes : SUPERRES_Disable;
	m_bVPUseSuperRes = (m_D3D11VP.SetSuperRes(superRes) == S_OK);

	if ((m_bVPUseRTXVideoHDR && !m_pDXGISwapChain4)
			|| (!m_bVPUseRTXVideoHDR && m_pDXGISwapChain4 && !SourceIsHDR())) {
		InitSwapChain(false);
		InitMediaType(pmt);
		return S_OK;
	}

	hr = m_TexSrcVideo.Create(m_pDevice, dxgiFormat, width, height, Tex2D_DynamicShaderWriteNoSRV);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::InitializeD3D11VP() : m_TexSrcVideo.Create() failed with error {}", HR2Str(hr));
		return hr;
	}

	m_srcWidth       = width;
	m_srcHeight      = height;
	m_srcParams      = params;
	m_srcDXGIFormat  = dxgiFormat;
	m_pCopyPlaneFn   = GetCopyPlaneFunction(params, VP_D3D11);

	DLog(L"CDX11VideoProcessor::InitializeD3D11VP() completed successfully");

	return S_OK;
}

HRESULT CDX11VideoProcessor::InitializeTexVP(const FmtConvParams_t& params, const UINT width, const UINT height)
{
	const auto& srcDXGIFormat = params.DX11Format;

	DLog(L"CDX11VideoProcessor::InitializeTexVP() started with input surface: {}, {} x {}", DXGIFormatToString(srcDXGIFormat), width, height);

	HRESULT hr = m_TexSrcVideo.CreateEx(m_pDevice, srcDXGIFormat, params.pDX11Planes, width, height, Tex2D_DynamicShaderWrite);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::InitializeTexVP() : m_TexSrcVideo.CreateEx() failed with error {}", HR2Str(hr));
		return hr;
	}

	m_srcWidth       = width;
	m_srcHeight      = height;
	m_srcParams      = params;
	m_srcDXGIFormat  = srcDXGIFormat;
	m_pCopyPlaneFn   = GetCopyPlaneFunction(params, VP_D3D11_SHADER);

	// set default ProcAmp ranges
	SetDefaultDXVA2ProcAmpRanges(m_DXVA2ProcAmpRanges);

	hr = UpdateConvertColorShader();

	SAFE_RELEASE(m_PSConvColorData.pVertexBuffer);
	EXECUTE_ASSERT(S_OK == CreateVertexBuffer(m_pDevice, &m_PSConvColorData.pVertexBuffer, m_srcWidth, m_srcHeight, m_srcRect, 0, false));

	DLog(L"CDX11VideoProcessor::InitializeTexVP() completed successfully");

	return S_OK;
}

void CDX11VideoProcessor::UpdatFrameProperties()
{
	m_srcPitch = m_srcWidth * m_srcParams.Packsize;
	m_srcLines = m_srcHeight * m_srcParams.PitchCoeff / 2;
}

BOOL CDX11VideoProcessor::GetAlignmentSize(const CMediaType& mt, SIZE& Size)
{
	if (VerifyMediaType(&mt)) {
		const auto& FmtParams = GetFmtConvParams(&mt);

		if (FmtParams.cformat == CF_RGB24) {
			Size.cx = ALIGN(Size.cx, 4);
		}
		else if (FmtParams.cformat == CF_RGB48 || FmtParams.cformat == CF_BGR48) {
			Size.cx = ALIGN(Size.cx, 2);
		}
		else if (FmtParams.cformat == CF_BGRA64 || FmtParams.cformat == CF_B64A) {
			// nothing
		}
		else {
			auto pBIH = GetBIHfromVIHs(&mt);
			if (!pBIH) {
				return FALSE;
			}

			auto biWidth = pBIH->biWidth;
			auto biHeight = labs(pBIH->biHeight);

			if (!m_Alignment.cx || m_Alignment.cformat != FmtParams.cformat
					|| m_Alignment.texture.desc.Width != biWidth || m_Alignment.texture.desc.Height != biHeight) {
				m_Alignment.texture.Release();
				m_Alignment.cformat = {};
				m_Alignment.cx = {};
			}

			if (!m_Alignment.texture.pTexture) {
				auto VP11Format = FmtParams.VP11Format;
				if (VP11Format != DXGI_FORMAT_UNKNOWN) {
					bool disableD3D11VP = false;
					switch (FmtParams.cformat) {
						case CF_NV12: disableD3D11VP = !m_VPFormats.bNV12;  break;
						case CF_P010:
						case CF_P016: disableD3D11VP = !m_VPFormats.bP01x;  break;
						case CF_YUY2: disableD3D11VP = !m_VPFormats.bYUY2;  break;
						default:      disableD3D11VP = !m_VPFormats.bOther; break;
					}
					if (disableD3D11VP) {
						VP11Format = DXGI_FORMAT_UNKNOWN;
					}
				}

				HRESULT hr = E_FAIL;
				if (VP11Format != DXGI_FORMAT_UNKNOWN) {
					hr = m_Alignment.texture.Create(m_pDevice, VP11Format, biWidth, biHeight, Tex2D_DynamicShaderWriteNoSRV);
				}
				if (FAILED(hr) && FmtParams.DX11Format != DXGI_FORMAT_UNKNOWN) {
					hr = m_Alignment.texture.CreateEx(m_pDevice, FmtParams.DX11Format, FmtParams.pDX11Planes, biWidth, biHeight, Tex2D_DynamicShaderWrite);
				}
				if (FAILED(hr)) {
					return FALSE;
				}

				UINT RowPitch = 0;
				D3D11_MAPPED_SUBRESOURCE mappedResource = {};
				if (SUCCEEDED(m_pDeviceContext->Map(m_Alignment.texture.pTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource))) {
					RowPitch = mappedResource.RowPitch;
					m_pDeviceContext->Unmap(m_Alignment.texture.pTexture, 0);
				}

				if (!RowPitch) {
					return FALSE;
				}

				m_Alignment.cformat = FmtParams.cformat;
				m_Alignment.cx = RowPitch / FmtParams.Packsize;
			}

			Size.cx = m_Alignment.cx;
		}

		if (FmtParams.cformat == CF_RGB24 || FmtParams.cformat == CF_XRGB32 || FmtParams.cformat == CF_ARGB32) {
			Size.cy = -abs(Size.cy); // only for biCompression == BI_RGB
		} else {
			Size.cy = abs(Size.cy);
		}

		return TRUE;

	}

	return FALSE;
}

HRESULT CDX11VideoProcessor::ProcessSample(IMediaSample* pSample)
{
	// ─ Per-frame trace, always on. The faults land inside the render path with
	// no engine-side marker, so every frame is bracketed here and along the way —
	// a crash then shows exactly which sub-step died, on which frame.
	static unsigned long long s_traceFrames = 0;
	const unsigned long long seq = ++s_traceFrames;

	REFERENCE_TIME rtStart, rtEnd;
	if (FAILED(pSample->GetTime(&rtStart, &rtEnd))) {
		rtStart = m_pFilter->m_FrameStats.GeTimestamp();
	}
	const REFERENCE_TIME rtFrameDur = m_pFilter->m_FrameStats.GetAverageFrameDuration();
	rtEnd = rtStart + rtFrameDur;

	DLog(L"[RT] ProcessSample #{} ENTER (rtStart={})", seq, (long long)rtStart);

	if (m_bMLDebug && m_pMLDebugFile) {
		fwprintf(m_pMLDebugFile, L"PS,%lld,%d\n", (long long)rtStart, (m_bMLActive ? 1 : 0));
		fflush(m_pMLDebugFile);
	}

	m_rtStart = rtStart;
	CRefTime rtClock(rtStart);

	DLog(L"[RT] ProcessSample #{} CopySample ENTER (rtStart={})", seq, (long long)rtStart);
	HRESULT hr = CopySample(pSample);
	if (FAILED(hr)) {
		DLog(L"[RT] ProcessSample #{} CopySample FAILED hr=0x{:08X}", seq, (unsigned)hr);
		m_RenderStats.failed++;
		return hr;
	}
	DLog(L"[RT] ProcessSample #{} CopySample done — Render ENTER", seq);

	// The experimental tone mapper is driven by the same per-frame statistics, so it
	// keeps them fresh on its own — see the extraction gate in Process().
	if (MLStatsNeeded()) {
		m_bMLFeaturesStale = true; // re-analyze on the next render
	}

	// always Render(1) a frame after CopySample()
	hr = Render(1, rtStart);
	DLog(L"[RT] ProcessSample #{} Render returned hr=0x{:08X} — draw stats", seq, (unsigned)hr);
	m_pFilter->m_DrawStats.Add(GetPreciseTick());
	if (m_pFilter->m_filterState == State_Running) {
		m_pFilter->StreamTime(rtClock);
	}

	// HTPC present-latency gauge: (wall clock since first present) - (stream
	// time since first present). Valid while decklink stream time advances 1:1
	// with real capture time.
	{
		const long long wall100 = (long long)((uint64_t)GetPreciseTick() * 10000000ULL / GetPreciseTicksPerSecondI());
		if (m_rtFirstPresent < 0) {
			m_rtFirstPresent = rtStart;
			m_wallFirstPresent100ns = wall100;
			m_presentLatencyMs = 0.0;
		} else {
			const long long lag100 = (wall100 - m_wallFirstPresent100ns) - (rtStart - m_rtFirstPresent);
			m_presentLatencyMs = lag100 > 0 ? (double)lag100 / 10000.0 : 0.0;
		}
	}

	m_RenderStats.syncoffset = rtClock - rtStart;

	int so = (int)std::clamp(m_RenderStats.syncoffset, -UNITS, UNITS);
#if SYNC_OFFSET_EX
	m_SyncDevs.Add(so - m_Syncs.Last());
#endif
	m_Syncs.Add(so);

	if (m_bDoubleFrames) {
		if (rtEnd < rtClock) {
			m_RenderStats.dropped2++;
			return S_FALSE; // skip frame
		}

		rtStart += rtFrameDur / 2;

		hr = Render(2, rtStart);
		m_pFilter->m_DrawStats.Add(GetPreciseTick());
		if (m_pFilter->m_filterState == State_Running) {
			m_pFilter->StreamTime(rtClock);
		}

		m_RenderStats.syncoffset = rtClock - rtStart;

		so = (int)std::clamp(m_RenderStats.syncoffset, -UNITS, UNITS);
#if SYNC_OFFSET_EX
		m_SyncDevs.Add(so - m_Syncs.Last());
#endif
		m_Syncs.Add(so);
	}

	return hr;
}

HRESULT CDX11VideoProcessor::CopySample(IMediaSample* pSample)
{
	CheckPointer(m_pDXGISwapChain1, E_FAIL);

	uint64_t tick = GetPreciseTick();

	// Get frame type
	m_SampleFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE; // Progressive
	m_bDoubleFrames = false;
	if (m_bInterlaced) {
		if (CComQIPtr<IMediaSample2> pMS2 = pSample) {
			AM_SAMPLE2_PROPERTIES props;
			if (SUCCEEDED(pMS2->GetProperties(sizeof(props), (BYTE*)&props))) {
				if ((props.dwTypeSpecificFlags & AM_VIDEO_FLAG_WEAVE) == 0) {
					if (props.dwTypeSpecificFlags & AM_VIDEO_FLAG_FIELD1FIRST) {
						m_SampleFormat = D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST; // Top-field first
					} else {
						m_SampleFormat = D3D11_VIDEO_FRAME_FORMAT_INTERLACED_BOTTOM_FIELD_FIRST; // Bottom-field first
					}
					m_bDoubleFrames = m_iVPDeinterlacing && m_bDeintDouble && m_D3D11VP.IsReady();
				}
			}
		}
	}

	HRESULT hr = S_OK;
	m_FieldDrawn = 0;
	bool updateStats = false;

	m_hdr10 = {};
	if (CComQIPtr<IMediaSideData> pMediaSideData = pSample) {
		// Side-data presence probe: which LAV side-data GUIDs actually arrived
		// on this sample (HW-decode delivery can't be reasoned about blind).
		{
			static const GUID kH10p = { 0x6ff47d98, 0x3ef9, 0x4335,
				{ 0xa6, 0x5e, 0xe7, 0x18, 0x80, 0xc1, 0x5b, 0xd9 } };
			const BYTE* q = nullptr; size_t z = 0;
			m_sideData =
				(SUCCEEDED(pMediaSideData->GetSideData(IID_MediaSideDataHDR, &q, &z)) ? 1 : 0)
				| (SUCCEEDED(pMediaSideData->GetSideData(IID_MediaSideDataHDRContentLightLevel, &q, &z)) ? 2 : 0)
				| (SUCCEEDED(pMediaSideData->GetSideData(kH10p, &q, &z)) ? 4 : 0)
				| (SUCCEEDED(pMediaSideData->GetSideData(IID_MediaSideDataDOVIMetadataV2, &q, &z)) ? 8 : 0);
		}
		if (SourceIsHDR10orHLG() && (m_bHdrPassthrough || m_bHdrLocalToneMapping)) {
			MediaSideDataHDR* hdr = nullptr;
			size_t size = 0;
			hr = pMediaSideData->GetSideData(IID_MediaSideDataHDR, (const BYTE**)&hdr, &size);
			if (SUCCEEDED(hr) && size == sizeof(MediaSideDataHDR)) {
				const auto& primaries_x = hdr->display_primaries_x;
				const auto& primaries_y = hdr->display_primaries_y;
				if (primaries_x[0] > 0. && primaries_x[1] > 0. && primaries_x[2] > 0.
						&& primaries_y[0] > 0. && primaries_y[1] > 0. && primaries_y[2] > 0.
						&& hdr->white_point_x > 0. && hdr->white_point_y > 0.
						&& hdr->max_display_mastering_luminance > 0. && hdr->min_display_mastering_luminance > 0.) {
					m_hdr10.bValid = true;

					m_hdr10.hdr10.RedPrimary[0]   = static_cast<UINT16>(std::lround(primaries_x[2] * 50000.0));
					m_hdr10.hdr10.RedPrimary[1]   = static_cast<UINT16>(std::lround(primaries_y[2] * 50000.0));
					m_hdr10.hdr10.GreenPrimary[0] = static_cast<UINT16>(std::lround(primaries_x[0] * 50000.0));
					m_hdr10.hdr10.GreenPrimary[1] = static_cast<UINT16>(std::lround(primaries_y[0] * 50000.0));
					m_hdr10.hdr10.BluePrimary[0]  = static_cast<UINT16>(std::lround(primaries_x[1] * 50000.0));
					m_hdr10.hdr10.BluePrimary[1]  = static_cast<UINT16>(std::lround(primaries_y[1] * 50000.0));
					m_hdr10.hdr10.WhitePoint[0]   = static_cast<UINT16>(std::lround(hdr->white_point_x * 50000.0));
					m_hdr10.hdr10.WhitePoint[1]   = static_cast<UINT16>(std::lround(hdr->white_point_y * 50000.0));

					m_hdr10.hdr10.MaxMasteringLuminance = static_cast<UINT>(std::lround(hdr->max_display_mastering_luminance));
					m_hdr10.hdr10.MinMasteringLuminance = static_cast<UINT>(std::lround(hdr->min_display_mastering_luminance * 10000.0));
				}
			}

			MediaSideDataHDRContentLightLevel* hdrCLL = nullptr;
			size = 0;
			m_rawCll = m_rawFall = -1; m_rawCllSize = -1;
			hr = pMediaSideData->GetSideData(IID_MediaSideDataHDRContentLightLevel, (const BYTE**)&hdrCLL, &size);
			m_rawCllSize = (int)size;
			if (SUCCEEDED(hr) && size == sizeof(MediaSideDataHDRContentLightLevel) && hdrCLL) {
				m_hdr10.hdr10.MaxContentLightLevel      = hdrCLL->MaxCLL;
				m_hdr10.hdr10.MaxFrameAverageLightLevel = hdrCLL->MaxFALL;
				m_rawCll  = (int)hdrCLL->MaxCLL;
				m_rawFall = (int)hdrCLL->MaxFALL;
			}
		}

		// Content-side ST 2086 snapshot (see m_hdr10Cll… docs): keep the last
		// values the SAMPLE carried. m_lastHdr10 is only written on an HDR10
		// swapchain (SDR displays skip it and it stays zero) — the panel's
		// MaxCLL/MaxFALL/Mastering rows must read this display-independent copy.
		if (m_hdr10.hdr10.MaxContentLightLevel)
			m_hdr10Cll = m_hdr10.hdr10.MaxContentLightLevel;
		if (m_hdr10.hdr10.MaxFrameAverageLightLevel)
			m_hdr10Fall = m_hdr10.hdr10.MaxFrameAverageLightLevel;
		if (m_hdr10.hdr10.MaxMasteringLuminance)
			m_hdr10Mmax = m_hdr10.hdr10.MaxMasteringLuminance;
		if (m_hdr10.hdr10.MinMasteringLuminance)
			m_hdr10Mmin = m_hdr10.hdr10.MinMasteringLuminance;

		size_t size = 0;
		MediaSideData3DOffset* offset = nullptr;
		hr = pMediaSideData->GetSideData(IID_MediaSideData3DOffset, (const BYTE**)&offset, &size);
		if (SUCCEEDED(hr) && size == sizeof(MediaSideData3DOffset) && offset->offset_count > 0 && offset->offset[0]) {
			m_nStereoSubtitlesOffsetInPixels = offset->offset[0];
		}

		if (m_srcParams.CSType == CS_YUV && (m_bHdrPreferDoVi || !SourceIsHDR10orHLG())) {
			MediaSideDataDOVIMetadata* pDOVIMetadata = nullptr;
			hr = pMediaSideData->GetSideData(IID_MediaSideDataDOVIMetadataV2, (const BYTE**)&pDOVIMetadata, &size);
			if (SUCCEEDED(hr) && size == sizeof(MediaSideDataDOVIMetadata) && CheckDoviMetadata(pDOVIMetadata, 1)) {
				const bool bYCCtoRGBChanged = !m_PSConvColorData.bEnable ||
					(memcmp(
						&m_Dovi.msd.ColorMetadata.ycc_to_rgb_matrix,
						&pDOVIMetadata->ColorMetadata.ycc_to_rgb_matrix,
						sizeof(MediaSideDataDOVIMetadata::ColorMetadata.ycc_to_rgb_matrix) + sizeof(MediaSideDataDOVIMetadata::ColorMetadata.ycc_to_rgb_offset)
					) != 0);
				const bool bRGBtoLMSChanged =
					(memcmp(
						&m_Dovi.msd.ColorMetadata.rgb_to_lms_matrix,
						&pDOVIMetadata->ColorMetadata.rgb_to_lms_matrix,
						sizeof(MediaSideDataDOVIMetadata::ColorMetadata.rgb_to_lms_matrix)
					) != 0);
				const bool bMappingCurvesChanged = !m_pDoviCurvesConstantBuffer ||
					(memcmp(
						&m_Dovi.msd.Mapping.curves,
						&pDOVIMetadata->Mapping.curves,
						sizeof(MediaSideDataDOVIMetadata::Mapping.curves)
					) != 0);

				const bool bMasteringLuminanceChanged = m_Dovi.msd.ColorMetadata.source_max_pq != pDOVIMetadata->ColorMetadata.source_max_pq
					|| m_Dovi.msd.ColorMetadata.source_min_pq != pDOVIMetadata->ColorMetadata.source_min_pq;

				bool bMMRChanged = false;
				if (bMappingCurvesChanged) {
					bool has_mmr = false;
					for (const auto& curve : pDOVIMetadata->Mapping.curves) {
						for (uint8_t i = 0; i < (curve.num_pivots - 1); i++) {
							if (curve.mapping_idc[i] == 1) {
								has_mmr = true;
								break;
							}
						}
					}
					if (m_Dovi.bHasMMR != has_mmr) {
						m_Dovi.bHasMMR = has_mmr;
						m_pDoviCurvesConstantBuffer.Release();
						bMMRChanged = true;
					}
				}

				memcpy(&m_Dovi.msd, pDOVIMetadata, size);
				const bool doviStateChanged = !m_Dovi.bValid;
				m_Dovi.bValid = true;

				// based on libplacebo source code
				constexpr float
					PQ_M1 = 2610.f / (4096.f * 4.f),
					PQ_M2 = 2523.f / 4096.f * 128.f,
					PQ_C1 = 3424.f / 4096.f,
					PQ_C2 = 2413.f / 4096.f * 32.f,
					PQ_C3 = 2392.f / 4096.f * 32.f;

				auto PqToLinearNits = [](float x) {
					x = powf(x, 1.0f / PQ_M2);
					x = fmaxf(x - PQ_C1, 0.0f) / (PQ_C2 - PQ_C3 * x);
					x = powf(x, 1.0f / PQ_M1);
					return x * 10000.0f;
				};
				auto LinearNitsToPq = [](float y) {
					y /= 10000.0f;
					y = fmaxf(y, 0.0f);
					y = powf(y, PQ_M1);
					y = (PQ_C1 + PQ_C2 * y) / (1.0f + PQ_C3 * y);
					return powf(y, PQ_M2);
				};

				// Level 1 + 3
				for (uint32_t i = 0; i < LAV_DOVI_MAX_EXTENSIONS; ++i) {
					if (pDOVIMetadata->Extensions[i].level == 1) {
						auto& Level1 = pDOVIMetadata->Extensions[i].Level1;

						m_DoviExtensionMetadata.L1.present = true;
						m_DoviExtensionMetadata.L1.min_pq = Level1.min_pq;
						m_DoviExtensionMetadata.L1.max_pq = Level1.max_pq;
						m_DoviExtensionMetadata.L1.avg_pq = Level1.avg_pq;

						for (uint32_t k = 0; k < LAV_DOVI_MAX_EXTENSIONS; ++k) {
							if (pDOVIMetadata->Extensions[k].level == 3) {
								auto& Level3 = pDOVIMetadata->Extensions[k].Level3;

								m_DoviExtensionMetadata.L1.min_pq = m_DoviExtensionMetadata.L1.min_pq + Level3.min_pq_offset - 2048;
								m_DoviExtensionMetadata.L1.max_pq = m_DoviExtensionMetadata.L1.max_pq + Level3.max_pq_offset - 2048;
								m_DoviExtensionMetadata.L1.avg_pq = m_DoviExtensionMetadata.L1.avg_pq + Level3.avg_pq_offset - 2048;

								break;
							}
						}

						m_DoviExtensionMetadata.L1.min_pq = static_cast<UINT>(PqToLinearNits(m_DoviExtensionMetadata.L1.min_pq / 4095.f));
						m_DoviExtensionMetadata.L1.max_pq = static_cast<UINT>(PqToLinearNits(m_DoviExtensionMetadata.L1.max_pq / 4095.f));
						m_DoviExtensionMetadata.L1.avg_pq = static_cast<UINT>(PqToLinearNits(m_DoviExtensionMetadata.L1.avg_pq / 4095.f));

						if (m_bHdrPassthroughSupport && m_bHdrLocalToneMapping) {
							if (m_DoviExtensionMetadata.L1 != m_DoviExtensionMetadata.L1Cached) {
								m_DoviExtensionMetadata.L1Cached = m_DoviExtensionMetadata.L1;
								UpdateStatsStatic();
							}
						}

						break;
					}
				}

				// Level 2
				float display_pq = LinearNitsToPq(m_iHdrDisplayMaxNits);
				int lower_index = -1, upper_index = -1;
				float closest_lower_dist = 1.0f, closest_upper_dist = 1.0f;
				bool level2Present = false;
				uint16_t max_target_max_pq = std::numeric_limits<uint16_t>::max();

				for (uint32_t i = 0; i < LAV_DOVI_MAX_EXTENSIONS; ++i) {
					if (pDOVIMetadata->Extensions[i].level == 2) {
						level2Present = true;

						auto& Level2 = pDOVIMetadata->Extensions[i].Level2;

						if (!m_bHdrPassthroughSupport) {
							if (Level2.target_max_pq < max_target_max_pq) {
								max_target_max_pq = Level2.target_max_pq;
								upper_index = i;
							}
						} else {
							float target_pq = Level2.target_max_pq / 4095.0f;
							if (target_pq <= display_pq) {
								float dist = display_pq - target_pq;
								if (dist < closest_lower_dist) {
									closest_lower_dist = dist;
									lower_index = i;
								}
							} else {
								float dist = target_pq - display_pq;
								if (dist < closest_upper_dist) {
									closest_upper_dist = dist;
									upper_index = i;
								}
							}
						}
					}
				}

				if (level2Present) {
					float t_slope = 1.0f, t_offset = 0.0f, t_power = 1.0f;
					float t_chroma = 0.0f, t_sat = 0.0f;

					// SCENARIO A: Display is BETWEEN two targets
					if (lower_index != -1 && upper_index != -1) {
						float lower_pq = pDOVIMetadata->Extensions[lower_index].Level2.target_max_pq / 4095.0f;
						float upper_pq = pDOVIMetadata->Extensions[upper_index].Level2.target_max_pq / 4095.0f;

						float weight = (upper_pq != lower_pq) ? (display_pq - lower_pq) / (upper_pq - lower_pq) : 0.0f;
						weight = std::clamp(weight, 0.0f, 1.0f);

						t_slope = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_slope),
											static_cast<float>(pDOVIMetadata->Extensions[upper_index].Level2.trim_slope),
											weight);
						t_offset = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_offset),
												static_cast<float>(pDOVIMetadata->Extensions[upper_index].Level2.trim_offset),
												weight);
						t_power = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_power),
											static_cast<float>(pDOVIMetadata->Extensions[upper_index].Level2.trim_power),
											weight);
						t_chroma = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_chroma_weight),
												static_cast<float>(pDOVIMetadata->Extensions[upper_index].Level2.trim_chroma_weight),
												weight);
						t_sat = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_saturation_gain),
											static_cast<float>(pDOVIMetadata->Extensions[upper_index].Level2.trim_saturation_gain),
											weight);
					}
					// SCENARIO B: Display is BRIGHTER than all targets (Interpolate towards Master/Neutral)
					else if (lower_index != -1 && upper_index == -1) {
						float master_pq = pDOVIMetadata->ColorMetadata.source_max_pq / 4095.0f;

						float lower_pq = pDOVIMetadata->Extensions[lower_index].Level2.target_max_pq / 4095.0f;
						float weight = (master_pq > lower_pq) ? (display_pq - lower_pq) / (master_pq - lower_pq) : 0.0f;
						weight = std::clamp(weight, 0.0f, 1.0f);

						t_slope = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_slope), 2048.0f, weight);
						t_offset = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_offset), 2048.0f, weight);
						t_power = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_power), 2048.0f, weight);
						t_chroma = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_chroma_weight), 2048.0f, weight);
						t_sat = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_saturation_gain), 2048.0f, weight);
					}
					// SCENARIO C: Display is DIMMER than all targets (Clamp to the lowest available target)
					else if (lower_index == -1 && upper_index != -1) {
						t_slope = pDOVIMetadata->Extensions[upper_index].Level2.trim_slope;
						t_offset = pDOVIMetadata->Extensions[upper_index].Level2.trim_offset;
						t_power = pDOVIMetadata->Extensions[upper_index].Level2.trim_power;
						t_chroma = pDOVIMetadata->Extensions[upper_index].Level2.trim_chroma_weight;
						t_sat = pDOVIMetadata->Extensions[upper_index].Level2.trim_saturation_gain;
					}

					// Final normalization to floating point coefficients
					m_DoviExtensionMetadata.L2.present = true;
					m_DoviExtensionMetadata.L2.trim_slope = t_slope / 4096.0f;
					m_DoviExtensionMetadata.L2.trim_offset = t_offset / 4096.0f;
					m_DoviExtensionMetadata.L2.trim_power = t_power / 4096.0f;
					m_DoviExtensionMetadata.L2.trim_saturation_gain = t_sat / 4096.0f;
					m_DoviExtensionMetadata.L2.trim_chroma_weight = t_chroma / 4096.0f;
				}

				SetDolbyVisionDynamicParams();

				if (bMasteringLuminanceChanged) {
					m_DoviMaxMasteringLuminance = static_cast<UINT>(PqToLinearNits(m_Dovi.msd.ColorMetadata.source_max_pq / 4095.f));
					m_DoviMinMasteringLuminance = static_cast<UINT>(PqToLinearNits(m_Dovi.msd.ColorMetadata.source_min_pq / 4095.f) * 10000.0f);

					if (size == sizeof(MediaSideDataDOVIMetadata)) {
						for (uint32_t i = 0; i < LAV_DOVI_MAX_EXTENSIONS; ++i) {
							if (pDOVIMetadata->Extensions[i].level == 6) {
								auto& Level6 = pDOVIMetadata->Extensions[i].Level6;

								m_DoviMaxMasteringLuminance = Level6.max_luminance;
								m_DoviMinMasteringLuminance = Level6.min_luminance;
								m_DoviMaxContentLightLevel = Level6.max_cll;
								m_DoviMaxFrameAverageLightLevel = Level6.max_fall;

								break;
							}
						}
					}
				}

				if (m_D3D11VP.IsReady()) {
					InitMediaType(&m_pFilter->m_inputMT);
				}
				else if (doviStateChanged) {
					UpdateStatsStatic();
				}

				if (bYCCtoRGBChanged) {
					DLog(L"CDX11VideoProcessor::CopySample() : DoVi ycc_to_rgb_matrix is changed");
					SetShaderConvertColorParams();
				}
				if (bRGBtoLMSChanged || bMMRChanged) {
					DLogIf(bRGBtoLMSChanged, L"CDX11VideoProcessor::CopySample() : DoVi rgb_to_lms_matrix is changed");
					DLogIf(bMMRChanged, L"CDX11VideoProcessor::CopySample() : DoVi has_mmr is changed");
					UpdateConvertColorShader();
				}
				if (bMappingCurvesChanged) {
					if (m_Dovi.bHasMMR) {
						hr = SetShaderDoviCurves();
					} else {
						hr = SetShaderDoviCurvesPoly();
					}
				}

				if (doviStateChanged && !SourceIsHDR10orHLG()) {
					ReleaseSwapChain();
					Init(m_hWnd, false);

					m_srcVideoTransferFunction = 0;
					InitMediaType(&m_pFilter->m_inputMT);
				}
			}
		}
	}

	if (CComQIPtr<IMediaSampleD3D11> pMSD3D11 = pSample) {
		if (m_iSrcFromGPU != 11) {
			m_iSrcFromGPU = 11;
			updateStats = true;
		}

		CComQIPtr<ID3D11Texture2D> pD3D11Texture2D;
		UINT ArraySlice = 0;
		hr = pMSD3D11->GetD3D11Texture(0, &pD3D11Texture2D, &ArraySlice);
		if (FAILED(hr)) {
			DLog(L"CDX11VideoProcessor::CopySample() : GetD3D11Texture() failed with error {}", HR2Str(hr));
			return hr;
		}

		D3D11_TEXTURE2D_DESC desc = {};
		pD3D11Texture2D->GetDesc(&desc);
		if (desc.Format != m_srcDXGIFormat) {
			return E_UNEXPECTED;
		}

#if 0 // fix for issue #16. disable reinitialization code.
		if (desc.Width != m_srcWidth || desc.Height != m_srcHeight) {
			if (m_D3D11VP.IsReady()) {
				hr = InitializeD3D11VP(m_srcParams, desc.Width, desc.Height);
			} else {
				hr = InitializeTexVP(m_srcParams, desc.Width, desc.Height);
			}
			if (FAILED(hr)) {
				return hr;
			}
			UpdatFrameProperties();
			updateStats = true;
		}
#endif

		if (m_D3D11VP.IsReady()) {
			m_D3D11VP.SetInputVideoData(pD3D11Texture2D, pSample, ArraySlice, m_SampleFormat);
		} else {
			// here should be used CopySubresourceRegion instead of CopyResource
			D3D11_BOX srcBox = { 0, 0, 0, m_srcWidth, m_srcHeight, 1 };
			m_pDeviceContext->CopySubresourceRegion(m_TexSrcVideo.pTexture, 0, 0, 0, 0, pD3D11Texture2D, ArraySlice, &srcBox);
		}
	}
	else {
		if (m_iSrcFromGPU != 0) {
			m_iSrcFromGPU = 0;
			updateStats = true;
		}

		BYTE* data = nullptr;
		const int size = pSample->GetActualDataLength();
		if (size >= abs(m_srcPitch) * (int)m_srcLines && S_OK == pSample->GetPointer(&data)) {
			// do not use UpdateSubresource for D3D11 VP here
			// because it can cause green screens and freezes on some configurations
			hr = MemCopyToTexSrcVideo(data, m_srcPitch);
			if (m_D3D11VP.IsReady()) {
				// ID3D11VideoProcessor does not use textures with D3D11_CPU_ACCESS_WRITE flag
				m_pDeviceContext->CopyResource(m_D3D11VP.GetNextInputTexture(m_SampleFormat), m_TexSrcVideo.pTexture);
			}
		}
	}

	if (updateStats) {
		UpdateStatsStatic();
	}

	m_RenderStats.copyticks = GetPreciseTick() - tick;

	return hr;
}

HRESULT CDX11VideoProcessor::Render(int field, const REFERENCE_TIME frameStartTime)
{
	// Frame canary + params row (kept at the very top so nothing can gate it out)
	if (m_bMLDebug && m_pMLDebugFile) {
		m_MLDebugWinW = (int)m_windowRect.Width();
		m_MLDebugWinH = (int)m_windowRect.Height();
		LogMLFrame();
	}
	m_nSkyFrame++;   // render-frame counter for the seg cadence (always)

	// ML params must be refreshed before this frame is processed; keep this
	// unconditional (only gated by active state) so a stale or zero-filled
	// constant buffer can never reach the grade shaders.
	if (m_bMLActive && m_pPSMLGamma) {
		SetMLParams();
	}

	// Feature extraction also lives here (NOT inside the swap-chain4/HDR
	// metadata block): playback can render through a path where that block is
	// bypassed, which silently stalled AUTO mode for a whole session.
	// Feature extraction runs while ML is armed — active OR auto-pending — so the
// AUTO path warms from the preloaded pipeline instead of waiting on activation.
	//
	// The experimental tone mapper feeds on the same statistics, so it arms the
	// extractor on its own: a user who selects it with the Zion grade off must
	// still get a curve, and the alternative — quietly reusing whatever stale
	// statistics were left over — reads to the user as "the control does nothing".
	// MLStatsNeeded() is the one predicate for "these statistics are wanted"; the
	// staleness flag and the convert-seen latch below use it too, so the three
	// cannot drift apart again.
	if (m_pPSMLGamma && m_pPSMLDownsample && MLStatsNeeded()) {
		m_MLDebugStale = m_bMLFeaturesStale ? 1 : 0;
		m_MLDebugConvertSeen = m_bMLConvertSeen ? 1 : 0;
		m_MLDebugTexConv = m_TexConvertOutput.pTexture ? 1 : 0;
		if (m_bMLFeaturesStale && m_bMLConvertSeen && m_TexConvertOutput.pTexture) {
			ExtractMLFeatures();
			m_bMLFeaturesStale = false;
			UpdateToneLut();

			// DELIVERED instrument. Placed HERE — beside the extraction, which is
			// known to run — and throttled on its own frame counter rather than on the
			// peak anchor's cut/lock state machine. Two earlier placements rode on that
			// machine and both failed silently: nested inside its "applied peak moved"
			// branch it never ran, and keyed on `m_SceneFrames == kSceneSettleFrames` it
			// ran EVERY frame, because m_SceneFrames freezes at that value once the
			// anchor locks. An instrument that logs nothing and an instrument that logs
			// 2001 lines a minute are the same bug: a trigger borrowed from someone
			// else's state.
			//
			// 30 render frames is ~0.5 s at 60, ~1.25 s at 24 — fine for a scene-level
			// number, and the read-back (2 Maps) is the only real cost.
			if (m_bMLDebug && m_MLDebugFrame - m_uDeliveredLastFrame >= 30) {
				m_uDeliveredLastFrame = m_MLDebugFrame;
				ReadDeliveredStats(m_MLDebugFrame);
			}

			// --- Per-scene peak for the Hable curve --------------------------
			// m_MLFeatures[0] is the per-scene peak of the signal the tone curve is
			// about to consume, in PQ. Measured on the PRE-tone-map texture, and
			// correctly domained since the extractor stopped keying its luma domain
			// off the SWAP CHAIN (which is SDR on this path) instead of the source
			// actually selected.
			//
			// *** THE COMMENT THAT USED TO SIT HERE WAS WRONG, AND IS GONE. ***
			// It said the smoothing had been "TAKEN INSTANTANEOUSLY, deliberately",
			// and that the earlier ~12-frame IIR had been removed because "a
			// brightness change that trails the picture reads as a fault". The code
			// immediately below it has done the opposite ever since the libplacebo
			// peak detection was ported: it filters, and the same stale paragraph was
			// left standing over it. Anyone reading top-down would have concluded the
			// lag was fixed.
			//
			// It is not fixed. Measured 2026-09-20 on a static scene, filt_pq ramped
			// 0.3202 -> 0.3785 across frames 172-252 — about 80 frames, over three
			// seconds — with mix = 0.000 on every line. The hysteresis never fires,
			// so nothing snaps and the IIR always wins.
			//
			// The cause was the bias term, and it was a real departure from
			// libplacebo. libplacebo scales the deviation by a WORKGROUP-active
			// fraction (frame_wg_active / frame_wg_count, colorspace.c:1231), which
			// stays near 1.0 on any normally-exposed frame because a workgroup counts
			// as active if ANY of its pixels is. m_MLActiveFrac was a PIXEL fraction,
			// a small number on a sparse dark frame, so it scaled delta below
			// thresh_low (0.01) and mix pinned to 0.
			//
			// FIXED 2026-09-20: BuildMLFeatures now reports a 16x16 CELL-active
			// fraction, the workgroup analogue. The consequence of the old behaviour
			// was not only lag — it was CLIPPING. peakSdrUnits = max(peak/sdrNits, 1)
			// means a stale LOW peak keeps the divisor at hable(1.0), i.e. "85 nits is
			// white"; cut to a bright scene and everything above 85 nits overflows and
			// is flattened by the closing saturate(). The normalisation stayed anchored
			// to the previous dark scene for ~3 seconds, which is what a hard-edged
			// blotch in the highlights of a bright shot actually is.
			//
			// Historical note, still true and still the reason to be careful here:
			// within a scene maxscl is a MAX, so it is set by the brightest object
			// and is very
			// the response time was the whole cost.
			// --- libplacebo peak detection, ported ---------------------------------
			// This is the code the mpv stack ran (shaders/colorspace.c,
			// sh_color_map_obj.peak) with its own constants: percentile 99.995, an
			// IIR of smoothing_period 20, scene hysteresis at 1.0/3.0.
			//
			// The ORDER is libplacebo's and it matters:
			//   1. measure avg_pq and max_pq (both PQ)
			//   2. ignore deviations below rounding error
			//   3. IIR low-pass                          <- the filter
			//   4. hysteresis: mix toward the measurement by how far it moved,
			//      smoothstepped between the two thresholds
			//
			// Step 4 is why this cannot lag. The IIR only wins for deviations BELOW
			// the low threshold (1 dB); above the high threshold (3 dB) the mix
			// coefficient reaches 1 and the value JUMPS in a single frame. The filter
			// is left doing what it is for — sub-1 dB drift, which is where noise
			// lives — and a real scene change goes straight through.
			//
			// An earlier version here used ONE hard threshold at 0.12 PQ, roughly 4x
			// libplacebo's high threshold. Ordinary scene changes fell into the
			// filter's bucket and ramped, and that was read as the filter being slow.
			// The filter was innocent; the threshold was the fault.
			constexpr float kPqMax        = 16383.0f;  // libplacebo PQ_MAX = (1<<14)-1
			constexpr float kSmoothPeriod = 20.0f;     // PL_PEAK_DETECT_HQ_DEFAULTS / --hdr-peak-decay-rate
						constexpr float kThreshLow    = 1.0f;      // PL_PEAK_DETECT_HQ_DEFAULTS
			constexpr float kThreshHigh   = 3.0f;
			constexpr float kLog10Pq      = 1e-2f;     // libplacebo's own "experimentally determined" constant

			float avgPq = m_MLFeatures[1];
			float maxPq = (m_MLPeakPq995 > 0.0f) ? m_MLPeakPq995 : m_MLFeatures[0];

			// Plausibility gate — NOT libplacebo's, and the one place this departs
			// from it. PQ 1.0 decodes to 10000 nits and is what the extractor
			// reports when it saturates (observed live: raw_pq=1.0000 /
			// raw_nits=10000.0). Fed to the filter it would poison the state for
			// many frames, so a read outside a sane band is treated as a failed read
			// and skipped outright — the filter simply holds.
			const float maxNits = exptm::PqToLinearNits(maxPq);
			if (maxNits > 10.0f && maxNits < 4000.0f) {
				if (m_PeakAvgPq == 0.0f) {
					// No state yet — take the first reading outright
					// (libplacebo: "if (!obj->peak.avg_pq)"), as a fresh scene.
					m_PeakAvgPq = avgPq;
					m_PeakMaxPq = maxPq;
					m_SceneLocked = false;
					m_SceneFrames = 0;
					// A fresh anchor means a fresh title or session: the exposure
					// ratchet must not carry across it.
					m_fRefWhiteAuto = 0.0f;
					m_MeanNitsSmooth = m_MLMeanNitsRaw;
					m_MeanNitsMax = 0.0f;
					m_ZoneSeen = false;
					m_ZoneChangeFrac = 0.0f;
					for (int i = 0; i < 25; ++i) m_PrevZoneMax[i] = 0.0f;
					for (int i = 0; i < 4; ++i) m_DeltaHistory[i] = 0.0f;
				} else {
					const float eps = 1.0f / kPqMax;
					if (fabsf(avgPq - m_PeakAvgPq) < eps) avgPq = m_PeakAvgPq;
					if (fabsf(maxPq - m_PeakMaxPq) < eps) maxPq = m_PeakMaxPq;
				}

				// 3. Snap / settle / LOCK, with asymmetric drift (2026-09-22).
				//
				// The IIR-20 under this comment used to do BOTH jobs — snap on a cut
				// and smooth within a scene — and the two need opposite behaviour:
				// cuts must arrive instantly, within a scene the anchor should not
				// move at all. Because the IIR always TRACKS, a bright element that
				// left frame (a lamp pans off, the sky shrinks) pulled the anchor
				// down at ~5%/frame and the whole picture LIFTED over ~0.8 s — the
				// "room breathing" that reads as the picture responding to nothing.
				//
				// Three speeds now:
				//   cut                        -> snap in one frame (mixc > 0.95)
				//   settle (~12 frames / 0.5s) -> fast IIR (period 5) converges on
				//                                the new scene's true level
				//   locked                     -> the anchor does NOT follow content;
				//                                only a slow DOWNWARD leak (period ~200,
				//                                ≈8 s) when the peak actually drops,
				//                                so the picture may brighten gently but
				//                                can never DIM within a scene. Dimming
				//                                pumps are visible; slow brightening is
				//                                not, and the cut detector still sees
				//                                live-vs-anchor on the next frame.
				constexpr int kSceneSettleFrames = 12;   // ~0.5 s at 24 fps
				// The LIGHTING-CHANGE gate (2026-09-22): a camera cut within one
				// physical scene is exposure-matched by the DOP — the global mean
				// stays in a tight band — whereas a true SCENE change (new light)
				// moves it. A frame can snap the anchor only if the mean shifted by
				// more than this band, so abrupt zone-global changes that carry the
				// same exposure (camera cuts, whip-pans back into the same light)
				// are correctly treated as light-invariant and the anchor holds.
				// Absolute in the feature domain; tune from the meanD CSV column.
				constexpr float kSceneMeanBand = 0.02f;
				//
				// (delta / mixc are computed below, after the histogram block.)

				// 4. Scene-change hysteresis. bias = the non-black CELL fraction, so a
				// letterboxed or mostly-black frame cannot register a change on its own.
				// Fixed 2026-09-20: this used to be a PIXEL fraction, which on a sparse
				// frame scaled delta below thresh_low and pinned mix at 0 — the snap
				// never fired and the IIR ramp alone set the peak, so a cut to a bright
				// scene kept the previous scene's normalisation for ~3 seconds and the
				// highlights clipped against it. See BuildMLFeatures.
				// libplacebo measures the deviation from the AVERAGE alone, and that misses
				// precisely the cuts that matter here. Two scenes either side of a cut can
				// have almost identical average brightness and very different PEAKS (one
				// has light sources, the other does not), so delta stays under thresh_low,
				// the hysteresis never fires, and the IIR glides the anchor from the old
				// scene's value to the new one's over ~0.8 s. The glide is visible in
				// whichever direction it runs — bright settling dim, or dim brightening —
				// and both of those were reported, in that order.
				//
				// Including the peak catches it: a peak jump IS a scene change. This was
				// safe to add only after the anchor's statistic became p99.5 measured over
				// the content rows, which is stable within a shot (a max spanning 0.003 PQ
				// across 4198 frames), so it cannot fire spuriously on ordinary frames.
				// The gate keys on the PEAK deviation alone, deliberately NOT the average
				// (2026-09-22). A character in dark clothing walking into the frame is
				// a large AVERAGE change but leaves the p99.5-percentile — the top of
				// the histogram is still the same bright lights/sky — essentially
				// untouched. Including |avg - avg_a| in delta made a dark object a
				// false scene cut: the anchor snapped to the darker reading and the
				// whole picture reacted. The percentile is the scene-stable statistic
				// (0.003 PQ across a shot), and it is also exactly what catches the
				// earlier missed-cut case (identical averages, different peaks).
				// Spike test, not level test: a real cut is a DISCONTINUITY — this frame's
				// delta dominates its recent baseline. Occlusion (dark clothing
				// walking past the lamp) and camera reframes move the peak as a RAMP,
				// where every frame's delta is comparable to its own recent past — so
				// the spike test never fires on them, however far p99.5 wanders.
				// Measured 2026-09-22: filt_pq swung 0.4496..0.5953 inside one scene
				// because the peak-only level test snapped on every ramp.
				const float delta = m_MLActiveFrac *
					fabsf(maxPq - m_PeakMaxPq);
				m_DeltaHistory[m_DeltaIdx++] = delta;
				m_DeltaIdx &= 3;
				float deltaBase = 0.0f;
				for (int i = 0; i < 4; ++i) deltaBase = std::max(deltaBase, m_DeltaHistory[i]);

				// Zonal coherence — the SPATIAL half of the cut test. A cut is
				// globally simultaneous: most of the 5x5 zone maxes move together.
				// Occlusion (a character walking past the lamp) moves only the
				// zones they touch — local, never majority. So this frame is a cut
				// only if it ALSO clears the zone-majority gate.
				if (m_ZoneSeen) {
					int zonesMoved = 0;
					for (int z = 0; z < 25; ++z) {
						const float a = m_MLFeatures[52 + z];
						const float b = m_PrevZoneMax[z];
						if (fabsf(a - b) > 0.1f * std::max(a, b) + 0.002f) ++zonesMoved;
					}
					m_ZoneChangeFrac = (float)zonesMoved / 25.0f;
				}
				for (int z = 0; z < 25; ++z) m_PrevZoneMax[z] = m_MLFeatures[52 + z];
				m_ZoneSeen = true;

				const float meanD = fabsf(avgPq - m_PeakAvgPq);
				const bool sceneCut = (delta > kThreshHigh * kLog10Pq) &&
				                      (delta > 3.0f * deltaBase + 1e-5f) &&
				                      (m_ZoneChangeFrac > 0.50f) &&
				                      (meanD > kSceneMeanBand);

				if (sceneCut) {
					// SNAP — a real scene change arrives in ONE frame.
					m_PeakAvgPq = avgPq;
					m_PeakMaxPq = maxPq;
					m_MeanNitsSmooth = m_MLMeanNitsRaw;
					m_SceneLocked = false;
					m_SceneFrames = 0;
					for (int i = 0; i < 4; ++i) m_DeltaHistory[i] = 0.0f;
				} else if (!m_SceneLocked) {
					// SETTLE (first ~0.5 s of a scene): bounded IIR converges the
					// anchor onto the scene's true level, then locks it absolutely.
					++m_SceneFrames;
					const float settleCoeff = 1.0f - expf(-1.0f / 5.0f);
					m_PeakAvgPq += settleCoeff * (avgPq - m_PeakAvgPq);
					m_PeakMaxPq += settleCoeff * (maxPq - m_PeakMaxPq);
					if (m_MeanNitsSmooth < 0.0f) {
						m_MeanNitsSmooth = m_MLMeanNitsRaw;
					} else {
						m_MeanNitsSmooth += settleCoeff * (m_MLMeanNitsRaw - m_MeanNitsSmooth);
					}
					if (m_SceneFrames >= kSceneSettleFrames) m_SceneLocked = true;
				}
				// LOCKED — nothing else moves the anchor. A detected cut is the only
				// way back to motion; a long graded change (a dolly into light, a
				// room brightening over minutes) is intentionally held at the
				// scene's opening level — the cost of absolute stillness.

				m_HableScenePeakNits = exptm::PqToLinearNits(m_PeakMaxPq);

				// AUTO EXPOSURE. Solved here because m_PeakAvgPq snaps at cuts, settles
				// briefly and then LOCKS — exactly the cadence at which an exposure
				// change is tolerable. Per-frame it would be auto-gain; per-title it
				// would need a hand dial, which is what this replaces.
				//
				// gain = 2.2*refWhite/peak, and below the shoulder the curve is
				// identity, so the delivered average is gain*srcAvg/sdrNits in linear.
				// Solving "delivered average encodes to T" for refWhite gives the form
				// below. Validated before writing it: T=0.42 predicts ~200 for a 33-nit
				// indoor scene average (the title that needed 200 by hand) and ~66 for a
				// 100-nit outdoor one (Everest, tuned to 80 by hand).
				// m_MeanNits*, NOT PqToLinearNits(m_PeakAvgPq): the mean of PQ codes
				// decodes to a fraction of a nit, so the solver built on it never passed
				// its own >1 nit guard and silently did nothing. The extractor now
				// averages in nits over the content rows.
				if (m_bAutoRefWhite && m_fCurvePeakNits > 10.0f && m_iSDRDisplayNits > 0) {
					// The TITLE's brightest scene, not the current one. Solving per scene
					// made Everest's dark interiors raise the exposure — and refWhite is
					// also the shoulder workload, so those scenes lost their highlights.
					// Hand tuning never did that: 80 for Everest and 200 for a dark indoor
					// master are per-TITLE choices, made on each title's brightest
					// material. A running max reproduces exactly that, is monotone within
					// a title, and cannot pump at cuts.
					if (m_MeanNitsSmooth > m_MeanNitsMax) {
						m_MeanNitsMax = m_MeanNitsSmooth;
					}
					const float srcAvg = m_MeanNitsMax;
					if (srcAvg > 1.0f) {
						float want = powf(m_fRefWhiteTarget, 2.2f) * (float)m_iSDRDisplayNits
						             * m_fCurvePeakNits / (2.2f * srcAvg);
						// HARD CEILING FROM THE SHOULDER, not an arbitrary number.
						// refWhite IS the shoulder workload (2.2*refWhite/sdrNits), and the
						// shoulder only reaches Gt7Reach(alpha) x the ceiling before the
						// soft clamp flattens everything above. A solver free to demand
						// 240 on a dark scene therefore guarantees blown highlights --
						// observed, on Everest, as refW=240 and pure-white peaks. The
						// ceiling is whatever the current reach can actually carry.
						const float maxRef = Gt7Reach(m_fHdrGt7Alpha) * (float)m_iSDRDisplayNits / 2.2f;
						want = fminf(want, maxRef);
						// And a floor/ceiling for the pathological cases (black leader,
						// bogus metadata) that no formula should be allowed to answer.
						want = std::clamp(want, 40.0f, 240.0f);
						if (m_fRefWhiteAuto > 0.0f) {
							want = std::clamp(want, 0.75f * m_fRefWhiteAuto, 1.333f * m_fRefWhiteAuto);
						}
						if (fabsf(want - m_fRefWhiteAuto) > 0.5f) {
							m_fRefWhiteAuto = want;
							SetShaderLuminanceParams();
						}
					}
				}

				const float chg = m_HableScenePeakNits - m_HableScenePeakApplied;
				if (m_HableScenePeakApplied < 0.0f || chg > 1.0f || chg < -1.0f) {
					m_HableScenePeakApplied = m_HableScenePeakNits;
					SetShaderLuminanceParams();   // per-frame push; the cbuffer is
					                              // otherwise only sent on settings changes
					// Logged on PUSH so the filter can be watched: filt_pq is what the
					// curve normalises by, raw is the frame's own measurement, and mix
					// is the hysteresis coefficient — 0 = filtered, 1 = jumped.
					if (m_bMLDebug && m_pMLDebugFile) {
						fwprintf(m_pMLDebugFile,
							L"HABLE_PEAK frame=%llu raw_pq=%.4f avg_pq=%.4f filt_pq=%.4f applied_nits=%.1f delta=%.4f zones=%.2f meanD=%.4f\n",
							(unsigned long long)m_MLDebugFrame, (double)maxPq, (double)avgPq,
							(double)m_PeakMaxPq, (double)m_HableScenePeakNits, (double)delta,
							(double)m_ZoneChangeFrac, (double)meanD);
						fflush(m_pMLDebugFile);
					}
				}

			}
		}
	}

	CheckPointer(m_TexSrcVideo.pTexture, E_FAIL);
	CheckPointer(m_pDXGISwapChain1, E_FAIL);

	if (field) {
		m_FieldDrawn = field;
	}

	CComPtr<ID3D11Texture2D> pBackBuffer;
	HRESULT hr = m_pDXGISwapChain1->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::Render() : GetBuffer() failed with error {}", HR2Str(hr));
		return hr;
	}

	uint64_t tick1 = GetPreciseTick();

	if (!m_windowRect.IsRectEmpty()) {
		// fill the BackBuffer with black
		ID3D11RenderTargetView* pRenderTargetView;
		if (S_OK == m_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pRenderTargetView)) {
			const FLOAT ClearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			m_pDeviceContext->ClearRenderTargetView(pRenderTargetView, ClearColor);
			pRenderTargetView->Release();
		}
	}

	if (m_pDXGISwapChain4) {
		if (m_Dovi.bValid) {
			if (!m_hdr10.bValid && m_lastHdr10.bValid) {
				m_hdr10.bValid = true;
				m_hdr10.hdr10 = m_lastHdr10.hdr10;
			}

			if (m_hdr10.bValid) {
				if (m_DoviMaxMasteringLuminance > m_hdr10.hdr10.MaxMasteringLuminance) {
					m_hdr10.hdr10.MaxMasteringLuminance = m_DoviMaxMasteringLuminance;
				}
				if (m_DoviMinMasteringLuminance && m_DoviMinMasteringLuminance != m_hdr10.hdr10.MinMasteringLuminance) {
					m_hdr10.hdr10.MinMasteringLuminance = m_DoviMinMasteringLuminance;
				}
				if (m_DoviMaxContentLightLevel && m_DoviMaxContentLightLevel != m_hdr10.hdr10.MaxContentLightLevel) {
					m_hdr10.hdr10.MaxContentLightLevel = m_DoviMaxContentLightLevel;
				}
				if (m_DoviMaxFrameAverageLightLevel && m_DoviMaxFrameAverageLightLevel != m_hdr10.hdr10.MaxFrameAverageLightLevel) {
					m_hdr10.hdr10.MaxFrameAverageLightLevel = m_DoviMaxFrameAverageLightLevel;
				}
			}
		}

		const DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
		if (m_currentSwapChainColorSpace != colorSpace) {
			UINT colorSpaceSupport = 0;
			if (SUCCEEDED(m_pDXGISwapChain4->CheckColorSpaceSupport(colorSpace, &colorSpaceSupport))
					&& (colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) {
				hr = m_pDXGISwapChain4->SetColorSpace1(colorSpace);
				DLogIf(FAILED(hr), L"CDX11VideoProcessor::Render() : SetColorSpace1() failed with error {}", HR2Str(hr));
				if (SUCCEEDED(hr)) {
					m_currentSwapChainColorSpace = colorSpace;

					if (!m_bVPUseRTXVideoHDR) {
						if (m_hdr10.bValid) {
							if (m_bHdrPassthrough) {
								hr = m_pDXGISwapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(DXGI_HDR_METADATA_HDR10), &m_hdr10.hdr10);
								DLogIf(FAILED(hr), L"CDX11VideoProcessor::Render() : SetHDRMetaData(hdr) failed with error {}", HR2Str(hr));
							}

							m_lastHdr10 = m_hdr10;
							UpdateStatsStatic();
						} else if (m_lastHdr10.bValid) {
							if (m_bHdrPassthrough) {
								hr = m_pDXGISwapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(DXGI_HDR_METADATA_HDR10), &m_lastHdr10.hdr10);
								DLogIf(FAILED(hr), L"CDX11VideoProcessor::Render() : SetHDRMetaData(lastHdr) failed with error {}", HR2Str(hr));
							}
						} else {
							m_lastHdr10.bValid = true;

							m_lastHdr10.hdr10.RedPrimary[0] = 34000; // Display P3 primaries
							m_lastHdr10.hdr10.RedPrimary[1] = 16000;
							m_lastHdr10.hdr10.GreenPrimary[0] = 13250;
							m_lastHdr10.hdr10.GreenPrimary[1] = 34500;
							m_lastHdr10.hdr10.BluePrimary[0] = 7500;
							m_lastHdr10.hdr10.BluePrimary[1] = 3000;
							m_lastHdr10.hdr10.WhitePoint[0] = 15635;
							m_lastHdr10.hdr10.WhitePoint[1] = 16450;
							m_lastHdr10.hdr10.MaxMasteringLuminance = m_DoviMaxMasteringLuminance ? m_DoviMaxMasteringLuminance : 1000; // 1000 nits
							m_lastHdr10.hdr10.MinMasteringLuminance = m_DoviMinMasteringLuminance ? m_DoviMinMasteringLuminance : 50;   // 0.005 nits
							if (m_DoviMaxContentLightLevel) {
								m_hdr10.hdr10.MaxContentLightLevel = m_DoviMaxContentLightLevel;
							}
							if (m_DoviMaxFrameAverageLightLevel) {
								m_hdr10.hdr10.MaxFrameAverageLightLevel = m_DoviMaxFrameAverageLightLevel;
							}

							if (m_bHdrPassthrough) {
								hr = m_pDXGISwapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(DXGI_HDR_METADATA_HDR10), &m_lastHdr10.hdr10);
								DLogIf(FAILED(hr), L"CDX11VideoProcessor::Render() : SetHDRMetaData(Display P3 standard) failed with error {}", HR2Str(hr));
							}

							UpdateStatsStatic();
						}
					}
				}
			}
		} else if (m_currentSwapChainColorSpace == colorSpace && m_hdr10.bValid) {
			if (memcmp(&m_hdr10.hdr10, &m_lastHdr10.hdr10, sizeof(m_hdr10.hdr10)) != 0) {
				if (m_bHdrPassthrough) {
					hr = m_pDXGISwapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(DXGI_HDR_METADATA_HDR10), &m_hdr10.hdr10);
					DLogIf(FAILED(hr), L"CDX11VideoProcessor::Render() : SetHDRMetaData(hdr) failed with error {}", HR2Str(hr));
				}

				m_lastHdr10 = m_hdr10;
				UpdateStatsStatic();
			}
		}

		if (m_bHdrLocalToneMapping && m_currentSwapChainColorSpace == colorSpace) {
			if (m_DoviExtensionMetadata.L1.present) {
				SetHDR10ShaderParams(m_DoviExtensionMetadata.L1.min_pq, m_DoviExtensionMetadata.L1.max_pq,
									 m_DoviExtensionMetadata.L1.max_pq, m_DoviExtensionMetadata.L1.avg_pq,
									 m_iHdrDisplayMaxNits, m_iHdrLocalToneMappingType == 5 ? 6 : m_iHdrLocalToneMappingType);
			} else if (m_lastHdr10.bValid) {
				SetHDR10ShaderParams(m_lastHdr10.hdr10.MinMasteringLuminance, m_lastHdr10.hdr10.MaxMasteringLuminance,
									 m_lastHdr10.hdr10.MaxContentLightLevel, m_lastHdr10.hdr10.MaxFrameAverageLightLevel,
									 m_iHdrDisplayMaxNits, m_iHdrLocalToneMappingType);
			}
		}

		}

	if (!m_renderRect.IsRectEmpty()) {
		hr = Process(pBackBuffer, m_srcRect, m_videoRect, m_FieldDrawn == 2);
	}

	if (!m_pPSHalfOUtoInterlace) {
		DrawSubtitles(pBackBuffer);
	}

	if (m_bShowStats) {
		hr = DrawStats(pBackBuffer);
	}

	if (m_bAlphaBitmapEnable) {
		D3D11_TEXTURE2D_DESC desc;
		pBackBuffer->GetDesc(&desc);
		D3D11_VIEWPORT VP = {
			m_AlphaBitmapNRectDest.left * desc.Width,
			m_AlphaBitmapNRectDest.top * desc.Height,
			(m_AlphaBitmapNRectDest.right - m_AlphaBitmapNRectDest.left) * desc.Width,
			(m_AlphaBitmapNRectDest.bottom - m_AlphaBitmapNRectDest.top) * desc.Height,
			0.0f,
			1.0f
		};
		hr = AlphaBlt(m_TexAlphaBitmap.pShaderResource, pBackBuffer,
			m_pAlphaBitmapVertex, &VP,
			m_pSamplerLinear);
	}

#if 0
	{ // Tearing test (very non-optimal implementation, use only for tests)
		static int nTearingPos = 0;

		ID3D11RenderTargetView* pRenderTargetView;
		if (S_OK == m_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pRenderTargetView)) {
			CD3D11Rectangle d3d11rect;
			HRESULT hr2 = d3d11rect.InitDeviceObjects(m_pDevice, m_pDeviceContext);

			const SIZE szWindow = m_windowRect.Size();
			RECT rcTearing;

			rcTearing.left = nTearingPos;
			rcTearing.top = 0;
			rcTearing.right = rcTearing.left + 4;
			rcTearing.bottom = szWindow.cy;
			hr2 = d3d11rect.Set(rcTearing, szWindow, D3DCOLOR_XRGB(255, 0, 0));
			hr2 = d3d11rect.Draw(pRenderTargetView, szWindow);

			rcTearing.left = (rcTearing.right + 15) % szWindow.cx;
			rcTearing.right = rcTearing.left + 4;
			hr2 = d3d11rect.Set(rcTearing, szWindow, D3DCOLOR_XRGB(255, 0, 0));
			hr2 = d3d11rect.Draw(pRenderTargetView, szWindow);

			pRenderTargetView->Release();
			d3d11rect.InvalidateDeviceObjects();

			nTearingPos = (nTearingPos + 7) % szWindow.cx;
		}
	}
#endif

	uint64_t tick3 = GetPreciseTick();
	m_RenderStats.paintticks = tick3 - tick1;

	if (m_bVBlankBeforePresent && m_pDXGIOutput) {
		hr = m_pDXGIOutput->WaitForVBlank();
		DLogIf(FAILED(hr), L"WaitForVBlank failed with error {}", HR2Str(hr));
	}

	if (m_bAdjustPresentTime) {
		SyncFrameToStreamTime(frameStartTime);
	}

	g_bPresent = true;
	hr = m_pDXGISwapChain1->Present(1, 0);
	g_bPresent = false;
	DLogIf(FAILED(hr), L"CDX11VideoProcessor::Render() : Present() failed with error {}", HR2Str(hr));

	m_RenderStats.presentticks = GetPreciseTick() - tick3;

	if (hr == DXGI_ERROR_INVALID_CALL && m_pFilter->m_bIsD3DFullscreen) {
		InitSwapChain(false);
	}

	return hr;
}

HRESULT CDX11VideoProcessor::FillBlack()
{
	CheckPointer(m_pDXGISwapChain1, E_ABORT);

	CComPtr<ID3D11Texture2D> pBackBuffer;
	HRESULT hr = m_pDXGISwapChain1->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::FillBlack() : GetBuffer() failed with error {}", HR2Str(hr));
		return hr;
	}

	ID3D11RenderTargetView* pRenderTargetView;
	hr = m_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pRenderTargetView);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::FillBlack() : CreateRenderTargetView() failed with error {}", HR2Str(hr));
		return hr;
	}

	const FLOAT ClearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	m_pDeviceContext->ClearRenderTargetView(pRenderTargetView, ClearColor);
	pRenderTargetView->Release();

	if (m_bShowStats) {
		hr = DrawStats(pBackBuffer);
	}

	if (m_bAlphaBitmapEnable) {
		D3D11_TEXTURE2D_DESC desc;
		pBackBuffer->GetDesc(&desc);
		D3D11_VIEWPORT VP = {
			m_AlphaBitmapNRectDest.left * desc.Width,
			m_AlphaBitmapNRectDest.top * desc.Height,
			(m_AlphaBitmapNRectDest.right - m_AlphaBitmapNRectDest.left) * desc.Width,
			(m_AlphaBitmapNRectDest.bottom - m_AlphaBitmapNRectDest.top) * desc.Height,
			0.0f,
			1.0f
		};
		hr = AlphaBlt(m_TexAlphaBitmap.pShaderResource, pBackBuffer,
			m_pAlphaBitmapVertex, &VP,
			m_pSamplerLinear);
	}

	g_bPresent = true;
	hr = m_pDXGISwapChain1->Present(1, 0);
	g_bPresent = false;
	DLogIf(FAILED(hr), L"CDX11VideoProcessor::FillBlack() : Present() failed with error {}", HR2Str(hr));

	if (hr == DXGI_ERROR_INVALID_CALL && m_pFilter->m_bIsD3DFullscreen) {
		InitSwapChain(false);
	}

	return hr;
}

void CDX11VideoProcessor::UpdateTexures()
{
	if (!m_srcWidth || !m_srcHeight) {
		return;
	}

	// TODO: try making w and h a multiple of 128.
	HRESULT hr = S_OK;

	if (m_D3D11VP.IsReady()) {
		if (m_bVPScaling) {
			CSize texsize = m_videoRect.Size();
			hr = m_TexConvertOutput.CheckCreate(m_pDevice, m_D3D11OutputFmt, texsize.cx, texsize.cy, Tex2D_DefaultShaderRTarget);
			if (FAILED(hr)) {
				hr = m_TexConvertOutput.CheckCreate(m_pDevice, m_D3D11OutputFmt, m_srcRectWidth, m_srcRectHeight, Tex2D_DefaultShaderRTarget);
			}
		} else {
			hr = m_TexConvertOutput.CheckCreate(m_pDevice, m_D3D11OutputFmt, m_srcRectWidth, m_srcRectHeight, Tex2D_DefaultShaderRTarget);
		}
	}
	else {
		hr = m_TexConvertOutput.CheckCreate(m_pDevice, m_InternalTexFmt, m_srcRectWidth, m_srcRectHeight, Tex2D_DefaultShaderRTarget);
	}
}

void CDX11VideoProcessor::UpdatePostScaleTexures()
{
	const bool needDither =
		(m_SwapChainFmt == DXGI_FORMAT_B8G8R8A8_UNORM && m_InternalTexFmt != DXGI_FORMAT_B8G8R8A8_UNORM
		|| m_SwapChainFmt == DXGI_FORMAT_R10G10B10A2_UNORM && m_InternalTexFmt == DXGI_FORMAT_R16G16B16A16_FLOAT);

	m_bDitherActive = (m_bUseDither && needDither && m_TexDither.pTexture);
	// A loaded calibration LUT forces the final pass even with dithering off —
	// it is the last display-referred stage and nothing else can host it.
	m_bFinalPass = m_bDitherActive || m_bCubeLutEnabled;
	if (m_bFinalPass) {
		m_pPSFinalPass.Release();
		m_bFinalPass = SUCCEEDED(CreatePShaderFromResource(
			&m_pPSFinalPass,
			(m_SwapChainFmt == DXGI_FORMAT_R10G10B10A2_UNORM) ? IDF_PS_11_FINAL_PASS_10 : IDF_PS_11_FINAL_PASS
		));
	}

	const UINT numPostScaleSteps = GetPostScaleSteps();
	HRESULT hr = m_TexsPostScale.CheckCreate(m_pDevice, m_InternalTexFmt, m_windowRect.Width(), m_windowRect.Height(), numPostScaleSteps);
	//UpdateStatsPostProc();
}

void CDX11VideoProcessor::UpdateUpscalingShaders()
{
	m_pShaderUpscaleX.Release();
	m_pShaderUpscaleY.Release();

	if (m_iUpscaling != UPSCALE_Nearest) {
		EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pShaderUpscaleX, s_Upscaling11ResIDs[m_iUpscaling].shaderX));
		if (m_iUpscaling == UPSCALE_Jinc2) {
			m_pShaderUpscaleY = m_pShaderUpscaleX;
		} else {
			EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pShaderUpscaleY, s_Upscaling11ResIDs[m_iUpscaling].shaderY));
		}
	}

	UpdateScalingStrings();
}

void CDX11VideoProcessor::UpdateDownscalingShaders()
{
	m_pShaderDownscaleX.Release();
	m_pShaderDownscaleY.Release();

	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pShaderDownscaleX, s_Downscaling11ResIDs[m_iDownscaling].shaderX));
	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pShaderDownscaleY, s_Downscaling11ResIDs[m_iDownscaling].shaderY));

	UpdateScalingStrings();
}

HRESULT CDX11VideoProcessor::UpdateConvertColorShader()
{
	m_pPSConvertColor.Release();
	m_pPSConvertColorDeint.Release();
	ID3DBlob* pShaderCode = nullptr;

	int convertType = (m_bConvertToSdr && !(m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping))) ? SHADER_CONVERT_TO_SDR
		: (m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping) && m_srcExFmt.VideoTransferFunction == MFVideoTransFunc_HLG) ? SHADER_CONVERT_TO_PQ
		: SHADER_CONVERT_NONE;

	MediaSideDataDOVIMetadata* pDOVIMetadata = m_Dovi.bValid ? &m_Dovi.msd : nullptr;

	HRESULT hr = GetShaderConvertColor(true,
		m_srcWidth,
		m_TexSrcVideo.desc.Width, m_TexSrcVideo.desc.Height,
		m_srcRect, m_srcParams, m_srcExFmt, pDOVIMetadata,
		m_iChromaScaling, convertType, false,
		&pShaderCode);
	if (S_OK == hr) {
		hr = m_pDevice->CreatePixelShader(pShaderCode->GetBufferPointer(), pShaderCode->GetBufferSize(), nullptr, &m_pPSConvertColor);
		pShaderCode->Release();
	}

	if (m_bInterlaced && m_srcParams.Subsampling == 420 && m_srcParams.pDX11Planes) {
		hr = GetShaderConvertColor(true,
			m_srcWidth,
			m_TexSrcVideo.desc.Width, m_TexSrcVideo.desc.Height,
			m_srcRect, m_srcParams, m_srcExFmt, pDOVIMetadata,
			m_iChromaScaling, convertType, true,
			&pShaderCode);
		if (S_OK == hr) {
			hr = m_pDevice->CreatePixelShader(pShaderCode->GetBufferPointer(), pShaderCode->GetBufferSize(), nullptr, &m_pPSConvertColorDeint);
			pShaderCode->Release();
		}
	}

	if (FAILED(hr)) {
		ASSERT(0);
		UINT resid = 0;
		if (m_srcParams.cformat == CF_YUY2) {
			resid = IDF_PS_11_CONVERT_YUY2;
		}
		else if (m_srcParams.pDX11Planes) {
			if (m_srcParams.pDX11Planes->FmtPlane3) {
				if (m_srcParams.cformat == CF_YV12 || m_srcParams.cformat == CF_YV16 || m_srcParams.cformat == CF_YV24) {
					resid = IDF_PS_11_CONVERT_PLANAR_YV;
				} else {
					resid = IDF_PS_11_CONVERT_PLANAR;
				}
			} else {
				resid = IDF_PS_11_CONVERT_BIPLANAR;
			}
		}
		else {
			resid = IDF_PS_11_CONVERT_COLOR;
		}
		EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSConvertColor, resid));

		return S_FALSE;
	}

	return hr;
}

void CDX11VideoProcessor::UpdateBitmapShader()
{
	if (m_bHdrDisplayModeEnabled
			&& (SourceIsHDR() || m_bVPUseRTXVideoHDR)) {
		UINT resid;
		float SDR_peak_lum;
		switch (m_iHdrOsdBrightness) {
		default:
			resid = IDF_PS_11_CONVERT_BITMAP_TO_PQ;
			SDR_peak_lum = 100;
			break;
		case 1:
			resid = IDF_PS_11_CONVERT_BITMAP_TO_PQ1;
			SDR_peak_lum = 50;
			break;
		case 2:
			resid = IDF_PS_11_CONVERT_BITMAP_TO_PQ2;
			SDR_peak_lum = 30;
			break;
		}
		m_pPS_BitmapToFrame.Release();
		EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPS_BitmapToFrame, resid));
		m_dwStatsTextColor = TransferPQ(D3DCOLOR_XRGB(255, 255, 255), SDR_peak_lum);
	}
	else {
		m_pPS_BitmapToFrame = m_pPS_Simple;
		m_dwStatsTextColor = D3DCOLOR_XRGB(255, 255, 255);
	}
}

HRESULT CDX11VideoProcessor::D3D11VPPass(ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect, const bool second)
{
	HRESULT hr = m_D3D11VP.SetRectangles(srcRect, dstRect);

	hr = m_D3D11VP.Process(pRenderTarget, m_SampleFormat, second);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::ProcessD3D11() : m_D3D11VP.Process() failed with error {}", HR2Str(hr));
	}

	return hr;
}

HRESULT CDX11VideoProcessor::ConvertColorPass(ID3D11Texture2D* pRenderTarget)
{
	CComPtr<ID3D11RenderTargetView> pRenderTargetView;

	HRESULT hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
	if (FAILED(hr)) {
		DLog(L"ConvertColorPass() : CreateRenderTargetView() failed with error {}", HR2Str(hr));
		return hr;
	}

	D3D11_VIEWPORT VP;
	VP.TopLeftX = 0;
	VP.TopLeftY = 0;
	VP.Width = (FLOAT)m_TexConvertOutput.desc.Width;
	VP.Height = (FLOAT)m_TexConvertOutput.desc.Height;
	VP.MinDepth = 0.0f;
	VP.MaxDepth = 1.0f;

	const UINT Stride = sizeof(VERTEX);
	const UINT Offset = 0;

	// Set resources
	m_pDeviceContext->IASetInputLayout(m_pVSimpleInputLayout);
	m_pDeviceContext->OMSetRenderTargets(1, &pRenderTargetView.p, nullptr);
	m_pDeviceContext->RSSetViewports(1, &VP);
	m_pDeviceContext->OMSetBlendState(nullptr, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
	m_pDeviceContext->VSSetShader(m_pVS_Simple, nullptr, 0);
	if (m_bDeintBlend && m_SampleFormat != D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE && m_pPSConvertColorDeint) {
		m_pDeviceContext->PSSetShader(m_pPSConvertColorDeint, nullptr, 0);
	} else {
		m_pDeviceContext->PSSetShader(m_pPSConvertColor, nullptr, 0);
	}
	m_pDeviceContext->PSSetShaderResources(0, 1, &m_TexSrcVideo.pShaderResource.p);
	m_pDeviceContext->PSSetShaderResources(1, 1, &m_TexSrcVideo.pShaderResource2.p);
	m_pDeviceContext->PSSetShaderResources(2, 1, &m_TexSrcVideo.pShaderResource3.p);
	m_pDeviceContext->PSSetSamplers(0, 1, &m_pSamplerPoint.p);
	m_pDeviceContext->PSSetSamplers(1, 1, &m_pSamplerLinear.p);
	m_pDeviceContext->PSSetConstantBuffers(0, 1, &m_PSConvColorData.pConstants);
	m_pDeviceContext->PSSetConstantBuffers(1, 1, &m_pCorrectionConstants.p);
	m_pDeviceContext->PSSetConstantBuffers(2, 1, &m_pDoviCurvesConstantBuffer.p);
	if (m_pDoViDynamicConstants) {
		m_pDeviceContext->PSSetConstantBuffers(3, 1, &m_pDoViDynamicConstants.p);
	}
	m_pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	m_pDeviceContext->IASetVertexBuffers(0, 1, &m_PSConvColorData.pVertexBuffer, &Stride, &Offset);

	// Draw textured quad onto render target
	m_pDeviceContext->Draw(4, 0);

	ID3D11ShaderResourceView* views[3] = {};
	m_pDeviceContext->PSSetShaderResources(0, 3, views);

	return hr;
}

HRESULT CDX11VideoProcessor::ResizeShaderPass(const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect, const int rotation)
{
	HRESULT hr = S_OK;
	const int w2 = dstRect.Width();
	const int h2 = dstRect.Height();
	const int k = m_bInterpolateAt50pct ? 2 : 1;

	int w1, h1;
	ID3D11PixelShader* resizerX;
	ID3D11PixelShader* resizerY;
	if (rotation == 90 || rotation == 270) {
		w1 = srcRect.Height();
		h1 = srcRect.Width();
		resizerX = (w1 == w2) ? nullptr : (w1 > k * w2) ? m_pShaderDownscaleY.p : m_pShaderUpscaleY.p; // use Y scaling here
		if (resizerX) {
			resizerY = (h1 == h2) ? nullptr : (h1 > k * h2) ? m_pShaderDownscaleY.p : m_pShaderUpscaleY.p;
		} else {
			resizerY = (h1 == h2) ? nullptr : (h1 > k * h2) ? m_pShaderDownscaleX.p : m_pShaderUpscaleX.p; // use X scaling here
		}
	} else {
		w1 = srcRect.Width();
		h1 = srcRect.Height();
		resizerX = (w1 == w2) ? nullptr : (w1 > k * w2) ? m_pShaderDownscaleX.p : m_pShaderUpscaleX.p;
		resizerY = (h1 == h2) ? nullptr : (h1 > k * h2) ? m_pShaderDownscaleY.p : m_pShaderUpscaleY.p;
	}

	if (resizerX && resizerY) {
		// two pass resize

		D3D11_TEXTURE2D_DESC desc;
		pRenderTarget->GetDesc(&desc);

		if (resizerX == resizerY) {
			// one pass resize
			hr = TextureResizeShader(Tex, pRenderTarget, srcRect, dstRect, resizerX, rotation, m_bFlip);
			DLogIf(FAILED(hr), L"CDX11VideoProcessor::ResizeShaderPass() : failed with error {}", HR2Str(hr));

			return hr;
		}

		// check intermediate texture
		const UINT texWidth  = desc.Width;
		const UINT texHeight = h1;

		if (m_TexResize.pTexture) {
			if (texWidth != m_TexResize.desc.Width || texHeight != m_TexResize.desc.Height) {
				m_TexResize.Release(); // need new texture
			}
		}

		if (!m_TexResize.pTexture) {
			// use only float textures here
			hr = m_TexResize.Create(m_pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, texWidth, texHeight, Tex2D_DefaultShaderRTarget);
			if (FAILED(hr)) {
				DLog(L"CDX11VideoProcessor::ResizeShaderPass() : m_TexResize.Create() failed with error {}", HR2Str(hr));
				return hr;
			}
		}

		CRect resizeRect(dstRect.left, 0, dstRect.right, texHeight);

		// First resize pass
		hr = TextureResizeShader(Tex, m_TexResize.pTexture, srcRect, resizeRect, resizerX, rotation, m_bFlip);
		// Second resize pass
		hr = TextureResizeShader(m_TexResize, pRenderTarget, resizeRect, dstRect, resizerY, 0, false);
	}
	else {
		if (resizerX) {
			// one pass resize for width
			hr = TextureResizeShader(Tex, pRenderTarget, srcRect, dstRect, resizerX, rotation, m_bFlip);
		}
		else if (resizerY) {
			// one pass resize for height
			hr = TextureResizeShader(Tex, pRenderTarget, srcRect, dstRect, resizerY, rotation, m_bFlip);
		}
		else {
			// no resize
			hr = TextureCopyRect(Tex, pRenderTarget, srcRect, dstRect, m_pPS_Simple, nullptr, rotation, m_bFlip);
		}
	}

	DLogIf(FAILED(hr), L"CDX11VideoProcessor::ResizeShaderPass() : failed with error {}", HR2Str(hr));

	return hr;
}

HRESULT CDX11VideoProcessor::FinalPass(const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect)
{
	// DELIVERED instrument, boundary B: `Tex` is the post-grade frame — after the
	// Zion shadow/highlight/gamma steps, before the calibration LUT and the dither
	// this pass adds. Sampled FIRST, before anything here binds its own resources,
	// so the downsample cannot disturb the final blit's slot layout.
	if (m_bMLDebug) {
		DownsampleDelivered(Tex, m_MLDeliveredPostGrade.p);
		m_bDeliveredB = true;
	}

	CComPtr<ID3D11RenderTargetView> pRenderTargetView;

	HRESULT hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::FinalPass() : CreateRenderTargetView() failed with error {}", HR2Str(hr));
		return hr;
	}

	hr = FillVertexBuffer(m_pDeviceContext, m_pVertexBuffer, Tex.desc.Width, Tex.desc.Height, srcRect, 0, false);
	if (FAILED(hr)) {
		return hr;
	}

	const FLOAT constants[8] = {
		(float)Tex.desc.Width / dither_size, (float)Tex.desc.Height / dither_size,
		m_bCubeLutEnabled ? 1.0f : 0.0f, (float)m_iCubeLutSize,
		m_bDitherActive ? 1.0f : 0.0f,
		(float)m_iCubeLutMode,
		0, 0
	};
	D3D11_MAPPED_SUBRESOURCE mr;
	hr = m_pDeviceContext->Map(m_pFinalPassConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::FinalPass() : Map() failed with error {}", HR2Str(hr));
		return hr;
	}

	memcpy(mr.pData, &constants, sizeof(constants));
	m_pDeviceContext->Unmap(m_pFinalPassConstantBuffer, 0);

	D3D11_VIEWPORT VP;
	VP.TopLeftX = (FLOAT)dstRect.left;
	VP.TopLeftY = (FLOAT)dstRect.top;
	VP.Width    = (FLOAT)dstRect.Width();
	VP.Height   = (FLOAT)dstRect.Height();
	VP.MinDepth = 0.0f;
	VP.MaxDepth = 1.0f;

	// Set resources
	m_pDeviceContext->PSSetShaderResources(1, 1, &m_TexDither.pShaderResource.p);
	m_pDeviceContext->PSSetSamplers(1, 1, &m_pSamplerDither.p);

	// Display calibration LUT on t2/s2. Bound only when loaded; the shader guards
	// on the enable flag, so an unbound slot is never sampled.
	const bool bLutBound = m_bCubeLutEnabled && m_pCubeLutSRV && m_pCubeLutSampler;
	if (bLutBound) {
		m_pDeviceContext->PSSetShaderResources(2, 1, &m_pCubeLutSRV.p);
		m_pDeviceContext->PSSetSamplers(2, 1, &m_pCubeLutSampler.p);
	}

	TextureBlt11(m_pDeviceContext, pRenderTargetView, VP, m_pVSimpleInputLayout, m_pVS_Simple, m_pPSFinalPass, Tex.pShaderResource, m_pSamplerPoint, m_pFinalPassConstantBuffer, m_pVertexBuffer);

	ID3D11ShaderResourceView* views[1] = {};
	m_pDeviceContext->PSSetShaderResources(1, 1, views);
	if (bLutBound) {
		m_pDeviceContext->PSSetShaderResources(2, 1, views);
	}

	return hr;
}

void CDX11VideoProcessor::DrawSubtitles(ID3D11Texture2D* pRenderTarget)
{
	HRESULT hr = S_OK;

	CComPtr<ISubPic> pSubPic = m_pFilter->GetSubPic(m_rtStart);
	if (pSubPic) {
		RECT rcSource, rcDest;
		hr = pSubPic->GetSourceAndDest(m_windowRect, m_videoRect, &rcSource, &rcDest, FALSE, {}, 0, FALSE);
		if (SUCCEEDED(hr)) {
			ID3D11RenderTargetView* pRenderTargetView;
			hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
			if (SUCCEEDED(hr)) {
				// Set render target and shaders
				m_pDeviceContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);
				m_pDeviceContext->IASetInputLayout(m_pVSimpleInputLayout);
				m_pDeviceContext->VSSetShader(m_pVS_Simple, nullptr, 0);
				m_pDeviceContext->PSSetShader(m_pPS_BitmapToFrame, nullptr, 0);

				// call the function for drawing subtitles
				hr = pSubPic->AlphaBlt(&rcSource, &rcDest, nullptr);

				pRenderTargetView->Release();
			}
		}
		return;
	}

	if (m_pFilter->m_pSub11CallBack) {
		ID3D11RenderTargetView* pRenderTargetView;
		hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
		if (SUCCEEDED(hr)) {
			const CRect rSrcPri(POINT(0, 0), m_windowRect.Size());
			const CRect rDstVid(m_videoRect);
			const auto rtStart = m_pFilter->m_rtStartTime + m_rtStart;

			// Set render target and shaders
			m_pDeviceContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);
			m_pDeviceContext->IASetInputLayout(m_pVSimpleInputLayout);
			m_pDeviceContext->VSSetShader(m_pVS_Simple, nullptr, 0);
			m_pDeviceContext->PSSetShader(m_pPS_BitmapToFrame, nullptr, 0);

			// call the function for drawing subtitles
			hr = m_pFilter->m_pSub11CallBack->Render11(rtStart, 0, m_rtAvgTimePerFrame, rDstVid, rDstVid, rSrcPri,
													   1., m_iStereo3dTransform == 1 ? m_nStereoSubtitlesOffsetInPixels : 0);

			pRenderTargetView->Release();
		}
	}
}

HRESULT CDX11VideoProcessor::Process(ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect, const bool second)
{
	HRESULT hr = S_OK;
	m_bDitherUsed = false;
	int rotation = m_iRotation;

	CRect rSrc = srcRect;
	Tex2D_t* pInputTexture = nullptr;

	const UINT numSteps = GetPostScaleSteps();

	if (m_D3D11VP.IsReady()) {
		if (!(m_iSwapEffect == SWAPEFFECT_Discard && (m_VendorId == PCIV_AMDATI || m_VendorId == PCIV_INTEL))) {
			const bool bNeedShaderTransform =
				(m_TexConvertOutput.desc.Width != dstRect.Width() || m_TexConvertOutput.desc.Height != dstRect.Height() || m_bFlip
				|| dstRect.right > m_windowRect.right || dstRect.bottom > m_windowRect.bottom)
				|| (m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping || m_bMLActive)); // At least on Nvidia we can sometimes get the "D3D11: Removing Device" error here when HDR Passthrough.
			if (!bNeedShaderTransform && !numSteps) {
				m_bVPScalingUseShaders = false;
				hr = D3D11VPPass(pRenderTarget, rSrc, dstRect, second);

				return hr;
			}
		}

		CRect rect(0, 0, m_TexConvertOutput.desc.Width, m_TexConvertOutput.desc.Height);
		hr = D3D11VPPass(m_TexConvertOutput.pTexture, rSrc, rect, second);
		pInputTexture = &m_TexConvertOutput;
		rSrc = rect;
		rotation = 0;
	}
	else if (m_PSConvColorData.bEnable) {
		ConvertColorPass(m_TexConvertOutput.pTexture);
		pInputTexture = &m_TexConvertOutput;
		rSrc.SetRect(0, 0, m_TexConvertOutput.desc.Width, m_TexConvertOutput.desc.Height);
	}
	else {
		pInputTexture = &m_TexSrcVideo;
	}

	if (numSteps) {
		m_MLDebugStepCount = numSteps;

		UINT step = 0;
		Tex2D_t* pTex = m_TexsPostScale.GetFirstTex();
		ID3D11Texture2D* pRT = pTex->pTexture;

		auto StepSetting = [&]() {
			step++;
			pInputTexture = pTex;
			if (step < numSteps) {
				pTex = m_TexsPostScale.GetNextTex();
				pRT = pTex->pTexture;
			} else {
				pRT = pRenderTarget;
			}
		};

		CRect rect;
		rect.IntersectRect(dstRect, CRect(0, 0, pTex->desc.Width, pTex->desc.Height));

		if (m_D3D11VP.IsReady()) {
			m_bVPScalingUseShaders = rSrc.Width() != dstRect.Width() || rSrc.Height() != dstRect.Height();
		}

		if (rSrc != dstRect || rotation != 0) {
			hr = ResizeShaderPass(*pInputTexture, pRT, rSrc, dstRect, rotation);
		} else {
			pTex = pInputTexture; // Hmm
		}

		if (m_pPSCorrection) {
			// Raw HDR/DV passthrough also bypasses the correction step (the
			// 10000/sdrNits scale) so the SDR display gets the UNconverted signal.
			StepSetting();

			// Experimental tone curve on t1/s1. TextureCopyRect only binds t0/s0 and
			// b0, so anything bound here survives into the draw — the same trick the
			// final pass uses for its dither and cube LUT. Unbound when inactive: the
			// shader branches on toneLutEnable, so a disabled mapper never samples it.
			const bool bToneLut = !m_bHDRPassthroughRaw && ToneLutActive();
			if (bToneLut) {
				m_pDeviceContext->PSSetShaderResources(1, 1, &m_pToneLutSRV.p);
				m_pDeviceContext->PSSetSamplers(1, 1, &m_pToneLutSampler.p);
			}

			hr = TextureCopyRect(*pInputTexture, pRT, rect, rect,
				m_bHDRPassthroughRaw ? m_pPS_Simple.p : m_pPSCorrection.p,
				m_bHDRPassthroughRaw ? nullptr : m_pCorrectionConstants, 0, false);

			if (bToneLut) {
				ID3D11ShaderResourceView* none[1] = {};
				m_pDeviceContext->PSSetShaderResources(1, 1, none);
			}

			// DELIVERED instrument, boundary A: the frame the tone curve just wrote.
			// StepSetting advanced pTex to the chain texture this step rendered INTO,
			// so *pTex is the correction's output — unless the correction is the last
			// step and drew straight to the swap chain, in which case there is no
			// texture to sample and boundary A reports n/a.
			if (m_bMLDebug && pRT != pRenderTarget) {
				DownsampleDelivered(*pTex, m_MLDeliveredCurveOut.p);
				m_bDeliveredA = true;
			}

			// [CBPROBE] read-back, armed by every [SHADER-CB] push. Nothing rebound
			// b0 between the draw above and this query, so this is the state the
			// correction pixel shader drew with.
			if (++m_uCorrectionDraws % 300 == 1) {
				DLog(L"renderer: [CBPROBE] alive: correction draw #{} (raw-passthrough={})",
					m_uCorrectionDraws, m_bHDRPassthroughRaw ? 1 : 0);
			}
			if (InterlockedCompareExchange(&m_iCbProbeTicks, 0, 0) > 0) {
				InterlockedDecrement(&m_iCbProbeTicks);
				ID3D11Buffer* pBound = nullptr;
				m_pDeviceContext->PSGetConstantBuffers(0, 1, &pBound);
				const bool bSame = pBound == (ID3D11Buffer*)m_pCorrectionConstants;
				float f0 = 0, f18 = 0, f20 = 0, f8 = 0, f21 = 0, f19 = 0, f13 = 0, f9 = 0, f15 = 0;
				D3D11_BUFFER_DESC bd = {};
				if (pBound) {
					pBound->GetDesc(&bd);
				}
				// Failure codes are LOGGED, never silent: a zero read without a
				// failed hr below means the GPU buffer really holds zero.
				HRESULT hrStage = S_OK, hrMap = 0x1337C0DE; // sentinel: map never attempted
				if (pBound && bd.ByteWidth >= sizeof(FLOAT) * 24) {
					if (!m_pCbProbeStaging) {
						const D3D11_BUFFER_DESC sd = {
							.ByteWidth        = sizeof(FLOAT) * 24,
							.Usage            = D3D11_USAGE_STAGING,
							.BindFlags        = 0,
							.CPUAccessFlags   = D3D11_CPU_ACCESS_READ,
							.MiscFlags        = 0,
							.StructureByteStride = 0,
						};
						hrStage = m_pDevice->CreateBuffer(&sd, nullptr, &m_pCbProbeStaging);
						if (FAILED(hrStage)) {
							m_pCbProbeStaging = nullptr;
						}
					}
					if (m_pCbProbeStaging) {
						m_pDeviceContext->CopyResource(m_pCbProbeStaging, pBound);
						D3D11_MAPPED_SUBRESOURCE ms = {};
						hrMap = m_pDeviceContext->Map(m_pCbProbeStaging, 0, D3D11_MAP_READ, 0, &ms);
						if (SUCCEEDED(hrMap)) {
							const float* pf = (const float*)ms.pData;
							f0 = pf[0]; f18 = pf[18]; f20 = pf[20];
							// [8] the curve selection and [21] the GT7 blend, read back
							// from the buffer the draw actually had bound: this is what
							// proves "GT7 ran" rather than "GT7 was asked for".
							f8 = pf[8]; f21 = pf[21];
							// [19] refWhite and [13] the scene peak — the two inputs to
							// the GT7 adapted gain. If the picture is "blown" the first
							// suspect is one of these being a value we did not intend:
							// refWhite=0 (silent, fallback to 1.0) or sPk<=10 (silent,
							// fallback to maxCLL) would both change the gain ~10x.
							f19 = pf[19]; f13 = pf[13];
							// [9] the maxCLL the shader actually divides by, and [15] the
							// softKnee pre-compressor gate. The GT7 branch now reads slot
							// 9 directly as its scene peak; on the file path m_lastHdr10
							// may not populate, and the CPU falls back (<=10 -> mMax ->
							// <=10 -> titlePeak clamped to as low as 100) — which would
							// make cbuffer[9] ~100 instead of 991 and blow the gain 10x.
							f9 = pf[9]; f15 = pf[15];
							m_pDeviceContext->Unmap(m_pCbProbeStaging, 0);
						}
					}
				}
				DLog(L"renderer: [CBPROBE] correction draw: bound=0x{:x} updated=0x{:x} same={} b0size={} b0usage={} stageHr={:x} mapHr={:x} LuminanceScale read={:.4f} expect={:.4f} hlN read={:.2f} clE read={:.4f} curve read={:.0f} gtB read={:.2f} refW read={:.1f} sPk read={:.1f} mCLL read={:.1f} knee read={:.2f}",
					(uintptr_t)pBound, (uintptr_t)(ID3D11Buffer*)m_pCorrectionConstants, bSame ? 1 : 0, bd.ByteWidth, (int)bd.Usage,
					(UINT)hrStage, (UINT)hrMap, f0, 10000.0f / m_iSDRDisplayNits, f18, f20, f8, f21, f19, f13, f9, f15);
				if (pBound) {
					pBound->Release();
				}
			}
		}

		if (m_pPSHDR10ToneMapping) {
			// Raw HDR/DV passthrough: keep the pipeline (and the ring step count)
			// intact but bind the copy shader so no tone compression happens —
			// the SDR display then shows the unprocessed HDR/DV signal.
			StepSetting();

			if (m_pDoViDynamicConstants && !m_bHDRPassthroughRaw) {
				m_pDeviceContext->PSSetConstantBuffers(1, 1, &m_pDoViDynamicConstants.p);
			}

			hr = TextureCopyRect(*pInputTexture, pRT, rect, rect,
				m_bHDRPassthroughRaw ? m_pPS_Simple.p : m_pPSHDR10ToneMapping.p,
				m_bHDRPassthroughRaw ? nullptr : m_pHDR10ToneMappingConstants, 0, false);
		}

		if (m_bMLActive && m_pPSMLGamma && m_pMLParams) {
			// Zion ML grade — hook order matches libplacebo: shadow -> highlight -> gamma
			// MLDebugPassthrough: bind the proven copy shader to all three steps
			// to bisect "bad ML shader math" vs "broken step plumbing".
			// Raw HDR/DV passthrough also copies every ML step (ring stays intact).
			const bool bCopy = m_bMLDebugCopy || m_bHDRPassthroughRaw;
			ID3D11PixelShader* psShadow    = bCopy ? m_pPS_Simple.p : m_pPSMLShadow.p;
			ID3D11PixelShader* psHighlight = bCopy ? m_pPS_Simple.p : m_pPSMLHighlight.p;
			ID3D11PixelShader* psGamma     = bCopy ? m_pPS_Simple.p : m_pPSMLGamma.p;

			// Remap face box UVs from convertTex [0,1] to post-scale shader UV space.
			// The shader's input.Tex spans [rect.left/inputW .. rect.right/inputW],
			// not [0..1], when the video is pillarboxed/letterboxed in a larger surface.
			const float inputW = (float)pInputTexture->desc.Width;
			const float inputH = (float)pInputTexture->desc.Height;
			const float uvOffX = (float)rect.left / inputW;
			const float uvOffY = (float)rect.top / inputH;
			const float uvScX  = (float)rect.Width() / inputW;
			const float uvScY  = (float)rect.Height() / inputH;
			// Always refresh the per-frame cbuffer: face boxes (if any) remap into
			// window UV AND the sky mask's UV remap follows the same rect.
			m_lastMLParams.MLSkyUVOffX = uvOffX;
			m_lastMLParams.MLSkyUVOffY = uvOffY;
			m_lastMLParams.MLSkyUVScX = uvScX;
			m_lastMLParams.MLSkyUVScY = uvScY;
			if (m_lastMLParams.NumFaces > 0 && (uvOffX > 0.001f || uvOffY > 0.001f || uvScX < 0.999f || uvScY < 0.999f)) {
				for (int i = 0; i < m_lastMLParams.NumFaces && i < 4; i++) {
					float& bx = m_lastMLParams.FaceBox[i * 4 + 0];
					float& by = m_lastMLParams.FaceBox[i * 4 + 1];
					float& bw = m_lastMLParams.FaceBox[i * 4 + 2];
					float& bh = m_lastMLParams.FaceBox[i * 4 + 3];
					bx = uvOffX + bx * uvScX;
					by = uvOffY + by * uvScY;
					bw = bw * uvScX;
					bh = bh * uvScY;
				}
			}
			m_pDeviceContext->UpdateSubresource(m_pMLParams, 0, nullptr, &m_lastMLParams, 0, 0);

			HRESULT hrs[4] = {};
			if (m_bMLDebug && m_pMLDebugFile && m_MLDebugFrame % 60 == 0) {
				fwprintf(m_pMLDebugFile, L"MLPASS rect=(%d,%d,%d,%d) inputTex=%ux%u convertTex=%ux%u uvOff=(%.4f,%.4f) uvSc=(%.4f,%.4f)",
					rect.left, rect.top, rect.right, rect.bottom,
					pInputTexture->desc.Width, pInputTexture->desc.Height,
					m_TexConvertOutput.desc.Width, m_TexConvertOutput.desc.Height,
					(double)uvOffX, (double)uvOffY, (double)uvScX, (double)uvScY);
				// The exact UVs the per-frame cbuffer refresh just pushed for the
				// sky mask — the shader computes cuv=(input.Tex-uvOff)/uvSc.
				fwprintf(m_pMLDebugFile, L" SKYUV=(%.4f,%.4f,%.4f,%.4f) sfrac=%.4f pfrac=%.4f luma=%.3f pDbg=%.0f pStr=%.3f\n",
					(double)m_lastMLParams.MLSkyUVOffX, (double)m_lastMLParams.MLSkyUVOffY,
					(double)m_lastMLParams.MLSkyUVScX, (double)m_lastMLParams.MLSkyUVScY,
					(double)m_SkyFrac, (double)m_PersonFrac, (double)m_SkyLuma,
					(double)m_lastMLParams.MLSkinPersonDebugVis, (double)m_lastMLParams.MLSkinPersonStrength);
				if (m_lastMLParams.NumFaces > 0) {
					for (int i = 0; i < m_lastMLParams.NumFaces && i < 4; i++)
						fwprintf(m_pMLDebugFile, L"  FACE_REMAP[%d] x=%.4f y=%.4f w=%.4f h=%.4f\n",
							i, (double)m_lastMLParams.FaceBox[i*4], (double)m_lastMLParams.FaceBox[i*4+1],
							(double)m_lastMLParams.FaceBox[i*4+2], (double)m_lastMLParams.FaceBox[i*4+3]);
				}
				fflush(m_pMLDebugFile);
			}
			if (m_bMLDebug && m_pMLDebugFile) {
				fwprintf(m_pMLDebugFile, L"MLPASS enter step=%u\n", (unsigned)(step + 1));
			}

			// Order: highlight (passthrough) → gamma tone curve → micro-contrast
			// — the MC pass runs on the post-tone-curve I_norm (what you see).
			StepSetting();
			hrs[0] = TextureCopyRect(*pInputTexture, pRT, rect, rect, psHighlight, m_bMLDebugCopy ? nullptr : m_pMLParams, 0, false);
			StepSetting();
			// Sky/person mask (640x360 R8G8: R=sky, G=person) + sampler bound on t2/s2 for
			// the gamma + MC passes; chroma re-binds t2 for the person channel below.
			if (m_SkyMaskSRV && m_pSamplerSky) {
				m_pDeviceContext->PSSetShaderResources(2, 1, &m_SkyMaskSRV.p);
				m_pDeviceContext->PSSetSamplers(2, 1, &m_pSamplerSky.p);
			}
			// Seg preview = the PRESENTED content rect (the same geometry cuv samples in
			// the shaders). Keying the mask to the presentation — not the full source
			// convert — keeps it aligned under player crop/zoom/pillarbox on any display
			// aspect (e.g. ultrawide 2.39:1 with a 2.39 picture auto-cropped to fill).
			// Staged in the next ExtractMLFeatures (1-frame latency, pre-existing).
			{
				const bool wantPers = m_bPersonEnabled || m_bPersonDebugVis;
				const bool wantSky  = m_bSkyEnabled || m_bSkyDebugVis;
				if ((wantPers || wantSky) && m_SegDownsample && m_pPSFaceDown && m_pFaceDownParams &&
					rect.right > rect.left && rect.bottom > rect.top) {
					const CRect segRect(0, 0, 640, 360);
					TextureCopyRect(*pInputTexture, m_SegDownsample, rect, segRect, m_pPSFaceDown, m_pFaceDownParams, 0, false);
				}
			}
			// Skin tone-preservation: pre-grade source (t3) for srcLumAt + the
			// Poisson-relaxed person×skin gate (t4). Unbound right after the gamma
			// draw; t2 stays for MC person .g.
			if (m_TexConvertOutput.pShaderResource && m_pSamplerSky) {
				m_pDeviceContext->PSSetShaderResources(3, 1, &m_TexConvertOutput.pShaderResource.p);
				m_pDeviceContext->PSSetSamplers(3, 1, &m_pSamplerSky.p);
			}
			if (m_SkinGateSRV && m_pSamplerGate) {
				m_pDeviceContext->PSSetShaderResources(4, 1, &m_SkinGateSRV.p);
				m_pDeviceContext->PSSetSamplers(4, 1, &m_pSamplerGate.p);
			}
			// Trained P(skin) LUT on t1/s1 for the gamma pass's skin-gated top-up —
			// the SAME LUT the chroma pass uses, sampled with the same derivation so
			// both passes agree on what counts as skin. t1 is free in this pass:
			// t0=tex, t2=sky/person mask, t3=source, t4=gate.
			if (m_SkinLUTSRV && m_pSamplerSkin) {
				m_pDeviceContext->PSSetShaderResources(1, 1, &m_SkinLUTSRV.p);
				m_pDeviceContext->PSSetSamplers(1, 1, &m_pSamplerSkin.p);
			}
			hrs[1] = TextureCopyRect(*pInputTexture, pRT, rect, rect, psGamma, m_bMLDebugCopy ? nullptr : m_pMLParams, 0, false);
			{
				ID3D11ShaderResourceView* nul = nullptr;   // unbind t1 (skin LUT), t3 (source), t4 (gate); t2 stays for MC
				m_pDeviceContext->PSSetShaderResources(1, 1, &nul);
				m_pDeviceContext->PSSetShaderResources(3, 1, &nul);
				m_pDeviceContext->PSSetShaderResources(4, 1, &nul);
			}
			if (m_bMLDebug && m_pMLDebugFile && m_MLDebugFrame % 60 == 0) {
				fwprintf(m_pMLDebugFile,
					L"GAMMA_DRAW shader=%s t2=%s pDbg=%.0f pStr=%.3f skyS=%.3f skyG=%.3f sfrac=%.3f pfrac=%.3f\n",
					(m_bMLDebugCopy ? L"copy" : L"gamma"),
					(m_SkyMaskSRV ? L"mask" : L"NULL"),
					(double)m_lastMLParams.MLSkinPersonDebugVis,
					(double)m_lastMLParams.MLSkinPersonStrength,
					(double)m_lastMLParams.MLSkyStrength,
					(double)m_lastMLParams.MLSkyGuard,
					(double)m_SkyFrac, (double)m_PersonFrac);
				fflush(m_pMLDebugFile);
			}
			StepSetting();
			// MC pass: skin LUT (t1) for person-gated skin damping; the R8G8 semseg
			// mask stays on t2 from the gamma step (person .g damping only — the
			// SKY channel was removed from MC in skyMC-decouple-v3).
			if (m_SkinLUTSRV && m_pSamplerSkin) {
				m_pDeviceContext->PSSetShaderResources(1, 1, &m_SkinLUTSRV.p);
				m_pDeviceContext->PSSetSamplers(1, 1, &m_pSamplerSkin.p);
			}
			hrs[2] = TextureCopyRect(*pInputTexture, pRT, rect, rect, psShadow, m_bMLDebugCopy ? nullptr : m_pMLParams, 0, false);
			if (m_bMLDebug && m_pMLDebugFile && m_MLDebugFrame % 60 == 0) {
				fwprintf(m_pMLDebugFile,
					L"MC_DRAW shader=%s lut=%s maskT2=%s mcStr=%.3f mcProtect=%.3f pStr=%.3f skyS=%.3f\n",
					(m_bMLDebugCopy ? L"copy" : L"shadow"),
					(m_SkinLUTSRV ? L"skin" : L"NULL"),
					(m_SkyMaskSRV ? L"mask" : L"NULL"),
					(double)m_lastMLParams.MCStrength,
					(double)m_lastMLParams.MCSkinProtect,
					(double)m_lastMLParams.MLSkinPersonStrength,
					(double)m_lastMLParams.MLSkyStrength);
				fflush(m_pMLDebugFile);
			}
			{
				ID3D11ShaderResourceView* nullSRVs[2] = {};
				m_pDeviceContext->PSSetShaderResources(1, 2, nullSRVs);
			}

			// log luma stats of the gamma-step INPUT (pre-grade content) every 8th frame
			if (m_bMLDebug && m_bMLDebugCopy == false && m_MLDebugFrame % 8 == 0) {
				LogMLFrameStats(*pInputTexture, L"IN");
			}

			// 4. chroma vector tuning (skin-guarded) — LUT on t1, semseg mask on t2 (person .g)
			hrs[3] = S_OK;
			if (m_pPSMLChroma && m_SkinLUTSRV) {
				StepSetting();
				m_pDeviceContext->PSSetShaderResources(1, 1, &m_SkinLUTSRV.p);
				m_pDeviceContext->PSSetSamplers(1, 1, &m_pSamplerSkin.p);
				if (m_SkyMaskSRV && m_pSamplerSky) {
					m_pDeviceContext->PSSetShaderResources(2, 1, &m_SkyMaskSRV.p);
					m_pDeviceContext->PSSetSamplers(2, 1, &m_pSamplerSky.p);
				}
				hrs[3] = TextureCopyRect(*pInputTexture, pRT, rect, rect,
					bCopy ? m_pPS_Simple.p : m_pPSMLChroma.p,
					m_bMLDebugCopy ? nullptr : m_pMLParams, 0, false);
				if (m_bMLDebug && m_pMLDebugFile && m_MLDebugFrame % 30 == 15) {
					fwprintf(m_pMLDebugFile,
						L"CHROMA_DRAW shader=%s lut=%s maskT2=%s gate=%.2f chN=%.3f chF=%.3f knee=%.3f skinP=%.3f pStr=%.3f pres=%.3f pfrac=%.3f\n",
						(m_bMLDebugCopy ? L"copy" : L"chroma"),
						(m_SkinLUTSRV ? L"skin" : L"NULL"),
						(m_SkyMaskSRV ? L"mask" : L"NULL"),
						(double)m_lastMLParams.MLChromaGate,
						(double)m_lastMLParams.MLChromaNeutral,
						(double)m_lastMLParams.MLChromaFire,
						(double)m_lastMLParams.MLChromaKnee,
						(double)m_lastMLParams.MLChromaSkinProtect,
						(double)m_lastMLParams.MLSkinPersonStrength,
						(double)m_lastMLParams.MLSkinPreserve,
						(double)m_PersonFrac);
					fflush(m_pMLDebugFile);
				}
				ID3D11ShaderResourceView* nullSRVs[2] = {};
				m_pDeviceContext->PSSetShaderResources(1, 2, nullSRVs);
			}

			if (m_bMLDebug && m_pMLDebugFile) {
				fwprintf(m_pMLDebugFile, L"MLPASS draws %s %s %s %s (hr=%x,%x,%x,%x)\n",
					m_bMLDebugCopy ? L"copy" : L"highlight",
					m_bMLDebugCopy ? L"copy" : L"gamma",
					m_bMLDebugCopy ? L"copy" : L"microcont",
					(m_pPSMLChroma ? (m_bMLDebugCopy ? L"copy" : L"chroma") : L"none"),
					(unsigned)hrs[0], (unsigned)hrs[1], (unsigned)hrs[2], (unsigned)hrs[3]);
				fflush(m_pMLDebugFile);
			}
		}

		// Skin-retention metric on real video (frame log; every ~30 frames):
			// source preview vs graded output over person/skin pixels.
			if (m_bMLDebug && m_pMLDebugFile && !m_bMLDebugCopy && (m_MLDebugFrame % 30 == 15)) {
				LogSkinMetric(pRT, rect);
			}

			if (m_bMLActive && m_pPSMLBars && m_bMLBarsEnabled && !m_bMLDebugCopy && !m_bHDRPassthroughRaw) {
			StepSetting();
			hr = TextureCopyRect(*pInputTexture, pRT, rect, rect, m_pPSMLBars, m_pMLParams, 0, false);
			if (m_bMLDebug && m_pMLDebugFile) {
				D3D11_TEXTURE2D_DESC rtDesc = {};
				pRT->GetDesc(&rtDesc);
				fwprintf(m_pMLDebugFile,
					L"BARS_DRAW step=%u hr=%x fTop=%.4f fBot=%.4f "
					L"rect=[%d,%d,%d,%d] inTex=%ux%u rtTex=%ux%u "
					L"videoRect=[%d,%d,%d,%d] winRect=[%d,%d,%d,%d] "
					L"uBarV0=%.4f uBarSpan=%.4f uBarFeather=%.1f uBarHL=%.0f "
					L"cropTop=%d cropBot=%d barAge=%u\n",
					(unsigned)step, (unsigned)hr,
					(double)m_lastMLParams.uBarTop, (double)m_lastMLParams.uBarBottom,
					rect.left, rect.top, rect.right, rect.bottom,
					pInputTexture->desc.Width, pInputTexture->desc.Height,
					rtDesc.Width, rtDesc.Height,
					m_videoRect.left, m_videoRect.top, m_videoRect.right, m_videoRect.bottom,
					m_windowRect.left, m_windowRect.top, m_windowRect.right, m_windowRect.bottom,
					(double)m_lastMLParams.uBarV0, (double)m_lastMLParams.uBarSpan,
					(double)m_lastMLParams.uBarFeather, (double)m_lastMLParams.uBarHighlight,
					(int)m_MLCropTop, (int)m_MLCropBot, m_MLCropAge);
				fflush(m_pMLDebugFile);
			}
		}

		if (m_pPostScaleShaders.size()) {
			static __int64 counter = 0;
			static long start = GetTickCount();

			long stop = GetTickCount();
			long diff = stop - start;
			if (diff >= 10 * 60 * 1000) {
				start = stop;    // reset after 10 min (ps float has its limits in both range and accuracy)
			}

			PS_EXTSHADER_CONSTANTS ConstData = {
				{1.0f / pTex->desc.Width, 1.0f / pTex->desc.Height },
				{(float)pTex->desc.Width, (float)pTex->desc.Height},
				counter++,
				(float)diff / 1000,
				0, 0
			};
			m_pDeviceContext->UpdateSubresource(m_pPostScaleConstants, 0, nullptr, &ConstData, 0, 0);

			for (UINT idx = 0; idx < m_pPostScaleShaders.size(); idx++) {
				StepSetting();
				hr = TextureCopyRect(*pInputTexture, pRT, rect, rect, m_pPostScaleShaders[idx].shader, m_pPostScaleConstants, 0, false);
			}
		}

		if (m_pPSHalfOUtoInterlace) {
			DrawSubtitles(pRT);

			StepSetting();
			FLOAT ConstData[] = {
				(float)pTex->desc.Height, 0,
				(float)dstRect.top / pTex->desc.Height, (float)dstRect.bottom / pTex->desc.Height,
			};
			D3D11_MAPPED_SUBRESOURCE mr;
			hr = m_pDeviceContext->Map(m_pHalfOUtoInterlaceConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
			if (SUCCEEDED(hr)) {
				memcpy(mr.pData, &ConstData, sizeof(ConstData));
				m_pDeviceContext->Unmap(m_pHalfOUtoInterlaceConstantBuffer, 0);
			}
			hr = TextureCopyRect(*pInputTexture, pRT, rect, rect, m_pPSHalfOUtoInterlace, m_pHalfOUtoInterlaceConstantBuffer, 0, false);
		}

		if (m_bFinalPass) {
			StepSetting();
			hr = FinalPass(*pTex, pRT, rect, rect);
			m_bDitherUsed = true;
		}
	}
	else {
		hr = ResizeShaderPass(*pInputTexture, pRenderTarget, rSrc, dstRect, rotation);
	}

	// Latch "a convert output exists" for whoever wants the statistics. Gated on the
	// same predicate as the extraction itself — gating it on the Zion grade alone
	// was what left the experimental mapper permanently unbuilt.
	if (MLStatsNeeded() && m_TexConvertOutput.pTexture) {
		m_bMLConvertSeen = true;
	}

	// Post-grade/present proof: stats of the exact texture slot the present
	// reads are compared against the grade INPUT (STATS IN) in the log.
	if (m_bMLDebug && m_pMLDebugFile && pInputTexture && m_MLDebugFrame % 8 == 0) {
		LogMLFrameStats(*pInputTexture, L"OUT");
	}

	DLogIf(FAILED(hr), L"CDX11VideoProcessor::Process() : failed with error {}", HR2Str(hr));

	return hr;
}

void CDX11VideoProcessor::SetVideoRect(const CRect& videoRect)
{
	m_videoRect = videoRect;
	UpdateRenderRect();
	UpdateTexures();
}

HRESULT CDX11VideoProcessor::SetWindowRect(const CRect& windowRect)
{
	m_windowRect = windowRect;
	UpdateRenderRect();

	HRESULT hr = S_OK;
	const UINT w = m_windowRect.Width();
	const UINT h = m_windowRect.Height();

	if (m_pDXGISwapChain1 && !m_bExclusiveScreen) {
		hr = m_pDXGISwapChain1->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
	}

	UpdateStatsByWindow();

	UpdatePostScaleTexures();

	return hr;
}

HRESULT CDX11VideoProcessor::Reset(bool bDisplayModeChange)
{
	DLog(L"CDX11VideoProcessor::Reset({})", bDisplayModeChange);
	m_rtFirstPresent = -1;   // re-anchor the present-latency gauge on the next frame

	CAutoLock cRendererLock(&m_HDRToggleLock);

	if (bDisplayModeChange) {
		if (m_bDisplayModeChangeAfterHDRToggle) {
			m_bDisplayModeChangeAfterHDRToggle = false;
			return S_OK;
		}
	}

	if ((m_bHdrPassthrough || m_bHdrLocalToneMapping) && SourceIsHDR10orHLG()) {
		MONITORINFOEXW mi = { sizeof(mi) };
		GetMonitorInfoW(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO*)&mi);
		DisplayConfig_t displayConfig = {};

		if (GetDisplayConfig(mi.szDevice, displayConfig)) {
			const auto bHdrPassthroughSupport = displayConfig.HDRSupported() && displayConfig.HDREnabled();
			if ((bHdrPassthroughSupport && !m_bHdrPassthroughSupport) || (!displayConfig.HDREnabled() && m_bHdrPassthroughSupport)) {
				m_bHDRModeChangeOutside = true;

				if (m_pFilter->m_inputMT.IsValid()) {
					CAutoLock cRendererLock(&m_pFilter->m_RendererLock);
					ReleaseSwapChain();

					if (m_iSwapEffect == SWAPEFFECT_Discard && !displayConfig.HDREnabled()) {
						m_pFilter->Init(true);
					} else {
						Init(m_hWnd, true);
					}
				}
			}
		}
	}

	return S_OK;
}

HRESULT CDX11VideoProcessor::GetCurentImage(long *pDIBImage)
{
	UINT w = m_srcRectWidth;
	UINT h = m_srcRectHeight;
	if (m_srcAnamorphic) {
		w = MulDiv(h, m_srcAspectRatioX, m_srcAspectRatioY);
	}
	if (m_iRotation == 90 || m_iRotation == 270) {
		std::swap(w, h);
	}
	const CRect imageRect(0, 0, w, h);

	const UINT dib_bitdepth = 32;
	const UINT dib_pitch    = CalcDibRowPitch(w, dib_bitdepth);

	BITMAPINFOHEADER* pBIH = (BITMAPINFOHEADER*)pDIBImage;
	ZeroMemory(pBIH, sizeof(BITMAPINFOHEADER));
	pBIH->biSize      = sizeof(BITMAPINFOHEADER);
	pBIH->biWidth     = w;
	pBIH->biHeight    = -(LONG)h; // top-down RGB bitmap
	pBIH->biPlanes    = 1;
	pBIH->biBitCount  = dib_bitdepth;
	pBIH->biSizeImage = dib_pitch * h;

	HRESULT hr = S_OK;
	CComPtr<ID3D11Texture2D> pRGB32Texture2D;
	D3D11_TEXTURE2D_DESC texdesc = CreateTex2DDesc(DXGI_FORMAT_B8G8R8X8_UNORM, w, h, Tex2D_DefaultRTarget);

	hr = m_pDevice->CreateTexture2D(&texdesc, nullptr, &pRGB32Texture2D);
	if (FAILED(hr)) {
		return hr;
	}

	const bool bHdrPassthrough = (m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping) && (SourceIsHDR() || m_bVPUseRTXVideoHDR));
	const bool bSavedHdrPassthrough = m_bHdrPassthrough;
	const bool bSavedHdrLocalToneMapping = m_bHdrLocalToneMapping;

	if (bHdrPassthrough) {
		m_pPSHDR10ToneMapping.Release();
		m_pHDR10ToneMappingConstants.Release();

		if (m_D3D11VP.IsReady()) {
			m_pPSCorrection.Release();

			auto resId = (m_srcExFmt.VideoTransferFunction == MFVideoTransFunc_2084 || m_bVPUseRTXVideoHDR) ? IDF_PS_11_CONVERT_PQ_TO_SDR : IDF_PS_11_FIXCONVERT_HLG_TO_SDR;
			EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSCorrection, resId));
			SetShaderLuminanceParams();
		} else {
			m_bHdrPassthrough = false;
			m_bHdrLocalToneMapping = false;
			UpdateConvertColorShader();
		}
	}

	const auto backupVidRect = m_videoRect;
	const auto backupWndRect = m_windowRect;
	m_videoRect  = imageRect;
	m_windowRect = imageRect;
	UpdateTexures();
	UpdatePostScaleTexures();

	auto pSub11CallBack = m_pFilter->m_pSub11CallBack;
	m_pFilter->m_pSub11CallBack = nullptr;

	hr = Process(pRGB32Texture2D, m_srcRect, imageRect, false);

	m_pFilter->m_pSub11CallBack = pSub11CallBack;

	m_videoRect  = backupVidRect;
	m_windowRect = backupWndRect;
	UpdateTexures();
	UpdatePostScaleTexures();

	if (bHdrPassthrough) {
		if (m_D3D11VP.IsReady()) {
			m_pCorrectionConstants.Release();
			m_pPSCorrection.Release();

			if (m_srcExFmt.VideoTransferFunction == MFVideoTransFunc_HLG) {
				EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSCorrection, IDF_PS_11_CONVERT_HLG_TO_PQ));
			}
		} else {
			m_bHdrPassthrough = bSavedHdrPassthrough;
			m_bHdrLocalToneMapping = bSavedHdrLocalToneMapping;
			UpdateConvertColorShader();
		}

		if (m_bHdrPassthroughSupport && m_bHdrLocalToneMapping) {
			EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSHDR10ToneMapping, IDF_PS_11_HDR10_TONEMAP));
			DLogIf(m_pPSHDR10ToneMapping, L"CDX11VideoProcessor::InitMediaType() m_pPSHDR10ToneMapping(type: '{}') created", m_iHdrLocalToneMappingType);
		}
	}

	if (FAILED(hr)) {
		return hr;
	}

	CComPtr<ID3D11Texture2D> pRGB32Texture2D_Shared;
	texdesc = CreateTex2DDesc(DXGI_FORMAT_B8G8R8X8_UNORM, w, h, Tex2D_StagingRead);

	hr = m_pDevice->CreateTexture2D(&texdesc, nullptr, &pRGB32Texture2D_Shared);
	if (FAILED(hr)) {
		return hr;
	}
	m_pDeviceContext->CopyResource(pRGB32Texture2D_Shared, pRGB32Texture2D);

	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (S_OK == m_pDeviceContext->Map(pRGB32Texture2D_Shared, 0, D3D11_MAP_READ, 0, &mr)) {
		CopyPlaneAsIs(h, (BYTE*)(pBIH + 1), dib_pitch, (BYTE*)mr.pData, mr.RowPitch);
		m_pDeviceContext->Unmap(pRGB32Texture2D_Shared, 0);
	} else {
		return E_FAIL;
	}

	return S_OK;
}

HRESULT CDX11VideoProcessor::GetDisplayedImage(BYTE **ppDib, unsigned* pSize)
{
	if (!m_pDXGISwapChain1 || !m_pDevice || !m_pDeviceContext) {
		return E_ABORT;
	}

	CComPtr<ID3D11Texture2D> pBackBuffer;
	HRESULT hr = m_pDXGISwapChain1->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::GetDisplayedImage() failed with error {}", HR2Str(hr));
		return hr;
	}

	D3D11_TEXTURE2D_DESC desc;
	pBackBuffer->GetDesc(&desc);

	if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM) {
		DLog(L"CDX11VideoProcessor::GetDisplayedImage() backbuffer format not supported");
		return E_FAIL;
	}

	D3D11_TEXTURE2D_DESC desc2 = CreateTex2DDesc(desc.Format, desc.Width, desc.Height, Tex2D_StagingRead);
	CComPtr<ID3D11Texture2D> pTexture2DShared;
	hr = m_pDevice->CreateTexture2D(&desc2, nullptr, &pTexture2DShared);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::GetDisplayedImage() failed with error {}", HR2Str(hr));
		return hr;
	}

	m_pDeviceContext->CopyResource(pTexture2DShared, pBackBuffer);

	UINT dib_bitdepth;
	CopyFrameDataFn pConvertToDibFunc;
	if (desc2.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
		if (m_bAllowDeepColorBitmaps) {
			dib_bitdepth = 48;
			pConvertToDibFunc = ConvertR10G10B10A2toBGR48;
		} else {
			dib_bitdepth = 32;
			pConvertToDibFunc = ConvertR10G10B10A2toBGR32;
		}
	} else {
		dib_bitdepth = 32;
		pConvertToDibFunc = CopyPlaneAsIs;
	}
	const UINT dib_pitch    = CalcDibRowPitch(desc.Width, dib_bitdepth);
	const UINT dib_size     = dib_pitch * desc.Height;

	*pSize = sizeof(BITMAPINFOHEADER) + dib_size;
	BYTE* p = (BYTE*)LocalAlloc(LMEM_FIXED, *pSize); // only this allocator can be used
	if (!p) {
		return E_OUTOFMEMORY;
	}

	BITMAPINFOHEADER* pBIH = (BITMAPINFOHEADER*)p;
	ZeroMemory(pBIH, sizeof(BITMAPINFOHEADER));
	pBIH->biSize      = sizeof(BITMAPINFOHEADER);
	pBIH->biWidth     = desc.Width;
	pBIH->biHeight    = -(LONG)desc.Height; // top-down RGB bitmap
	pBIH->biBitCount  = dib_bitdepth;
	pBIH->biPlanes    = 1;
	pBIH->biSizeImage = dib_size;

	D3D11_MAPPED_SUBRESOURCE mappedResource = {};
	hr = m_pDeviceContext->Map(pTexture2DShared, 0, D3D11_MAP_READ, 0, &mappedResource);
	if (SUCCEEDED(hr)) {
		pConvertToDibFunc(desc.Height, (BYTE*)(pBIH + 1), dib_pitch, (BYTE*)mappedResource.pData, mappedResource.RowPitch);
		m_pDeviceContext->Unmap(pTexture2DShared, 0);
		*ppDib = p;
	} else {
		LocalFree(p);
	}

	return hr;
}

HRESULT CDX11VideoProcessor::GetVPInfo(std::wstring& str)
{
	str = L"DirectX 11";
	str += std::format(L"\nGraphics adapter: {}", m_strAdapterDescription);
	str.append(L"\nVideoProcessor  : ");
	if (m_D3D11VP.IsReady()) {
		D3D11_VIDEO_PROCESSOR_CAPS caps;
		UINT rateConvIndex;
		D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS rateConvCaps;
		m_D3D11VP.GetVPParams(caps, rateConvIndex, rateConvCaps);

		str += std::format(L"D3D11, RateConversion_{}", rateConvIndex);

		str.append(L"\nDeinterlaceTech.:");
		if (rateConvCaps.ProcessorCaps) {
			if (rateConvCaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BLEND)               str.append(L" Blend,");
			if (rateConvCaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BOB)                 str.append(L" Bob,");
			if (rateConvCaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_ADAPTIVE)            str.append(L" Adaptive,");
			if (rateConvCaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_MOTION_COMPENSATION) str.append(L" Motion Compensation,");
			if (rateConvCaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_INVERSE_TELECINE)                str.append(L" Inverse Telecine,");
			if (rateConvCaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_FRAME_RATE_CONVERSION)           str.append(L" Frame Rate Conversion");
		} else {
			str.append(L" none");
		}
		str_trim_end(str, ',');
		str += std::format(L"\nReference Frames: Past {}, Future {}", rateConvCaps.PastFrames, rateConvCaps.FutureFrames);
	} else {
		str.append(L"Shaders");
	}

	str.append(m_strStatsDispInfo);

	if (m_pPostScaleShaders.size()) {
		str.append(L"\n\nPost scale pixel shaders:");
		for (const auto& pshader : m_pPostScaleShaders) {
			str += std::format(L"\n  {}", pshader.name);
		}
	}

#ifdef _DEBUG
	str.append(L"\n\nDEBUG info:");
	str += std::format(L"\nSource tex size: {}x{}", m_srcWidth, m_srcHeight);
	str += std::format(L"\nSource rect    : {},{},{},{} - {}x{}", m_srcRect.left, m_srcRect.top, m_srcRect.right, m_srcRect.bottom, m_srcRect.Width(), m_srcRect.Height());
	str += std::format(L"\nVideo rect     : {},{},{},{} - {}x{}", m_videoRect.left, m_videoRect.top, m_videoRect.right, m_videoRect.bottom, m_videoRect.Width(), m_videoRect.Height());
	str += std::format(L"\nWindow rect    : {},{},{},{} - {}x{}", m_windowRect.left, m_windowRect.top, m_windowRect.right, m_windowRect.bottom, m_windowRect.Width(), m_windowRect.Height());

	if (m_pDevice) {
		std::vector<std::pair<const DXGI_FORMAT, UINT>> formatsYUV = {
			{ DXGI_FORMAT_NV12,               0 },
			{ DXGI_FORMAT_P010,               0 },
			{ DXGI_FORMAT_P016,               0 },
			{ DXGI_FORMAT_YUY2,               0 },
			{ DXGI_FORMAT_Y210,               0 },
			{ DXGI_FORMAT_Y216,               0 },
			{ DXGI_FORMAT_AYUV,               0 },
			{ DXGI_FORMAT_Y410,               0 },
			{ DXGI_FORMAT_Y416,               0 },
		};
		std::vector<std::pair<const DXGI_FORMAT, UINT>> formatsRGB = {
			{ DXGI_FORMAT_B8G8R8X8_UNORM,     0 },
			{ DXGI_FORMAT_B8G8R8A8_UNORM,     0 },
			{ DXGI_FORMAT_R10G10B10A2_UNORM,  0 },
			{ DXGI_FORMAT_R16G16B16A16_UNORM, 0 },
		};
		for (auto& [format, formatSupport] : formatsYUV) {
			m_pDevice->CheckFormatSupport(format, &formatSupport);
		}
		for (auto& [format, formatSupport] : formatsRGB) {
			m_pDevice->CheckFormatSupport(format, &formatSupport);
		}

		int count = 0;
		str += L"\nD3D11 VP input formats  :";
		for (const auto& [format, formatSupport] : formatsYUV) {
			if (formatSupport & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT) {
				str.append(L" ");
				str.append(DXGIFormatToString(format));
				count++;
			}
		}
		if (count) {
			str += L"\n ";
		}
		for (const auto& [format, formatSupport] : formatsRGB) {
			if (formatSupport & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT) {
				str.append(L" ");
				str.append(DXGIFormatToString(format));
			}
		}

		count = 0;
		str += L"\nShader Texture2D formats:";
		for (const auto& [format, formatSupport] : formatsYUV) {
			if (formatSupport & (D3D11_FORMAT_SUPPORT_TEXTURE2D|D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) {
				str.append(L" ");
				str.append(DXGIFormatToString(format));
				count++;
			}
		}
		if (count) {
			str += L"\n ";
		}
		for (const auto& [format, formatSupport] : formatsRGB) {
			if (formatSupport & (D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) {
				str.append(L" ");
				str.append(DXGIFormatToString(format));
			}
		}
	}
#endif

	return S_OK;
}

void CDX11VideoProcessor::Configure(const Settings_t& config)
{
	bool changeWindow            = false;
	bool changeDevice            = false;
	bool changeVP                = false;
	bool changeHDR               = false;
	bool changeTextures          = false;
	bool changeConvertShader     = false;
	bool changeBitmapShader      = false;
	bool changeUpscalingShader   = false;
	bool changeDowndcalingShader = false;
	bool changeNumTextures       = false;
	bool changeResizeStats       = false;
	bool changeSuperRes          = false;
	bool changeRTXVideoHDR       = false;
	bool changeLuminanceParams   = false;
	bool changeInputColor        = false;

	// settings that do not require preparation
	m_bShowStats           = config.bShowStats;
	m_bDeintDouble         = config.bDeintDouble;
	m_bInterpolateAt50pct  = config.bInterpolateAt50pct;
	m_bVBlankBeforePresent = config.bVBlankBeforePresent;
	m_bAdjustPresentTime   = config.bAdjustPresentTime;
	m_bDeintBlend          = config.bDeintBlend;

	// checking what needs to be changed

	if (config.iResizeStats != m_iResizeStats) {
		m_iResizeStats = config.iResizeStats;
		changeResizeStats = true;
	}

	if (config.iTexFormat != m_iTexFormat) {
		m_iTexFormat = config.iTexFormat;
		changeTextures = true;
	}

	if (m_srcParams.cformat == CF_NV12) {
		changeVP = config.VPFmts.bNV12 != m_VPFormats.bNV12;
	}
	else if (m_srcParams.cformat == CF_P010 || m_srcParams.cformat == CF_P016) {
		changeVP = config.VPFmts.bP01x != m_VPFormats.bP01x;
	}
	else if (m_srcParams.cformat == CF_YUY2) {
		changeVP = config.VPFmts.bYUY2 != m_VPFormats.bYUY2;
	}
	else {
		changeVP = config.VPFmts.bOther != m_VPFormats.bOther;
	}
	m_VPFormats = config.VPFmts;

	if (config.iVPDeinterlacing != m_iVPDeinterlacing) {
		m_iVPDeinterlacing = config.iVPDeinterlacing;
		if (m_bInterlaced) {
			changeVP = true;
		}
	}

	if (config.bVPScaling != m_bVPScaling) {
		m_bVPScaling = config.bVPScaling;
		changeTextures = true;
		changeVP = true; // temporary solution
	}

	if (config.iChromaScaling != m_iChromaScaling) {
		m_iChromaScaling = config.iChromaScaling;
		changeConvertShader = m_PSConvColorData.bEnable && (m_srcParams.Subsampling == 420 || m_srcParams.Subsampling == 422);
	}

	if (config.iHdrOsdBrightness != m_iHdrOsdBrightness) {
		m_iHdrOsdBrightness = config.iHdrOsdBrightness;
		changeBitmapShader = true;
	}

	if (config.iUpscaling != m_iUpscaling) {
		m_iUpscaling = config.iUpscaling;
		changeUpscalingShader = true;
	}
	if (config.iDownscaling != m_iDownscaling) {
		m_iDownscaling = config.iDownscaling;
		changeDowndcalingShader = true;
	}

	if (config.bUseDither != m_bUseDither) {
		m_bUseDither = config.bUseDither;
		changeNumTextures = m_InternalTexFmt != DXGI_FORMAT_B8G8R8A8_UNORM;
	}

	if (config.iSwapEffect != m_iSwapEffect) {
		m_iSwapEffect = config.iSwapEffect;
		changeWindow = !m_pFilter->m_bExclusiveScreen;
	}

	if (config.bHdrPreferDoVi != m_bHdrPreferDoVi) {
		if (m_Dovi.bValid && !config.bHdrPreferDoVi && SourceIsHDR10orHLG()) {
			m_Dovi = {};
			m_DoviExtensionMetadata = {};
			changeVP = true;
		}
		m_bHdrPreferDoVi = config.bHdrPreferDoVi;
	}

	if (config.bHdrPassthrough != m_bHdrPassthrough) {
		m_bHdrPassthrough = config.bHdrPassthrough;
		changeHDR = true;
	}

	if (config.bHdrLocalToneMapping != m_bHdrLocalToneMapping ||
			config.iHdrLocalToneMappingType != m_iHdrLocalToneMappingType) {
		const bool bEnableChanged = config.bHdrLocalToneMapping != m_bHdrLocalToneMapping;
		const bool bCurveChanged  = config.iHdrLocalToneMappingType != m_iHdrLocalToneMappingType;
		m_bHdrLocalToneMapping = config.bHdrLocalToneMapping;
		m_iHdrLocalToneMappingType = config.iHdrLocalToneMappingType;

		// changeHDR belongs to the ENABLE toggle only — and the split matters more
		// than it looks, for two reasons.
		//
		// 1. changeHDR is expensive and dangerous on an HDR source: it reaches
		//    ReleaseSwapChain() + m_pFilter->Init(true) + InitMediaType() (see the
		//    `if (changeHDR)` block ~line 7676). That is the exact teardown the
		//    bHDRPassthroughRaw comment below records as a hard fault — and the
		//    diagnosis there was that the FAULT LAY IN THE TEARDOWN, not in the
		//    setting. Dragging a curve row should not rebuild the graph.
		// 2. It also made the live path dead code. That block `return`s, and
		//    changeLuminanceParams is not consumed until ~7735, so the flag set here
		//    was DISCARDED and the new selection only landed as a side effect of
		//    InitMediaType re-creating everything.
		//
		// Nothing about the SELECTION needs a rebuild. Verified against every read of
		// m_iHdrLocalToneMappingType: it is cbuffer[8] on the PQ->SDR path, an argument
		// to SetHDR10ShaderParams() re-read per frame on the HDR-output path (which
		// creates that shader lazily), a stats/OSD string, and the ToneLutActive()
		// predicate. The two sites that CREATE the HDR10 tone-map shader gate on
		// m_bHdrLocalToneMapping, never on the type — which is why the enable toggle
		// keeps changeHDR and the type does not.
		if (bEnableChanged) {
			changeHDR = true;
		}
		if (bCurveChanged || bEnableChanged) {
			changeLuminanceParams = true;
		}
	}

	if (config.bHDRPassthroughRaw != m_bHDRPassthroughRaw) {
		m_bHDRPassthroughRaw = config.bHDRPassthroughRaw;
		// NO changeHDR — deliberately, and this is the crash fix.
		//
		// The raw toggle only changes WHICH shader the render loop binds: per frame
		// it picks m_pPS_Simple over the correction / HDR10-tone-map shader, and
		// copies the ML steps (see the bind sites in Render()). All of that is read
		// every frame, so the toggle takes effect on the next one.
		//
		// changeHDR did not do that. It took the HDR-output path, and because the
		// source is HDR10 that meant ReleaseSwapChain() + m_pFilter->Init(true) +
		// InitMediaType() — tearing the swap chain and media type out from under a
		// live render thread. Observed as a hard fault on toggling the row:
		//   Unplugged.exe, faulting module MpcVideoRenderer64.ax v0.10.8.2637,
		//   exception 0xc0000005 at offset 0x93ccf.
		// Nothing about "show me the raw signal" needs a media-type re-init.
	}

	if (config.iHdrDisplayMaxNits != m_iHdrDisplayMaxNits) {
		m_iHdrDisplayMaxNits = config.iHdrDisplayMaxNits;
		//changeHDR = true;
	}

	if (config.iOutputGamut != m_iOutputGamut) {
		m_iOutputGamut = config.iOutputGamut;
		// Read from the correction shader's constant buffer, so it needs the SAME
		// update mechanism as iSDRDisplayNits just below — changeLuminanceParams
		// -> SetShaderLuminanceParams(). NOT changeConvertShader: that flag
		// rebuilds the YCbCr->RGB shader and left the gamut value never reaching
		// the GPU, which made the control appear to do nothing.
		if (SourceIsHDR()) {
			changeLuminanceParams = true;
		}
	}

	if (config.fHdrDisplayMinNits != m_fHdrDisplayMinNits) {
		m_fHdrDisplayMinNits = config.fHdrDisplayMinNits;
		// Shader-constant only: SetHDR10ShaderParams memcmps its cbuffer, so the
		// next call picks the new value up without a media-type or swap-chain
		// rebuild. Same treatment as iHdrDisplayMaxNits above.
		//
		// The experimental curve reads it too, and as its OUTPUT BLACK — force a
		// rebuild, or the new black waits for the next scene change to appear.
		if (m_iExperimentalToneMapper != 0) {
			m_bToneLutReady = false;
		}
		// The standard curves read it in the converter's cbuffer, so re-push.
		changeLuminanceParams = true;
	}

	if (config.fHdrTitlePeakNits != m_fHdrTitlePeakNits) {
		m_fHdrTitlePeakNits = config.fHdrTitlePeakNits;
		// Read by the converter's cbuffer (the standard curves' normalisation) AND
		// by the experimental spline's index axis, so both need to see it.
		changeLuminanceParams = true;
		m_bToneLutReady = false;
	}

	if (config.fHdrHableKnee != m_fHdrHableKnee) {
		m_fHdrHableKnee = config.fHdrHableKnee;
		// Shader-constant only: the Hable branch reads it from the converter's
		// cbuffer, so a re-push is enough. No curve rebuild — it is a per-pixel
		// blend weight, not a table.
		changeLuminanceParams = true;
	}

	// Soft-plus pre-compressor parameters — shader-constant only, cbuffer re-push.
	if (config.fSoftKnee != m_fSoftKnee ||
		config.fSoftKneeRatio != m_fSoftKneeRatio ||
		config.fSoftKneeWidth != m_fSoftKneeWidth) {
		m_fSoftKnee      = config.fSoftKnee;
		m_fSoftKneeRatio = config.fSoftKneeRatio;
		m_fSoftKneeWidth = config.fSoftKneeWidth;
		changeLuminanceParams = true;
	}

	// Hable reference white — shader-constant only, cbuffer re-push.
	if (config.fHdrRefWhiteNits != m_fHdrRefWhiteNits) {
		m_fHdrRefWhiteNits = config.fHdrRefWhiteNits;
		changeLuminanceParams = true;
	}

	// Highlight neutral roll — shader-constant only, cbuffer re-push. A live drag
	// on the rail therefore re-fills the converter's cbuffer alone (no curve
	// rebuild, no graph op): the picture changes in real time.
	if (config.fHdrHighlightNeutral != m_fHdrHighlightNeutral) {
		m_fHdrHighlightNeutral = std::clamp(config.fHdrHighlightNeutral, 0.0f, 1.0f);
		changeLuminanceParams = true;
	}

	// STAGE 2: the stored exponent now feeds the shader via cbuffer[20]; a change
	// re-pushes it live so the drag takes effect immediately (guarded in HLSL).
	if (config.fHdrChromaExp != m_fHdrChromaExp) {
		m_fHdrChromaExp = std::clamp(config.fHdrChromaExp, -1.0f, 3.5f);
		changeLuminanceParams = true;
	}

	// GT7 operator knobs — cbuffer[21..23], live re-push like the two above, so a
	// drag on the rail changes the picture without a curve rebuild or a graph op.
	// The fade window is validated as a PAIR: an inverted or degenerate window would
	// make smoothstep return garbage, and the shader falls back to Polyphony's
	// 0.98/1.16 when it sees one.
	if (config.fHdrGt7Blend != m_fHdrGt7Blend) {
		m_fHdrGt7Blend = std::clamp(config.fHdrGt7Blend, 0.0f, 1.0f);
		changeLuminanceParams = true;
	}
	if (config.fHdrGt7FadeStart != m_fHdrGt7FadeStart) {
		m_fHdrGt7FadeStart = std::clamp(config.fHdrGt7FadeStart, 0.05f, 3.0f);
		changeLuminanceParams = true;
	}
	if (config.fHdrGt7FadeEnd != m_fHdrGt7FadeEnd) {
		m_fHdrGt7FadeEnd = std::clamp(config.fHdrGt7FadeEnd, 0.05f, 4.0f);
		changeLuminanceParams = true;
	}
	// GT7 Adapt's scene-peak weight (selection 10 only). Live re-push, and note the
	// clamp: 0 is a legal value (it makes the curve identical to plain GT7), so this
	// must not be an "if (v > 0)" style enable.
	if (config.fHdrGt7Adapt != m_fHdrGt7Adapt) {
		m_fHdrGt7Adapt = std::clamp(config.fHdrGt7Adapt, 0.0f, 1.0f);
		changeLuminanceParams = true;
	}
	// The shoulder's reach. Bounded away from 1.0, where k = (LS-1)/(alpha-1) divides
	// by zero, and from 0 — the shader reads 0 as "unset" and falls back to Polyphony's
	// 0.25, so a clamped-to-zero value would silently mean something else.
	if (config.fHdrGt7Alpha != m_fHdrGt7Alpha) {
		m_fHdrGt7Alpha = std::clamp(config.fHdrGt7Alpha, 0.02f, 0.90f);
		changeLuminanceParams = true;
	}
	// 0 is the "off" value and must survive the clamp, so this is a range clamp and
	// not an enable test.
	if (config.fHdrGt7Desat != m_fHdrGt7Desat) {
		m_fHdrGt7Desat = std::clamp(config.fHdrGt7Desat, 0.0f, 1.0f);
		changeLuminanceParams = true;
	}
	// Capacity target. 0 would mean "collapse everything to grey", so the floor is
	// well above it; 1 is the neutral boundary behaviour.
	if (config.fGamutCap != m_fGamutCap) {
		m_fGamutCap = std::clamp(config.fGamutCap, 0.20f, 1.0f);
		changeLuminanceParams = true;
	}
	if (config.fHdrGt7Dbg != m_fHdrGt7Dbg) {
		m_fHdrGt7Dbg = std::clamp(config.fHdrGt7Dbg, 0.0f, 4.0f);
		changeLuminanceParams = true;
	}
	if (config.fCtlWhiteNits != m_fCtlWhiteNits) {
		m_fCtlWhiteNits = std::clamp(config.fCtlWhiteNits, 100.0f, 400.0f);
	}
	if (config.bMLController != m_bMLController) {
		m_bMLController = config.bMLController;
		// Drop the warm start: values solved under one mode are not a good starting
		// point for the other. (SetMLParams runs every frame while the grade is up,
		// so no change-flag is needed here.)
		m_ctlS = 1.0f; m_ctlG = 1.0f; m_ctlR = 0.0f;
	}
	// Auto exposure. Changing either input invalidates the solved value, so drop it
	// and let the anchor block re-solve on the next frame rather than slewing from a
	// number that was solved under different rules.
	if (config.bHdrAutoRefWhite != m_bAutoRefWhite ||
	    config.fHdrRefWhiteTarget != m_fRefWhiteTarget) {
		m_bAutoRefWhite = config.bHdrAutoRefWhite;
		m_fRefWhiteTarget = std::clamp(config.fHdrRefWhiteTarget, 0.20f, 0.70f);
		m_fRefWhiteAuto = 0.0f;
		changeLuminanceParams = true;
	}

	if (config.iExperimentalToneMapper != m_iExperimentalToneMapper) {
		m_iExperimentalToneMapper = config.iExperimentalToneMapper;
		// Also covers switching back to Off: ToneLutActive() goes false, the next
		// SetShaderLuminanceParams writes toneLutEnable = 0, and the converter
		// takes the shipped branch again. Drop the built table so re-enabling
		// rebuilds from the current scene rather than reusing a stale one.
		m_bToneLutReady = false;
		if (m_iExperimentalToneMapper == 0) {
			m_MLToneMaxSmooth = -1.0f;
		}
		// Reads the correction shader's constant buffer — same flag as iOutputGamut.
		changeLuminanceParams = true;
	}

	if (config.fToneLutContrast != m_fToneLutContrast) {
		m_fToneLutContrast = config.fToneLutContrast;
		m_bToneLutReady = false;   // the curve is built from it
	}

	if (config.fToneLutPivotScale != m_fToneLutPivotScale) {
		m_fToneLutPivotScale = config.fToneLutPivotScale;
		m_bToneLutReady = false;   // pivot scale changes the built curve
	}

	if (config.iHdrToggleDisplay != m_iHdrToggleDisplay) {
		if (config.iHdrToggleDisplay == HDRTD_Disabled || m_iHdrToggleDisplay == HDRTD_Disabled) {
			changeHDR = true;
		}
		m_iHdrToggleDisplay = config.iHdrToggleDisplay;
	}

	if (config.bConvertToSdr != m_bConvertToSdr) {
		m_bConvertToSdr = config.bConvertToSdr;
		if (SourceIsHDR()) {
			// Re-select the converter shader DIRECTLY. This used to ask for a full
			// VP re-init (changeVP) and that request is silently dropped in several
			// situations — Configure() clears changeVP for Dolby Vision sources, and
			// a re-init that lands before the media type has settled does nothing.
			// Whenever it was dropped, the control simply had no effect, which is
			// why Convert to SDR behaved intermittently: the setting was stored
			// correctly every time, but only took on the runs where a rebuild
			// happened to follow it.
			//
			// bConvertToSdr is not a shader constant, it decides WHICH shader exists,
			// so it cannot be pushed as one — but it can be re-selected in place,
			// which is all a re-init was achieving.
			UpdateCorrectionShader();
			// The correction pass is one step of the post-scale ring, so the ring's
			// step count can change with it (see the m_pPSCorrection block in the
			// render path). Mirrors what UpdateMLActive() does for the ML steps.
			UpdatePostScaleTexures();
		}
	}

	if (config.iVPSuperRes != m_iVPSuperRes) {
		m_iVPSuperRes = config.iVPSuperRes;
		changeSuperRes = true;
	}

	if (config.bVPRTXVideoHDR != m_bVPRTXVideoHDR) {
		m_bVPRTXVideoHDR = config.bVPRTXVideoHDR;
		changeRTXVideoHDR = true;
	}

	if (config.iSDRDisplayNits != m_iSDRDisplayNits) {
		DLog(L"renderer: [SDR] display nits {} -> {}  converting={}  (live re-push)",
			m_iSDRDisplayNits, config.iSDRDisplayNits, config.bConvertToSdr ? 1 : 0);
		m_iSDRDisplayNits = config.iSDRDisplayNits;
		// ALWAYS re-push: sdrNits is the OUTPUT/DISPLAY scale — LuminanceScale in
		// the converter cbuffer. The old `if (SourceIsHDR())` gate silently broke
		// live drags on the file path, where m_decExFmt carries no colour info
		// (flags=0 -> SourceIsHDR()==false) even though the picture IS an HDR
		// convert: the value was stored and echoed back in logs while the frame
		// never moved. (2026-09-25, live-drag report.)
		changeLuminanceParams = true;
		// SDR white is the experimental curve's OUTPUT WHITE, so the table itself
		// is stale, not just the constant that scales it.
		if (m_iExperimentalToneMapper != 0) {
			m_bToneLutReady = false;
		}
	}

	if (config.bMLEnabled != m_bML || config.iMLMode != m_iMLMode ||
			config.fMLGamma != m_fMLGamma || config.fMLShadow != m_fMLShadow || config.fMLShadowDehaze != m_fMLShadowDehaze || config.fMLHighlightRolloffCtrl != m_fMLHighlightRolloffCtrl || config.fMLGammaTopUp != m_fMLGammaTopUp || config.fMLShadowPedestal != m_fMLShadowPedestal || config.fMLTargetMid != m_fMLTargetMid ||
			config.fMLHighlight != m_fMLHighlight ||
			config.bMLGammaEnabled != m_bMLGammaEnabled ||
			config.bMLShadowEnabled != m_bMLShadowEnabled ||
			config.bMLHighlightEnabled != m_bMLHighlightEnabled ||
			config.bMLDebugCopy != m_bMLDebugCopy || config.bMLDebugLog != m_bMLDebug || config.bMLShadowDebugVis != m_bMLShadowDebugVis || config.bMLContrastDebugVis != m_bMLContrastDebugVis || config.bMLSkyEnabled != m_bSkyEnabled || config.bMLSkyDebugVis != m_bSkyDebugVis || config.fMLSkyStrength != m_fMLSkyStrength ||
			config.bMLPersonEnabled != m_bPersonEnabled || config.fMLPersonStrength != m_fPersonStrength || config.bMLPersonDebugVis != m_bPersonDebugVis || config.fMLSkinPreserve != m_fMLSkinPreserve ||
			config.fMLShadowZoneCeil != m_fMLShadowZoneCeil || config.fMLShadowSkinProtect != m_fMLShadowSkinProtect || config.fMLShadowRatioCap != m_fMLShadowRatioCap ||
			config.bMLContrastEnabled != m_bMLContrastEnabled || config.fMLContrast != m_fMLContrast ||
			config.fMLContrastZoneLo != m_fMLContrastZoneLo || config.fMLContrastSkinProtect != m_fMLContrastSkinProtect || config.fMLContrastRatioCap != m_fMLContrastRatioCap ||
			config.bMLHuntEnabled != m_bMLHuntEnabled || config.fMLHunt != m_fMLHunt ||
			config.bMLChromaEnabled != m_bMLChromaEnabled ||
			config.bMLChromaNeutralEnabled != m_bMLChromaNeutralEnabled ||
			config.fMLChromaNeutral != m_fMLChromaNeutral ||
			config.bMLChromaFireEnabled != m_bMLChromaFireEnabled ||
			config.fMLChromaFire != m_fMLChromaFire ||
			config.bMLChromaKneeEnabled != m_bMLChromaKneeEnabled ||
			config.fMLChromaKnee != m_fMLChromaKnee ||
			config.bMLChromaSkinEnabled != m_bMLChromaSkinEnabled ||
			config.fMLChromaSkinProtect != m_fMLChromaSkinProtect ||
			config.bMLBars != m_bMLBarsEnabled ||
			config.bMLBarsHighlight != m_bMLBarsHighlight ||
			config.bMLFaceProtectEnabled != m_bFaceProtectEnabled ||
			config.fMLFaceProtect != m_fFaceProtect ||
			config.fMLFaceFeather != m_fFaceFeather ||
			config.fMLFaceExpand != m_fFaceExpand ||
			config.bMLFaceDebugVis != m_bFaceDebugVis) {
		m_bML          = config.bMLEnabled;
		m_iMLMode      = config.iMLMode;
		m_fMLGamma     = config.fMLGamma;
		m_fMLShadow    = config.fMLShadow;
		m_fMLShadowDehaze = config.fMLShadowDehaze;
		m_fMLHighlightRolloffCtrl = config.fMLHighlightRolloffCtrl;
		m_fMLGammaTopUp = config.fMLGammaTopUp;
		m_fMLShadowPedestal = std::clamp(config.fMLShadowPedestal, 0.0f, 0.08f);
		// 0 is a meaningful value here — it means OFF, manual top-up only — so the
		// clamp must not be an enable test.
		m_fMLTargetMid = std::clamp(config.fMLTargetMid, 0.0f, 0.95f);
		m_fMLHighlight = config.fMLHighlight;
		m_bMLToneCurveEnabled = config.bMLToneCurveEnabled;
		m_bMLGammaEnabled   = config.bMLGammaEnabled;
		m_bMLShadowEnabled  = config.bMLShadowEnabled;
		m_bMLHighlightEnabled = config.bMLHighlightEnabled;
		m_bMLDebugCopy = config.bMLDebugCopy;
		m_bMLDebug     = config.bMLDebugLog;
		if (m_pFaceDetect) m_pFaceDetect->SetLogEnabled(m_bMLDebug);
		if (m_pSkySeg) m_pSkySeg->SetLogEnabled(m_bMLDebug);
		m_bMLShadowDebugVis = config.bMLShadowDebugVis;
		m_bMLContrastDebugVis = config.bMLContrastDebugVis;
			m_bSkyEnabled          = config.bMLSkyEnabled;
			m_bSkyDebugVis         = config.bMLSkyDebugVis;
			m_fMLSkyStrength      = config.fMLSkyStrength;
			m_bPersonEnabled       = config.bMLPersonEnabled;
			m_fPersonStrength      = config.fMLPersonStrength;
			m_bPersonDebugVis      = config.bMLPersonDebugVis;
			m_fMLSkinPreserve      = config.fMLSkinPreserve;
		m_fMLShadowZoneCeil    = config.fMLShadowZoneCeil;
		m_fMLShadowSkinProtect = config.fMLShadowSkinProtect;
		m_fMLShadowRatioCap    = config.fMLShadowRatioCap;
		m_bMLContrastEnabled   = config.bMLContrastEnabled;
		m_fMLContrast          = config.fMLContrast;
		m_fMLContrastZoneLo    = config.fMLContrastZoneLo;
		m_fMLContrastSkinProtect = config.fMLContrastSkinProtect;
		m_fMLContrastRatioCap  = config.fMLContrastRatioCap;
		m_bMLHuntEnabled = config.bMLHuntEnabled;
		m_fMLHunt        = config.fMLHunt;
		m_bMLChromaEnabled = config.bMLChromaEnabled;
		m_bMLChromaNeutralEnabled = config.bMLChromaNeutralEnabled;
		m_fMLChromaNeutral = config.fMLChromaNeutral;
		m_bMLChromaFireEnabled = config.bMLChromaFireEnabled;
		m_fMLChromaFire    = config.fMLChromaFire;
		m_bMLChromaKneeEnabled = config.bMLChromaKneeEnabled;
		m_fMLChromaKnee    = config.fMLChromaKnee;
		m_bMLChromaSkinEnabled = config.bMLChromaSkinEnabled;
		m_fMLChromaSkinProtect = config.fMLChromaSkinProtect;
		m_bMLBarsEnabled   = config.bMLBars;
		m_bMLBarsHighlight = config.bMLBarsHighlight;
		m_bFaceProtectEnabled = config.bMLFaceProtectEnabled;
		m_fFaceProtect     = config.fMLFaceProtect;
		m_fFaceFeather     = config.fMLFaceFeather;
		m_fFaceExpand      = config.fMLFaceExpand;
		m_bFaceDebugVis    = config.bMLFaceDebugVis;
		DLog(L"CDX11VideoProcessor::Configure() configSeq=%u ML: enable=%d mode=%d toneCurve=%d gamma=%.3f(en=%d) shadow=%.3f(en=%d) "
			L"highlight=%.3f(en=%d) chroma=%d hunt=%.2f(en=%d) chN=%.2f(en=%d) chF=%.2f(en=%d) "
			L"knee=%.2f skin=%.2f bars=%d barsHL=%d debugLog=%d debugCopy=%d sizeof(Settings_t)=%zu",
			config.configSeq, (int)config.bMLEnabled, config.iMLMode, (int)config.bMLToneCurveEnabled,
			config.fMLGamma, (int)config.bMLGammaEnabled, config.fMLShadow, (int)config.bMLShadowEnabled,
			config.fMLHighlight, (int)config.bMLHighlightEnabled, (int)config.bMLChromaEnabled,
			config.fMLHunt, (int)config.bMLHuntEnabled, config.fMLChromaNeutral, (int)config.bMLChromaNeutralEnabled,
			config.fMLChromaFire, (int)config.bMLChromaFireEnabled, config.fMLChromaKnee, config.fMLChromaSkinProtect,
			(int)config.bMLBars, (int)config.bMLBarsHighlight,
			(int)config.bMLDebugLog, (int)config.bMLDebugCopy, sizeof(Settings_t));
		UpdateMLActive();
	}

	// forced input colour space (live HDMI / DeckLink) — re-derive the media
	// type so the override lands on m_srcExFmt + m_decExFmt before the tone-map
	// and swapchain decisions read them (mirrors the changeHDR path).
	if (config.iInputColorSpace != m_InputColorSpaceOverride) {
		if (m_pMLDebugFile) {
			fwprintf(m_pMLDebugFile, L"Configure: inputColorSpace %d -> %d (changeInputColor)\n",
				m_InputColorSpaceOverride, config.iInputColorSpace);
			fflush(m_pMLDebugFile);
		}
		m_InputColorSpaceOverride = config.iInputColorSpace;
		changeInputColor = true;
	}

	if (!m_pFilter->GetActive()) {
		return;
	}

	// High-level: a settings application that will RENEGOTIATE the media type or
	// the swap chain. These are the destructive Configure paths — the ones this
	// project has already faulted on — so the trigger and the thread it runs on
	// are exactly what a post-mortem needs.
	if (changeWindow || changeInputColor || changeHDR) {
		m_pFilter->LogLine("renderer: Configure — renegotiation triggered  window=%d inputColor=%d hdr=%d "
			"ics=%d active=1", changeWindow ? 1 : 0, changeInputColor ? 1 : 0, changeHDR ? 1 : 0,
			m_InputColorSpaceOverride);
	}

	// apply new settings

	if (changeWindow) {
		ReleaseSwapChain();
		EXECUTE_ASSERT(S_OK == m_pFilter->Init(true));

		if (changeHDR && (SourceIsHDR10orHLG() || m_bVPUseRTXVideoHDR || m_bVPRTXVideoHDR) || m_iHdrToggleDisplay) {
			m_srcVideoTransferFunction = 0;
			InitMediaType(&m_pFilter->m_inputMT);
		}
		return;
	}

	if (changeInputColor) {
		// force re-derive of the input colour space (ICS override or back to
		// Auto) — invalidate the PQ/HLG transition latch so the HDR-toggle and
		// shader selection re-evaluate against the new signal.
		if (m_pMLDebugFile) {
			fwprintf(m_pMLDebugFile, L"Configure: changeInputColor -> InitMediaType(override=%d)\n",
				m_InputColorSpaceOverride);
			fflush(m_pMLDebugFile);
		}
		m_srcVideoTransferFunction = 0;
		InitMediaType(&m_pFilter->m_inputMT);

		return;
	}

	if (changeHDR) {
		if (SourceIsHDR10orHLG() || m_bVPUseRTXVideoHDR || m_bVPRTXVideoHDR || m_iHdrToggleDisplay) {
			if (m_iSwapEffect == SWAPEFFECT_Discard) {
				ReleaseSwapChain();
				m_pFilter->Init(true);
			}

			m_srcVideoTransferFunction = 0;
			InitMediaType(&m_pFilter->m_inputMT);

			return;
		}
	}

	if (m_Dovi.bValid) {
		changeVP = false;
	}
	if (changeVP) {
		InitMediaType(&m_pFilter->m_inputMT);
		if (m_bVPUseRTXVideoHDR || m_bVPRTXVideoHDR) {
			InitSwapChain(false);
		}

		return; // need some test
	}

	if (changeRTXVideoHDR) {
		InitMediaType(&m_pFilter->m_inputMT);

		return;
	}

	// changes that do not require a global rebuild of the video processor

	if (changeTextures) {
		UpdateTexParams(m_srcParams.CDepth);
		if (m_D3D11VP.IsReady()) {
			// update m_D3D11OutputFmt
			EXECUTE_ASSERT(S_OK == InitializeD3D11VP(m_srcParams, m_srcWidth, m_srcHeight, &m_pFilter->m_inputMT));
		}
		UpdateTexures();
		UpdatePostScaleTexures();
	}

	if (changeConvertShader) {
		UpdateConvertColorShader();
	}

	if (changeBitmapShader) {
		UpdateBitmapShader();
	}

	if (changeUpscalingShader) {
		UpdateUpscalingShaders();
	}
	if (changeDowndcalingShader) {
		UpdateDownscalingShaders();
	}

	if (changeLuminanceParams) {
		SetShaderLuminanceParams();
	}

	if (changeNumTextures) {
		UpdatePostScaleTexures();
	}

	if (changeResizeStats) {
		UpdateStatsByWindow();
		UpdateStatsByDisplay();
	}

	if (changeSuperRes) {
		auto superRes = (m_bVPScaling && (m_srcParams.CDepth == 8 || !m_bACMEnabled)) ? m_iVPSuperRes : SUPERRES_Disable;
		m_bVPUseSuperRes = (m_D3D11VP.SetSuperRes(superRes) == S_OK);
	}

	UpdateStatsStatic();
}

void CDX11VideoProcessor::SetRotation(int value)
{
	m_iRotation = value;
	if (m_D3D11VP.IsReady()) {
		m_D3D11VP.SetRotation(static_cast<D3D11_VIDEO_PROCESSOR_ROTATION>(value / 90));
	}
}

void CDX11VideoProcessor::SetStereo3dTransform(int value)
{
	m_iStereo3dTransform = value;

	if (m_iStereo3dTransform == 1) {
		if (!m_pPSHalfOUtoInterlace) {
			EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSHalfOUtoInterlace, IDF_PS_11_HALFOU_TO_INTERLACE));
		}
	}
	else {
		m_pPSHalfOUtoInterlace.Release();
	}
}

void CDX11VideoProcessor::Flush()
{
	if (m_D3D11VP.IsReady()) {
		m_D3D11VP.ResetFrameOrder();
	}

	m_rtStart = 0;

	m_DoviExtensionMetadata = {};
#ifndef NDEBUG
	UpdateStatsStatic();
#endif
}

void CDX11VideoProcessor::ClearPreScaleShaders()
{
	for (auto& pExtShader : m_pPreScaleShaders) {
		pExtShader.shader.Release();
	}
	m_pPreScaleShaders.clear();
	DLog(L"CDX11VideoProcessor::ClearPreScaleShaders().");
}


void CDX11VideoProcessor::ClearPostScaleShaders()
{
	for (auto& pExtShader : m_pPostScaleShaders) {
		pExtShader.shader.Release();
	}
	m_pPostScaleShaders.clear();
	//UpdateStatsPostProc();
	DLog(L"CDX11VideoProcessor::ClearPostScaleShaders().");
}

HRESULT CDX11VideoProcessor::AddPreScaleShader(const std::wstring& name, const std::string& srcCode)
{
#ifdef _DEBUG
	if (!m_pDevice) {
		return E_ABORT;
	}

	ID3DBlob* pShaderCode = nullptr;
	HRESULT hr = CompileShader(srcCode, nullptr, "ps_4_0", &pShaderCode);
	if (S_OK == hr) {
		m_pPreScaleShaders.emplace_back();
		hr = m_pDevice->CreatePixelShader(pShaderCode->GetBufferPointer(), pShaderCode->GetBufferSize(), nullptr, &m_pPreScaleShaders.back().shader);
		if (S_OK == hr) {
			m_pPreScaleShaders.back().name = name;
			//UpdatePreScaleTexures(); //TODO
			DLog(L"CDX11VideoProcessor::AddPreScaleShader() : \"{}\" pixel shader added successfully.", name);
		}
		else {
			DLog(L"CDX11VideoProcessor::AddPreScaleShader() : create pixel shader \"{}\" FAILED!", name);
			m_pPreScaleShaders.pop_back();
		}
		pShaderCode->Release();
	}

	if (S_OK == hr && m_D3D11VP.IsReady() && m_bVPScaling) {
		return S_FALSE;
	}

	return hr;
#else
	return E_NOTIMPL;
#endif
}

HRESULT CDX11VideoProcessor::AddPostScaleShader(const std::wstring& name, const std::string& srcCode)
{
	if (!m_pDevice) {
		return E_ABORT;
	}

	ID3DBlob* pShaderCode = nullptr;
	HRESULT hr = CompileShader(srcCode, nullptr, "ps_4_0", &pShaderCode);
	if (S_OK == hr) {
		m_pPostScaleShaders.emplace_back();
		hr = m_pDevice->CreatePixelShader(pShaderCode->GetBufferPointer(), pShaderCode->GetBufferSize(), nullptr, &m_pPostScaleShaders.back().shader);
		if (S_OK == hr) {
			m_pPostScaleShaders.back().name = name;
			UpdatePostScaleTexures();
			DLog(L"CDX11VideoProcessor::AddPostScaleShader() : \"{}\" pixel shader added successfully.", name);
		}
		else {
			DLog(L"CDX11VideoProcessor::AddPostScaleShader() : create pixel shader \"{}\" FAILED!", name);
			m_pPostScaleShaders.pop_back();
		}
		pShaderCode->Release();
	}

	return hr;
}

ISubPicAllocator* CDX11VideoProcessor::GetSubPicAllocator()
{
	if (!m_pSubPicAllocator && m_pDevice) {
		m_pSubPicAllocator = new CDX11SubPicAllocator(m_pDevice, { 1280, 720 });
	}
	return m_pSubPicAllocator;
}

void CDX11VideoProcessor::UpdateStatsPresent()
{
	DXGI_SWAP_CHAIN_DESC1 swapchain_desc;
	if (m_pDXGISwapChain1 && S_OK == m_pDXGISwapChain1->GetDesc1(&swapchain_desc)) {
		m_strStatsPresent.assign(L"\nPresentation  : ");
		switch (swapchain_desc.SwapEffect) {
		case DXGI_SWAP_EFFECT_DISCARD:
			m_strStatsPresent.append(L"Discard");
			break;
		case DXGI_SWAP_EFFECT_SEQUENTIAL:
			m_strStatsPresent.append(L"Sequential");
			break;
		case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL:
			m_strStatsPresent.append(L"Flip sequential");
			break;
		case DXGI_SWAP_EFFECT_FLIP_DISCARD:
			m_strStatsPresent.append(L"Flip discard");
			break;
		}
		m_strStatsPresent.append(L", ");
		m_strStatsPresent.append(DXGIFormatToString(swapchain_desc.Format));

		if ((m_bVBlankBeforePresent && m_pDXGIOutput) || m_bAdjustPresentTime) {
			m_strStatsPresent.append(L"\nFrame sync    :");
			if (m_bVBlankBeforePresent && m_pDXGIOutput) {
				m_strStatsPresent.append(L" wait VBlank");
			}
			if (m_bAdjustPresentTime) {
				if (m_strStatsPresent.back() != ':') {
					m_strStatsPresent += ',';
				}
				m_strStatsPresent.append(L" adjust present time");
			}
		}
	}
}

void CDX11VideoProcessor::UpdateStatsStatic()
{
	if (!m_bShowStats) {
		return;
	}

	if (m_srcParams.cformat) {
		m_strStatsHeader = std::format(L"MPC VR {}, Direct3D 11, Windows {}", _CRT_WIDE(VERSION_STR), GetWindowsVersion());

		UpdateStatsInputFmt();

		m_strStatsVProc.assign(L"\nVideoProcessor: ");
		if (m_D3D11VP.IsReady()) {
			m_strStatsVProc += std::format(L"D3D11 VP, output to {}", DXGIFormatToString(m_D3D11OutputFmt));
		} else {
			m_strStatsVProc.append(L"Shaders");
			if (m_srcParams.Subsampling == 420 || m_srcParams.Subsampling == 422) {
				m_strStatsVProc.append(L", Chroma scaling: ");
				switch (m_iChromaScaling) {
				case CHROMA_Nearest:
					m_strStatsVProc.append(L"Nearest-neighbor");
					break;
				case CHROMA_Bilinear:
					m_strStatsVProc.append(L"Bilinear");
					break;
				case CHROMA_CatmullRom:
					m_strStatsVProc.append(L"Catmull-Rom");
					break;
				}
			}
		}
		m_strStatsVProc += std::format(L"\nInternalFormat: {}", DXGIFormatToString(m_InternalTexFmt));

		if (SourceIsHDR() || m_bVPUseRTXVideoHDR) {
			m_strStatsHDR.assign(L"\nHDR processing: ");
			if (m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping)) {
				if (m_bHdrPassthrough) {
					m_strStatsHDR.append(L"Passthrough");
				} else if (m_bHdrLocalToneMapping) {
					m_strStatsHDR.append(L"Local tone mapping:");
					switch (m_iHdrLocalToneMappingType) {
						case 1:
							m_strStatsHDR.append(L" ACES");
							break;
						case 2:
							m_strStatsHDR.append(L" Reinhard");
							break;
						case 3:
							m_strStatsHDR.append(L" Habel");
							break;
						case 4:
							m_strStatsHDR.append(L" Mobius");
							break;
						case 5:
							if (m_DoviExtensionMetadata.L1.present) {
								m_strStatsHDR.append(L" ST 2094-10");
							} else {
								m_strStatsHDR.append(L" BT2390");
							}
							break;
						default:
							break;
					}
					m_strStatsHDR.append(std::format(L"\n Display Max Nits: {} nits", m_iHdrDisplayMaxNits));
				}
				if (m_bVPUseRTXVideoHDR) {
					m_strStatsHDR.append(L", RTX Video HDR*");
				}
				if (m_bHdrLocalToneMapping && m_DoviExtensionMetadata.L1.present) {
					m_strStatsHDR += std::format(L", {} nits", m_DoviExtensionMetadata.L1.max_pq);
#ifndef NDEBUG
					if (m_DoviExtensionMetadata.L1.present || m_DoviExtensionMetadata.L2.present) {
						m_strStatsHDR.append(L"\n Dolby Vision metadata:");

						if (m_DoviExtensionMetadata.L1.present) {
							m_strStatsHDR.append(std::format(L"\n  L1: min/max/avg pq : {}/{}/{}",
															 m_DoviExtensionMetadata.L1.min_pq,
															 m_DoviExtensionMetadata.L1.max_pq,
															 m_DoviExtensionMetadata.L1.avg_pq));
						}

						if (m_DoviExtensionMetadata.L2.present) {
							m_strStatsHDR.append(
								std::format(L"\n  L2: trim_slope: {:.2f}\n      trim_offset: {:.2f}\n      trim_power: {:.2f}\n      trim_saturation_gain: {:.2f}\n      trim_chroma_weight: {:.2f}",
											m_DoviExtensionMetadata.L2.trim_slope,
											m_DoviExtensionMetadata.L2.trim_offset,
											m_DoviExtensionMetadata.L2.trim_power,
											m_DoviExtensionMetadata.L2.trim_saturation_gain,
											m_DoviExtensionMetadata.L2.trim_chroma_weight));
						}
					}
#endif
				} else if (m_lastHdr10.bValid) {
					if (m_bHdrLocalToneMapping) {
						m_strStatsHDR += std::format(L"\n HDR10 metadata  : Max Luminance: {}", m_lastHdr10.hdr10.MaxMasteringLuminance);
						if (m_lastHdr10.hdr10.MaxContentLightLevel) {
							m_strStatsHDR += std::format(L", MaxCLL: {}", m_lastHdr10.hdr10.MaxContentLightLevel);
						}
						m_strStatsHDR += L" nits";
					} else {
						m_strStatsHDR += std::format(L", {} nits", m_lastHdr10.hdr10.MaxMasteringLuminance);
					}
				}
			} else if (m_bConvertToSdr) {
				m_strStatsHDR.append(L"Convert to SDR");
#ifndef NDEBUG
				if (m_DoviExtensionMetadata.L2.present) {
					m_strStatsHDR.append(L"\n Dolby Vision metadata:");
					m_strStatsHDR.append(
						std::format(L"\n  L2: trim_slope: {:.2f}\n      trim_offset: {:.2f}\n      trim_power: {:.2f}\n      trim_saturation_gain: {:.2f}\n      trim_chroma_weight: {:.2f}",
									m_DoviExtensionMetadata.L2.trim_slope,
									m_DoviExtensionMetadata.L2.trim_offset,
									m_DoviExtensionMetadata.L2.trim_power,
									m_DoviExtensionMetadata.L2.trim_saturation_gain,
									m_DoviExtensionMetadata.L2.trim_chroma_weight));
				}
#endif
			} else {
				m_strStatsHDR.append(L"Not used");
			}
		} else {
			m_strStatsHDR.clear();
		}

		UpdateStatsPresent();
	}
	else {
		m_strStatsHeader = L"Error";
		m_strStatsVProc.clear();
		m_strStatsInputFmt.clear();
		//m_strStatsPostProc.clear();
		m_strStatsHDR.clear();
		m_strStatsPresent.clear();
	}
}

/*
void CDX11VideoProcessor::UpdateStatsPostProc()
{
	if (m_strCorrection || m_pPostScaleShaders.size() || m_bFinalPass) {
		m_strStatsPostProc.assign(L"\nPostProcessing:");
		if (m_strCorrection) {
			m_strStatsPostProc += std::format(L" {},", m_strCorrection);
		}
		if (m_pPostScaleShaders.size()) {
			m_strStatsPostProc += std::format(L" shaders[{}],", m_pPostScaleShaders.size());
		}
		if (m_bFinalPass) {
			m_strStatsPostProc.append(L" dither");
		}
		str_trim_end(m_strStatsPostProc, ',');
	}
	else {
		m_strStatsPostProc.clear();
	}
}
*/

HRESULT CDX11VideoProcessor::DrawStats(ID3D11Texture2D* pRenderTarget)
{
	if (m_windowRect.IsRectEmpty()) {
		return E_ABORT;
	}

	std::wstring str;
	str.reserve(700);
	str.assign(m_strStatsHeader);
	str.append(m_strStatsDispInfo);
	str += std::format(L"\nGraph. Adapter: {}", m_strAdapterDescription);

	wchar_t frametype = (m_SampleFormat != D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE) ? 'i' : 'p';
	str += std::format(
		L"\nFrame rate    : {:7.3f}{},{:7.3f}",
		m_pFilter->m_FrameStats.GetAverageFps(),
		frametype,
		m_pFilter->m_DrawStats.GetAverageFps()
	);

	str.append(m_strStatsInputFmt);
	if (m_Dovi.bValid && m_Dovi.bHasMMR) {
		str.append(L", MMR");
	}
	str.append(m_strStatsVProc);

	const UINT dstW = m_videoRect.Width();
	const UINT dstH = m_videoRect.Height();
	if (m_iRotation) {
		str += std::format(L"\nScaling       : {}x{} r{}\u00B0> {}x{}", m_srcRectWidth, m_srcRectHeight, m_iRotation, dstW, dstH);
	} else {
		str += std::format(L"\nScaling       : {}x{} -> {}x{}", m_srcRectWidth, m_srcRectHeight, dstW, dstH);
	}
	if (m_srcRectWidth != dstW || m_srcRectHeight != dstH) {
		if (m_D3D11VP.IsReady() && m_bVPScaling && !m_bVPScalingUseShaders) {
			str.append(L" D3D11");
			if (m_bVPUseSuperRes && m_srcRectWidth < dstW && m_srcRectHeight < dstH) {
				str.append(L" SuperResolution*");
			}
		} else {
			str += L' ';
			if (m_strShaderX) {
				str.append(m_strShaderX);
				if (m_strShaderY && m_strShaderY != m_strShaderX) {
					str += L'/';
					str.append(m_strShaderY);
				}
			} else if (m_strShaderY) {
				str.append(m_strShaderY);
			}
		}
	}

	if (m_strCorrection || m_pPostScaleShaders.size() || m_bDitherUsed) {
		str.append(L"\nPostProcessing:");
		if (m_strCorrection) {
			str += std::format(L" {},", m_strCorrection);
		}
		if (m_pPostScaleShaders.size()) {
			str += std::format(L" shaders[{}],", m_pPostScaleShaders.size());
		}
		if (m_bDitherUsed) {
			str.append(L" dither");
		}
		str_trim_end(str, ',');
	}
	str.append(m_strStatsHDR);
	str.append(m_strStatsPresent);

	str += std::format(L"\nFrames        : {:5}, skipped: {}/{}, failed: {}",
		m_pFilter->m_FrameStats.GetFrames(), m_pFilter->m_DrawStats.m_dropped, m_RenderStats.dropped2, m_RenderStats.failed);

	str += std::format(L"\nTimes(ms)     : Copy{:3}, Paint{:3}, Present{:3}",
		m_RenderStats.copyticks    * 1000 / GetPreciseTicksPerSecondI(),
		m_RenderStats.paintticks   * 1000 / GetPreciseTicksPerSecondI(),
		m_RenderStats.presentticks * 1000 / GetPreciseTicksPerSecondI());

	str += std::format(L"\nSync offset   : {:+3} ms", (m_RenderStats.syncoffset + 5000) / 10000);

#if SYNC_OFFSET_EX
	{
		const auto [so_min, so_max] = m_Syncs.MinMax();
		const auto [sod_min, sod_max] = m_SyncDevs.MinMax();
		str += std::format(L", range[{:+3.0f};{:+3.0f}], max change{:+3.0f}/{:+3.0f}",
			so_min / 10000.0f,
			so_max / 10000.0f,
			sod_min / 10000.0f,
			sod_max / 10000.0f);
	}
#endif
#if TEST_TICKS
	str += std::format(L"\n1:{:6.3f}, 2:{:6.3f}, 3:{:6.3f}, 4:{:6.3f}, 5:{:6.3f}, 6:{:6.3f} ms",
		m_RenderStats.t1 * 1000 / GetPreciseTicksPerSecond(),
		m_RenderStats.t2 * 1000 / GetPreciseTicksPerSecond(),
		m_RenderStats.t3 * 1000 / GetPreciseTicksPerSecond(),
		m_RenderStats.t4 * 1000 / GetPreciseTicksPerSecond(),
		m_RenderStats.t5 * 1000 / GetPreciseTicksPerSecond(),
		m_RenderStats.t6 * 1000 / GetPreciseTicksPerSecond());
#endif

	ID3D11RenderTargetView* pRenderTargetView = nullptr;
	HRESULT hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
	if (S_OK == hr) {
		SIZE rtSize = m_windowRect.Size();

		m_StatsBackground.Draw(pRenderTargetView, rtSize);

		hr = m_Font3D.Draw2DText(pRenderTargetView, rtSize, m_StatsTextPoint.x, m_StatsTextPoint.y, m_dwStatsTextColor, str.c_str());
		static int col = m_StatsRect.right;
		if (--col < m_StatsRect.left) {
			col = m_StatsRect.right;
		}
		m_Rect3D.Set({ col, m_StatsRect.bottom - 11, col + 5, m_StatsRect.bottom - 1 }, rtSize, D3DCOLOR_XRGB(128, 255, 128));
		m_Rect3D.Draw(pRenderTargetView, rtSize);


		if (CheckGraphPlacement()) {
			m_Underlay.Draw(pRenderTargetView, rtSize);

			m_Lines.Draw();

			m_SyncLine.ClearPoints(rtSize);
			m_SyncLine.AddGFPoints(m_GraphRect.left, m_Xstep, m_Yaxis, m_Yscale,
				m_Syncs.Data(), m_Syncs.OldestIndex(), m_Syncs.Size(), D3DCOLOR_XRGB(100, 200, 100));
			m_SyncLine.UpdateVertexBuffer();
			m_SyncLine.Draw();
		}
		pRenderTargetView->Release();
	}

	return hr;
}

// IMFVideoProcessor

STDMETHODIMP CDX11VideoProcessor::SetProcAmpValues(DWORD dwFlags, DXVA2_ProcAmpValues *pValues)
{
	CheckPointer(pValues, E_POINTER);
	if (m_srcParams.cformat == CF_NONE) {
		return MF_E_TRANSFORM_TYPE_NOT_SET;
	}

	if (dwFlags & DXVA2_ProcAmp_Brightness) {
		m_DXVA2ProcAmpValues.Brightness.ll = std::clamp(pValues->Brightness.ll, m_DXVA2ProcAmpRanges[0].MinValue.ll, m_DXVA2ProcAmpRanges[0].MaxValue.ll);
	}
	if (dwFlags & DXVA2_ProcAmp_Contrast) {
		m_DXVA2ProcAmpValues.Contrast.ll = std::clamp(pValues->Contrast.ll, m_DXVA2ProcAmpRanges[1].MinValue.ll, m_DXVA2ProcAmpRanges[1].MaxValue.ll);
	}
	if (dwFlags & DXVA2_ProcAmp_Hue) {
		m_DXVA2ProcAmpValues.Hue.ll = std::clamp(pValues->Hue.ll, m_DXVA2ProcAmpRanges[2].MinValue.ll, m_DXVA2ProcAmpRanges[2].MaxValue.ll);
	}
	if (dwFlags & DXVA2_ProcAmp_Saturation) {
		m_DXVA2ProcAmpValues.Saturation.ll = std::clamp(pValues->Saturation.ll, m_DXVA2ProcAmpRanges[3].MinValue.ll, m_DXVA2ProcAmpRanges[3].MaxValue.ll);
	}

	if (dwFlags & DXVA2_ProcAmp_Mask) {
		CAutoLock cRendererLock(&m_pFilter->m_RendererLock);

		m_D3D11VP.SetProcAmpValues(&m_DXVA2ProcAmpValues);

		if (!m_D3D11VP.IsReady()) {
			SetShaderConvertColorParams();
		}
	}

	return S_OK;
}

// IMFVideoMixerBitmap

STDMETHODIMP CDX11VideoProcessor::SetAlphaBitmap(const MFVideoAlphaBitmap *pBmpParms)
{
	CheckPointer(pBmpParms, E_POINTER);
	CAutoLock cRendererLock(&m_pFilter->m_RendererLock);

	HRESULT hr = S_FALSE;

	if (pBmpParms->GetBitmapFromDC && pBmpParms->bitmap.hdc) {
		HBITMAP hBitmap = (HBITMAP)GetCurrentObject(pBmpParms->bitmap.hdc, OBJ_BITMAP);
		if (!hBitmap) {
			return E_INVALIDARG;
		}
		DIBSECTION info = {0};
		if (!::GetObjectW(hBitmap, sizeof(DIBSECTION), &info)) {
			return E_INVALIDARG;
		}
		BITMAP& bm = info.dsBm;
		if (!bm.bmWidth || !bm.bmHeight || bm.bmBitsPixel != 32 || !bm.bmBits) {
			return E_INVALIDARG;
		}

		hr = m_TexAlphaBitmap.CheckCreate(m_pDevice, DXGI_FORMAT_B8G8R8A8_UNORM, bm.bmWidth, bm.bmHeight, Tex2D_DefaultShader);
		DLogIf(FAILED(hr), L"CDX11VideoProcessor::SetAlphaBitmap() : CheckCreate() failed with error {}", HR2Str(hr));
		if (S_OK == hr) {
			m_pDeviceContext->UpdateSubresource(m_TexAlphaBitmap.pTexture, 0, nullptr, bm.bmBits, bm.bmWidthBytes, 0);
		}
	} else {
		return E_INVALIDARG;
	}

	m_bAlphaBitmapEnable = SUCCEEDED(hr) && m_TexAlphaBitmap.pShaderResource;

	if (m_bAlphaBitmapEnable) {
		m_AlphaBitmapRectSrc = { 0, 0, (LONG)m_TexAlphaBitmap.desc.Width, (LONG)m_TexAlphaBitmap.desc.Height };
		m_AlphaBitmapNRectDest = { 0, 0, 1, 1 };

		m_pAlphaBitmapVertex.Release();

		hr = UpdateAlphaBitmapParameters(&pBmpParms->params);
	}

	return hr;
}

STDMETHODIMP CDX11VideoProcessor::UpdateAlphaBitmapParameters(const MFVideoAlphaBitmapParams *pBmpParms)
{
	CheckPointer(pBmpParms, E_POINTER);
	CAutoLock cRendererLock(&m_pFilter->m_RendererLock);

	if (m_bAlphaBitmapEnable) {
		if (pBmpParms->dwFlags & MFVideoAlphaBitmap_SrcRect) {
			m_AlphaBitmapRectSrc = pBmpParms->rcSrc;
			m_pAlphaBitmapVertex.Release();
		}
		if (!m_pAlphaBitmapVertex) {
			HRESULT hr = CreateVertexBuffer(m_pDevice, &m_pAlphaBitmapVertex, m_TexAlphaBitmap.desc.Width, m_TexAlphaBitmap.desc.Height, m_AlphaBitmapRectSrc, 0, false);
			if (FAILED(hr)) {
				m_bAlphaBitmapEnable = false;
				return hr;
			}
		}
		if (pBmpParms->dwFlags & MFVideoAlphaBitmap_DestRect) {
			m_AlphaBitmapNRectDest = pBmpParms->nrcDest;
		}
		DWORD validFlags = MFVideoAlphaBitmap_SrcRect|MFVideoAlphaBitmap_DestRect;

		return ((pBmpParms->dwFlags & validFlags) == validFlags) ? S_OK : S_FALSE;
	} else {
		return MF_E_NOT_INITIALIZED;
	}
}

void CDX11VideoProcessor::SetCallbackDevice()
{
	if (!m_bCallbackDeviceIsSet && m_pDevice && m_pFilter->m_pSub11CallBack) {
		m_bCallbackDeviceIsSet = SUCCEEDED(m_pFilter->m_pSub11CallBack->SetDevice11(m_pDevice));
	}
}

void CDX11VideoProcessor::UpdateSubPic()
{
	ASSERT(m_pDevice);

	if (m_pFilter->m_pSubPicProvider) {
		if (m_pSubPicAllocator) {
			m_pSubPicAllocator->ChangeDevice(m_pDevice);
		}

		if (m_pFilter->m_pSubPicQueue) {
			m_pFilter->m_pSubPicQueue->Invalidate();
			m_pFilter->m_pSubPicQueue->SetSubPicProvider(m_pFilter->m_pSubPicProvider);
		}
	}
}

void CDX11VideoProcessor::SwitchFullScreen(bool set)
{
	m_bFullScreen = set;

	if (HandleHDRToggle()) {
		InitMediaType(&m_pFilter->m_inputMT);
		InitSwapChain(false);
	}
}
