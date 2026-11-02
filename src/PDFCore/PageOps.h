// PageOps.h — structural page edits: rotate, delete, insert, move, import.
//
// Each operation validates every index before calling PDFium, because some
// PDFium calls (FPDF_MovePages) can leave the document in an indeterminate
// state when they fail half way; refusing bad input up front means a failure
// here leaves the document unchanged. Saving is Document::SaveAs (temp file +
// atomic rename), so the user's file is only replaced by a complete copy.
//
// After any of these, page count, order and sizes change: callers re-read
// DisplayedPageSizesPt and reset views and caches.
//
// THREADING: engine thread only.
#pragma once

#include "Document.h"
#include "Result.h"

#include <vector>

namespace pdf {

// Adds `quarterTurns` × 90° clockwise to each listed page's /Rotate.
Status RotatePages(Document& doc, const std::vector<int>& pages, int quarterTurns);

// Deletes the listed pages (any order, duplicates rejected). Deleting every
// page is refused: a PDF must have at least one page to be useful here.
Status DeletePages(Document& doc, std::vector<int> pages);

// Inserts a blank page of widthPt × heightPt before `index` (== PageCount()
// appends).
Status InsertBlankPage(Document& doc, int index, double widthPt, double heightPt);

// Moves the listed pages, in the given order, so the first lands at
// `destIndex` of the resulting document (PDFium's FPDF_MovePages contract).
Status MovePages(Document& doc, const std::vector<int>& pages, int destIndex);

// Inserts `srcPages` of `src` (empty = all) before `index` of `dest`.
Status ImportPages(Document& dest, const Document& src, const std::vector<int>& srcPages,
                   int index);

// True if `pages` is non-empty, every entry in [0, pageCount), no duplicates.
bool ValidPageList(const std::vector<int>& pages, int pageCount);

}  // namespace pdf
