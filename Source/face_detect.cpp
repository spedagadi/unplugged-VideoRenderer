#include "stdafx.h"
#include "face_detect.h"
#include "Utils/Util.h"
#include "Utils/StringUtil.h"

// ONNX Runtime headers — manual init to prevent static initializer from
// firing the delay-load thunk before PreloadOnnxRuntime() has loaded the DLL.
#define ORT_API_MANUAL_INIT
#include "onnxrt/onnxruntime_cxx_api.h"
#include "onnxrt/dml_provider_factory.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <numeric>

// Pre-load onnxruntime.dll from the same directory as this module (.ax)
static HMODULE g_hOrtDll = nullptr;
static bool PreloadOnnxRuntime()
{
	if (g_hOrtDll) return true;

	HMODULE hThis = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCWSTR)&PreloadOnnxRuntime, &hThis);
	if (!hThis) {
		OutputDebugStringW(L"[FACE] GetModuleHandleExW failed\n");
		return false;
	}

	wchar_t path[MAX_PATH] = {};
	GetModuleFileNameW(hThis, path, MAX_PATH);
	wchar_t* slash = wcsrchr(path, L'\\');
	if (slash) {
		wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"onnxruntime.dll");
	}

	g_hOrtDll = LoadLibraryW(path);
	if (!g_hOrtDll) {
		wchar_t buf[128];
		swprintf_s(buf, L"[FACE] LoadLibrary failed err=%lu path=%s\n", ::GetLastError(), path);
		OutputDebugStringW(buf);
	}
	return g_hOrtDll != nullptr;
}

struct CFaceDetect::OrtState {
	Ort::Env             env{ORT_LOGGING_LEVEL_WARNING, "mpcvr_face"};
	Ort::SessionOptions  opts;
	std::unique_ptr<Ort::Session> session;
	Ort::MemoryInfo      memInfo{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};

	std::vector<const char*> inputNames;
	std::vector<const char*> outputNames;
	std::vector<std::string> inputNameStrs;
	std::vector<std::string> outputNameStrs;
};

// Log to both CSV file (if set) and OutputDebugStringW
void CFaceDetect::Log(const wchar_t* fmt, ...)
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

CFaceDetect::CFaceDetect() = default;

CFaceDetect::~CFaceDetect()
{
	Shutdown();
}

HRESULT CFaceDetect::Init(const std::wstring& modelPath)
{
	if (m_ort) return S_FALSE;

	Log(L"FACE_INIT starting model=%s\n", modelPath.c_str());

	if (!PreloadOnnxRuntime()) {
		m_lastError = L"onnxruntime.dll not found next to .ax";
		Log(L"FACE_INIT FAILED: DLL not found\n");
		return E_FAIL;
	}
	Log(L"FACE_INIT DLL loaded OK handle=0x%p\n", g_hOrtDll);

	__try {
		Ort::InitApi(OrtGetApiBase()->GetApi(ORT_API_VERSION));
		Log(L"FACE_INIT ORT API initialized\n");
		return InitInternal(modelPath);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		wchar_t buf[128];
		swprintf_s(buf, L"SEH exception 0x%08X", GetExceptionCode());
		m_lastError = buf;
		Log(L"FACE_INIT FAILED: %s\n", buf);
		m_ort.reset();
		return E_FAIL;
	}
}

