#include "DocxWriter.h"

#include "ZipWriter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <new>
#include <utility>

namespace pdf {

namespace {
// Units: 1 pt = 20 twips (dxa); 1 pt = 12700 EMU; font sizes in half-points.
long Twips(float pt) { return std::lround(pt * 20.0f); }
long long Emu(float pt) { return std::llround(static_cast<double>(pt) * 12700.0); }

constexpr const char* kNsW = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
constexpr const char* kNsR = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";

const char* kContentTypesHead =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
    "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
    "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
    "<Default Extension=\"png\" ContentType=\"image/png\"/>"
    "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
    "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
    "</Types>";

const char* kRootRels =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
    "</Relationships>";

// Defaults matter: without docDefaults Word adds 8 pt after every paragraph
// and 1.08 line spacing (its Normal template), which would distort a layout
// taken from the PDF. Explicit zeros keep what Analyze measured.
const char* kStyles =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
    "<w:docDefaults>"
    "<w:rPrDefault><w:rPr>"
    "<w:rFonts w:ascii=\"Times New Roman\" w:hAnsi=\"Times New Roman\" w:eastAsia=\"SimSun\" w:cs=\"Times New Roman\"/>"
    "<w:sz w:val=\"24\"/><w:szCs w:val=\"24\"/><w:lang w:val=\"en-US\" w:eastAsia=\"zh-CN\"/>"
    "</w:rPr></w:rPrDefault>"
    "<w:pPrDefault><w:pPr><w:spacing w:before=\"0\" w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/></w:pPr></w:pPrDefault>"
    "</w:docDefaults>"
    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/></w:style>"
    "</w:styles>";

void AppendUtf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// XML 1.0 Char production; the extractor already filters, this is the last
// guard so nothing a later stage adds can make Word refuse the file.
bool XmlChar(char32_t c) {
    if (c == 0x09 || c == 0x0A || c == 0x0D) return true;
    if (c < 0x20 || c > 0x10FFFF) return false;
    if (c >= 0xD800 && c <= 0xDFFF) return false;
    return c != 0xFFFE && c != 0xFFFF;
}

bool IsCjk(char32_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) ||
           (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF) ||
           (c >= 0x20000 && c <= 0x2FFFF);
}

std::string Hex2(unsigned v) {
    char b[3];
    std::snprintf(b, sizeof(b), "%02X", v & 0xFF);
    return b;
}

struct Writer {
    std::string body;
    std::string rels;  // word/_rels/document.xml.rels entries after styles
    // Pictures to package: media name and the PNG (owned by the document,
    // which outlives the writer).
    std::vector<std::pair<std::string, const std::vector<std::uint8_t>*>> images;
    int imageCount = 0;
    int docPrId = 0;

    void RunProps(const TextRun& r) {
        const std::string font = XmlEscape(WordFontName(r.fontName));
        body += "<w:rPr>";
        if (!font.empty())
            body += "<w:rFonts w:ascii=\"" + font + "\" w:hAnsi=\"" + font + "\" w:eastAsia=\"" +
                    font + "\" w:cs=\"" + font + "\"/>";
        if (r.weight >= 600) body += "<w:b/><w:bCs/>";
        if (r.italic) body += "<w:i/><w:iCs/>";
        if (r.r || r.g || r.b) body += "<w:color w:val=\"" + Hex2(r.r) + Hex2(r.g) + Hex2(r.b) + "\"/>";
        const long half = std::clamp(std::lround(r.fontSize * 2.0f), 2L, 3276L);
        body += "<w:sz w:val=\"" + std::to_string(half) + "\"/><w:szCs w:val=\"" +
                std::to_string(half) + "\"/>";
        body += "</w:rPr>";
    }

    void Text(const std::u32string& t) {
        std::string utf8;
        for (char32_t c : t)
            if (XmlChar(c)) AppendUtf8(utf8, c);
        if (utf8.empty()) return;
        // preserve: leading/trailing/double spaces are significant here.
        body += "<w:t xml:space=\"preserve\">" + XmlEscape(utf8) + "</w:t>";
    }

