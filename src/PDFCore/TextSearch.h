// TextSearch.h — find text on pages and return highlight rectangles.
//
// Search runs on the engine thread one page at a time (FPDFText_FindStart),
// so the UI can stream results, cancel between pages and show progress on a
// 5000-page file without blocking anything. Results carry rectangles in PDF
// user space (points, y up), merged per line by FPDFText_CountRects; the
// viewer maps them to the screen with the same PageGeometry transform it uses
// for the page bitmap, so highlights line up at any zoom and rotation.
//
// A hostile page can contain millions of characters, so matches per page and
// rectangles per match are capped; a capped page reports truncated = true.
//
// THREADING: engine thread only (it calls PDFium).
#pragma once

#include "Document.h"
#include "PageGeometry.h"
#include "Result.h"

#include <string>
#include <vector>

namespace pdf {

constexpr int kMaxMatchesPerPage = 1000;
constexpr int kMaxRectsPerMatch = 64;
// A selection can span a whole page of lines (two-column text: two rects per
// line), so it gets a much larger cap than a single search match.
constexpr int kMaxRectsPerSelection = 2048;
constexpr std::size_t kMaxQueryChars = 256;

// A rectangle as fractions (0..1) of the DISPLAYED page, origin top-left:
// rotation and CropBox offset already applied. Computed with PDFium's own
// page-to-device transform (FPDF_PageToDevice), the same mapping
// FPDF_RenderPageBitmap uses, so a highlight drawn at
// pageRect.x + left * pageRect.width lines up with the bitmap exactly.
struct UnitRect {
    double left = 0, top = 0, right = 0, bottom = 0;
};

struct TextMatch {
    int page = 0;
    int charIndex = 0;  // first matched character in the page's text
    int charCount = 0;
    std::vector<BoxPt> rects;     // PDF user space, one per line the match spans
    std::vector<UnitRect> units;  // the same rectangles on the displayed page
};

// A text selection on one page between two points given as UnitRect-style
// fractions of the displayed page.
struct TextSelection {
    int page = 0;
    int first = 0;  // character index; count 0 = nothing selected
    int count = 0;
    std::vector<UnitRect> units;
    std::wstring text;  // capped at kMaxCopyChars
};

struct SearchOptions {
    bool matchCase = false;
    bool wholeWord = false;
};

struct PageSearchResult {
    std::vector<TextMatch> matches;
    bool truncated = false;  // hit kMaxMatchesPerPage
};

// Finds every occurrence of `query` on page `index`. An empty query, or one
// longer than kMaxQueryChars, is an error; a page without a text layer
// (a scan) simply has no matches.
Result<PageSearchResult> SearchPage(const Document& doc, int index, const std::wstring& query,
                                    const SearchOptions& options);

// Whole text of page `index` from `first` for `count` characters (-1 = to
// the end), for copying a selection. Capped at kMaxCopyChars.
constexpr int kMaxCopyChars = 1 << 20;
Result<std::wstring> PageText(const Document& doc, int index, int first = 0, int count = -1);

// Characters between the points (ax, ay) and (bx, by), fractions of the
// displayed page as in UnitRect, in reading order whichever way the user
// dragged. A point off any glyph snaps to the nearest one within a small
// tolerance; if neither end is near text the selection is empty.
Result<TextSelection> SelectText(const Document& doc, int index, double ax, double ay, double bx,
                                 double by);

}  // namespace pdf