HRESULT CFaceDetect::InitInternal(const std::wstring& modelPath)
{
	try {
		Log(L"FACE_INIT creating Ort::Env...\n");
		m_ort = std::make_unique<OrtState>();
		Log(L"FACE_INIT Ort::Env OK\n");

		m_ort->opts.SetIntraOpNumThreads(1);
		m_ort->opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
		m_ort->opts.DisableMemPattern();
		m_ort->opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);

		try {
			Log(L"FACE_INIT adding DirectML EP...\n");
			OrtSessionOptionsAppendExecutionProvider_DML(m_ort->opts, 0);
			Log(L"FACE_INIT DirectML EP OK\n");
		}
		catch (const Ort::Exception& e) {
			Log(L"FACE_INIT DirectML failed (%S), using CPU\n", e.what());
		}
		catch (...) {
			Log(L"FACE_INIT DirectML failed (unknown), using CPU\n");
		}

		Log(L"FACE_INIT creating session...\n");
		m_ort->session = std::make_unique<Ort::Session>(m_ort->env, modelPath.c_str(), m_ort->opts);
		Log(L"FACE_INIT session OK\n");

		Ort::AllocatorWithDefaultOptions alloc;

		size_t numInputs = m_ort->session->GetInputCount();
		for (size_t i = 0; i < numInputs; i++) {
			auto name = m_ort->session->GetInputNameAllocated(i, alloc);
			m_ort->inputNameStrs.push_back(name.get());
		}
		for (auto& s : m_ort->inputNameStrs)
			m_ort->inputNames.push_back(s.c_str());

		size_t numOutputs = m_ort->session->GetOutputCount();
		for (size_t i = 0; i < numOutputs; i++) {
			auto name = m_ort->session->GetOutputNameAllocated(i, alloc);
			m_ort->outputNameStrs.push_back(name.get());
		}
		for (auto& s : m_ort->outputNameStrs)
			m_ort->outputNames.push_back(s.c_str());

		auto typeInfo = m_ort->session->GetInputTypeInfo(0);
		auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
		auto dims = tensorInfo.GetShape();
		if (dims.size() == 4) {
			m_modelH = (dims[2] > 0) ? (int)dims[2] : 0;
			m_modelW = (dims[3] > 0) ? (int)dims[3] : 0;
		}

		Log(L"FACE_INIT model: inputs=%zu outputs=%zu shape=%dx%d\n",
			numInputs, numOutputs, m_modelW, m_modelH);
	}
	catch (const Ort::Exception& e) {
		m_lastError = L"Ort: " + A2WStr(e.what());
		Log(L"FACE_INIT FAILED Ort: %S\n", e.what());
		m_ort.reset();
		return E_FAIL;
	}
	catch (const std::exception& e) {
		m_lastError = L"std: " + A2WStr(e.what());
		Log(L"FACE_INIT FAILED std: %S\n", e.what());
		m_ort.reset();
		return E_FAIL;
	}
	catch (...) {
		m_lastError = L"unknown exception";
		Log(L"FACE_INIT FAILED unknown exception\n");
		m_ort.reset();
		return E_FAIL;
	}

	Log(L"FACE_INIT complete (synchronous mode, no background thread)\n");
	m_ready = true;
	Log(L"FACE_INIT SUCCESS ready=1\n");
	return S_OK;
}

void CFaceDetect::Shutdown()
{
	if (m_running) {
		m_running = false;
		m_cv.notify_all();
		if (m_thread.joinable())
			m_thread.join();
	}
	m_ready = false;
	m_ort.reset();
}

void CFaceDetect::SubmitFrame(const uint8_t* bgra, int width, int height, int stride)
{
	if (!m_running || !bgra) return;

	const int mW = (m_modelW > 0) ? m_modelW : width;
	const int mH = (m_modelH > 0) ? m_modelH : height;

	// Letterbox: fit width x height into mW x mH preserving aspect ratio
	const float scale = std::min((float)mW / width, (float)mH / height);
	const int scaledW = (int)(width * scale);
	const int scaledH = (int)(height * scale);
	const int padX = (mW - scaledW) / 2;
	const int padY = (mH - scaledH) / 2;

	int writeSlot = 1 - m_readIdx.load(std::memory_order_acquire);
	auto& slot = m_slots[writeSlot];

	slot.texW = width;
	slot.texH = height;
	slot.scaledW = scaledW;
	slot.scaledH = scaledH;
	slot.padX = padX;
	slot.padY = padY;

	const size_t pixels = (size_t)mW * mH;
	slot.buf.assign(3 * pixels, 0.0f); // black padding

	float* ch_b = slot.buf.data();
	float* ch_g = slot.buf.data() + pixels;
	float* ch_r = slot.buf.data() + pixels * 2;

	// Bilinear sample from source into the letterboxed region
	for (int row = 0; row < scaledH; row++) {
		const float srcY = (row + 0.5f) / scale - 0.5f;
		const int y0 = std::max(0, std::min(height - 1, (int)srcY));
		const int y1 = std::min(height - 1, y0 + 1);
		const float fy = srcY - (float)y0;
		const uint8_t* row0 = bgra + y0 * stride;
		const uint8_t* row1 = bgra + y1 * stride;
		const int outRow = row + padY;
		for (int col = 0; col < scaledW; col++) {
			const float srcX = (col + 0.5f) / scale - 0.5f;
			const int x0 = std::max(0, std::min(width - 1, (int)srcX));
			const int x1 = std::min(width - 1, x0 + 1);
			const float fx = srcX - (float)x0;
			const float w00 = (1 - fx) * (1 - fy);
			const float w10 = fx * (1 - fy);
			const float w01 = (1 - fx) * fy;
			const float w11 = fx * fy;
			const int idx = outRow * mW + (col + padX);
			ch_b[idx] = w00 * row0[x0*4+0] + w10 * row0[x1*4+0] + w01 * row1[x0*4+0] + w11 * row1[x1*4+0];
			ch_g[idx] = w00 * row0[x0*4+1] + w10 * row0[x1*4+1] + w01 * row1[x0*4+1] + w11 * row1[x1*4+1];
			ch_r[idx] = w00 * row0[x0*4+2] + w10 * row0[x1*4+2] + w01 * row1[x0*4+2] + w11 * row1[x1*4+2];
		}
	}

	m_submitCount++;

	if (m_logEnabled && m_logFile && m_submitCount % 60 == 1) {
		fwprintf(m_logFile, L"FACE_PREPROCESS tex=%dx%d model=%dx%d scale=%.4f scaled=%dx%d pad=(%d,%d) bufSize=%zu frame=%llu\n",
			width, height, mW, mH, (double)scale, scaledW, scaledH, padX, padY,
			slot.buf.size(), m_submitCount);
		fflush(m_logFile);
	}

	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_writeIdx = writeSlot;
		m_frameReady = true;
	}
	m_cv.notify_one();
}

