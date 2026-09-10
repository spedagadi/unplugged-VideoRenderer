#include "stdafx.h"
#include "sky_seg.h"
#include "Utils/Util.h"
#include "Utils/StringUtil.h"

// ONNX Runtime — manual init (see face_detect.cpp for the rationale).
#define ORT_API_MANUAL_INIT
#include "onnxrt/onnxruntime_cxx_api.h"
#include "onnxrt/dml_provider_factory.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cmath>

namespace {
double NowMs_qpc()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)freq.QuadPart;
}
} // namespace

// Pre-load onnxruntime.dll from the same directory as this module (.ax);
// /DELAYLOAD keeps an eager resolver from grabbing a stale System32 copy.
static HMODULE g_hSkyOrtDll = nullptr;
static bool PreloadSkyOnnxRuntime()
{
    if (g_hSkyOrtDll) return true;

    HMODULE hThis = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&PreloadSkyOnnxRuntime, &hThis);
    if (!hThis) return false;

    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(hThis, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash)
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"onnxruntime.dll");

    g_hSkyOrtDll = LoadLibraryW(path);
    if (!g_hSkyOrtDll) {
        wchar_t buf[128];
        swprintf_s(buf, L"[SKY] LoadLibrary failed err=%lu path=%s\n", ::GetLastError(), path);
        OutputDebugStringW(buf);
    }
    return g_hSkyOrtDll != nullptr;
}

struct CSkySeg::OrtState {
    Ort::Env            env{ORT_LOGGING_LEVEL_WARNING, "mpcvr_sky"};
    Ort::SessionOptions opts;
    std::unique_ptr<Ort::Session> session;
    Ort::MemoryInfo     memInfo{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};
    std::vector<const char*> inputNames;
    std::vector<const char*> outputNames;
    std::vector<std::string> inputNameStrs;
    std::vector<std::string> outputNameStrs;
};

void CSkySeg::Log(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf, fmt, ap);
    va_end(ap);
    OutputDebugStringW(buf);
    if (m_logFile) {
        fwprintf(m_logFile, L"%s", buf);
        fflush(m_logFile);
    }
}

CSkySeg::CSkySeg() = default;   // after OrtState is complete (header pimpl pitfall)

CSkySeg::~CSkySeg()
{
    Shutdown();
}

HRESULT CSkySeg::Init(const std::wstring& modelPath)
{
    if (m_ort) return S_FALSE;
    Log(L"SKY_INIT starting model=%s\n", modelPath.c_str());

    if (!PreloadSkyOnnxRuntime()) {
        m_lastError = L"onnxruntime.dll not found next to .ax";
        Log(L"SKY_INIT FAILED: DLL not found\n");
        return E_FAIL;
    }
    Log(L"SKY_INIT DLL loaded OK\n");

    // SEH guarded call must live in a function WITHOUT any C++ objects needing
    // unwinding (C2712) and without C++ try/catch (C2713) — its only job is
    // to call InitInternal once the raw API pointer is live.
    __try {
        Ort::InitApi(OrtGetApiBase()->GetApi(ORT_API_VERSION));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        m_lastError = L"ORT API init failed";
        return E_FAIL;
    }
    return InitInternal(modelPath);
}

