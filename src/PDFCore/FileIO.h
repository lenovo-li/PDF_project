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

}  // namespace pdf