int CFaceDetect::GetFaces(FaceBox* out, int maxFaces) const
{
	int slot = m_readIdx.load(std::memory_order_acquire);
	const auto& faces = m_facesBuf[slot];
	int n = std::min((int)faces.size(), maxFaces);
	for (int i = 0; i < n; i++)
		out[i] = faces[i];
	return n;
}

int CFaceDetect::DetectSync(const uint8_t* bgra, int width, int height, int stride,
                            FaceBox* out, int maxFaces)
{
	if (!m_ready || !bgra || !m_ort || !m_ort->session) return 0;

	const int mW = (m_modelW > 0) ? m_modelW : width;
	const int mH = (m_modelH > 0) ? m_modelH : height;

	const float scale = std::min((float)mW / width, (float)mH / height);
	const int scaledW = (int)(width * scale);
	const int scaledH = (int)(height * scale);
	const int padX = (mW - scaledW) / 2;
	const int padY = (mH - scaledH) / 2;

	const size_t pixels = (size_t)mW * mH;
	std::vector<float> buf(3 * pixels, 0.0f);
	float* ch_b = buf.data();
	float* ch_g = buf.data() + pixels;
	float* ch_r = buf.data() + pixels * 2;

	for (int row = 0; row < scaledH; row++) {
		const float srcY = (row + 0.5f) / scale - 0.5f;
		const int y0 = std::max(0, std::min(height - 1, (int)srcY));
		const int y1 = std::min(height - 1, y0 + 1);
		const float fy = srcY - (float)y0;
		const uint8_t* row0 = bgra + y0 * stride;
		const uint8_t* row1 = bgra + y1 * stride;
		const int outRow = row + padY;
		for (int col = 0; col < scaledW; col++) {
			const float srcX = (col + 0.5f) / scale - 0.5f;
			const int x0 = std::max(0, std::min(width - 1, (int)srcX));
			const int x1 = std::min(width - 1, x0 + 1);
			const float fx = srcX - (float)x0;
			const float w00 = (1 - fx) * (1 - fy);
			const float w10 = fx * (1 - fy);
			const float w01 = (1 - fx) * fy;
			const float w11 = fx * fy;
			const int idx = outRow * mW + (col + padX);
			ch_b[idx] = w00*row0[x0*4+0] + w10*row0[x1*4+0] + w01*row1[x0*4+0] + w11*row1[x1*4+0];
			ch_g[idx] = w00*row0[x0*4+1] + w10*row0[x1*4+1] + w01*row1[x0*4+1] + w11*row1[x1*4+1];
			ch_r[idx] = w00*row0[x0*4+2] + w10*row0[x1*4+2] + w01*row1[x0*4+2] + w11*row1[x1*4+2];
		}
	}

	m_submitCount++;

	try {
		std::array<int64_t, 4> inputShape = {1, 3, (int64_t)mH, (int64_t)mW};
		auto inputTensor = Ort::Value::CreateTensor<float>(
			m_ort->memInfo, buf.data(), buf.size(),
			inputShape.data(), inputShape.size());

		auto outputs = m_ort->session->Run(
			Ort::RunOptions{nullptr},
			m_ort->inputNames.data(), &inputTensor, 1,
			m_ort->outputNames.data(), m_ort->outputNames.size());

		std::vector<FaceBox> candidates;
		const int strides[] = {8, 16, 32};
		for (int si = 0; si < 3; si++) {
			const int s = strides[si];
			const int fh = mH / s;
			const int fw = mW / s;
			const float* cls_data = outputs[si].GetTensorData<float>();
			const float* obj_data = outputs[3 + si].GetTensorData<float>();
			const float* bbox_data = outputs[6 + si].GetTensorData<float>();

			for (int r = 0; r < fh; r++) {
				for (int c = 0; c < fw; c++) {
					const int idx2 = r * fw + c;
					float score = std::sqrt(std::max(0.0f, cls_data[idx2]) * std::max(0.0f, obj_data[idx2]));
					if (score < SCORE_THRESH) continue;

					const float* bp = bbox_data + idx2 * 4;
					float cx = (c + bp[0]) * s;
					float cy = (r + bp[1]) * s;
					float bw = std::exp(bp[2]) * s;
					float bh = std::exp(bp[3]) * s;

					float ux = (cx - bw * 0.5f - padX) / scaledW;
					float uy = (cy - bh * 0.5f - padY) / scaledH;
					float uw = bw / scaledW;
					float uh = bh / scaledH;

					if (ux + uw > 0 && uy + uh > 0 && ux < 1.0f && uy < 1.0f) {
						ux = std::max(0.0f, ux);
						uy = std::max(0.0f, uy);
						uw = std::min(uw, 1.0f - ux);
						uh = std::min(uh, 1.0f - uy);
						candidates.push_back({ux, uy, uw, uh, score});
					}
				}
			}
		}

		std::sort(candidates.begin(), candidates.end(),
			[](const FaceBox& a, const FaceBox& b) { return a.score > b.score; });

		std::vector<bool> suppressed(candidates.size(), false);
		int n = 0;
		for (size_t i = 0; i < candidates.size() && n < maxFaces && n < MAX_FACES; i++) {
			if (suppressed[i]) continue;
			out[n++] = candidates[i];
			for (size_t j = i + 1; j < candidates.size(); j++) {
				if (!suppressed[j] && IoU(candidates[i], candidates[j]) > NMS_THRESH)
					suppressed[j] = true;
			}
		}

		if (m_logEnabled && m_logFile && m_submitCount % 60 == 1) {
			fwprintf(m_logFile, L"FACE_SYNC frame=%llu candidates=%zu kept=%d scale=%.3f pad=(%d,%d)\n",
				m_submitCount, candidates.size(), n, (double)scale, padX, padY);
			fflush(m_logFile);
		}
		return n;
	}
	catch (const Ort::Exception& e) {
		Log(L"FACE_SYNC Ort exception: %S\n", e.what());
		return 0;
	}
	catch (...) {
		Log(L"FACE_SYNC unknown exception\n");
		return 0;
	}
}

