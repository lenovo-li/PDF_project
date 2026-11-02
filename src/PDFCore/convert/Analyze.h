// Analyze.h — characters → lines → paragraphs → sections (pure, headless).
//
// Heuristics, in order:
// 1. Columns: a vertical gutter (no glyph covers it, at least kMinGutterPt
//    wide, in the middle 30–70 % of the page, with text on both sides) splits
//    the page in two; the left column is read before the right.
// 2. Lines: within a column, characters whose baselines are within half a
//    font size are one line, ordered left to right; a gap wider than a
//    quarter of the font size becomes a space if there is none.
// 3. Paragraphs: a new one starts on a gap larger than 1.6 typical line
//    pitches, a font-size change of more than 20 %, an indented first line,
//    or after a line that ends well short of the column's right edge.
// 4. Runs: consecutive characters with the same font, size, weight, italic
//    and colour.
// 5. Sections: consecutive pages of the same size; margins from the text
//    block's extent on those pages.
//
// Not detected: tables, headers/footers, lists, footnotes, text rotated on the
// page, more than two columns.
#pragma once

#include "Model.h"

#include <vector>

namespace pdf {

constexpr float kMinGutterPt = 12.0f;

// Lines of one column (or page), top to bottom.
std::vector<TextLine> BuildLines(std::vector<TextChar> chars);

// Paragraphs from lines of one column. `columnLeft`/`columnRight` bound the
// column's text, for indent and short-line detection.
std::vector<Paragraph> BuildParagraphs(const std::vector<TextLine>& lines, float columnLeft,
                                       float columnRight);

// x of a gutter splitting `chars` into two columns on a page `pageWidth`
// wide, or a negative value if there is none.
float FindGutter(const std::vector<TextChar>& chars, float pageWidth);

// The whole pipeline over extracted pages, in page order. Taken by value:
// scanned pages' PNGs are moved into the result, not copied.
AnalyzedDocument Analyze(std::vector<ExtractedPage> pages);

}  // namespace pdf
