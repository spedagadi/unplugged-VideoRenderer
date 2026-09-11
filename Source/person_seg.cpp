#include "stdafx.h"
#include "person_seg.h"
#include "Utils/Util.h"
#include "Utils/StringUtil.h"

// ONNX Runtime — manual init, same rationale as face_detect.cpp / sky_seg.cpp.
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
// (Sky seg preloads it too; LoadLibrary is ref-counted so the second call is a no-op.)
static HMODULE g_hPersonOrtDll = nullptr;
static bool PreloadPersonOnnxRuntime()
{
    if (g_hPersonOrtDll) return true;

    HMODULE hThis = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&PreloadPersonOnnxRuntime, &hThis);
    if (!hThis) return false;

    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(hThis, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash)
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"onnxruntime.dll");

    g_hPersonOrtDll = LoadLibraryW(path);
    return g_hPersonOrtDll != nullptr;
}

struct CPersonSeg::OrtState {
    Ort::Env            env{ORT_LOGGING_LEVEL_WARNING, "mpcvr_person"};
    Ort::SessionOptions opts;
    std::unique_ptr<Ort::Session> session;
    Ort::MemoryInfo     memInfo{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};
    std::vector<const char*> inputNames;
    std::vector<const char*> outputNames;
    std::vector<std::string> inputNameStrs;
    std::vector<std::string> outputNameStrs;
};

void CPersonSeg::Log(const wchar_t* fmt, ...)
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

CPersonSeg::CPersonSeg() = default;

CPersonSeg::~CPersonSeg()
{
    Shutdown();
}

HRESULT CPersonSeg::Init(const std::wstring& modelPath)
{
    if (m_ort) return S_FALSE;
    Log(L"PSEG_INIT starting model=%s\n", modelPath.c_str());

    if (!PreloadPersonOnnxRuntime()) {
        m_lastError = L"onnxruntime.dll not found next to .ax";
        Log(L"PSEG_INIT FAILED: DLL not found\n");
        return E_FAIL;
    }
    Log(L"PSEG_INIT DLL loaded OK\n");

    __try {
        Ort::InitApi(OrtGetApiBase()->GetApi(ORT_API_VERSION));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        m_lastError = L"ORT API init failed";
        return E_FAIL;
    }
    return InitInternal(modelPath);
}

HRESULT CPersonSeg::InitInternal(const std::wstring& modelPath)
{
    try {
        m_ort = std::make_unique<OrtState>();
        m_ort->opts.SetIntraOpNumThreads(1);
        m_ort->opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        m_ort->opts.DisableMemPattern();
        m_ort->opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        try {
            OrtSessionOptionsAppendExecutionProvider_DML(m_ort->opts, 0);
            Log(L"PSEG_INIT DirectML EP OK\n");
        }
        catch (...) { Log(L"PSEG_INIT DirectML failed, using CPU\n"); }

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

        Log(L"PSEG_INIT ok inputs=%zu outputs=%zu shape=%dx%d\n", nIn, nOut, m_modelW, m_modelH);
        m_ready = m_modelW > 0 && m_modelH > 0;
        return m_ready ? S_OK : E_FAIL;
    }
    catch (const Ort::Exception& e) {
        m_lastError = L"Ort: " + A2WStr(e.what());
        Log(L"PSEG_INIT FAILED Ort: %S\n", e.what());
        m_ort.reset();
        return E_FAIL;
    }
    catch (...) {
        m_lastError = L"unknown init error";
        m_ort.reset();
        return E_FAIL;
    }
}

void CPersonSeg::Shutdown()
{
    m_ready = false;
    m_ort.reset();
}

// Bilinear upscale of a float image `src` (HxW) into `dst` (hOut x wOut),
// using the same (y+0.5)*invScale-0.5 source mapping as the letterbox preprocess.
static void BilinearResize(const float* src, int sW, int sH,
                           float* dst, int dW, int dH)
{
    const float iw = (float)sW / (float)dW;
    const float ih = (float)sH / (float)dH;
    for (int dy = 0; dy < dH; dy++) {
        const float sy = ((float)dy + 0.5f) * ih - 0.5f;
        const int y0 = std::max(0, std::min(sH - 1, (int)sy));
        const int y1 = std::min(sH - 1, y0 + 1);
        const float fy = sy - (float)y0;
        float* rowD = dst + (size_t)dy * dW;
        for (int dx = 0; dx < dW; dx++) {
            const float sx = ((float)dx + 0.5f) * iw - 0.5f;
            const int x0 = std::max(0, std::min(sW - 1, (int)sx));
            const int x1 = std::min(sW - 1, x0 + 1);
            const float fx = sx - (float)x0;
            const float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy);
            const float w01 = (1 - fx) * fy, w11 = fx * fy;
            rowD[dx] = w00 * src[y0 * sW + x0] + w10 * src[y0 * sW + x1]
                     + w01 * src[y1 * sW + x0] + w11 * src[y1 * sW + x1];
        }
    }
}

static inline float Sigmoid(float x) { return 1.0f / (1.0f + expf(-x)); }

