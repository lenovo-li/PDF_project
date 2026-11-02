// ImageExport.h — write rendered page bitmaps to image files.
//
// Uses WIC (Windows Imaging Component), part of Windows, so there is no new
// dependency. Pixels come from Document::RenderPage, which already enforces the
// render size limits. The encoder writes to memory and the file goes through
// WriteFileAtomic like every other save, so a failed export never leaves a
// truncated file behind.
//
// THREADING: any thread with COM initialized (the engine thread initializes
// COM for this; see Engine.cpp). No PDFium calls here.
#pragma once

#include "Render.h"
#include "Result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pdf {

// Encodes a BGRA bitmap as PNG in memory (for embedding, e.g. in a .docx).
Result<std::vector<std::uint8_t>> EncodePng(const Bitmap& bitmap);

// Encodes a BGRA bitmap as PNG and writes it to `path` atomically.
Status SavePng(const Bitmap& bitmap, const std::wstring& path);

}  // namespace pdf
