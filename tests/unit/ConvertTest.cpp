// PDF → Word end to end: a PDF built with known text goes through
// ExtractPage → Analyze → WriteDocx, and the .docx is read back. The expected
// structure (paragraph count, text per paragraph) is the fixture.

#include "../framework/FormFixture.h"
#include "../framework/Test.h"
#include "../framework/TestEngine.h"
#include "../framework/ZipReader.h"

#include <PDFCore/Document.h>
#include <PDFCore/convert/Convert.h>
#include <PDFCore/convert/Extract.h>

#include <map>
#include <string>
#include <vector>

using namespace pdf;

namespace {
// Text of each <w:p> in document order (all <w:t> contents joined).
std::vector<std::string> Paragraphs(const std::string& xml) {
    std::vector<std::string> out;
    for (std::size_t at = xml.find("<w:p>"); at != std::string::npos; at = xml.find("<w:p>", at + 1)) {
        const std::size_t end = xml.find("</w:p>", at);
        std::string text;
        for (std::size_t t = xml.find("<w:t", at); t != std::string::npos && t < end;
             t = xml.find("<w:t", t + 1)) {
            const std::size_t open = xml.find('>', t) + 1;
            text += xml.substr(open, xml.find("</w:t>", open) - open);
        }
        out.push_back(text);
    }
    return out;
}

// Helvetica 12 pt is ~6 pt per lowercase glyph: these fill most of a
// 72..540 pt column, so they read as wrapped lines of one paragraph.
const std::wstring kLong1 = L"the quick brown fox jumps over the lazy dog and keeps on running far";
const std::wstring kLong2 = L"away from the farm until the sun goes down behind the hills at last";

// Page 1: a 24 pt title, then two paragraphs of two lines each (the second
// line of each is short, ending the paragraph). Page 2: blank (a "scan").
Result<Document> SampleDoc() {
    auto doc = Document::CreateEmpty();
    if (!doc) return doc.error();
    Document& d = doc.value();
    if (auto s = d.AddBlankPage(612, 792); !s) return s.error();
    if (auto s = d.AddBlankPage(612, 792); !s) return s.error();
    struct L { const wchar_t* t; double x, y, size; };
    const L lines[] = {
        {L"Report Title", 72, 720, 24},
        {kLong1.c_str(), 72, 680, 12},
        {L"short end.", 72, 666, 12},
        {kLong2.c_str(), 72, 630, 12},
        {L"the end.", 72, 616, 12},
    };
    for (const L& l : lines)
        if (auto s = d.AddTextLine(0, l.t, l.x, l.y, l.size); !s) return s.error();
    auto bytes = d.SaveToMemory();
    if (!bytes) return bytes.error();
    return Document::OpenMemory(std::move(bytes.value()));
}

std::string Utf8(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += static_cast<char>(c);  // ASCII fixtures only
    return s;
}
}  // namespace

TEST(Convert, ExtractReadsCharactersInDisplayedSpace) {
    pdftest::OnEngine([] {
        auto doc = SampleDoc();
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto page = ExtractPage(doc.value(), 0);
        if (!page) { EXPECT_TRUE(page.ok()); return; }
        EXPECT_TRUE(page.value().pagePng.empty());
        EXPECT_TRUE(page.value().chars.size() > 100u);
        const TextChar& first = page.value().chars.front();  // 'R' of the title
        EXPECT_EQ(first.unicode, U'R');
        EXPECT_TRUE(first.left > 71 && first.left < 74);
        EXPECT_TRUE(first.baseline > 70 && first.baseline < 74);  // 792 - 720, y down
        EXPECT_TRUE(first.fontSize > 23 && first.fontSize < 25);
        EXPECT_FALSE(ExtractPage(doc.value(), 2).ok());  // out of range
    });
}

TEST(Convert, BlankPageIsEmbeddedAsPicture) {
    pdftest::OnEngine([] {
        auto doc = SampleDoc();
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto page = ExtractPage(doc.value(), 1);
        if (!page) { EXPECT_TRUE(page.ok()); return; }
        EXPECT_TRUE(page.value().chars.empty());
        const auto& png = page.value().pagePng;
        EXPECT_TRUE(png.size() > 8u && png[0] == 0x89 && png[1] == 'P');
    });
}

