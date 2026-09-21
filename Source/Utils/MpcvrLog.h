// MpcvrLog — the renderer's own trace, ALWAYS ON, timestamped.
//
// Why (2026-09-21): the renderer accumulated crashes (0xc0000005 at a fixed
// address) that no engine-side log could explain, because the only trace the
// DLL had, DLog, was `#define DLog(...) __noop` in Release — the module that
// faulted had no voice. This is that voice, made real.
//
// Two destinations, same content, same clock:
//   1. A host that installs IMpcvrLogSink gets RAW lines (no stamp — the host
//      stamps them, so they interleave with the engine's/monitor's lines at
//      millisecond resolution in the SAME file).
//   2. Every build writes its OWN stamped copy to mpcvr_trace.log beside the
//      .ax, so a standalone run under another player still has a trace.
// Plus OutputDebugString, for DebugView.
//
// Deliberately independent of ml_debug.csv: that file is the image-quality log
// and is not routed through here.

#pragma once

namespace mpcvr {
    // One complete line, no trailing newline; UTF-8. Host sink receives the RAW
    // line (un-stamped) so it can add its own timestamp prefix.
    void TraceLog(const char* line);

    // Install the host sink (per process — the renderer is single-instance).
    void SetTraceSink(void* ctx, void(__cdecl* fn)(void*, const char*));
}