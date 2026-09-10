#pragma once

// Zion ML — person instance segmentation (yolo26n-seg, COCO 80-class,
// person = class 0). ONNX Runtime + DirectML, mirroring CSkySeg.
//
// Runtime contract (probed from the exported ONNX):
//   input   images   : float [1, 3, 640, 640]  (RGB, 0..1, gray-114 letterbox)
//   output  output0  : float [1, 300, 38]      (post-NMS detections)
//                          38 = x1,y1,x2,y2, score, cls, mask_coeff[32]
//   output  output1  : float [1, 32, 160, 160] (mask prototypes)
//
// Person mask = union over instances { cls==0, score>=0.25 } of
// sigmoid(proto . coeff) resized to 640x640, thresholded > 0.5, cropped to
// the box, then letterbox-cropped to the content rect (same geometry as the
// sky map, so the renderer packs into the same R8G8 texture).
//
// NOTE: the shipped yolo26n-seg.onnx is stamped opset 22 (rejected by the
// bundled ORT 1.21); use the re-stamped yolo26n-seg_op21.onnx (same weights).

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

struct OrtEnv;
struct OrtSession;
struct OrtSessionOptions;
struct OrtRunOptions;

class CPersonSeg {
public:
    struct Result {
        int             mapW = 0;    // person map dims = content rect (e.g. 640x360)
        int             mapH = 0;
        std::vector<unsigned char> map;   // R8-like: 255 = person, 0 = not
        float           personFrac = 0.0f; // fraction of valid-letterbox pixels == person
        struct Box { float x, y, w, h; };  // per-person boxes in content UV [0,1]
        Box             boxes[8];
        int             nBoxes = 0;
        double          preMs = 0, runMs = 0, decMs = 0;
        bool            ran = false;
    };

    CPersonSeg();
    ~CPersonSeg();

    CPersonSeg(const CPersonSeg&) = delete;
    CPersonSeg& operator=(const CPersonSeg&) = delete;

    HRESULT Init(const std::wstring& modelPath);
    void    Shutdown();
    void    SetLogFile(FILE* f)   { m_logFile = f; }
    void    SetLogEnabled(bool e) { m_logEnabled = e; }
    bool    IsReady() const       { return m_ready; }
    const std::wstring& GetLastError() const { return m_lastError; }

    // Synchronous inference on the caller's thread (render thread, throttled by
    // the caller ~every Nth frame, same cadence as sky). bgra = SDR-ized BGRA8
    // preview (the sky seg preview); writes Result.map (person union mask).
    bool DetectPersons(const uint8_t* bgra, int width, int height, int stride, Result& out);

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

    // Per-frame scratch (reused across calls — no per-call allocations so the
    // per-frame inference path stays cheap).
    std::vector<float>  m_mask160;   // sigmoid mask at proto res (160x160)
    std::vector<float>  m_mask640;   // upscaled to model res
    std::vector<uint8_t> m_map640;    // union person mask at model res
};