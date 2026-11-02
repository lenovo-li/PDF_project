#include "PageOps.h"

#include "Trace.h"

#include <fpdf_edit.h>
#include <fpdf_ppo.h>
#include <fpdfview.h>

#include <algorithm>
#include <functional>
#include <unordered_set>

namespace pdf {

bool ValidPageList(const std::vector<int>& pages, int pageCount) {
    if (pages.empty() || static_cast<int>(pages.size()) > pageCount) return false;
    std::unordered_set<int> seen;
    seen.reserve(pages.size());
    for (int p : pages) {
        if (p < 0 || p >= pageCount || !seen.insert(p).second) return false;
    }
    return true;
}

Status RotatePages(Document& doc, const std::vector<int>& pages, int quarterTurns) {
    if (!ValidPageList(pages, doc.PageCount())) return Error{L"页码无效"};
    for (int p : pages) {
        auto g = doc.PageGeometryAt(p);
        if (!g) return g.error();
        if (auto s = doc.SetPageRotation(p, g.value().rotation + quarterTurns * 90); !s) return s;
    }
    return Ok{};
}

Status DeletePages(Document& doc, std::vector<int> pages) {
    const int n = doc.PageCount();
    if (!ValidPageList(pages, n)) return Error{L"页码无效"};
    if (static_cast<int>(pages.size()) >= n) return Error{L"不能删除全部页面"};
    // Highest index first, so earlier deletions do not shift later ones.
    std::sort(pages.begin(), pages.end(), std::greater<>());
    for (int p : pages) FPDFPage_Delete(doc.Raw(), p);
    TraceF("[PDFTool.PageOps] deleted %zu page(s), %d left\n", pages.size(), doc.PageCount());
    return Ok{};
}

Status InsertBlankPage(Document& doc, int index, double widthPt, double heightPt) {
    if (index < 0 || index > doc.PageCount()) return Error{L"插入位置无效"};
    if (!(widthPt > 0 && heightPt > 0 && widthPt <= 14400 && heightPt <= 14400))
        return Error{L"页面尺寸无效"};
    FPDF_PAGE page = FPDFPage_New(doc.Raw(), index, widthPt, heightPt);
    if (!page) return Error{L"无法新建页面"};
    FPDF_ClosePage(page);
    return Ok{};
}

Status MovePages(Document& doc, const std::vector<int>& pages, int destIndex) {
    const int n = doc.PageCount();
    if (!ValidPageList(pages, n)) return Error{L"页码无效"};
    // PDFium requires destIndex + count <= pageCount; checking it here is what
    // keeps a failed move from leaving the document half-moved.
    if (destIndex < 0 || destIndex + static_cast<int>(pages.size()) > n)
        return Error{L"移动目标位置无效"};
    if (!FPDF_MovePages(doc.Raw(), pages.data(), static_cast<unsigned long>(pages.size()),
                        destIndex)) {
        TraceF("[PDFTool.PageOps] FPDF_MovePages failed (%zu pages -> %d)\n", pages.size(),
               destIndex);
        return Error{L"移动页面失败"};
    }
    return Ok{};
}

Status ImportPages(Document& dest, const Document& src, const std::vector<int>& srcPages,
                   int index) {
    if (index < 0 || index > dest.PageCount()) return Error{L"插入位置无效"};
    if (!srcPages.empty() && !ValidPageList(srcPages, src.PageCount())) return Error{L"页码无效"};
    if (dest.PageCount() + (srcPages.empty() ? src.PageCount() : static_cast<int>(srcPages.size())) >
        kMaxPages)
        return Error{L"页数超出上限"};
    const BOOL ok = FPDF_ImportPagesByIndex(
        dest.Raw(), src.Raw(), srcPages.empty() ? nullptr : srcPages.data(),
        static_cast<unsigned long>(srcPages.size()), index);
    if (!ok) {
        TraceF("[PDFTool.PageOps] FPDF_ImportPagesByIndex failed, err=%lu\n", FPDF_GetLastError());
        return Error{L"插入页面失败"};
    }
    return Ok{};
}

}  // namespace pdf
