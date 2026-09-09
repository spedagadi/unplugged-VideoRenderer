// test_ml_sky — Stage-1 external profiler for yolo26n-sem.onnx under
// ONNX Runtime + DirectML (mirrors the CFaceDetect pattern used in MPCVR).
//
// Build:  test_ml_sky.bat
// Usage:  test_ml_sky.exe <model.onnx> <frame.rgb> <W> <H>
//   frame.rgb = raw RGB (no header), W x H, 3 bytes/pixel, top-left origin.
//   Preprocess mirrors ultralytics: letterbox to 640x640 gray(114), /255,
//   NCHW float. Output0 is a uint8 [1,640,640] Cityscapes class map (sky=10).
//
// Prints a per-phase ms breakdown (init/preproc/run/decode/total), steady-state
// run timing, and the sky fraction + a coarse low-res sky map.

#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <vector>
#include <string>
#include <cmath>
#include <memory>

#define ORT_API_MANUAL_INIT
#include "onnxrt/onnxruntime_cxx_api.h"
#include "onnxrt/dml_provider_factory.h"

static HMODULE LoadOrtDll()
{
    // Explicit paths only — never a bare name (System32 has a stale 1.17 copy).
    // Prefer the DLL the renderer actually deploys beside the .ax.
    const wchar_t* cands[] = { L"C:\\Code\\VideoRenderer\\_bin\\MpcVideoRenderer-0.10.8.2602\\onnxruntime.dll",
                               L"C:\\Code\\VideoRenderer\\Source\\onnxrt\\onnxruntime.dll" };
    for (auto c : cands) {
        HMODULE h = LoadLibraryW(c);
        if (h) return h;
    }
    return nullptr;
}

static double NowMs()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

struct SkyResult {
    double init_ms = 0, preproc_ms = 0, run_ms = 0, decode_ms = 0;
    float  sky_frac = 0.0f;
    int    modelW = 0, modelH = 0;
    std::vector<unsigned char> mask; // H x W class map (ArgMax)
};

