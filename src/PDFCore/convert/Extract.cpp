#include "Extract.h"

#include "../Document.h"
#include "../ImageExport.h"
#include "../Render.h"
#include "../Trace.h"
#include "../UnitMapping.h"

#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>

namespace pdf {

namespace {
struct PageCloser {
    void operator()(fpdf_page_t__* p) const { FPDF_ClosePage(p); }
};
struct TextPageCloser {
    void operator()(fpdf_textpage_t__* p) const { FPDFText_ClosePage(p); }
};
using PagePtr = std::unique_ptr<fpdf_page_t__, PageCloser>;
using TextPagePtr = std::unique_ptr<fpdf_textpage_t__, TextPageCloser>;

// Hostile pages can claim millions of glyphs; a real dense page has ~10 k.
constexpr int kMaxCharsPerPage = 200000;
// Fewer real characters than this and the page is treated as a scan.
constexpr int kMinCharsForTextPage = 10;
// Scanned pages are embedded at this resolution, within this many bytes.
constexpr double kScanDpi = 150.0;
constexpr std::uint64_t kScanBitmapBytes = 64ull * 1024 * 1024;
// PDF font descriptor flag (ISO 32000-1, 9.8.2): bit 7, Italic.
constexpr int kFontFlagItalic = 1 << 6;

// XML 1.0 allows tab, LF, CR and everything from U+0020 except surrogates and
// U+FFFE/FFFF. Anything else in the text layer would make Word refuse the file.
bool IsXmlChar(char32_t c) {
    if (c == 0x09 || c == 0x0A || c == 0x0D) return true;
    if (c < 0x20 || c > 0x10FFFF) return false;
    if (c >= 0xD800 && c <= 0xDFFF) return false;
    return c != 0xFFFE && c != 0xFFFF;
}

std::string Lower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Base font name without the six-letter subset tag ("ABCDEF+Arial-Bold").
std::string ReadFontName(FPDF_TEXTPAGE text, int i, int* flags) {
    const unsigned long bytes = FPDFText_GetFontInfo(text, i, nullptr, 0, flags);
    if (bytes < 2 || bytes > 512) return {};
    std::string buf(bytes, '\0');
    FPDFText_GetFontInfo(text, i, buf.data(), bytes, flags);
    buf.resize(bytes - 1);
    if (buf.size() > 7 && buf[6] == '+') buf.erase(0, 7);
    // The name is the raw /BaseFont bytes, not guaranteed UTF-8: Chinese PDFs
    // often carry GBK names, and a hostile one any byte. Only printable ASCII
    // is kept; it goes into the .docx XML, and Word rejects bad UTF-8 or
    // control characters there. A non-ASCII name maps to no Word font anyway.
    for (char c : buf)
        if (c < 0x21 || c > 0x7E) return {};
    return buf;
}

// Weight from the font when PDFium knows it, otherwise from the name.
int WeightOf(FPDF_TEXTPAGE text, int i, const std::string& lowerName) {
    const int w = FPDFText_GetFontWeight(text, i);
    if (w >= 100 && w <= 1000) return w;
    if (lowerName.find("black") != std::string::npos ||
        lowerName.find("heavy") != std::string::npos)
        return 900;
    if (lowerName.find("bold") != std::string::npos) return 700;
    return 400;
}

// Displayed size of the page in points (rotation applied), without parsing it.
bool DisplayedSize(const Document& doc, int index, float* w, float* h) {
    FS_SIZEF s{};
    if (!FPDF_GetPageSizeByIndexF(doc.Raw(), index, &s)) return false;
    if (!(s.width > 0 && s.height > 0 && s.width <= kMaxPageSidePt && s.height <= kMaxPageSidePt))
        return false;
    *w = s.width;
    *h = s.height;
    return true;
}

// Pictures on a text page. Smaller than this on either side (icons,
// bullets, a word set as an image inside a line) they are left out: as a
// paragraph of their own they would break the text around them apart.
constexpr float kMinImageSidePt = 24.0f;
constexpr int kMaxImagesPerPage = 64;
constexpr std::uint64_t kMaxImageBitmapBytes = 64ull * 1024 * 1024;  // one decoded image
constexpr std::uint64_t kMaxPageImageBytes = 64ull * 1024 * 1024;    // PNGs of one page

struct BitmapCloser {
    void operator()(fpdf_bitmap_t__* b) const { FPDFBitmap_Destroy(b); }
};
using BitmapPtr = std::unique_ptr<fpdf_bitmap_t__, BitmapCloser>;

// PDFium bitmap (any format) -> opaque BGRA, composited on white as Word
// shows a picture on a white page. False if the format is unknown.
bool ToBgra(FPDF_BITMAP src, Bitmap* out) {
    const int w = FPDFBitmap_GetWidth(src), h = FPDFBitmap_GetHeight(src);
    const int stride = FPDFBitmap_GetStride(src);
    const auto* data = static_cast<const std::uint8_t*>(FPDFBitmap_GetBuffer(src));
    const int format = FPDFBitmap_GetFormat(src);
    if (w <= 0 || h <= 0 || !data || stride <= 0) return false;
    if (static_cast<std::uint64_t>(w) * h * 4 > kMaxImageBitmapBytes) return false;
    out->width = w;
    out->height = h;
    out->pixels.assign(static_cast<std::size_t>(w) * h * 4, 0xFF);
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* s = data + static_cast<std::size_t>(y) * stride;
        std::uint8_t* d = out->pixels.data() + static_cast<std::size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x, d += 4) {
            switch (format) {
                case FPDFBitmap_Gray: d[0] = d[1] = d[2] = s[x]; break;
                case FPDFBitmap_BGR: d[0] = s[3 * x]; d[1] = s[3 * x + 1]; d[2] = s[3 * x + 2]; break;
                case FPDFBitmap_BGRx: d[0] = s[4 * x]; d[1] = s[4 * x + 1]; d[2] = s[4 * x + 2]; break;
                case FPDFBitmap_BGRA:
                case FPDFBitmap_BGRA_Premul: {
                    const unsigned a = s[4 * x + 3];
                    const bool premul = format == FPDFBitmap_BGRA_Premul;
                    for (int c = 0; c < 3; ++c) {
                        const unsigned v = s[4 * x + c];
                        // over white: v*a + 255*(1-a), with v already *a when premultiplied
                        d[c] = static_cast<std::uint8_t>(
                            premul ? std::min(255u, v + (255 - a)) : (v * a + 255 * (255 - a)) / 255);
                    }
                    break;
                }
                default: return false;
            }
        }
    }
    return true;
}