    void Paragraph(const pdf::Paragraph& p, const Section& s, const std::string& sectPr) {
        body += "<w:p><w:pPr>";
        if (p.pageBreakBefore) body += "<w:pageBreakBefore/>";
        const long before = std::clamp(Twips(p.spacingBefore), 0L, 31680L);
        const long line = std::clamp(std::lround(p.lineSpacing * 240.0f), 120L, 1440L);
        body += "<w:spacing w:before=\"" + std::to_string(before) + "\" w:after=\"0\" w:line=\"" +
                std::to_string(line) + "\" w:lineRule=\"auto\"/>";
        const float contentWidth = std::max(36.0f, s.widthPt - s.marginLeft - s.marginRight);
        const float left = std::clamp(p.leftIndent - s.marginLeft, 0.0f, contentWidth * 0.8f);
        if (left > 0.5f || std::fabs(p.firstLineIndent) > 0.5f) {
            body += "<w:ind w:left=\"" + std::to_string(Twips(left)) + "\"";
            if (p.firstLineIndent > 0.5f)
                body += " w:firstLine=\"" + std::to_string(Twips(std::min(p.firstLineIndent, contentWidth / 2))) + "\"";
            else if (p.firstLineIndent < -0.5f)
                body += " w:hanging=\"" + std::to_string(Twips(std::min(-p.firstLineIndent, left))) + "\"";
            body += "/>";
        }
        switch (p.align) {
            case Alignment::Center:    body += "<w:jc w:val=\"center\"/>"; break;
            case Alignment::Right:     body += "<w:jc w:val=\"right\"/>"; break;
            case Alignment::Justified: body += "<w:jc w:val=\"both\"/>"; break;
            default: break;
        }
        body += sectPr;  // section break: last paragraph of a non-final section
        body += "</w:pPr>";

        if (!p.imagePng.empty()) {
            Picture(p, s);
        } else {
            for (std::size_t li = 0; li < p.lines.size(); ++li) {
                const TextLine& l = p.lines[li];
                for (std::size_t ri = 0; ri < l.runs.size(); ++ri) {
                    std::u32string text = l.runs[ri].text;
                    // Lines of one paragraph join with a space, except after
                    // a hyphen or between CJK characters (no word spaces).
                    if (ri + 1 == l.runs.size() && li + 1 < p.lines.size() && !text.empty()) {
                        const TextLine& next = p.lines[li + 1];
                        const char32_t last = text.back();
                        const char32_t first = next.runs.empty() || next.runs.front().text.empty()
                                                   ? U' ' : next.runs.front().text.front();
                        if (last != U'-' && last != U' ' && !(IsCjk(last) && IsCjk(first)))
                            text += U' ';
                    }
                    body += "<w:r>";
                    RunProps(l.runs[ri]);
                    Text(text);
                    body += "</w:r>";
                }
            }
        }
        body += "</w:p>";
    }