// The expected-structure fixture for SampleDoc: four paragraphs of text and
// one picture, the two-line paragraphs joined into one Word paragraph each.
TEST(Convert, SampleDocumentHasExpectedStructure) {
    pdftest::OnEngine([] {
        auto doc = SampleDoc();
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto docx = ConvertToDocx(doc.value());
        if (!docx) { EXPECT_TRUE(docx.ok()); return; }
        std::map<std::string, pdftest::ZipEntry> e;
        EXPECT_TRUE(pdftest::ReadZip(docx.value(), &e));
        const std::string& xml = e["word/document.xml"].data;
        const auto paras = Paragraphs(xml);
        EXPECT_EQ(paras.size(), 4u);  // title, two body paragraphs, the picture
        if (paras.size() != 4u) return;
        EXPECT_EQ(paras[0], std::string("Report Title"));
        EXPECT_EQ(paras[1], Utf8(kLong1) + " short end.");
        EXPECT_EQ(paras[2], Utf8(kLong2) + " the end.");
        EXPECT_EQ(paras[3], std::string());  // picture paragraph
        EXPECT_EQ(e.count("word/media/image1.png"), 1u);
        EXPECT_TRUE(xml.find("<w:sz w:val=\"48\"/>") != std::string::npos);  // 24 pt title
        EXPECT_TRUE(xml.find("<w:pageBreakBefore/>") != std::string::npos);
    });
}

TEST(Convert, RotatedPageIsLandscapeSection) {
    pdftest::OnEngine([] {
        auto doc = SampleDoc();
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        EXPECT_TRUE(doc.value().SetPageRotation(0, 90).ok());
        auto page = ExtractPage(doc.value(), 0);
        if (!page) { EXPECT_TRUE(page.ok()); return; }
        EXPECT_EQ(page.value().widthPt, 792.0f);
        EXPECT_EQ(page.value().heightPt, 612.0f);
        auto docx = ConvertToDocx(doc.value());
        if (!docx) { EXPECT_TRUE(docx.ok()); return; }
        std::map<std::string, pdftest::ZipEntry> e;
        EXPECT_TRUE(pdftest::ReadZip(docx.value(), &e));
        const std::string& xml = e["word/document.xml"].data;
        EXPECT_TRUE(xml.find("w:orient=\"landscape\"") != std::string::npos);
        EXPECT_TRUE(xml.find("<w:sectPr>") != xml.rfind("<w:sectPr>"));  // two sections
    });
}

TEST(Convert, EmptyDocumentIsAnError) {
    pdftest::OnEngine([] {
        auto doc = Document::CreateEmpty();
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        EXPECT_FALSE(ConvertToDocx(doc.value()).ok());
    });
}

// A /BaseFont that is not ASCII (GBK, or garbage) must not reach the XML.
TEST(Convert, NonAsciiFontNameIsDropped) {
    pdftest::OnEngine([] {
        const std::string content = "BT /F1 12 Tf 72 700 Td (hello world text) Tj ET";
        std::vector<std::string> objs;
        objs.push_back("<< /Type /Catalog /Pages 2 0 R >>");
        objs.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
        objs.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
                       "/Resources << /Font << /F1 5 0 R >> >> >>");
        objs.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content +
                       "\nendstream");
        objs.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /#CB#CE#CC#E5#01x >>");
        auto doc = Document::OpenMemory(pdftest::BuildPdf(objs));
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto page = ExtractPage(doc.value(), 0);
        if (!page) { EXPECT_TRUE(page.ok()); return; }
        EXPECT_FALSE(page.value().chars.empty());
        for (const TextChar& ch : page.value().chars) EXPECT_TRUE(ch.fontName.empty());
    });
}

