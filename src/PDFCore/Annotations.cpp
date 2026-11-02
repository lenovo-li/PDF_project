#include "Annotations.h"

#include "Trace.h"
#include "UnitMapping.h"

#include <fpdf_annot.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <new>

namespace pdf {

namespace {
struct PageCloser {
    void operator()(fpdf_page_t__* p) const { FPDF_ClosePage(p); }
};
struct AnnotCloser {
    void operator()(fpdf_annotation_t__* a) const { FPDFPage_CloseAnnot(a); }
};
struct TextPageCloser {
    void operator()(fpdf_textpage_t__* t) const { FPDFText_ClosePage(t); }
};
using PagePtr = std::unique_ptr<fpdf_page_t__, PageCloser>;
using AnnotPtr = std::unique_ptr<fpdf_annotation_t__, AnnotCloser>;
using TextPagePtr = std::unique_ptr<fpdf_textpage_t__, TextPageCloser>;

Result<PagePtr> LoadPage(const Document& doc, int index) {
    if (index < 0 || index >= doc.PageCount()) return Error{L"页码超出范围", FPDF_ERR_PAGE};
    PagePtr page(FPDF_LoadPage(doc.Raw(), index));
    if (!page) return Error{L"无法加载页面", FPDF_GetLastError()};
    return page;
}

AnnotKind KindOf(FPDF_ANNOTATION_SUBTYPE s) {
    switch (s) {
        case FPDF_ANNOT_HIGHLIGHT: return AnnotKind::Highlight;
        case FPDF_ANNOT_UNDERLINE: return AnnotKind::Underline;
        case FPDF_ANNOT_STRIKEOUT: return AnnotKind::StrikeOut;
        case FPDF_ANNOT_TEXT:      return AnnotKind::Note;
        case FPDF_ANNOT_FREETEXT:  return AnnotKind::FreeText;
        case FPDF_ANNOT_SQUARE:    return AnnotKind::Square;
        case FPDF_ANNOT_INK:       return AnnotKind::Ink;
        default:                   return AnnotKind::Other;
    }
}

bool FiniteBox(const BoxPt& b) {
    return std::isfinite(b.left) && std::isfinite(b.right) && std::isfinite(b.bottom) &&
           std::isfinite(b.top) && b.right > b.left && b.top > b.bottom;
}

FS_RECTF ToFsRect(const BoxPt& b) {
    return {static_cast<float>(b.left), static_cast<float>(b.top), static_cast<float>(b.right),
            static_cast<float>(b.bottom)};
}

BoxPt FromFsRect(const FS_RECTF& r) {
    return {r.left, r.bottom, r.right, r.top};
}

// /Contents through the size-then-fill protocol; capped, never trusted.
std::wstring ReadContents(fpdf_annotation_t__* a) {
    const unsigned long bytes = FPDFAnnot_GetStringValue(a, "Contents", nullptr, 0);
    if (bytes < 2 || bytes > (kMaxAnnotTextChars + 1) * 2) return {};
    std::wstring buf(bytes / 2, L'\0');
    FPDFAnnot_GetStringValue(a, "Contents", reinterpret_cast<FPDF_WCHAR*>(buf.data()), bytes);
    buf.resize(bytes / 2 - 1);  // drop the terminator
    return buf;
}

// Creates an annotation with the common fields set; the caller adds the rest.
// Free text gets no /C: for that subtype PDFium's appearance generator fills
// the whole box with /C as a background, which hides text of the same colour.
// Its text colour comes from /DA instead.
Result<AnnotPtr> Create(fpdf_page_t__* page, FPDF_ANNOTATION_SUBTYPE type, AnnotColor c) {
    AnnotPtr a(FPDFPage_CreateAnnot(page, type));
    if (!a) {
        TraceF("[PDFTool.Annot] FPDFPage_CreateAnnot(%d) failed\n", type);
        return Error{L"无法创建注释"};
    }
    if (type != FPDF_ANNOT_FREETEXT &&
        !FPDFAnnot_SetColor(a.get(), FPDFANNOT_COLORTYPE_Color, c.r, c.g, c.b, c.a)) {
        TraceF("[PDFTool.Annot] FPDFAnnot_SetColor failed\n");
        return Error{L"无法设置颜色"};
    }
    FPDFAnnot_SetFlags(a.get(), FPDF_ANNOT_FLAG_PRINT);  // print it, like Acrobat does
    return a;
}

// Removes the annotation that `a` points to from `page`. The handle becomes
// invalid after removal. Returns the removed annotation's index before removal.
int Discard(fpdf_page_t__* page, fpdf_annotation_t__* a) {
    const int idx = FPDFPage_GetAnnotIndex(page, a);
    if (idx >= 0 && idx < FPDFPage_GetAnnotCount(page)) FPDFPage_RemoveAnnot(page, idx);
    return idx;
}
}  // namespace

Result<AnnotList> ListAnnotations(const Document& doc, int index) {
    try {
        auto page = LoadPage(doc, index);
        if (!page) return page.error();
        AnnotList out;
        const int n = FPDFPage_GetAnnotCount(page.value().get());
        std::size_t totalBytes = 0;
        for (int i = 0; i < n; ++i) {
            if (static_cast<int>(out.annots.size()) >= kMaxAnnotsListed) {
                out.truncated = true;
                break;
            }
            if (totalBytes >= kMaxAnnotTextBytesPerPage) {
                out.truncated = true;
                break;
            }
            AnnotPtr a(FPDFPage_GetAnnot(page.value().get(), i));
            if (!a) continue;
            AnnotInfo info;
            info.index = i;
            info.kind = KindOf(FPDFAnnot_GetSubtype(a.get()));
            FS_RECTF r{};
            if (FPDFAnnot_GetRect(a.get(), &r)) {
                info.rect = FromFsRect(r);
                info.unitRect = detail::BoxToUnit(page.value().get(), info.rect);
            }
            unsigned int R = 0, G = 0, B = 0, A = 255;
            if (FPDFAnnot_GetColor(a.get(), FPDFANNOT_COLORTYPE_Color, &R, &G, &B, &A))
                info.color = {static_cast<std::uint8_t>(R), static_cast<std::uint8_t>(G),
                              static_cast<std::uint8_t>(B), static_cast<std::uint8_t>(A)};
            info.contents = ReadContents(a.get());
            totalBytes += info.contents.size() * sizeof(wchar_t);
            out.annots.push_back(std::move(info));
        }
        return out;
    } catch (const std::bad_alloc&) {
        TraceF("[PDFTool.Annot] ListAnnotations out of memory, page=%d\n", index);
        return Error{L"内存不足", 0};
    }
}

Result<BoxPt> UnitRectToPage(const Document& doc, int index, const UnitRect& u) {
    auto page = LoadPage(doc, index);
    if (!page) return page.error();
    PointF tl, br;
    if (!detail::UnitToPoint(page.value().get(), u.left, u.top, &tl) ||
        !detail::UnitToPoint(page.value().get(), u.right, u.bottom, &br))
        return Error{L"坐标换算失败"};
    // UnitRect is top-left origin; BoxPt is bottom-left, so swap y.
    return BoxPt{std::min(tl.x, br.x), std::min(tl.y, br.y), std::max(tl.x, br.x),
                 std::max(tl.y, br.y)};
}

Result<std::vector<PointF>> UnitPointsToPage(const Document& doc, int index,
                                             const std::vector<PointF>& units) {
    auto page = LoadPage(doc, index);
    if (!page) return page.error();
    std::vector<PointF> pts;
    pts.reserve(units.size());
    for (const PointF& u : units) {
        PointF p;
        if (!detail::UnitToPoint(page.value().get(), u.x, u.y, &p))
            return Error{L"坐标换算失败"};
        pts.push_back(p);
    }
    return pts;
}

Status AddMarkup(Document& doc, int index, AnnotKind kind, const std::vector<BoxPt>& rects,
                 AnnotColor color) {
    FPDF_ANNOTATION_SUBTYPE type = 0;
    switch (kind) {
        case AnnotKind::Highlight: type = FPDF_ANNOT_HIGHLIGHT; break;
        case AnnotKind::Underline: type = FPDF_ANNOT_UNDERLINE; break;
        case AnnotKind::StrikeOut: type = FPDF_ANNOT_STRIKEOUT; break;
        default: return Error{L"不是文字标记类型"};
    }
    if (rects.empty() || rects.size() > 2048) return Error{L"标记范围无效"};
    for (const BoxPt& b : rects)
        if (!FiniteBox(b)) return Error{L"标记范围无效"};
    auto page = LoadPage(doc, index);
    if (!page) return page.error();
    auto a = Create(page.value().get(), type, color);
    if (!a) return a.error();
    BoxPt bound{1e30, 1e30, -1e30, -1e30};
    for (const BoxPt& b : rects) {
        // QuadPoints order used by Acrobat and PDFium's generator:
        // top-left, top-right, bottom-left, bottom-right.
        const FS_QUADPOINTSF q{static_cast<float>(b.left),  static_cast<float>(b.top),
                               static_cast<float>(b.right), static_cast<float>(b.top),
                               static_cast<float>(b.left),  static_cast<float>(b.bottom),
                               static_cast<float>(b.right), static_cast<float>(b.bottom)};
        if (!FPDFAnnot_AppendAttachmentPoints(a.value().get(), &q)) {
            Discard(page.value().get(), a.value().get());
            return Error{L"无法设置标记位置"};
        }
        bound = {std::min(bound.left, b.left), std::min(bound.bottom, b.bottom),
                 std::max(bound.right, b.right), std::max(bound.top, b.top)};
    }
    const FS_RECTF r = ToFsRect(bound);
    if (!FPDFAnnot_SetRect(a.value().get(), &r)) {
        Discard(page.value().get(), a.value().get());
        return Error{L"无法设置标记矩形"};
    }
    return Ok{};
}

Status AddMarkupForChars(Document& doc, int index, AnnotKind kind, int first, int count,
                         AnnotColor color) {
    if (first < 0 || count <= 0 || count > 100000) return Error{L"字符范围无效"};
    auto page = LoadPage(doc, index);
    if (!page) return page.error();
    TextPagePtr text(FPDFText_LoadPage(page.value().get()));
    if (!text) return Error{L"无法读取页面文字"};
    const int total = FPDFText_CountChars(text.get());
    if (first >= total) return Error{L"字符超出范围"};
    const int actualCount = std::min(count, total - first);
    const int n = FPDFText_CountRects(text.get(), first, actualCount);
    if (n <= 0) return Error{L"选区无矩形"};
    if (n > 2048) return Error{L"选区矩形过多"};
    std::vector<BoxPt> rects;
    rects.reserve(n);
    for (int r = 0; r < n; ++r) {
        double left = 0, top = 0, right = 0, bottom = 0;
        if (FPDFText_GetRect(text.get(), r, &left, &top, &right, &bottom))
            rects.push_back({left, bottom, right, top});
    }
    if (rects.empty()) return Error{L"选区无有效矩形"};
    return AddMarkup(doc, index, kind, rects, color);
}

Status AddNote(Document& doc, int index, PointF at, const std::wstring& text, AnnotColor color) {
    if (!std::isfinite(at.x) || !std::isfinite(at.y)) return Error{L"位置无效"};
    if (text.size() > kMaxAnnotTextChars) return Error{L"文字过长"};
    auto page = LoadPage(doc, index);
    if (!page) return page.error();
    auto a = Create(page.value().get(), FPDF_ANNOT_TEXT, color);
    if (!a) return a.error();
    const FS_RECTF r{static_cast<float>(at.x), static_cast<float>(at.y + 20),
                     static_cast<float>(at.x + 20), static_cast<float>(at.y)};  // 20 pt icon
    if (!FPDFAnnot_SetRect(a.value().get(), &r)) {
        Discard(page.value().get(), a.value().get());
        return Error{L"无法设置位置"};
    }
    if (!FPDFAnnot_SetStringValue(a.value().get(), "Contents",
                                   reinterpret_cast<FPDF_WIDESTRING>(text.c_str()))) {
        Discard(page.value().get(), a.value().get());
        return Error{L"无法设置文字"};
    }
    return Ok{};
}

Status AddFreeText(Document& doc, int index, const BoxPt& rect, const std::wstring& text,
                   AnnotColor color) {
    if (!FiniteBox(rect)) return Error{L"范围无效"};
    if (text.empty() || text.size() > kMaxAnnotTextChars) return Error{L"文字为空或过长"};
    auto page = LoadPage(doc, index);
    if (!page) return page.error();
    auto a = Create(page.value().get(), FPDF_ANNOT_FREETEXT, color);
    if (!a) return a.error();
    const FS_RECTF r = ToFsRect(rect);
    if (!FPDFAnnot_SetRect(a.value().get(), &r)) {
        Discard(page.value().get(), a.value().get());
        return Error{L"无法设置范围"};
    }
    if (!FPDFAnnot_SetStringValue(a.value().get(), "Contents",
                                   reinterpret_cast<FPDF_WIDESTRING>(text.c_str()))) {
        Discard(page.value().get(), a.value().get());
        return Error{L"无法设置文字"};
    }
    // /DA (default appearance): 12 pt Helvetica in the annotation colour.
    const std::wstring da = L"/Helv 12 Tf " + std::to_wstring(color.r / 255.0) + L" " +
                            std::to_wstring(color.g / 255.0) + L" " +
                            std::to_wstring(color.b / 255.0) + L" rg";
    FPDFAnnot_SetStringValue(a.value().get(), "DA", reinterpret_cast<FPDF_WIDESTRING>(da.c_str()));
    return Ok{};
}

Status AddSquare(Document& doc, int index, const BoxPt& rect, AnnotColor color,
                 float borderWidth) {
    if (!FiniteBox(rect) || !(borderWidth > 0 && borderWidth <= 50)) return Error{L"参数无效"};
    auto page = LoadPage(doc, index);
    if (!page) return page.error();
    auto a = Create(page.value().get(), FPDF_ANNOT_SQUARE, color);
    if (!a) return a.error();
    const FS_RECTF r = ToFsRect(rect);
    if (!FPDFAnnot_SetRect(a.value().get(), &r)) {
        Discard(page.value().get(), a.value().get());
        return Error{L"无法设置范围"};
    }
    if (!FPDFAnnot_SetBorder(a.value().get(), 0, 0, borderWidth)) {
        Discard(page.value().get(), a.value().get());
        return Error{L"无法设置边框"};
    }
    return Ok{};
}

Status AddInk(Document& doc, int index, const std::vector<std::vector<PointF>>& strokes,
              AnnotColor color, float borderWidth) {
    if (strokes.empty() || !(borderWidth > 0 && borderWidth <= 50)) return Error{L"参数无效"};
    std::size_t total = 0;
    for (const auto& s : strokes) total += s.size();
    if (total == 0 || total > static_cast<std::size_t>(kMaxInkPoints))
        return Error{L"笔迹点数无效"};
    for (const auto& s : strokes)
        for (const PointF& p : s)
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) return Error{L"笔迹点无效"};
    auto page = LoadPage(doc, index);
    if (!page) return page.error();
    auto a = Create(page.value().get(), FPDF_ANNOT_INK, color);
    if (!a) return a.error();
    BoxPt bound{1e30, 1e30, -1e30, -1e30};
    for (const auto& s : strokes) {
        if (s.empty()) continue;
        std::vector<FS_POINTF> pts;
        pts.reserve(s.size());
        for (const PointF& p : s) {
            pts.push_back({static_cast<float>(p.x), static_cast<float>(p.y)});
            bound = {std::min(bound.left, p.x), std::min(bound.bottom, p.y),
                     std::max(bound.right, p.x), std::max(bound.top, p.y)};
        }
        if (FPDFAnnot_AddInkStroke(a.value().get(), pts.data(), pts.size()) < 0) {
            Discard(page.value().get(), a.value().get());
            return Error{L"无法添加笔迹"};
        }
    }
    // The stroke has width: pad the rect so the line is not clipped.
    const double pad = borderWidth;
    const FS_RECTF r =
        ToFsRect({bound.left - pad, bound.bottom - pad, bound.right + pad, bound.top + pad});
    if (!FPDFAnnot_SetRect(a.value().get(), &r)) {
        Discard(page.value().get(), a.value().get());
        return Error{L"无法设置范围"};
    }
    if (!FPDFAnnot_SetBorder(a.value().get(), 0, 0, borderWidth)) {
        Discard(page.value().get(), a.value().get());
        return Error{L"无法设置边框"};
    }
    return Ok{};
}

