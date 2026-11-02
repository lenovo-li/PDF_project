// Annotations.h — read, add and remove page annotations (FPDFAnnot_*).
//
// Coordinates are PDF user space (points, y up), the same space TextSearch
// returns in TextMatch::rects, so a text-markup annotation is created straight
// from a search hit or a selection. The viewer converts to the screen with
// the page's display transform as it does for highlights.
//
// Annotations live in the Document in the engine's store; they reach the file
// on Save (FPDF_SaveAsCopy writes the annotation dictionaries). PDFium
// generates an appearance stream for the markup, square and ink types it
// creates, which is what Document::RenderPage draws (FPDF_ANNOT flag).
//
// Untrusted input: a page may hold thousands of annotations; listing is
// capped and each string read is size-checked.
//
// THREADING: engine thread only.
#pragma once

#include "Document.h"
#include "PageGeometry.h"
#include "Result.h"
#include "TextSearch.h"  // UnitRect

#include <cstdint>
#include <string>
#include <vector>

namespace pdf {

// Untrusted-input caps. The byte budget bounds a page's listed /Contents in
// total: 500 annotations of 64 K chars each would otherwise be 64 MiB per
// page, for every page in view.
constexpr int kMaxAnnotsListed = 500;
constexpr std::size_t kMaxAnnotTextChars = 64 * 1024;
constexpr std::size_t kMaxAnnotTextBytesPerPage = 1024 * 1024;  // 1 MiB
constexpr int kMaxInkPoints = 10000;

enum class AnnotKind { Highlight, Underline, StrikeOut, Note, FreeText, Square, Ink, Other };

struct AnnotColor {
    std::uint8_t r = 255, g = 220, b = 0, a = 255;  // default: highlighter yellow
};

struct AnnotInfo {
    int index = 0;  // position in the page's /Annots array (for RemoveAnnotation)
    AnnotKind kind = AnnotKind::Other;
    BoxPt rect;
    UnitRect unitRect;  // rect as a fraction of the displayed page, for drawing and hit-test
    AnnotColor color;
    std::wstring contents;  // /Contents: the note text or free text
};

// Annotations on page `index` (at most kMaxAnnotsListed; `truncated` says so).
struct AnnotList {
    std::vector<AnnotInfo> annots;
    bool truncated = false;
};
Result<AnnotList> ListAnnotations(const Document& doc, int index);

// The kinds this module creates are the only ones the UI may select, edit or
// delete. Links, form widgets, popups and anything else are left alone:
// deleting a widget would break a form, a popup belongs to its parent.
inline bool IsEditableKind(AnnotKind k) { return k != AnnotKind::Other; }

// Fractions of the displayed page (UnitRect convention) -> user space, for
// creating annotations from a drag in the viewer. Rotation and CropBox offset
// are handled by PDFium's display matrix, as for rendering.
Result<BoxPt> UnitRectToPage(const Document& doc, int index, const UnitRect& u);
Result<std::vector<PointF>> UnitPointsToPage(const Document& doc, int index,
                                             const std::vector<PointF>& units);

// Text markup over `rects` (one per line, e.g. TextMatch::rects).
Status AddMarkup(Document& doc, int index, AnnotKind kind, const std::vector<BoxPt>& rects,
                 AnnotColor color);

// Text markup over characters [first, first + count) of the page's text
// layer (a TextSelection), one quad per line.
Status AddMarkupForChars(Document& doc, int index, AnnotKind kind, int first, int count,
                         AnnotColor color);

// Sticky note (an icon at `at`, the text shown when opened).
Status AddNote(Document& doc, int index, PointF at, const std::wstring& text, AnnotColor color);

// Text drawn directly on the page inside `rect`.
Status AddFreeText(Document& doc, int index, const BoxPt& rect, const std::wstring& text,
                   AnnotColor color);

// Rectangle outline.
Status AddSquare(Document& doc, int index, const BoxPt& rect, AnnotColor color,
                 float borderWidth);

// Freehand strokes; each stroke is a polyline in user space.
Status AddInk(Document& doc, int index, const std::vector<std::vector<PointF>>& strokes,
              AnnotColor color, float borderWidth);

// Replaces /Contents (note or free text) of an editable annotation.
Status SetAnnotationText(Document& doc, int index, int annotIndex, const std::wstring& text);

// Removes the annotation at `annotIndex` (from ListAnnotations) on page `index`,
// and the popup a note points to. Only editable kinds; indices of later
// annotations on the page shift down, so list again before the next removal.
Status RemoveAnnotation(Document& doc, int index, int annotIndex);

}  // namespace pdf
