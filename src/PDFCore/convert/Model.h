// Model.h — data model for PDF→Word conversion (extract → analyze → write).
//
// The conversion pipeline:
// 1. Extract: iterate FPDFText characters per page → ExtractedPage (position,
//    font, color, text), skipping generated whitespace and control characters.
// 2. Analyze: group characters into lines and paragraphs, detect columns,
//    infer alignment/spacing, cluster font runs → AnalyzedDocument.
// 3. Write: serialize to DOCX (zip with XML parts) → bytes.
//
// Pure data: no PDFium, no file I/O. Extract is the only stage that touches
// PDFium (via Document). The rest can be tested in isolation.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pdf {

// A character from the PDF text layer, in displayed-page space: points, origin
// at the top-left of the page as shown (/Rotate and CropBox applied), x right,
// y down. Text that is itself rotated on the page is not normalized.
struct TextChar {
    char32_t unicode = 0;            // code point (surrogate pairs combined)
    float left = 0, right = 0;       // horizontal extent of the glyph box
    float baseline = 0;              // y of the baseline
    float fontSize = 0;              // pt
    std::uint8_t r = 0, g = 0, b = 0;  // fill colour
    int weight = 400;                // 400 normal, 700 bold
    bool italic = false;
    bool generated = false;          // a space PDFium inserted between words
    std::string fontName;            // UTF-8, subset prefix ("ABCDEF+") removed
};

// A picture placed on a text page (a screenshot on a slide), in the same
// displayed-page space as TextChar. Analyze puts it between the paragraphs
// above and below it.
struct PageImage {
    float left = 0, top = 0, right = 0, bottom = 0;  // pt, y down
    std::vector<std::uint8_t> png;
};

struct ExtractedPage {
    int pageIndex = 0;
    float widthPt = 0, heightPt = 0;  // displayed size (after rotation)
    std::vector<TextChar> chars;
    bool truncated = false;  // hit the per-page character cap
    // A page with (almost) no text layer — a scan — is kept as a picture of
    // the whole page instead of being dropped. PNG bytes, empty for text pages.
    std::vector<std::uint8_t> pagePng;
    std::vector<PageImage> images;  // text pages only
};

// PNG bytes a page carries (whole-page picture plus placed images), for the
// converter's total memory cap.
inline std::uint64_t ImageBytes(const ExtractedPage& p) {
    std::uint64_t n = p.pagePng.size();
    for (const PageImage& i : p.images) n += i.png.size();
    return n;
}

// A maximal run of characters with the same font attributes.
struct TextRun {
    std::u32string text;  // UTF-32
    std::string fontName;
    float fontSize = 12.0f;
    int weight = 400;
    bool italic = false;
    std::uint8_t r = 0, g = 0, b = 0;
};

// A line of text (characters with similar y, ordered left-to-right).
struct TextLine {
    std::vector<TextRun> runs;
    float y = 0;           // baseline y
    float left = 0, right = 0;  // line bounding box
};

enum class Alignment { Left, Center, Right, Justified };

// A paragraph: consecutive lines with no large vertical gap or style change.
struct Paragraph {
    std::vector<TextLine> lines;
    Alignment align = Alignment::Left;
    float spacingBefore = 0;  // pt
    float spacingAfter = 0;
    float lineSpacing = 1.0f;  // multiple (1.0 = single, 1.5 = 1.5x)
    // x of the body lines' left edge, pt from the PAGE's left edge (the writer
    // subtracts the section margin), so columns and pages need no conversion.
    float leftIndent = 0;
    float firstLineIndent = 0;  // first line relative to leftIndent; negative = hanging
    bool pageBreakBefore = false;
    // An image paragraph (a scanned page): PNG bytes and the size to show it
    // at. `lines` is empty when this is set.
    std::vector<std::uint8_t> imagePng;
    float imageWidthPt = 0, imageHeightPt = 0;
};

// One section (contiguous pages with the same page size).
struct Section {
    std::vector<Paragraph> paragraphs;
    float widthPt = 0, heightPt = 0;
    // Margins from where the text actually sits, so Word's content area
    // matches the PDF's text block.
    float marginLeft = 72, marginRight = 72, marginTop = 72, marginBottom = 72;
};

struct AnalyzedDocument {
    std::vector<Section> sections;
};

}  // namespace pdf