HRESULT CSkySeg::InitInternal(const std::wstring& modelPath)
{
    try {
        m_ort = std::make_unique<OrtState>();
        m_ort->opts.SetIntraOpNumThreads(1);
        m_ort->opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        m_ort->opts.DisableMemPattern();
        m_ort->opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        try {
            OrtSessionOptionsAppendExecutionProvider_DML(m_ort->opts, 0);
            Log(L"SKY_INIT DirectML EP OK\n");
        }
        catch (...) { Log(L"SKY_INIT DirectML failed, using CPU\n"); }

        m_ort->session = std::make_unique<Ort::Session>(m_ort->env, modelPath.c_str(), m_ort->opts);

        Ort::AllocatorWithDefaultOptions alloc;
        const size_t nIn = m_ort->session->GetInputCount();
        for (size_t i = 0; i < nIn; i++)
            m_ort->inputNameStrs.push_back(m_ort->session->GetInputNameAllocated(i, alloc).get());
        for (auto& s : m_ort->inputNameStrs) m_ort->inputNames.push_back(s.c_str());
        const size_t nOut = m_ort->session->GetOutputCount();
        for (size_t i = 0; i < nOut; i++)
            m_ort->outputNameStrs.push_back(m_ort->session->GetOutputNameAllocated(i, alloc).get());
        for (auto& s : m_ort->outputNameStrs) m_ort->outputNames.push_back(s.c_str());

        auto tensorInfo = m_ort->session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
        auto dims = tensorInfo.GetShape();
        m_modelW = (dims.size() == 4 && dims[3] > 0) ? (int)dims[3] : 0;
        m_modelH = (dims.size() == 4 && dims[2] > 0) ? (int)dims[2] : 0;

        Log(L"SKY_INIT ok inputs=%zu outputs=%zu shape=%dx%d\n", nIn, nOut, m_modelW, m_modelH);
        m_ready = m_modelW > 0 && m_modelH > 0;
        return m_ready ? S_OK : E_FAIL;
    }
    catch (const Ort::Exception& e) {
        m_lastError = L"Ort: " + A2WStr(e.what());
        Log(L"SKY_INIT FAILED Ort: %S\n", e.what());
        m_ort.reset();
        return E_FAIL;
    }
    catch (...) {
        m_lastError = L"unknown init error";
        m_ort.reset();
        return E_FAIL;
    }
}

void CSkySeg::Shutdown()
{
    m_ready = false;
    m_ort.reset();
}

bool CSkySeg::DetectSky(const uint8_t* bgra, int width, int height, int stride, Result& out)
{
    out = Result{};
    if (!m_ready || !bgra || !m_ort || !m_ort->session) return false;

    const int mW = m_modelW, mH = m_modelH;
    const float scale = std::min((float)mW / width, (float)mH / height);
    const int sW = (int)(width * scale), sH = (int)(height * scale);
    const int padX = (mW - sW) / 2, padY = (mH - sH) / 2;
    const float invScale = 1.0f / scale;

    // ---- preprocess: letterbox (gray 114) + BGRA->RGB NCHW float /255 ----
    const double t0 = NowMs_qpc();
    const size_t px = (size_t)mH * mW;
    std::vector<float> x(3 * px, 114.0f / 255.0f);
    for (int ry = 0; ry < sH; ry++) {
        const float srcY = (float)(ry + 0.5f) * invScale - 0.5f;
        const int y0 = std::max(0, std::min(height - 1, (int)srcY));
        const int y1 = std::min(height - 1, y0 + 1);
        const float fy = srcY - (float)y0;
        const uint8_t* row0 = bgra + (size_t)y0 * stride;
        const uint8_t* row1 = bgra + (size_t)y1 * stride;
        const int oy = ry + padY;
        for (int rx = 0; rx < sW; rx++) {
            const float srcX = (float)(rx + 0.5f) * invScale - 0.5f;
            const int x0 = std::max(0, std::min(width - 1, (int)srcX));
            const int x1 = std::min(width - 1, x0 + 1);
            const float fx = srcX - (float)x0;
            const float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy);
            const float w01 = (1 - fx) * fy, w11 = fx * fy;
            const size_t idx = (size_t)oy * mW + (rx + padX);
            for (int c = 0; c < 3; c++) {
                // BGRA buffers: r at +2, g at +1, b at +0; model wants RGB order.
                const int off = c == 0 ? 2 : (c == 1 ? 1 : 0);
                const float v = (w00 * row0[x0 * 4 + off] + w10 * row0[x1 * 4 + off] +
                                 w01 * row1[x0 * 4 + off] + w11 * row1[x1 * 4 + off]) * (1.0f / 255.0f);
                x[c * px + idx] = v;
            }
        }
    }
    out.preMs = NowMs_qpc() - t0;

    // ---- run ----
    const double t1 = NowMs_qpc();
    std::array<int64_t, 4> shape = {1, 3, mH, mW};
    auto in = Ort::Value::CreateTensor<float>(m_ort->memInfo, x.data(), x.size(), shape.data(), shape.size());
    std::vector<Ort::Value> outputs;
    try {
        outputs = m_ort->session->Run(Ort::RunOptions{nullptr},
            m_ort->inputNames.data(), &in, 1, m_ort->outputNames.data(), m_ort->outputNames.size());
    }
    catch (const Ort::Exception& e) {
        Log(L"SKY_RUN Ort exception: %S\n", e.what());
        return false;
    }
    out.runMs = NowMs_qpc() - t1;

    // ---- decode: uint8 [1,H,W] class map -> full-res 640x640 sky mask ----
    const double t2 = NowMs_qpc();
    const uint8_t* cls = nullptr;
    for (auto& o : outputs) {
        const auto shp = o.GetTensorTypeAndShapeInfo().GetShape();
        if (o.IsTensor() && shp.size() == 3 && shp[1] == mH && shp[2] == mW) {
            cls = o.GetTensorData<uint8_t>();
            break;
        }
    }
    if (!cls) { Log(L"SKY_DECODE no [1,H,W] uint8 output\n"); return false; }

    // Crop the gray letterbox padding: the shader samples content UV [0,1], so