Status SetAnnotationText(Document& doc, int index, int annotIndex, const std::wstring& text) {
    if (text.size() > kMaxAnnotTextChars) return Error{L"文字过长"};
    auto page = LoadPage(doc, index);
    if (!page) return page.error();
    if (annotIndex < 0 || annotIndex >= FPDFPage_GetAnnotCount(page.value().get()))
        return Error{L"注释不存在"};
    AnnotPtr a(FPDFPage_GetAnnot(page.value().get(), annotIndex));
    if (!a) return Error{L"无法获取注释"};
    const AnnotKind kind = KindOf(FPDFAnnot_GetSubtype(a.get()));
    if (kind != AnnotKind::Note && kind != AnnotKind::FreeText)
        return Error{L"此类型注释不能编辑文字"};
    if (!FPDFAnnot_SetStringValue(a.get(), "Contents",
                                   reinterpret_cast<FPDF_WIDESTRING>(text.c_str())))
        return Error{L"无法设置文字"};
    // PDFium draws an existing appearance stream as-is and only generates one
    // when /AP is missing, so the old words would stay on the page. Clearing
    // Normal clears every mode.
    if (kind == AnnotKind::FreeText &&
        !FPDFAnnot_SetAP(a.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr))
        TraceF("[PDFTool.Annot] FPDFAnnot_SetAP(clear) failed, page=%d\n", index);
    return Ok{};
}

