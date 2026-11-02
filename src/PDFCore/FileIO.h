// FileIO.h — Unicode-path file helpers used by the engine.
#pragma once

#include "Result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pdf {

// Reads a whole file. Refuses files larger than maxBytes before allocating.
Result<std::vector<std::uint8_t>> ReadWholeFile(const std::wstring& path,
                                                std::uint64_t maxBytes);

// Writes `bytes` to a temp file next to `path`, flushes it, then replaces
// `path` with MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH).
// On any failure the temp file is removed and `path` is left untouched.
Status WriteFileAtomic(const std::wstring& path, const std::vector<std::uint8_t>& bytes);

// UTF-16 <-> UTF-8. PDF passwords from the UI are wide strings; PDFium and
// qpdf take UTF-8 bytes. Invalid UTF-16 (a lone surrogate) is replaced by
// U+FFFD rather than failing.
std::string WideToUtf8(const std::wstring& s);

// "<dir>\<stem>_<n>.pdf" for split output: dir and stem from `base` (which
// may end in .pdf), n 1-based and zero-padded to the width of `count`.
std::wstring NumberedPath(const std::wstring& base, int n, int count);

}  // namespace pdf