// Office/PowerPoint exports draw text at a huge nominal size under a small
// scale ("0.05 0 0 0.05 0 0 cm ... 240 Tf" is 12 pt on paper). The font size
// must include the matrix: with the nominal 240 every line of the page fell
// into one band and the lines came out interleaved.
TEST(Convert, ScaledTextUsesEffectiveFontSize) {
    pdftest::OnEngine([] {
        const std::string content =
            "q 0.05 0 0 0.05 0 0 cm BT /F1 240 Tf 1440 14000 Td (first line of text here) Tj "
            "0 -320 Td (second line of text here) Tj ET Q";
        std::vector<std::string> objs;
        objs.push_back("<< /Type /Catalog /Pages 2 0 R >>");
        objs.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
        objs.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
                       "/Resources << /Font << /F1 5 0 R >> >> >>");
        objs.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content +
                       "\nendstream");
        objs.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
        auto doc = Document::OpenMemory(pdftest::BuildPdf(objs));
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto page = ExtractPage(doc.value(), 0);
        if (!page) { EXPECT_TRUE(page.ok()); return; }
        if (page.value().chars.empty()) { EXPECT_TRUE(false); return; }
        const float size = page.value().chars.front().fontSize;
        EXPECT_TRUE(size > 11.5f && size < 12.5f);
        auto docx = ConvertToDocx(doc.value());
        if (!docx) { EXPECT_TRUE(docx.ok()); return; }
        std::map<std::string, pdftest::ZipEntry> e;
        EXPECT_TRUE(pdftest::ReadZip(docx.value(), &e));
        const auto paras = Paragraphs(e["word/document.xml"].data);
        // Two short lines (each ends a paragraph), in reading order. With the
        // nominal size they shared one band and came out letter-interleaved.
        EXPECT_EQ(paras.size(), 2u);
        if (paras.size() == 2u) {
            EXPECT_EQ(paras[0], std::string("first line of text here"));
            EXPECT_EQ(paras[1], std::string("second line of text here"));
        }
    });
}

// A screenshot on a text page (slides, manuals) is kept as a picture in
// place; a tiny one (an icon) is left out.
TEST(Convert, PicturesOnTextPagesAreKept) {
    pdftest::OnEngine([] {
        const std::string pixels(40 * 40, static_cast<char>(0x40));
        const std::string content =
            "BT /F1 12 Tf 72 720 Td (paragraph above the picture) Tj ET "
            "q 200 0 0 150 72 520 cm /Im1 Do Q "
            "q 10 0 0 10 300 520 cm /Im1 Do Q "
            "BT /F1 12 Tf 72 480 Td (paragraph below the picture) Tj ET";
        std::vector<std::string> objs;
        objs.push_back("<< /Type /Catalog /Pages 2 0 R >>");
        objs.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
        objs.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
                       "/Resources << /Font << /F1 5 0 R >> /XObject << /Im1 6 0 R >> >> >>");
        objs.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content +
                       "\nendstream");
        objs.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
        objs.push_back("<< /Type /XObject /Subtype /Image /Width 40 /Height 40 /ColorSpace "
                       "/DeviceGray /BitsPerComponent 8 /Length 1600 >>\nstream\n" + pixels +
                       "\nendstream");
        auto doc = Document::OpenMemory(pdftest::BuildPdf(objs));
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto page = ExtractPage(doc.value(), 0);
        if (!page) { EXPECT_TRUE(page.ok()); return; }
        EXPECT_EQ(page.value().images.size(), 1u);  // the 10 pt icon is skipped
        if (page.value().images.size() == 1u) {
            const PageImage& img = page.value().images[0];
            EXPECT_TRUE(img.left > 71 && img.left < 73 && img.right > 271 && img.right < 273);
            EXPECT_TRUE(img.top > 121 && img.top < 123);  // 792 - 670
            EXPECT_TRUE(img.png.size() > 8u && img.png[0] == 0x89);
        }
        auto docx = ConvertToDocx(doc.value());
        if (!docx) { EXPECT_TRUE(docx.ok()); return; }
        std::map<std::string, pdftest::ZipEntry> e;
        EXPECT_TRUE(pdftest::ReadZip(docx.value(), &e));
        const auto paras = Paragraphs(e["word/document.xml"].data);
        EXPECT_EQ(paras.size(), 3u);
        if (paras.size() == 3u) {
            EXPECT_EQ(paras[0], std::string("paragraph above the picture"));
            EXPECT_EQ(paras[1], std::string());  // the picture
            EXPECT_EQ(paras[2], std::string("paragraph below the picture"));
        }
        EXPECT_EQ(e.count("word/media/image1.png"), 1u);
    });
}
