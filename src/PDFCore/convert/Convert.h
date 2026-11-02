// Convert.h — PDF → Word entry points.
//
// The UI drives extraction page by page on the engine thread (ExtractPage, so
// a long document can be cancelled and show progress between pages), then
// calls FinishDocx with the collected pages. ConvertToDocx does all of it in
// one call, for tests and small documents.
#pragma once

#include "Model.h"
#include "../Result.h"

#include <cstdint>
#include <vector>

namespace pdf {

class Document;

// Upper bound on pages converted in one document.
constexpr int kMaxConvertPages = 5000;
// Upper bound on the embedded scanned-page PNGs together. The pages, the
// document XML and the zip are all in memory at once; past this the caller
// stops with an error instead of exhausting it.
constexpr std::uint64_t kMaxConvertImageBytes = 1024ull * 1024 * 1024;

// Upper bound on characters kept across the whole document (~64 B each, and
// Analyze copies them again): with 5000 pages × the per-page cap a small
// hostile file could otherwise ask for tens of GB.
constexpr std::uint64_t kMaxConvertChars = 4'000'000;

// Analyze + write. Pure. Consumes `pages` (their PNGs move into the package).
Result<std::vector<std::uint8_t>> FinishDocx(std::vector<ExtractedPage> pages);

// Extract every page, analyze, write. Engine thread.
Result<std::vector<std::uint8_t>> ConvertToDocx(const Document& doc);

}  // namespace pdf