bool CPersonSeg::DetectPersons(const uint8_t* bgra, int width, int height, int stride, Result& out)
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
        Log(L"PSEG_RUN Ort exception: %S\n", e.what());
        return false;
    }
    out.runMs = NowMs_qpc() - t1;

    // ---- decode: post-NMS dets [1,300,38] + protos [1,32,160,160] ----
    const double t2 = NowMs_qpc();
    const float* dets = nullptr;
    const float* proto = nullptr;
    int nDets = 0, protoC = 0, protoH = 0, protoW = 0;
    for (auto& o : outputs) {
        const auto shp = o.GetTensorTypeAndShapeInfo().GetShape();
        if (o.IsTensor() && !dets && shp.size() == 3 && shp[1] > 0 && shp[2] >= 38) {
            dets = o.GetTensorData<float>(); nDets = (int)shp[1];
        } else if (o.IsTensor() && !proto && shp.size() == 4 && shp[1] > 0) {
            proto = o.GetTensorData<float>(); protoC = (int)shp[1]; protoH = (int)shp[2]; protoW = (int)shp[3];
        }
    }
    if (!dets || !proto) { Log(L"PSEG_DECODE missing output tensors\n"); return false; }
    // model always emits into the model input rect (letterbox). Content lives at
    // padX..padX+sW / padY..padY+sH; for the 640x360 preview sW=640 sH=360 padX=0 padY=140.

    // union mask at model res (mH x mW) — buffers reused across calls so the
    // per-frame path does zero allocation.
    const size_t n160 = (size_t)protoH * protoW;
    const size_t n640 = (size_t)mH * mW;
    if (m_mask160.size() != n160) m_mask160.assign(n160, 0.0f);
    if (m_mask640.size() != n640) m_mask640.assign(n640, 0.0f);
    if (m_map640.size() != n640) m_map640.assign(n640, 0);
    std::fill(m_map640.begin(), m_map640.end(), 0);

    const float scoreThr = 0.25f;
    Result::Box pendingBoxes[8];
    int nPending = 0;
    for (int i = 0; i < nDets; i++) {
        const float* r = dets + (size_t)i * 38;
        const float score = r[4];
        const float cls = r[5];
        if (score < scoreThr || cls != 0.0f) continue;
        const float x1 = r[0], y1 = r[1], x2 = r[2], y2 = r[3];
        if (x2 <= x1 || y2 <= y1 || x1 < -0.5f || y1 < -0.5f) continue;  // padded row
        if (nPending < 8) {
            pendingBoxes[nPending].x = (x1 - padX) / (float)sW;
            pendingBoxes[nPending].y = (y1 - padY) / (float)sH;
            pendingBoxes[nPending].w = (x2 - x1) / (float)sW;
            pendingBoxes[nPending].h = (y2 - y1) / (float)sH;
            nPending++;
        }
        const int ix1 = std::max(0, (int)std::floor(x1));
        const int iy1 = std::max(0, (int)std::floor(y1));
        const int ix2 = std::min(mW, (int)std::ceil(x2));
        const int iy2 = std::min(mH, (int)std::ceil(y2));
        if (ix2 <= ix1 || iy2 <= iy1) continue;

        // raw mask = sigmoid( sum_k coeff[k] * proto[k] ) at 160x160
        std::fill(m_mask160.begin(), m_mask160.end(), 0.0f);
        const float* coeff = r + 6;
        for (int c = 0; c < protoC; c++) {
            const float k = coeff[c];
            const float* pc = proto + (size_t)c * n160;
            for (size_t p = 0; p < n160; p++)
                m_mask160[p] += k * pc[p];
        }
        for (size_t p = 0; p < n160; p++)
            m_mask160[p] = Sigmoid(m_mask160[p]);

        // upscale to model res, threshold, OR into the union map.
        // LOW threshold (0.25 vs the naive 0.5): the mask becomes an OVER-inclusive
        // prior — under complex interactions (a hug, people separating) it fattens
        // around the true bodies, so it can never under-cover a face mid-motion.
        // The tone gate's edge-pin (UpdateSkinGate) does the fine boundary, so the
        // added spill is clamped by the source luma, not printed by the mask.
        BilinearResize(m_mask160.data(), protoW, protoH, m_mask640.data(), mW, mH);
        for (int y = iy1; y < iy2; y++)
            for (int xx = ix1; xx < ix2; xx++)
                if (m_mask640[(size_t)y * mW + xx] > 0.25f)
                    m_map640[(size_t)y * mW + xx] = 255;
    }

    // crop the letterbox padding -> content-aligned person map (like the sky map)
    out.mapW = sW;
    out.mapH = sH;
    out.map.assign((size_t)sH * sW, 0);
    long long nPerson = 0;
    for (int ry = 0; ry < sH; ry++)
        for (int rx = 0; rx < sW; rx++) {
            const uint8_t v = m_map640[(size_t)(ry + padY) * mW + (rx + padX)];
            out.map[(size_t)ry * sW + rx] = v;
            if (v) nPerson++;
        }
    out.personFrac = (float)nPerson / (float)((size_t)sH * sW);
    out.decMs = NowMs_qpc() - t2;
    out.ran = true;

    m_callCount++;
    if (m_logEnabled && m_logFile && (m_callCount & 15) == 1) {
        fwprintf(m_logFile, L"PSEG_TRACE call=%llu pre=%.1fms run=%.1fms dec=%.1fms pfrac=%.4f pad=(%d,%d,%d,%d)\n",
            (unsigned long long)m_callCount, out.preMs, out.runMs, out.decMs,
            (double)out.personFrac, padX, padY, sW, sH);
        fflush(m_logFile);
    }
    return true;
}