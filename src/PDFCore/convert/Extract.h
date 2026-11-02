// Extract.h — extract text characters from a PDF page's text layer.
//
// Iterates FPDFText_GetUnicode/GetCharBox/GetFontSize/... per character, in
// displayed page space (top-left origin, /Rotate applied). Skips generated
// whitespace, invalid XML characters, and pages with fewer than ~10 chars
// (scanned pages are rendered as images instead).
//
// THREADING: engine thread (calls PDFium).
#pragma once

#include "Model.h"
#include "../Result.h"

namespace pdf {

class Document;

// Extracts one page. A page with fewer than 10 real characters (a scan) gets
// `pagePng` (150 dpi) instead of characters. At most 200 000 characters are
// read per page; `truncated` says the cap was hit. Spaces PDFium generates
// between words are kept, placed right after the glyph they follow; its
// generated line breaks are dropped (Analyze rebuilds lines from geometry).
Result<ExtractedPage> ExtractPage(const Document& doc, int pageIndex);

}  // namespace pdf
