// Analyze: characters → lines → paragraphs → sections, on hand-built
// character sets whose expected structure is known. Pure: no PDFium.

#include "../framework/Test.h"

#include <PDFCore/convert/Analyze.h>
#include <PDFCore/convert/DocxWriter.h>

#include <string>

using namespace pdf;

namespace {
// Lays out `text` from x = left on one baseline, 0.5 × size per glyph; spaces
// are real characters (as the extractor keeps them).
void Put(std::vector<TextChar>& out, const std::u32string& text, float left, float baseline,
         float size = 10, int weight = 400) {
    float x = left;
    for (char32_t c : text) {
        TextChar ch;
        ch.unicode = c;
        ch.left = x;
        ch.right = x + size * 0.5f;
        ch.baseline = baseline;
        ch.fontSize = size;
        ch.weight = weight;
        ch.fontName = "Helvetica";
        out.push_back(ch);
        x = ch.right;
    }
}

std::string LineText(const TextLine& l) {
    std::u32string s;
    for (const TextRun& r : l.runs) s += r.text;
    return ToUtf8(s);
}

std::string ParaText(const Paragraph& p) {
    std::string s;
    for (const TextLine& l : p.lines) s += (s.empty() ? "" : "|") + LineText(l);
    return s;
}

ExtractedPage Page(std::vector<TextChar> chars, float w = 612, float h = 792) {
    ExtractedPage p;
    p.widthPt = w;
    p.heightPt = h;
    p.chars = std::move(chars);
    return p;
}
}  // namespace

TEST(Analyze, LinesFromBaselinesInReadingOrder) {
    std::vector<TextChar> c;
    Put(c, U"world", 130, 100);
    Put(c, U"Hello", 72, 100.8f);  // same line, slightly off baseline, given later
    Put(c, U"next", 72, 114);
    const auto lines = BuildLines(c);
    EXPECT_EQ(lines.size(), 2u);
    if (lines.size() != 2u) return;
    // No space char between "Hello" (ends at 97) and "world" (130): the gap
    // is wider than a quarter em, so a space is inserted.
    EXPECT_EQ(LineText(lines[0]), std::string("Hello world"));
    EXPECT_EQ(LineText(lines[1]), std::string("next"));
    EXPECT_TRUE(lines[0].y < lines[1].y);
}

TEST(Analyze, StyleChangesSplitRunsButSpacesDoNot) {
    std::vector<TextChar> c;
    Put(c, U"bold ", 72, 100, 10, 700);
    Put(c, U"plain", 97, 100, 10, 400);
    const auto lines = BuildLines(c);
    if (lines.size() != 1u) { EXPECT_EQ(lines.size(), 1u); return; }
    EXPECT_EQ(lines[0].runs.size(), 2u);
    if (lines[0].runs.size() == 2u) {
        EXPECT_EQ(ToUtf8(lines[0].runs[0].text), std::string("bold "));
        EXPECT_EQ(lines[0].runs[0].weight, 700);
    }
}

TEST(Analyze, ParagraphsSplitOnGapIndentAndShortLine) {
    std::vector<TextLine> lines;
    std::vector<TextChar> c;
    // Paragraph 1: two full lines and a short last line.
    Put(c, std::u32string(80, U'a'), 72, 100);
    Put(c, std::u32string(80, U'b'), 72, 112);
    Put(c, U"end", 72, 124);
    // Paragraph 2: after a blank gap, first line indented.
    Put(c, std::u32string(76, U'c'), 92, 160);
    Put(c, std::u32string(80, U'd'), 72, 172);
    const auto built = BuildLines(c);
    const auto paras = BuildParagraphs(built, 72, 472);
    EXPECT_EQ(paras.size(), 2u);
    if (paras.size() != 2u) return;
    EXPECT_EQ(paras[0].lines.size(), 3u);
    EXPECT_EQ(paras[1].lines.size(), 2u);
    EXPECT_TRUE(paras[1].spacingBefore > 10);  // the blank gap
    EXPECT_TRUE(paras[1].firstLineIndent > 15 && paras[1].firstLineIndent < 25);
    EXPECT_TRUE(paras[1].leftIndent > 71 && paras[1].leftIndent < 73);
}

