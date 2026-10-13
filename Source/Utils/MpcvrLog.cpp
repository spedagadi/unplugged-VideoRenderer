#include "stdafx.h"   // precompiled header is mandatory in this project (C1010)
#include "MpcvrLog.h"

#include <windows.h>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace mpcvr {

static std::mutex s_mtx;
static void*      s_ctx = nullptr;
static void(__cdecl* s_fn)(void*, const char*) = nullptr;
static FILE*      s_file = nullptr;
static unsigned long long s_t0 = GetTickCount64();

// Per-frame gate. Written from the renderer's own debug setting (bMLDebugLog) and
// read on the hot path, so it is a plain relaxed atomic -- a load, not a lock.
static std::atomic<bool> s_verbose{ false };

// Trace size, tracked as it is written so the cap can be enforced DURING a run.
// The old code only rotated when the file was opened, which is why a three-hour
// Dune session reached 87 MB: the cap was checked once, at launch.
static long long s_bytes = 0;
static char      s_path[MAX_PATH] = {};

static constexpr long long kTraceCapBytes = 8ll * 1024 * 1024;

void SetTraceVerbose(bool verbose)
{
    const bool was = s_verbose.exchange(verbose);
    if (was == verbose) return;
    TraceLog(verbose
        ? "renderer: [TRACE] verbose ON -- per-frame lines now written, same flag as ml_debug.csv"
        : "renderer: [TRACE] verbose OFF -- lifecycle lines only, trace stays shareable");
}

bool TraceVerbose()
{
    return s_verbose.load(std::memory_order_relaxed);
}

static void WriteBanner()
{
    if (!s_file) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(s_file, "==== mpcvr_trace.log  (%02u:%02u:%02u.%03u) ====\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    s_bytes = ftell(s_file);
    fflush(s_file);
}

static void OpenLocked()
{
    if (s_file) return;

    if (!s_path[0]) {
        // Beside the module that runs us, so a deployed .ax logs regardless of the
        // working directory.
        HMODULE h = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&TraceLog, &h);
        char dir[MAX_PATH] = {};
        if (h && GetModuleFileNameA(h, dir, MAX_PATH)) {
            char* slash = strrchr(dir, '\\');
            if (slash) {
                *slash = 0;
                _snprintf_s(s_path, MAX_PATH, _TRUNCATE, "%s\\mpcvr_trace.log", dir);
            }
        }
        if (!s_path[0]) strcpy_s(s_path, "mpcvr_trace.log");
    }

    fopen_s(&s_file, s_path, "a");
    WriteBanner();
}

// Close, move the current trace to .prev, start again. Caller holds s_mtx.
static void RotateLocked()
{
    if (s_file) { fclose(s_file); s_file = nullptr; }

    char prev[MAX_PATH];
    _snprintf_s(prev, MAX_PATH, _TRUNCATE, "%s", s_path);
    char* dot = strrchr(prev, '.');
    if (dot) strcpy_s(dot, sizeof(".prev.log"), ".prev.log");
    else     strcat_s(prev, ".prev.log");
    DeleteFileA(prev);
    MoveFileA(s_path, prev);

    s_bytes = 0;
    fopen_s(&s_file, s_path, "a");
    WriteBanner();
    if (s_file) {
        fputs("[TRACE] size cap reached, previous trace moved to mpcvr_trace.prev.log\n", s_file);
        s_bytes = ftell(s_file);
        fflush(s_file);
    }
}

void SetTraceSink(void* ctx, void(__cdecl* fn)(void*, const char*))
{
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_ctx = ctx;
        s_fn  = fn;
    }
    // OUTSIDE the lock: TraceLog takes s_mtx itself, and std::mutex is not
    // recursive -- calling it while holding the lock deadlocks the attaching
    // thread. The sink attach happens on the graph-build thread, so this would
    // have hung the graph, not the log.
    if (fn) {
        TraceLog("renderer: [TRACE] host log sink attached; renderer trace will interleave "
                 "with the engine log on the same clock");
    }
}

void TraceLog(const char* line)
{
    if (!line || !line[0]) return;
    std::lock_guard<std::mutex> lk(s_mtx);
    OpenLocked();

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
        s_bytes += (long long)strlen(stamped) + 1;
        if (s_bytes > kTraceCapBytes) {
            RotateLocked();
        }
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
