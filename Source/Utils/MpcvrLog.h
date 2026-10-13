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
// ONE switch for both channels (changed 2026-10-11, was "deliberately
// independent"). Per-frame trace lines are now VLog, gated by the same flag that
// lights ml_debug.csv -- because an ALWAYS-ON per-frame trace is not a log a user
// can attach to a bug report: the Dune session wrote 87 MB in three hours, and
// four [RT] ProcessSample lines per frame (96 lines/second) were almost all of
// it. Default = off, so a shared trace is lifecycle only; flip the debug flag and
// BOTH the CSV and the per-frame lines appear, on the same clock, for one session.

#pragma once

namespace mpcvr {
    // One complete line, no trailing newline; UTF-8. Host sink receives the RAW
    // line (un-stamped) so it can add its own timestamp prefix.
    void TraceLog(const char* line);

    // The per-frame gate. Set from the renderer's own debug setting (bMLDebugLog)
    // so trace verbosity and the CSV can never disagree about what is on.
    void SetTraceVerbose(bool verbose);
    bool TraceVerbose();

    // Install the host sink (per process — the renderer is single-instance).
    void SetTraceSink(void* ctx, void(__cdecl* fn)(void*, const char*));
}