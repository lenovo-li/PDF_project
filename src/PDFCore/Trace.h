// Trace.h — OutputDebugStringA helper for PDFCore.
//
// Tag format: "[PDFTool.Module] message\n". Visible in the VS Output window
// and DebugView, so a user's screenshot pinpoints a failure. Never pass
// passwords or document text through here.
#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace pdf {

inline void TraceF(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
}

}  // namespace pdf