// the mask must cover ONLY the content sub-rect [padX..padX+sW, padY..padY+sH]
// of the model output — a full-640 upload made the top pad read as a hard
// horizontal "not-sky" band. 255=sky, 0=not.
    out.map.resize((size_t)sH * sW);
    for (int my = 0; my < sH; my++)
        for (int mx = 0; mx < sW; mx++)
            out.map[(size_t)my * sW + mx] = (cls[(size_t)(my + padY) * mW + (mx + padX)] == 10) ? 255 : 0;
    out.mapW = sW;
    out.mapH = sH;

    // sky_frac over the valid (non-letterbox-pad) region
    long long skyValid = 0, valid = 0;
    for (int my = padY; my < padY + sH; my++)
        for (int mx = padX; mx < padX + sW; mx++)
            if (cls[(size_t)my * mW + mx] == 10) { skyValid++; valid++; }
    out.skyFrac = valid ? (float)skyValid / (float)valid : 0.0f;

    // sky luma from the source BGRA (gamma-2.2 luma), sampled on a grid
    {
        long long ss = 0, sn = 0;
        for (int my = padY; my < padY + sH; my += 4) {
            const float sy = (my - padY) * invScale;
            const int syi = std::max(0, std::min(height - 1, (int)sy));
            const uint8_t* row = bgra + (size_t)syi * stride;
            for (int mx = padX; mx < padX + sW; mx += 4) {
                const float sx = (mx - padX) * invScale;
                const int sxi = std::max(0, std::min(width - 1, (int)sx));
                if (cls[(size_t)my * mW + mx] == 10) {
                    const int b = row[sxi * 4 + 0], g = row[sxi * 4 + 1], r = row[sxi * 4 + 2];
                    ss += (long long)((r * 299 + g * 587 + b * 114) / 1000);
                    sn++;
                }
            }
        }
        out.skyLuma = sn ? (float)ss / (float)sn / 255.0f : 0.0f;
    }
    out.decMs = NowMs_qpc() - t2;
    out.ran = true;

    m_callCount++;
    if (m_logEnabled && m_logFile && (m_callCount & 15) == 1) {
        fwprintf(m_logFile, L"SKY_TRACE call=%llu pre=%.1fms run=%.1fms dec=%.1fms sfrac=%.4f luma=%.3f pad=(%d,%d,%d,%d)\n",
            (unsigned long long)m_callCount, out.preMs, out.runMs, out.decMs,
            (double)out.skyFrac, (double)out.skyLuma, padX, padY, sW, sH);
        fflush(m_logFile);
    }
    return true;
}