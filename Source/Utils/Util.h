//
// Copyright (c) 2020-2025 v0lt, Aleksoid
//
// SPDX-License-Identifier: MIT
//

#pragma once

// ─ DLog — made REAL in every build (previously `__noop` in Release) ─────────
//
// The renderer crashed at a fixed address across a whole investigation while its
// only trace, DLog, compiled to nothing in Release — so the module that faulted
// had no voice, and every diagnosis was built from the survivors' logs. Since
// 2026-09-21 DLog is always on, routed through mpcvr::TraceLog:
//
//   * timestamped (ms) into mpcvr_trace.log beside the .ax, independently;
//   * forwarded RAW to a host that installed IMpcvrLogSink, so the same lines
//     interleave with the engine's monitor/decider lines in the one s3engine.log
//     on the same clock;
//   * and to OutputDebugString.
//
// This is the DEBUG/state/format trace — it is NOT the image-quality log
// (ml_debug.csv), which is untouched and deliberately separate.

#include "MpcvrLog.h"

namespace mpcvr_detail {
    inline std::string Utf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s((size_t)n, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
        return s;
    }
}

inline void DLog(std::wstring_view str)
{
#ifdef _DEBUG
    DbgLogInfo(LOG_TRACE, 3, str.data());
#endif
    mpcvr::TraceLog(mpcvr_detail::Utf8(std::wstring(str)).c_str());
}

inline void DLog(std::string_view str)
{
#ifdef _DEBUG
    DbgLogInfo(LOG_TRACE, 3, str.data());
#endif
    mpcvr::TraceLog(std::string(str).c_str());
}

template <typename... Args>
inline void DLog(std::wformat_string<Args...> fmt, Args&&... args)
{
    mpcvr::TraceLog(mpcvr_detail::Utf8(std::format(fmt, std::forward<Args>(args)...)).c_str());
}

template <typename... Args>
inline void DLog(std::format_string<Args...> fmt, Args&&... args)
{
    mpcvr::TraceLog(std::format(fmt, std::forward<Args>(args)...).c_str());
}

#define DLogIf(f,...) {if (f) DLog(__VA_ARGS__);}

#define SAFE_RELEASE(p)      { if (p) { (p)->Release(); (p) = nullptr; } }
#define SAFE_CLOSE_HANDLE(p) { if (p) { if ((p) != INVALID_HANDLE_VALUE) ASSERT(CloseHandle(p)); (p) = nullptr; } }
#define SAFE_DELETE(p)       { if (p) { delete (p); (p) = nullptr; } }

#define QI(i) (riid == __uuidof(i)) ? GetInterface((i*)this, ppv) :
#define IFQIRETURN(i) if (riid == __uuidof(i)) { return GetInterface((i*)this, ppv); }

#define ALIGN(x, a)           __ALIGN_MASK(x,(decltype(x))(a)-1)
#define __ALIGN_MASK(x, mask) (((x)+(mask))&~(mask))

template <typename T>
// If the specified value is out of range, set to default values.
inline T discard(T const& val, T const& def, T const& lo, T const& hi)
{
	return (val > hi || val < lo) ? def : val;
}

template <typename T>
inline T round_pow2(T number, T pow2)
{
	ASSERT(pow2 > 0);
	ASSERT(!(pow2 & (pow2 - 1)));
	--pow2;
	if (number < 0) {
		return (number - pow2) & ~pow2;
	} else {
		return (number + pow2) & ~pow2;
	}
}

[[nodiscard]] bool IsWindows11_24H2OrGreater();
LPCWSTR GetWindowsVersion();

inline std::wstring GUIDtoWString(const GUID& guid)
{
	std::wstring str(39, 0);
	int ret = StringFromGUID2(guid, &str[0], 39);
	if (ret) {
		str.resize(ret - 1);
	} else {
		str.clear();
	}
	return str;
}

std::wstring HR2Str(const HRESULT hr);

HRESULT GetDataFromResource(LPVOID& data, DWORD& size, UINT resid);

// Usage: SetThreadName ((DWORD)-1, "MainThread");
void SetThreadName(DWORD dwThreadID, const char* threadName);
