#include "stdafx.h"   // precompiled header is mandatory in this project (C1010)
#include "MpcvrLog.h"

#include <windows.h>
#include <mutex>
#include <cstdio>
#include <cstring>

namespace mpcvr {

static std::mutex s_mtx;
static void*      s_ctx = nullptr;
static void(__cdecl* s_fn)(void*, const char*) = nullptr;
static FILE*      s_file = nullptr;
static unsigned long long s_t0 = GetTickCount64();

static void EnsureFile()
{
    if (s_file) return;
    // Put it beside the module that runs us, so a deployed .ax logs regardless
    // of the working directory.
    HMODULE h = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&TraceLog, &h);
    char dir[MAX_PATH] = {};
    char path[MAX_PATH] = {};
    if (h && GetModuleFileNameA(h, dir, MAX_PATH)) {
        char* slash = strrchr(dir, '\\');
        if (slash) {
            *slash = 0;
            _snprintf_s(path, MAX_PATH, _TRUNCATE, "%s\\mpcvr_trace.log", dir);
        }
    }
    if (!path[0]) _snprintf_s(path, MAX_PATH, _TRUNCATE, "mpcvr_trace.log");

    // Rotate: keep ONE previous trace (capped at 16 MB), so the run that died
    // survives as mpcvr_trace.prev.log. Same policy as the engine's s3engine.log.
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad) &&
            (((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow) >
                16ull * 1024 * 1024) {
            char prev[MAX_PATH];
            _snprintf_s(prev, MAX_PATH, _TRUNCATE, "%s", path);
            char* dot = strrchr(prev, '.');
            if (dot) strcpy_s(dot, sizeof(".prev.log"), ".prev.log");
            else    strcat_s(prev, ".prev.log");
            DeleteFileA(prev);
            MoveFileA(path, prev);
        }
    }

    fopen_s(&s_file, path, "a");
    if (s_file) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(s_file, "==== mpcvr_trace.log  (%02u:%02u:%02u.%03u) ====\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        fflush(s_file);
    }
}

void SetTraceSink(void* ctx, void(__cdecl* fn)(void*, const char*))
{
    std::lock_guard<std::mutex> lk(s_mtx);
    s_ctx = ctx;
    s_fn  = fn;
    if (s_fn) {
        TraceLog("renderer: [TRACE] host log sink attached; renderer trace will interleave "
                 "with the engine log on the same clock");
    }
}

void TraceLog(const char* line)
{
    if (!line || !line[0]) return;
    std::lock_guard<std::mutex> lk(s_mtx);
    EnsureFile();

    SYSTEMTIME st{}; GetLocalTime(&st);
    const unsigned long long el = GetTickCount64() - s_t0;
    char stamped[2048];
    _snprintf_s(stamped, sizeof(stamped), _TRUNCATE,
                "%02u:%02u:%02u.%03u +%7llu  %s",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, el, line);

    if (s_file) {
        fputs(stamped, s_file);
        fputc('\n', s_file);
        fflush(s_file);
    }
    if (s_fn) {
        // RAW line: the host stamps, so these interleave with the engine's
        // monitor/decider lines on the same clock and same mutex.
        s_fn(s_ctx, line);
    }
    OutputDebugStringA(stamped);
    OutputDebugStringA("\n");
}

} // namespace mpcvr