TEST(Analyze, CenteredAndRightAlignedLines) {
    std::vector<TextChar> c;
    Put(c, std::u32string(80, U'x'), 72, 100);   // full width: defines the column
    Put(c, U"Centered", 252, 130);                // 252..292, column 72..472
    Put(c, std::u32string(80, U'y'), 72, 160);
    Put(c, U"Right", 447.5f, 190);                // ends at the right edge
    const auto paras = BuildParagraphs(BuildLines(c), 72, 472);
    if (paras.size() != 4u) { EXPECT_EQ(paras.size(), 4u); return; }
    EXPECT_EQ(paras[1].align, Alignment::Center);
    EXPECT_EQ(paras[3].align, Alignment::Right);
    EXPECT_EQ(paras[0].align, Alignment::Left);
}

TEST(Analyze, TwoColumnsAreReadLeftThenRight) {
    std::vector<TextChar> c;
    for (int i = 0; i < 6; ++i) {
        const float y = 100 + 12.0f * i;
        Put(c, std::u32string(40, U'L'), 72, y);   // 72..272
        Put(c, std::u32string(40, U'R'), 340, y);  // 340..540
    }
    EXPECT_TRUE(FindGutter(c, 612) > 272 && FindGutter(c, 612) < 340);
    const AnalyzedDocument doc = Analyze({Page(c)});
    if (doc.sections.size() != 1u) { EXPECT_EQ(doc.sections.size(), 1u); return; }
    const auto& paras = doc.sections[0].paragraphs;
    if (paras.size() < 2u) { EXPECT_TRUE(paras.size() >= 2u); return; }
    EXPECT_EQ(ParaText(paras.front()).substr(0, 1), std::string("L"));
    EXPECT_EQ(ParaText(paras.back()).substr(0, 1), std::string("R"));
    for (const Paragraph& p : paras)
        for (const TextLine& l : p.lines) {
            const std::string t = LineText(l);
            EXPECT_TRUE(t.find('L') == std::string::npos || t.find('R') == std::string::npos);
        }
}

TEST(Analyze, SingleColumnHasNoGutter) {
    std::vector<TextChar> c;
    for (int i = 0; i < 6; ++i) Put(c, std::u32string(80, U'a'), 72, 100 + 12.0f * i);
    EXPECT_TRUE(FindGutter(c, 612) < 0);
}

TEST(Analyze, PageSizeChangeStartsSectionAndPagesBreak) {
    std::vector<TextChar> a, b, d;
    Put(a, U"first page text", 72, 100);
    Put(b, U"second page text", 72, 100);
    Put(d, U"landscape page", 72, 100);
    const AnalyzedDocument doc =
        Analyze({Page(a), Page(b), Page(d, 792, 612)});
    EXPECT_EQ(doc.sections.size(), 2u);
    if (doc.sections.size() != 2u) return;
    const auto& s0 = doc.sections[0].paragraphs;
    EXPECT_EQ(s0.size(), 2u);
    if (s0.size() == 2u) {
        EXPECT_FALSE(s0[0].pageBreakBefore);
        EXPECT_TRUE(s0[1].pageBreakBefore);
    }
    EXPECT_TRUE(doc.sections[0].marginLeft > 71 && doc.sections[0].marginLeft < 73);
    EXPECT_EQ(doc.sections[1].widthPt, 792.0f);
}

