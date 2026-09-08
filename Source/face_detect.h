#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct OrtEnv;
struct OrtSession;
struct OrtSessionOptions;
struct OrtRunOptions;

class CFaceDetect {
public:
	struct FaceBox {
		float x, y, w, h;  // normalized UV [0,1] in original texture space
		float score;
	};

	CFaceDetect();
	~CFaceDetect();

	CFaceDetect(const CFaceDetect&) = delete;
	CFaceDetect& operator=(const CFaceDetect&) = delete;

	HRESULT Init(const std::wstring& modelPath);
	void    Shutdown();
	void    SetLogFile(FILE* f) { m_logFile = f; }
	void    SetLogEnabled(bool e) { m_logEnabled = e; }

	void SubmitFrame(const uint8_t* bgra, int width, int height, int stride);
	int  GetFaces(FaceBox* out, int maxFaces) const;

	// Synchronous: preprocess + infer + decode in one call on the caller's thread.
	// Returns number of faces written to out[]. No background thread involved.
	int  DetectSync(const uint8_t* bgra, int width, int height, int stride,
	                FaceBox* out, int maxFaces);

	bool IsReady() const { return m_ready.load(std::memory_order_acquire); }
	const std::wstring& GetLastError() const { return m_lastError; }

private:
	static constexpr int    MAX_FACES = 4;
	static constexpr float  SCORE_THRESH = 0.7f;
	static constexpr float  NMS_THRESH = 0.3f;

	void DetectThread();
	HRESULT InitInternal(const std::wstring& modelPath);
	void Log(const wchar_t* fmt, ...);

	static float IoU(const FaceBox& a, const FaceBox& b);

	// ONNX Runtime (opaque pointers to avoid header dependency in callers)
	struct OrtState;
	std::unique_ptr<OrtState> m_ort;

	int m_modelW = 0;
	int m_modelH = 0;

	// Per-frame slot: model input buffer + letterbox metadata
	struct FrameSlot {
		std::vector<float> buf;
		int texW = 0, texH = 0;
		int scaledW = 0, scaledH = 0;
		int padX = 0, padY = 0;
	};
	FrameSlot m_slots[2];
	int m_writeIdx = 0;

	// Double-buffered output
	std::vector<FaceBox> m_facesBuf[2];
	std::atomic<int> m_readIdx{0};

	// Thread synchronization
	std::thread             m_thread;
	std::mutex              m_mutex;
	std::condition_variable m_cv;
	std::atomic<bool>       m_running{false};
	std::atomic<bool>       m_ready{false};
	bool                    m_frameReady = false;
	std::wstring            m_lastError;
	FILE*                   m_logFile = nullptr;   // init/error log (always)
	bool                    m_logEnabled = false;  // per-frame cadence only under frame log
	uint64_t                m_submitCount = 0;
};