// Image objects placed directly on the page. Images inside form XObjects
// are not visited (their bounds are in the form's own space).
void ExtractImages(const Document& doc, FPDF_PAGE page, ExtractedPage* out) {
    const int n = FPDFPage_CountObjects(page);
    std::uint64_t bytes = 0;
    for (int i = 0; i < n && static_cast<int>(out->images.size()) < kMaxImagesPerPage; ++i) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_IMAGE) continue;
        float l = 0, b = 0, r = 0, t = 0;
        if (!FPDFPageObj_GetBounds(obj, &l, &b, &r, &t)) continue;
        const UnitRect u = detail::BoxToUnit(page, {l, b, r, t});
        PageImage img;
        img.left = static_cast<float>(std::clamp(u.left, 0.0, 1.0) * out->widthPt);
        img.right = static_cast<float>(std::clamp(u.right, 0.0, 1.0) * out->widthPt);
        img.top = static_cast<float>(std::clamp(u.top, 0.0, 1.0) * out->heightPt);
        img.bottom = static_cast<float>(std::clamp(u.bottom, 0.0, 1.0) * out->heightPt);
        if (img.right - img.left < kMinImageSidePt || img.bottom - img.top < kMinImageSidePt) continue;
        // Mask and matrix applied, so the picture looks as it does on the page.
        BitmapPtr bmp(FPDFImageObj_GetRenderedBitmap(doc.Raw(), page, obj));
        Bitmap bgra;
        if (!bmp || !ToBgra(bmp.get(), &bgra)) {
            TraceF("[PDFTool.Convert] page %d: image %d not decodable, skipped\n", out->pageIndex, i);
            continue;
        }
        bmp.reset();
        auto png = EncodePng(bgra);
        if (!png) continue;
        if (bytes + png.value().size() > kMaxPageImageBytes) break;
        bytes += png.value().size();
        img.png = std::move(png.value());
        out->images.push_back(std::move(img));
    }
}

Status RenderScan(const Document& doc, int index, ExtractedPage* out) {
    const SizePx px = FitRenderSize(out->widthPt * kScanDpi / 72.0,
                                    out->heightPt * kScanDpi / 72.0, kScanBitmapBytes);
    if (px.width <= 0 || px.height <= 0) return Error{L"页面太大，无法转为图片"};
    auto bmp = doc.RenderPage(index, px.width, px.height);
    if (!bmp) return bmp.error();
    auto png = EncodePng(bmp.value());
    if (!png) return png.error();
    out->pagePng = std::move(png.value());
    return Ok{};
}
}  // namespace