TEST(Analyze, ScannedPageBecomesImageParagraph) {
    ExtractedPage scan = Page({});
    scan.pagePng = {1, 2, 3, 4};
    const AnalyzedDocument doc = Analyze({scan});
    if (doc.sections.size() != 1u || doc.sections[0].paragraphs.size() != 1u) {
        EXPECT_TRUE(false);
        return;
    }
    const Paragraph& p = doc.sections[0].paragraphs[0];
    EXPECT_EQ(p.imagePng.size(), 4u);
    EXPECT_TRUE(p.lines.empty());
    EXPECT_EQ(p.imageWidthPt, 612.0f);
    EXPECT_EQ(doc.sections[0].marginLeft, 18.0f);
}

// No extra gap, no indent, same size: only the short line tells the
// paragraphs apart.
TEST(Analyze, ShortLineAloneEndsAParagraph) {
    std::vector<TextChar> c;
    Put(c, std::u32string(80, U'a'), 72, 100);
    Put(c, U"short", 72, 112);
    Put(c, std::u32string(80, U'b'), 72, 124);
    Put(c, std::u32string(80, U'b'), 72, 136);
    const auto paras = BuildParagraphs(BuildLines(c), 72, 472);
    EXPECT_EQ(paras.size(), 2u);
    if (paras.size() == 2u) {
        EXPECT_EQ(paras[0].lines.size(), 2u);
        EXPECT_EQ(paras[1].spacingBefore, 0.0f);
    }
}

// Hostile geometry: a page narrower than the 18 pt minimum margin x 3, and
// glyph boxes as wide as the page. Must finish and stay in range.
TEST(Analyze, TinyPageAndHugeGlyphBoxesAreSafe) {
    std::vector<TextChar> c;
    Put(c, U"abcdefghijklmnop", 1, 10, 2);
    const AnalyzedDocument tiny = Analyze({Page(c, 30, 30)});
    if (tiny.sections.size() != 1u) { EXPECT_EQ(tiny.sections.size(), 1u); return; }
    EXPECT_TRUE(tiny.sections[0].marginLeft <= 10.0f);  // at most a third of 30

    std::vector<TextChar> wide;
    for (int i = 0; i < 2000; ++i) {
        TextChar ch;
        ch.unicode = U'x';
        ch.left = 0;
        ch.right = 144000;
        ch.baseline = 10.0f + i;
        ch.fontSize = 10;
        wide.push_back(ch);
    }
    EXPECT_TRUE(FindGutter(wide, 144000) < 0);  // fully covered: no gutter
}

// A picture between two paragraphs lands between them; one above all text
// comes first; one below everything comes last.
TEST(Analyze, PicturesAreInsertedInReadingOrder) {
    std::vector<TextChar> c;
    Put(c, U"above", 72, 100);
    Put(c, U"below", 72, 300);
    ExtractedPage page = Page(c);
    auto pic = [](float top, std::uint8_t tag) {
        PageImage i;
        i.left = 72;
        i.right = 272;
        i.top = top;
        i.bottom = top + 100;
        i.png = {tag};
        return i;
    };
    page.images = {pic(400, 3), pic(150, 2), pic(20, 1)};  // given out of order
    const AnalyzedDocument doc = Analyze({page});
    if (doc.sections.size() != 1u) { EXPECT_EQ(doc.sections.size(), 1u); return; }
    const auto& ps = doc.sections[0].paragraphs;
    EXPECT_EQ(ps.size(), 5u);
    if (ps.size() != 5u) return;
    EXPECT_EQ(ps[0].imagePng.size(), 1u);
    if (!ps[0].imagePng.empty()) EXPECT_EQ(ps[0].imagePng[0], 1);
    EXPECT_EQ(ParaText(ps[1]), std::string("above"));
    if (!ps[2].imagePng.empty()) EXPECT_EQ(ps[2].imagePng[0], 2);
    EXPECT_EQ(ParaText(ps[3]), std::string("below"));
    if (!ps[4].imagePng.empty()) EXPECT_EQ(ps[4].imagePng[0], 3);
    EXPECT_EQ(ps[2].imageWidthPt, 200.0f);
}
