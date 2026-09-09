#pragma once

// Zion ML — sky semantic segmentation (yolo26n-sem, Cityscapes 19-class,
// sky = class index 10). ONNX Runtime + DirectML, mirroring CFaceDetect.
//
// Runtime contract (probed from the exported ONNX):
//   input   images   : float [1, 3, 640, 640]  (RGB, 0..1, gray-114 letterbox)
//   output  output0  : uint8 [1, 640, 640]      (argmax class index map)
//
// DetectSky() consumes the same SDR-ized 480x288 BGRA8 preview the face
// detector uses (PQ->SDR is already the right domain for Cityscapes images).

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

struct OrtEnv;
struct OrtSession;
struct OrtSessionOptions;
struct OrtRunOptions;

class CSkySeg {
public:
    struct Result {
        int             mapW = 0;    // sky map dims (= model res, 640x640)
        int             mapH = 0;
        std::vector<unsigned char> map;   // R8-like: 255 = sky, 0 = not
        float           skyFrac = 0.0f;   // fraction of valid-letterbox pixels == sky
        float           skyLuma = 0.0f;   // mean gamma-2.2 luma of sky pixels
        double          preMs = 0, runMs = 0, decMs = 0;
        bool            ran = false;
    };

    CSkySeg();   // defined out-of-line: defaulting in the header would
    ~CSkySeg();  // instantiate unique_ptr<OrtState> on an incomplete type

    CSkySeg(const CSkySeg&) = delete;
    CSkySeg& operator=(const CSkySeg&) = delete;

    HRESULT Init(const std::wstring& modelPath);
    void    Shutdown();
    void    SetLogFile(FILE* f)   { m_logFile = f; }
    void    SetLogEnabled(bool e) { m_logEnabled = e; }
    bool    IsReady() const       { return m_ready; }
    const std::wstring& GetLastError() const { return m_lastError; }

    // Synchronous inference on the caller's thread (render thread, throttled
    // to ~every Nth frame by the caller). bgra = SDR-ized BGRA8 preview;
    // writes Result.map (downsampled sky mask) + skyFrac/skyLuma.
    bool DetectSky(const uint8_t* bgra, int width, int height, int stride, Result& out);

private:
    HRESULT InitInternal(const std::wstring& modelPath); // called from the SEH wrapper
    struct OrtState;
    std::unique_ptr<OrtState> m_ort;
    int    m_modelW = 0, m_modelH = 0;
    bool   m_ready = false;
    FILE*  m_logFile = nullptr;
    bool   m_logEnabled = false;
    std::wstring m_lastError;
    uint64_t m_callCount = 0;
    void Log(const wchar_t* fmt, ...);
};