Result<ExtractedPage> ExtractPage(const Document& doc, int pageIndex) {
    if (pageIndex < 0 || pageIndex >= doc.PageCount())
        return Error{L"页码超出范围", FPDF_ERR_PAGE};
    ExtractedPage out;
    out.pageIndex = pageIndex;
    if (!DisplayedSize(doc, pageIndex, &out.widthPt, &out.heightPt)) {
        out.widthPt = 612;  // as DisplayedPageSizesPt does for an unreadable size
        out.heightPt = 792;
    }

    PagePtr page(FPDF_LoadPage(doc.Raw(), pageIndex));
    if (!page) return Error{L"无法加载页面", FPDF_GetLastError()};
    TextPagePtr text(FPDFText_LoadPage(page.get()));
    if (!text) return Error{L"无法读取页面文字"};

    const int n = FPDFText_CountChars(text.get());
    const int limit = std::min(std::max(n, 0), kMaxCharsPerPage);
    out.truncated = n > kMaxCharsPerPage;
    try {
        out.chars.reserve(static_cast<std::size_t>(limit));
        int real = 0;
        for (int i = 0; i < limit; ++i) {
            char32_t cp = FPDFText_GetUnicode(text.get(), i);
            // PDFium reports UTF-16 code units; join a surrogate pair.
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < limit) {
                const char32_t lo = FPDFText_GetUnicode(text.get(), i + 1);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ++i;
                }
            }
            const bool generated = FPDFText_IsGenerated(text.get(), i) == 1;
            if (generated) {
                // Line breaks are rebuilt from geometry; keep only spaces,
                // after the glyph they follow.
                if (cp != U' ' || out.chars.empty() || out.chars.back().generated) continue;
                TextChar sp = out.chars.back();
                sp.unicode = U' ';
                sp.left = sp.right;
                sp.generated = true;
                out.chars.push_back(std::move(sp));
                continue;
            }
            if (!IsXmlChar(cp) || cp == U'\r' || cp == U'\n') continue;

            FS_RECTF box{};
            if (!FPDFText_GetLooseCharBox(text.get(), i, &box)) continue;
            const UnitRect u = detail::BoxToUnit(page.get(), {box.left, box.bottom, box.right, box.top});
            double ox = 0, oy = 0;
            if (!FPDFText_GetCharOrigin(text.get(), i, &ox, &oy)) continue;
            const UnitRect o = detail::BoxToUnit(page.get(), {ox, oy, ox, oy});

            TextChar ch;
            ch.unicode = cp;
            ch.left = static_cast<float>(u.left * out.widthPt);
            ch.right = static_cast<float>(u.right * out.widthPt);
            ch.baseline = static_cast<float>(o.top * out.heightPt);
            // FPDFText_GetFontSize is the nominal Tf size, before the text and
            // CTM matrices. Office exports draw at e.g. 641 Tf under a 0.05
            // scale (32 pt on paper); the vertical scale of the effective
            // matrix turns it into the size actually shown.
            double size = FPDFText_GetFontSize(text.get(), i);
            FS_MATRIX m{};
            if (FPDFText_GetMatrix(text.get(), i, &m)) {
                const double scale = std::sqrt(static_cast<double>(m.c) * m.c +
                                               static_cast<double>(m.d) * m.d);
                if (scale > 0 && std::isfinite(scale)) size *= scale;
            }
            if (!(size > 0 && size < 2000))  // missing or absurd: use the box
                size = (u.bottom - u.top) * out.heightPt;
            ch.fontSize = static_cast<float>(size);
            unsigned int R = 0, G = 0, B = 0, A = 255;
            if (FPDFText_GetFillColor(text.get(), i, &R, &G, &B, &A)) {
                ch.r = static_cast<std::uint8_t>(R);
                ch.g = static_cast<std::uint8_t>(G);
                ch.b = static_cast<std::uint8_t>(B);
            }
            int flags = 0;
            ch.fontName = ReadFontName(text.get(), i, &flags);
            const std::string lower = Lower(ch.fontName);
            ch.weight = WeightOf(text.get(), i, lower);
            ch.italic = (flags & kFontFlagItalic) != 0 ||
                        lower.find("italic") != std::string::npos ||
                        lower.find("oblique") != std::string::npos;
            out.chars.push_back(std::move(ch));
            if (cp != U' ') ++real;
        }
        if (real >= kMinCharsForTextPage) ExtractImages(doc, page.get(), &out);
        if (real < kMinCharsForTextPage) {
            // A scan (or a page that is all drawing): keep it as a picture so
            // nothing visible is lost. Stray characters (OCR-less scans often
            // carry a page number) are dropped with it.
            out.chars.clear();
            text.reset();
            page.reset();
            if (auto s = RenderScan(doc, pageIndex, &out); !s) return s.error();
            TraceF("[PDFTool.Convert] page %d: %d chars, embedded as image (%zu bytes)\n",
                   pageIndex, real, out.pagePng.size());
        }
    } catch (const std::bad_alloc&) {
        TraceF("[PDFTool.Convert] page %d: out of memory extracting text\n", pageIndex);
        return Error{L"内存不足"};
    }
    return out;
}

}  // namespace pdf
