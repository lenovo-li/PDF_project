#include "TextSearch.h"

#include "Trace.h"
#include "UnitMapping.h"

#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace pdf {

namespace {
struct PageCloser {
    void operator()(fpdf_page_t__* p) const { FPDF_ClosePage(p); }
};
struct TextPageCloser {
    void operator()(fpdf_textpage_t__* t) const { FPDFText_ClosePage(t); }
};
struct FindCloser {
    void operator()(fpdf_schhandle_t__* h) const { FPDFText_FindClose(h); }
};
using PagePtr = std::unique_ptr<fpdf_page_t__, PageCloser>;
using TextPagePtr = std::unique_ptr<fpdf_textpage_t__, TextPageCloser>;
using FindPtr = std::unique_ptr<fpdf_schhandle_t__, FindCloser>;

// Loads page + text page, children closed before parents by declaration order
// at the call site (text page declared after page, destroyed first).
Status LoadText(const Document& doc, int index, PagePtr& page, TextPagePtr& text) {
    if (index < 0 || index >= doc.PageCount()) return Error{L"页码超出范围", FPDF_ERR_PAGE};
    page.reset(FPDF_LoadPage(doc.Raw(), index));
    if (!page) {
        const unsigned long code = FPDF_GetLastError();
        TraceF("[PDFTool.Search] FPDF_LoadPage failed, page=%d, err=%lu\n", index, code);
        return Error{L"无法加载页面", code};
    }
    text.reset(FPDFText_LoadPage(page.get()));
    if (!text) {
        TraceF("[PDFTool.Search] FPDFText_LoadPage failed, page=%d\n", index);
        return Error{L"无法读取页面文字"};
    }
    return Ok{};
}
using detail::BoxToUnit;

// Rectangles covering characters [first, first + count), merged per line.
// Rectangles that are empty after clamping (text outside the CropBox) are
// dropped. Returns false if `maxRects` cut the list short.
bool CollectRects(fpdf_page_t__* page, fpdf_textpage_t__* text, int first, int count,
                  std::vector<BoxPt>* pts, std::vector<UnitRect>* units, int maxRects) {
    const int n = FPDFText_CountRects(text, first, count);
    for (int r = 0; r < n && r < maxRects; ++r) {
        double left = 0, top = 0, right = 0, bottom = 0;
        if (!FPDFText_GetRect(text, r, &left, &top, &right, &bottom)) continue;
        const BoxPt b{left, bottom, right, top};
        const UnitRect u = BoxToUnit(page, b);
        if (u.right <= u.left || u.bottom <= u.top) continue;
        if (pts) pts->push_back(b);
        if (units) units->push_back(u);
    }
    return n <= maxRects;
}
}  // namespace

Result<PageSearchResult> SearchPage(const Document& doc, int index, const std::wstring& query,
                                    const SearchOptions& options) {
    if (query.empty()) return Error{L"搜索内容为空"};
    if (query.size() > kMaxQueryChars) return Error{L"搜索内容过长"};

    PagePtr page;
    TextPagePtr text;  // after page: destroyed first
    if (auto s = LoadText(doc, index, page, text); !s) return s.error();

    PageSearchResult out;
    if (FPDFText_CountChars(text.get()) <= 0) return out;  // no text layer

    // FPDF_WIDESTRING is UTF-16LE, NUL-terminated; wchar_t is UTF-16 on Windows.
    static_assert(sizeof(wchar_t) == sizeof(unsigned short));
    unsigned long flags = 0;
    if (options.matchCase) flags |= FPDF_MATCHCASE;
    if (options.wholeWord) flags |= FPDF_MATCHWHOLEWORD;
    FindPtr find(FPDFText_FindStart(text.get(),
                                    reinterpret_cast<FPDF_WIDESTRING>(query.c_str()), flags, 0));
    if (!find) return out;

    while (FPDFText_FindNext(find.get())) {
        if (static_cast<int>(out.matches.size()) >= kMaxMatchesPerPage) {
            out.truncated = true;
            TraceF("[PDFTool.Search] page %d: match cap %d reached\n", index, kMaxMatchesPerPage);
            break;
        }
        TextMatch m;
        m.page = index;
        m.charIndex = FPDFText_GetSchResultIndex(find.get());
        m.charCount = FPDFText_GetSchCount(find.get());
        if (m.charIndex < 0 || m.charCount <= 0) continue;
        CollectRects(page.get(), text.get(), m.charIndex, m.charCount, &m.rects, &m.units,
                     kMaxRectsPerMatch);
        out.matches.push_back(std::move(m));
    }
    return out;
}