Status RemoveAnnotation(Document& doc, int index, int annotIndex) {
    auto page = LoadPage(doc, index);
    if (!page) return page.error();
    const int n = FPDFPage_GetAnnotCount(page.value().get());
    if (annotIndex < 0 || annotIndex >= n) return Error{L"注释不存在"};
    AnnotPtr a(FPDFPage_GetAnnot(page.value().get(), annotIndex));
    if (!a) return Error{L"无法获取注释"};
    const AnnotKind kind = KindOf(FPDFAnnot_GetSubtype(a.get()));
    if (!IsEditableKind(kind)) return Error{L"此类型注释不能删除"};
    // A note's /Popup is its own entry in /Annots; left behind it would be an
    // orphan. Find it before removing anything, while indices are valid.
    int popupIdx = -1;
    if (kind == AnnotKind::Note) {
        AnnotPtr popup(FPDFAnnot_GetLinkedAnnot(a.get(), "Popup"));
        // A hostile /Popup may point at a link or form widget; only a real
        // popup goes with its note.
        if (popup && FPDFAnnot_GetSubtype(popup.get()) == FPDF_ANNOT_POPUP)
            popupIdx = FPDFPage_GetAnnotIndex(page.value().get(), popup.get());
        if (popupIdx == annotIndex) popupIdx = -1;  // malformed self-link
    }
    a.reset();  // close handles before the entries they point at go away
    // Remove the higher index first, so the lower one does not shift.
    const int hi = std::max(annotIndex, popupIdx), lo = std::min(annotIndex, popupIdx);
    if (!FPDFPage_RemoveAnnot(page.value().get(), hi)) {
        TraceF("[PDFTool.Annot] FPDFPage_RemoveAnnot(%d) failed, page=%d\n", hi, index);
        return Error{L"删除注释失败"};
    }
    if (lo >= 0 && !FPDFPage_RemoveAnnot(page.value().get(), lo)) {
        TraceF("[PDFTool.Annot] FPDFPage_RemoveAnnot(%d) failed, page=%d\n", lo, index);
        return Error{L"删除注释失败"};
    }
    return Ok{};
}

}  // namespace pdf
