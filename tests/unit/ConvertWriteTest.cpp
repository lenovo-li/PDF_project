// ZipWriter and DocxWriter: the archive reads back (ZipReader.h checks CRCs
// and headers), the package has the parts Word requires, and the document XML
// carries the analyzed structure. Pure: no PDFium.

#include "../framework/Test.h"
#include "../framework/ZipReader.h"

#include <PDFCore/convert/DocxWriter.h>
#include <PDFCore/convert/ZipWriter.h>

#include <map>
#include <string>

using namespace pdf;

namespace {
std::size_t Count(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
    return n;
}

TextLine Line(const std::u32string& text, float y, float left, float right, float size = 12,
              int weight = 400) {
    TextLine l;
    TextRun r;
    r.text = text;
    r.fontName = "Helvetica";
    r.fontSize = size;
    r.weight = weight;
    l.runs.push_back(r);
    l.y = y;
    l.left = left;
    l.right = right;
    return l;
}

Section Letter() {
    Section s;
    s.widthPt = 612;
    s.heightPt = 792;
    return s;
}
}  // namespace

TEST(Zip, RoundTripsStoredAndDeflatedEntries) {
    ZipWriter z;
    const std::string text(5000, 'a');  // compresses well: deflated
    EXPECT_TRUE(z.Add("a/text.xml", text).ok());
    EXPECT_TRUE(z.Add("b.bin", std::vector<std::uint8_t>{1, 2, 3}, false).ok());
    EXPECT_TRUE(z.Add("empty", std::string()).ok());
    EXPECT_FALSE(z.Add("b.bin", std::string("dup")).ok());
    EXPECT_FALSE(z.Add("/abs", std::string("x")).ok());
    EXPECT_EQ(z.EntryCount(), 3u);
    const auto bytes = z.Finish();
    EXPECT_EQ(z.EntryCount(), 0u);
    std::map<std::string, pdftest::ZipEntry> e;
    EXPECT_TRUE(pdftest::ReadZip(bytes, &e));
    EXPECT_EQ(e.size(), 3u);
    EXPECT_EQ(e["a/text.xml"].data, text);
    EXPECT_EQ(e["a/text.xml"].method, 8);
    EXPECT_EQ(e["b.bin"].method, 0);
    EXPECT_EQ(e["b.bin"].data, std::string("\x01\x02\x03"));
    EXPECT_EQ(e["empty"].data, std::string());
    EXPECT_EQ(e["a/text.xml"].flags & (1u << 11), 1u << 11);  // UTF-8 names
}

TEST(Zip, Crc32MatchesKnownValue) {
    const std::string s = "123456789";  // the standard check value
    EXPECT_EQ(Crc32(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()), 0xCBF43926u);
}

TEST(Docx, EscapesAndConvertsText) {
    EXPECT_EQ(XmlEscape("a<b>&\"'"), std::string("a&lt;b&gt;&amp;&quot;&apos;"));
    EXPECT_EQ(ToUtf8(U"A中\U0001F600"), std::string("A\xE4\xB8\xAD\xF0\x9F\x98\x80"));
}

TEST(Docx, MapsPdfFontNames) {
    EXPECT_EQ(WordFontName("ABCDEF+TimesNewRomanPS-BoldMT"), std::string("Times New Roman"));
    EXPECT_EQ(WordFontName("Helvetica-Oblique"), std::string("Arial"));
    EXPECT_EQ(WordFontName("SimSun"), std::string("SimSun"));
    EXPECT_EQ(WordFontName("MyFont-Bold"), std::string("MyFont"));
    EXPECT_EQ(WordFontName("GaramondMT"), std::string("Garamond"));
}

TEST(Docx, PackageHasRequiredParts) {
    AnalyzedDocument doc;
    doc.sections.push_back(Letter());
    Paragraph p;
    p.lines.push_back(Line(U"Hello & <world>", 100, 72, 200));
    doc.sections[0].paragraphs.push_back(p);
    auto bytes = WriteDocx(doc);
    if (!bytes) { EXPECT_TRUE(bytes.ok()); return; }
    std::map<std::string, pdftest::ZipEntry> e;
    EXPECT_TRUE(pdftest::ReadZip(bytes.value(), &e));
    for (const char* part : {"[Content_Types].xml", "_rels/.rels", "word/document.xml",
                             "word/styles.xml", "word/_rels/document.xml.rels"})
        EXPECT_TRUE(e.count(part) == 1);
    const std::string& xml = e["word/document.xml"].data;
    EXPECT_TRUE(xml.find("Hello &amp; &lt;world&gt;") != std::string::npos);
    EXPECT_EQ(Count(xml, "<w:p>"), 1u);
    EXPECT_EQ(Count(xml, "<w:sectPr>"), 1u);
    EXPECT_TRUE(xml.find("w:w=\"12240\" w:h=\"15840\"") != std::string::npos);  // Letter in twips
    EXPECT_TRUE(e["[Content_Types].xml"].data.find("/word/document.xml") != std::string::npos);
}