Result<std::wstring> PageText(const Document& doc, int index, int first, int count) {
    PagePtr page;
    TextPagePtr text;
    if (auto s = LoadText(doc, index, page, text); !s) return s.error();
    const int total = FPDFText_CountChars(text.get());
    if (total <= 0) return std::wstring{};
    if (first < 0 || first >= total) return Error{L"文字位置超出范围"};
    int n = (count < 0 || count > total - first) ? total - first : count;
    if (n > kMaxCopyChars) n = kMaxCopyChars;
    // Size-then-fill: the buffer holds n UTF-16 units plus the terminator.
    std::wstring buf(static_cast<std::size_t>(n) + 1, L'\0');
    const int written = FPDFText_GetText(text.get(), first, n,
                                         reinterpret_cast<unsigned short*>(buf.data()));
    buf.resize(written > 0 ? static_cast<std::size_t>(written) - 1 : 0);  // drop NUL
    return buf;
}

Result<TextSelection> SelectText(const Document& doc, int index, double ax, double ay, double bx,
                                 double by) {
    for (double v : {ax, ay, bx, by})
        if (!std::isfinite(v)) return Error{L"选择位置无效"};
    PagePtr page;
    TextPagePtr text;
    if (auto s = LoadText(doc, index, page, text); !s) return s.error();
    TextSelection sel;
    sel.page = index;
    const int total = FPDFText_CountChars(text.get());
    if (total <= 0) return sel;

    // Unit point -> PDF user space with the inverse of BoxToUnit's mapping,
    // then the character there. The tolerance widens step by step so a drag
    // that starts in the margin or the space between lines still lands on text.
    auto charAt = [&](double ux, double uy) {
        PointF p;
        if (!detail::UnitToPoint(page.get(), ux, uy, &p)) return -1;
        for (double tol : {6.0, 24.0, 96.0}) {
            const int i = FPDFText_GetCharIndexAtPos(text.get(), p.x, p.y, tol, tol);
            if (i >= 0) return i;
        }
        return -1;
    };
    int a = charAt(ax, ay);
    int b = charAt(bx, by);
    if (a < 0 && b < 0) return sel;
    // One end far from any text (a drag past the last line, or from the top
    // margin): snap it to the page's start or end in the drag's direction, so
    // the selection grows instead of collapsing to one character.
    const bool forward = by > ay || (by == ay && bx >= ax);
    if (a < 0) a = forward ? 0 : total - 1;
    if (b < 0) b = forward ? total - 1 : 0;
    sel.first = std::min(a, b);
    sel.count = std::max(a, b) - sel.first + 1;
    CollectRects(page.get(), text.get(), sel.first, sel.count, nullptr, &sel.units,
                 kMaxRectsPerSelection);

    const int n = std::min(sel.count, kMaxCopyChars);
    std::wstring buf(static_cast<std::size_t>(n) + 1, L'\0');
    const int written = FPDFText_GetText(text.get(), sel.first, n,
                                         reinterpret_cast<unsigned short*>(buf.data()));
    buf.resize(written > 0 ? static_cast<std::size_t>(written) - 1 : 0);
    sel.text = std::move(buf);
    return sel;
}

}  // namespace pdf