    void Picture(const pdf::Paragraph& p, const Section& s) {
        ++imageCount;
        ++docPrId;
        const std::string name = "image" + std::to_string(imageCount) + ".png";
        const std::string rid = "rIdImg" + std::to_string(imageCount);
        images.emplace_back(name, &p.imagePng);  // added after the XML parts
        rels += "<Relationship Id=\"" + rid + "\" Type=\"" + std::string(kNsR) +
                "/image\" Target=\"media/" + name + "\"/>";
        // Fit inside the content area (slightly under, so Word never pushes
        // the picture onto an extra page), keeping the aspect ratio.
        const float aw = std::max(36.0f, s.widthPt - s.marginLeft - s.marginRight) * 0.98f;
        const float ah = std::max(36.0f, s.heightPt - s.marginTop - s.marginBottom) * 0.98f;
        const float k = std::min({aw / std::max(1.0f, p.imageWidthPt),
                                  ah / std::max(1.0f, p.imageHeightPt), 1.0f});
        const long long cx = Emu(p.imageWidthPt * k), cy = Emu(p.imageHeightPt * k);
        const std::string ext = "cx=\"" + std::to_string(cx) + "\" cy=\"" + std::to_string(cy) + "\"";
        const std::string id = std::to_string(docPrId);
        body += "<w:r><w:drawing>"
                "<wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">"
                "<wp:extent " + ext + "/>"
                "<wp:docPr id=\"" + id + "\" name=\"Page " + id + "\"/>"
                "<wp:cNvGraphicFramePr><a:graphicFrameLocks noChangeAspect=\"1\"/></wp:cNvGraphicFramePr>"
                "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
                "<pic:pic><pic:nvPicPr><pic:cNvPr id=\"" + id + "\" name=\"" + name + "\"/><pic:cNvPicPr/></pic:nvPicPr>"
                "<pic:blipFill><a:blip r:embed=\"" + rid + "\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
                "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext " + ext + "/></a:xfrm>"
                "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr>"
                "</pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>";
    }
};

// Word accepts pages from 0.1 in to 22 in (144 to 31680 twips) per side.
constexpr float kMinPagePt = 7.2f, kMaxPagePt = 1584.0f;

// A section's page geometry (no paragraphs) inside Word's limits. An
// oversized page is scaled down with its margins, so the content area keeps
// its proportions; everything after this works on the fitted geometry.
Section FitGeometry(const Section& s) {
    Section g;
    g.widthPt = std::clamp(s.widthPt, kMinPagePt, kMaxPagePt);
    g.heightPt = std::clamp(s.heightPt, kMinPagePt, kMaxPagePt);
    const float k = std::min({1.0f, g.widthPt / std::max(1.0f, s.widthPt),
                              g.heightPt / std::max(1.0f, s.heightPt)});
    // Margins stay non-negative and leave at least a third of each side.
    g.marginLeft = std::clamp(s.marginLeft * k, 0.0f, g.widthPt / 3);
    g.marginRight = std::clamp(s.marginRight * k, 0.0f, g.widthPt / 3);
    g.marginTop = std::clamp(s.marginTop * k, 0.0f, g.heightPt / 3);
    g.marginBottom = std::clamp(s.marginBottom * k, 0.0f, g.heightPt / 3);
    return g;
}

std::string SectPr(const Section& s) {  // s: fitted geometry
    const bool landscape = s.widthPt > s.heightPt;
    std::string x = "<w:sectPr><w:pgSz w:w=\"" + std::to_string(Twips(s.widthPt)) + "\" w:h=\"" +
                    std::to_string(Twips(s.heightPt)) + "\"";
    if (landscape) x += " w:orient=\"landscape\"";
    x += "/><w:pgMar w:top=\"" + std::to_string(Twips(s.marginTop)) + "\" w:right=\"" +
         std::to_string(Twips(s.marginRight)) + "\" w:bottom=\"" +
         std::to_string(Twips(s.marginBottom)) + "\" w:left=\"" +
         std::to_string(Twips(s.marginLeft)) +
         "\" w:header=\"0\" w:footer=\"0\" w:gutter=\"0\"/></w:sectPr>";
    return x;
}
}  // namespace

std::string ToUtf8(const std::u32string& s) {
    std::string out;
    for (char32_t c : s) AppendUtf8(out, c);
    return out;
}

std::string XmlEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;
        }
    }
    return out;
}