void CFaceDetect::DetectThread()
{
	uint64_t frameCount = 0;
	Log(L"FACE_THREAD started\n");

	while (m_running) {
		int slot;
		{
			std::unique_lock<std::mutex> lk(m_mutex);
			m_cv.wait(lk, [this] { return m_frameReady || !m_running; });
			if (!m_running) break;
			slot = m_writeIdx;
			m_frameReady = false;
		}

		auto& frame = m_slots[slot];

		if (frame.buf.empty() || !m_ort || !m_ort->session) {
			if (frameCount % 60 == 0) {
				Log(L"FACE_THREAD skip: bufEmpty=%d ort=%d session=%d frame=%llu\n",
					(int)frame.buf.empty(), (m_ort ? 1 : 0),
					(m_ort && m_ort->session ? 1 : 0), frameCount);
			}
			frameCount++;
			continue;
		}

		const int mW = (m_modelW > 0) ? m_modelW : frame.texW;
		const int mH = (m_modelH > 0) ? m_modelH : frame.texH;

		try {
			std::array<int64_t, 4> inputShape = {1, 3, (int64_t)mH, (int64_t)mW};
			auto inputTensor = Ort::Value::CreateTensor<float>(
				m_ort->memInfo, frame.buf.data(),
				frame.buf.size(), inputShape.data(), inputShape.size());

			auto outputs = m_ort->session->Run(
				Ort::RunOptions{nullptr},
				m_ort->inputNames.data(), &inputTensor, 1,
				m_ort->outputNames.data(), m_ort->outputNames.size());

			if (frameCount % 60 == 0) {
				Log(L"FACE_INFER frame=%llu input=%dx%d outputs=%zu\n",
					frameCount, mW, mH, outputs.size());
			}

			// Decode face boxes from YuNet 12-tensor output
			std::vector<FaceBox> candidates;
			const int strides[] = {8, 16, 32};
			int hitsPerStride[3] = {};

			for (int si = 0; si < 3; si++) {
				const int s = strides[si];
				const int fh = mH / s;
				const int fw = mW / s;
				const float* cls_data = outputs[si].GetTensorData<float>();
				const float* obj_data = outputs[3 + si].GetTensorData<float>();
				const float* bbox_data = outputs[6 + si].GetTensorData<float>();

				for (int r = 0; r < fh; r++) {
					for (int c = 0; c < fw; c++) {
						const int idx = r * fw + c;
						float cls_val = std::max(0.0f, cls_data[idx]);
						float obj_val = std::max(0.0f, obj_data[idx]);
						float score = std::sqrt(cls_val * obj_val);

						if (score < SCORE_THRESH) continue;
						hitsPerStride[si]++;

						const float* bp = bbox_data + idx * 4;
						float cx = (c + bp[0]) * s;
						float cy = (r + bp[1]) * s;
						float bw = std::exp(bp[2]) * s;
						float bh = std::exp(bp[3]) * s;

						// Undo letterbox: model pixel coords -> original texture UV [0,1]
						float ux = (cx - bw * 0.5f - frame.padX) / frame.scaledW;
						float uy = (cy - bh * 0.5f - frame.padY) / frame.scaledH;
						float uw = bw / frame.scaledW;
						float uh = bh / frame.scaledH;

						if (ux + uw > 0 && uy + uh > 0 && ux < 1.0f && uy < 1.0f) {
							ux = std::max(0.0f, ux);
							uy = std::max(0.0f, uy);
							uw = std::min(uw, 1.0f - ux);
							uh = std::min(uh, 1.0f - uy);
							candidates.push_back({ux, uy, uw, uh, score});
						}
					}
				}
			}

			// NMS
			std::sort(candidates.begin(), candidates.end(),
				[](const FaceBox& a, const FaceBox& b) { return a.score > b.score; });

			std::vector<FaceBox> kept;
			std::vector<bool> suppressed(candidates.size(), false);
			for (size_t i = 0; i < candidates.size() && kept.size() < MAX_FACES; i++) {
				if (suppressed[i]) continue;
				kept.push_back(candidates[i]);
				for (size_t j = i + 1; j < candidates.size(); j++) {
					if (!suppressed[j] && IoU(candidates[i], candidates[j]) > NMS_THRESH)
						suppressed[j] = true;
				}
			}

			int outSlot = 1 - m_readIdx.load(std::memory_order_acquire);
			m_facesBuf[outSlot] = std::move(kept);
			const int nKept = (int)m_facesBuf[outSlot].size();
			m_readIdx.store(outSlot, std::memory_order_release);

			if (frameCount % 60 == 0) {
				Log(L"FACE_DECODE frame=%llu stride_hits=[%d,%d,%d] candidates=%zu nms_kept=%d letterbox=(%d,%d,%d,%d)\n",
					frameCount, hitsPerStride[0], hitsPerStride[1], hitsPerStride[2],
					candidates.size(), nKept,
					frame.padX, frame.padY, frame.scaledW, frame.scaledH);
				for (int fi = 0; fi < nKept; fi++) {
					const auto& f = m_facesBuf[outSlot][fi];
					Log(L"FACE_BOX[%d] uv=(%.3f,%.3f,%.3f,%.3f) score=%.3f px=(%d,%d,%d,%d)\n",
						fi, (double)f.x, (double)f.y, (double)f.w, (double)f.h, (double)f.score,
						(int)(f.x * frame.texW), (int)(f.y * frame.texH),
						(int)(f.w * frame.texW), (int)(f.h * frame.texH));
				}
			}
			frameCount++;
		}
		catch (const Ort::Exception& e) {
			Log(L"FACE_THREAD Ort exception frame=%llu: %S\n", frameCount, e.what());
			int outSlot = 1 - m_readIdx.load(std::memory_order_acquire);
			m_facesBuf[outSlot].clear();
			m_readIdx.store(outSlot, std::memory_order_release);
			frameCount++;
		}
		catch (...) {
			Log(L"FACE_THREAD unknown exception frame=%llu\n", frameCount);
			int outSlot = 1 - m_readIdx.load(std::memory_order_acquire);
			m_facesBuf[outSlot].clear();
			m_readIdx.store(outSlot, std::memory_order_release);
			frameCount++;
		}
	}
	Log(L"FACE_THREAD exiting\n");
}

float CFaceDetect::IoU(const FaceBox& a, const FaceBox& b)
{
	float x1 = std::max(a.x, b.x);
	float y1 = std::max(a.y, b.y);
	float x2 = std::min(a.x + a.w, b.x + b.w);
	float y2 = std::min(a.y + a.h, b.y + b.h);
	float inter = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
	float areaA = a.w * a.h;
	float areaB = b.w * b.h;
	float unionArea = areaA + areaB - inter;
	return (unionArea > 0) ? inter / unionArea : 0.0f;
}