bool Run(const std::wstring& modelPath, const unsigned char* rgb, int srcW, int srcH,
         SkyResult& out)
{
    const double t0 = NowMs();

    HMODULE hOrt = LoadOrtDll();
    if (!hOrt) { printf("FAIL onnxruntime.dll not found\n"); return false; }
    Ort::InitApi(OrtGetApiBase()->GetApi(ORT_API_VERSION));

    Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // ---- session init ----
    const double ti0 = NowMs();
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "mpcvr_sky_spike");
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(1);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    opts.DisableMemPattern();
    opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    bool dml = false;
    try {
        OrtSessionOptionsAppendExecutionProvider_DML(opts, 0);
        dml = true;
    } catch (...) { printf("DirectML EP unavailable -> CPU\n"); }
    Ort::Session session(env, modelPath.c_str(), opts);
    out.init_ms = NowMs() - ti0;

    // ---- probe input shape (assume NHWC? no — NCHW [1,3,H,W]) ----
    auto typeInfo = session.GetInputTypeInfo(0);
    auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
    auto dims = tensorInfo.GetShape();
    const int mH = (int)(dims[2] > 0 ? dims[2] : 640);
    const int mW = (int)(dims[3] > 0 ? dims[3] : 640);
    out.modelH = mH; out.modelW = mW;

    // ---- preprocess: letterbox + /255 + NCHW ----
    const double tp0 = NowMs();
    const float scale = std::min((float)mW / srcW, (float)mH / srcH);
    const int sW = (int)(srcW * scale), sH = (int)(srcH * scale);
    const int padX = (mW - sW) / 2, padY = (mH - sH) / 2;
    const size_t px = (size_t)mH * mW;
    std::vector<float> x(3 * px, 114.0f / 255.0f); // gray padding
    for (int ry = 0; ry < sH; ry++) {
        const float sy = (float)(ry + 0.5f) / scale - 0.5f;
        const int y0 = std::max(0, std::min(srcH - 1, (int)sy));
        const int y1 = std::min(srcH - 1, y0 + 1);
        const float fy = sy - (float)y0;
        const unsigned char* row0 = rgb + (size_t)y0 * srcW * 3;
        const unsigned char* row1 = rgb + (size_t)y1 * srcW * 3;
        const int oy = ry + padY;
        for (int rx = 0; rx < sW; rx++) {
            const float sx = (float)(rx + 0.5f) / scale - 0.5f;
            const int x0 = std::max(0, std::min(srcW - 1, (int)sx));
            const int x1 = std::min(srcW - 1, x0 + 1);
            const float fx = sx - (float)x0;
            const float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy);
            const float w01 = (1 - fx) * fy,     w11 = fx * fy;
            const size_t idx = ((size_t)oy * mW + (rx + padX));
            for (int c = 0; c < 3; c++) {
                const unsigned r0 = row0[x0 * 3 + c], r1 = row0[x1 * 3 + c];
                const unsigned r2 = row1[x0 * 3 + c], r3 = row1[x1 * 3 + c];
                float v = (w00 * r0 + w10 * r1 + w01 * r2 + w11 * r3) / 255.0f;
                x[c * px + idx] = v;
            }
        }
    }
    out.preproc_ms = NowMs() - tp0;

    // ---- session run ----
    const double tr0 = NowMs();
    std::array<int64_t, 4> shape = {1, 3, mH, mW};
    std::array<const char*, 1> inNames = {"images"};
    std::array<const char*, 1> outNames = {"output0"};
    auto in = Ort::Value::CreateTensor<float>(memInfo, x.data(), x.size(), shape.data(), shape.size());
    std::vector<Ort::Value> outputs;
    for (int i = 0; i < 5; i++) { // steady-state: 5 runs, report last
        outputs = session.Run(Ort::RunOptions{nullptr}, inNames.data(), &in, 1, outNames.data(), 1);
    }
    out.run_ms = (NowMs() - tr0) / 5.0;

    // ---- decode: uint8 [1,H,W] class map, sky=10, valid (non-pad) region ----
    const double td0 = NowMs();
    const unsigned char* cls = nullptr;
    for (auto& o : outputs) {
        if (o.IsTensor() && o.GetTensorTypeAndShapeInfo().GetShape().size() == 3) {
            auto shp = o.GetTensorTypeAndShapeInfo().GetShape();
            if (shp[1] == (int64_t)mH && shp[2] == (int64_t)mW) {
                cls = o.GetTensorData<unsigned char>();
                break;
            }
        }
    }
    if (!cls) { printf("FAIL: no [1,H,W] uint8 output found\n"); return false; }
    out.mask.assign(cls, cls + (size_t)mH * mW);
    long long skyCnt = 0, validCnt = 0, skyAll = 0;
    for (int y = 0; y < mH; y++)
        for (int x = 0; x < mW; x++) {
            const unsigned char c = out.mask[(size_t)y * mW + x];
            if (y >= padY && y < padY + sH && x >= padX && x < padX + sW) {
                validCnt++;
                if (c == 10) skyCnt++;
            }
            if (c == 10) skyAll++;
        }
    out.sky_frac = (float)skyCnt / (float)validCnt;
    out.decode_ms = NowMs() - td0;
    printf("EP=%s  model %dx%d  init=%.2fms preproc=%.2fms run=%.2fms/run(5-run avg) decode=%.2fms\n",
           dml ? "DirectML" : "CPU", mW, mH, out.init_ms, out.preproc_ms, out.run_ms, out.decode_ms);
    printf("sky_frac(valid)=%.4f  sky_frac(all)=%.4f\n", out.sky_frac, (float)skyAll / (mH * mW));

    // coarse 16x9 sky occupancy map
    printf("coarse sky map (16 cols x 9 rows, '#'=sky>50pct):\n");
    for (int by = 0; by < 9; by++) {
        printf("  ");
        for (int bx = 0; bx < 16; bx++) {
            int cnt = 0, n = 0;
            for (int y = by * mH / 9; y < (by + 1) * mH / 9; y++)
                for (int x = bx * mW / 16; x < (bx + 1) * mW / 16; x++) {
                    if (y >= padY && y < padY + sH && x >= padX && x < padX + sW) { n++; if (out.mask[(size_t)y * mW + x] == 10) cnt++; }
                }
            printf("%c", (n && cnt * 2 > n) ? '#' : (n ? '.' : ' '));
        }
        printf("\n");
    }
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 5) {
        printf("usage: test_ml_sky.exe <model.onnx> <frame.rgb> <W> <H>\n");
        return 1;
    }
    wchar_t wmodel[512], wfrm[512];
    mbstowcs(wmodel, argv[1], 511);
    mbstowcs(wfrm, argv[2], 511);
    const int W = atoi(argv[3]), H = atoi(argv[4]);
    if (W <= 0 || H <= 0) { printf("bad dims\n"); return 1; }

    FILE* f = nullptr;
    _wfopen_s(&f, wfrm, L"rb");
    if (!f) { printf("cannot open %s\n", argv[2]); return 1; }
    std::vector<unsigned char> rgb((size_t)W * H * 3);
    if (fread(rgb.data(), 1, rgb.size(), f) != rgb.size()) { printf("short read\n"); fclose(f); return 1; }
    fclose(f);

    SkyResult r;
    const double t0 = NowMs();
    bool ok = Run(std::wstring(wmodel), rgb.data(), W, H, r);
    printf("total-wall=%.2fms\n", NowMs() - t0);
    return ok ? 0 : 2;
}