std::string WordFontName(const std::string& pdfName) {
    std::string n = pdfName;
    if (n.size() > 7 && n[6] == '+') n.erase(0, 7);
    std::string key;
    for (char c : n)
        if (std::isalnum(static_cast<unsigned char>(c)))
            key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    struct Map { const char* prefix; const char* word; };
    static const Map kKnown[] = {
        {"timesnewroman", "Times New Roman"}, {"times", "Times New Roman"},
        {"arial", "Arial"}, {"helvetica", "Arial"}, {"couriernew", "Courier New"},
        {"courier", "Courier New"}, {"calibri", "Calibri"}, {"cambria", "Cambria"},
        {"georgia", "Georgia"}, {"verdana", "Verdana"}, {"tahoma", "Tahoma"},
        {"symbol", "Symbol"}, {"simsun", "SimSun"}, {"songti", "SimSun"}, {"stsong", "SimSun"},
        {"simhei", "SimHei"}, {"heiti", "SimHei"}, {"microsoftyahei", "Microsoft YaHei"},
        {"kaiti", "KaiTi"}, {"stkaiti", "KaiTi"}, {"fangsong", "FangSong"},
        {"dengxian", "DengXian"}, {"msmincho", "MS Mincho"}, {"msgothic", "MS Gothic"},
    };
    for (const Map& m : kKnown)
        if (key.rfind(m.prefix, 0) == 0) return m.word;
    // Unknown: drop "-Bold", ",Italic", "MT" style suffixes; bold/italic are
    // carried by run properties instead.
    const std::size_t cut = n.find_first_of("-,");
    if (cut != std::string::npos) n.resize(cut);
    if (n.size() > 2 && n.compare(n.size() - 2, 2, "MT") == 0) n.resize(n.size() - 2);
    if (n.size() > 2 && n.compare(n.size() - 2, 2, "PS") == 0) n.resize(n.size() - 2);
    return n;
}

Result<std::vector<std::uint8_t>> WriteDocx(const AnalyzedDocument& doc) {
    try {
        Writer w;
        w.body.reserve(1 << 16);
        if (doc.sections.empty()) {
            w.body += "<w:p/>";
        }
        for (std::size_t si = 0; si < doc.sections.size(); ++si) {
            const Section& src = doc.sections[si];
            const Section s = FitGeometry(src);
            const bool last = si + 1 == doc.sections.size();
            // Every section but the last ends with a paragraph carrying its
            // sectPr; the last one's goes at the end of the body.
            const std::string sect = last ? std::string() : SectPr(s);
            if (src.paragraphs.empty()) {
                w.body += "<w:p><w:pPr>" + sect + "</w:pPr></w:p>";
                continue;
            }
            for (std::size_t pi = 0; pi < src.paragraphs.size(); ++pi)
                w.Paragraph(src.paragraphs[pi], s, pi + 1 == src.paragraphs.size() ? sect : std::string());
        }
        const std::string finalSect =
            doc.sections.empty() ? SectPr(Section{{}, 612, 792})
                                 : SectPr(FitGeometry(doc.sections.back()));

        std::string document =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<w:document xmlns:w=\"" + std::string(kNsW) + "\" xmlns:r=\"" + kNsR + "\""
            " xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\""
            " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
            " xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
            "<w:body>" + w.body + finalSect + "</w:body></w:document>";
        std::string docRels =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"" + std::string(kNsR) + "/styles\" Target=\"styles.xml\"/>" +
            w.rels + "</Relationships>";

        // [Content_Types].xml first: some readers sniff the package by it.
        ZipWriter pkg;
        if (auto s = pkg.Add("[Content_Types].xml", std::string(kContentTypesHead)); !s) return s.error();
        if (auto s = pkg.Add("_rels/.rels", std::string(kRootRels)); !s) return s.error();
        if (auto s = pkg.Add("word/document.xml", document); !s) return s.error();
        if (auto s = pkg.Add("word/styles.xml", std::string(kStyles)); !s) return s.error();
        if (auto s = pkg.Add("word/_rels/document.xml.rels", docRels); !s) return s.error();
        for (const auto& [name, png] : w.images)  // PNG is already compressed: store
            if (auto s = pkg.Add("word/media/" + name, *png, /*compress=*/false); !s) return s.error();
        return pkg.Finish();
    } catch (const std::bad_alloc&) {
        return Error{L"内存不足，无法生成 Word 文档"};
    }
}

}  // namespace pdf