TEST(Docx, SectionsAlignmentIndentAndRunProperties) {
    AnalyzedDocument doc;
    doc.sections.push_back(Letter());
    Section land;
    land.widthPt = 792;
    land.heightPt = 612;
    doc.sections.push_back(land);

    Paragraph centered;
    centered.align = Alignment::Center;
    centered.lines.push_back(Line(U"Title", 80, 250, 360, 20, 700));
    Paragraph body;
    body.leftIndent = 72 + 36;  // half an inch inside the 72 pt margin
    body.firstLineIndent = 18;
    body.lines.push_back(Line(U"first", 120, 126, 540));
    body.lines.push_back(Line(U"second", 134, 108, 300));
    doc.sections[0].paragraphs = {centered, body};
    Paragraph next;
    next.lines.push_back(Line(U"landscape", 90, 72, 200));
    doc.sections[1].paragraphs = {next};

    auto bytes = WriteDocx(doc);
    if (!bytes) { EXPECT_TRUE(bytes.ok()); return; }
    std::map<std::string, pdftest::ZipEntry> e;
    EXPECT_TRUE(pdftest::ReadZip(bytes.value(), &e));
    const std::string& xml = e["word/document.xml"].data;
    EXPECT_EQ(Count(xml, "<w:sectPr>"), 2u);
    EXPECT_EQ(Count(xml, "w:orient=\"landscape\""), 1u);
    EXPECT_TRUE(xml.find("<w:jc w:val=\"center\"/>") != std::string::npos);
    EXPECT_TRUE(xml.find("<w:b/>") != std::string::npos);
    EXPECT_TRUE(xml.find("<w:sz w:val=\"40\"/>") != std::string::npos);  // 20 pt
    EXPECT_TRUE(xml.find("w:left=\"720\" w:firstLine=\"360\"") != std::string::npos);
    // Lines of one paragraph join with a space.
    EXPECT_TRUE(xml.find(">first </w:t>") != std::string::npos);
}

TEST(Docx, CjkLinesJoinWithoutSpace) {
    AnalyzedDocument doc;
    doc.sections.push_back(Letter());
    Paragraph p;
    p.lines.push_back(Line(U"中文", 100, 72, 540));
    p.lines.push_back(Line(U"段落", 114, 72, 120));
    doc.sections[0].paragraphs.push_back(p);
    auto bytes = WriteDocx(doc);
    if (!bytes) { EXPECT_TRUE(bytes.ok()); return; }
    std::map<std::string, pdftest::ZipEntry> e;
    EXPECT_TRUE(pdftest::ReadZip(bytes.value(), &e));
    EXPECT_TRUE(e["word/document.xml"].data.find(ToUtf8(U"中文") + "</w:t>") !=
                std::string::npos);
}

TEST(Docx, ImageParagraphIsPackagedAndReferenced) {
    AnalyzedDocument doc;
    doc.sections.push_back(Letter());
    Paragraph p;
    p.imagePng = {0x89, 'P', 'N', 'G', 1, 2, 3};
    p.imageWidthPt = 612;
    p.imageHeightPt = 792;
    doc.sections[0].paragraphs.push_back(p);
    auto bytes = WriteDocx(doc);
    if (!bytes) { EXPECT_TRUE(bytes.ok()); return; }
    std::map<std::string, pdftest::ZipEntry> e;
    EXPECT_TRUE(pdftest::ReadZip(bytes.value(), &e));
    EXPECT_EQ(e["word/media/image1.png"].data.size(), 7u);
    EXPECT_EQ(e["word/media/image1.png"].method, 0);  // stored, PNG is compressed already
    EXPECT_TRUE(e["word/_rels/document.xml.rels"].data.find("media/image1.png") != std::string::npos);
    EXPECT_TRUE(e["word/document.xml"].data.find("r:embed=\"rIdImg1\"") != std::string::npos);
}

TEST(Docx, DropsCharactersXmlCannotHold) {
    AnalyzedDocument doc;
    doc.sections.push_back(Letter());
    Paragraph p;
    p.lines.push_back(Line(std::u32string(U"a") + char32_t(1) + U"b" + char32_t(0xFFFF), 100, 72, 200));
    doc.sections[0].paragraphs.push_back(p);
    auto bytes = WriteDocx(doc);
    if (!bytes) { EXPECT_TRUE(bytes.ok()); return; }
    std::map<std::string, pdftest::ZipEntry> e;
    EXPECT_TRUE(pdftest::ReadZip(bytes.value(), &e));
    EXPECT_TRUE(e["word/document.xml"].data.find(">ab</w:t>") != std::string::npos);
}

TEST(Docx, OversizedPageIsFittedToWordLimits) {
    AnalyzedDocument doc;
    Section s;
    s.widthPt = 14400;  // 200 in
    s.heightPt = 7200;
    s.marginLeft = s.marginRight = s.marginTop = s.marginBottom = 720;
    doc.sections.push_back(s);
    auto bytes = WriteDocx(doc);
    if (!bytes) { EXPECT_TRUE(bytes.ok()); return; }
    std::map<std::string, pdftest::ZipEntry> e;
    EXPECT_TRUE(pdftest::ReadZip(bytes.value(), &e));
    const std::string& xml = e["word/document.xml"].data;
    EXPECT_TRUE(xml.find("w:w=\"31680\" w:h=\"31680\"") != std::string::npos);
    // Margins scaled with the page: 720 pt x (1584 / 14400) = 79.2 pt.
    EXPECT_TRUE(xml.find("w:left=\"1584\"") != std::string::npos);
}
