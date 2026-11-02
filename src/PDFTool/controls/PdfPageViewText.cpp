// PdfPageViewText.cpp — search highlights and text selection for PdfPageView.
// Split from PdfPageView.cpp to keep layout/rendering and text features apart.

#include "PdfPageView.h"

#include <PDFCore/Engine.h>
#include <PDFCore/Trace.h>

#include <FluentUI/graphics/PixelSnapping.h>
#include <FluentUI/input/InputManager.h>

#include <algorithm>
#include <cstring>

using namespace fluent;

namespace {
constexpr double kGapDip = 12.0;  // must match PdfPageView.cpp
}

void PdfPageView::AddHighlights(const std::vector<pdf::TextMatch>& matches) {
    for (const pdf::TextMatch& m : matches) highlights_[m.page].push_back(m.units);
    Invalidate();
}

void PdfPageView::ClearHighlights() {
    highlights_.clear();
    currentMatchPage_ = currentMatchIndex_ = -1;
    Invalidate();
}

void PdfPageView::SetCurrentMatch(int page, int matchOnPage) {
    currentMatchPage_ = page;
    currentMatchIndex_ = matchOnPage;
    if (layout_ && page >= 0 && page < layout_->PageCount()) {
        auto it = highlights_.find(page);
        if (it != highlights_.end() && matchOnPage >= 0 &&
            matchOnPage < static_cast<int>(it->second.size()) && !it->second[matchOnPage].empty()) {
            // Bring the match's first line into the middle third of the view if
            // it is not already visible, so the eye does not have to hunt.
            const pdf::PageRectDip& r = layout_->PageRect(page);
            const pdf::UnitRect& u = it->second[matchOnPage].front();
            const double top = r.y + u.top * r.height;
            const double bottom = r.y + u.bottom * r.height;
            if (top < scroll_.Offset() || bottom > scroll_.Offset() + bounds_.h)
                scroll_.SetOffset(static_cast<float>(top - bounds_.h / 3));
            UpdateCurrentPage();
        }
    }
    Invalidate();
}

void PdfPageView::ClearSelection() {
    ++selTicket_;  // results for the old drag are now stale
    // Esc or a new document mid-drag: give the pointer back, or the view keeps
    // capture and swallows the next click elsewhere.
    if (selecting_ && Context().input && Context().input->Captured() == this)
        Context().input->ReleaseCapture(this);
    // selPending_ is left alone: a job may still be in flight, and resetting it
    // would let a second one start. Its result is dropped by the ticket.
    selecting_ = selDirty_ = false;
    selection_ = {};
    Invalidate();
}

bool PdfPageView::CopySelection() const {
    const std::wstring& sel = selection_.text;
    if (sel.empty()) return false;
    // Window from the context, not GetActiveWindow(), as LogViewer does: the
    // clipboard needs a live owner window.
    HWND owner = Context().hwnd ? Context().hwnd : GetActiveWindow();
    const SIZE_T bytes = (sel.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) return false;
    void* dst = GlobalLock(mem);
    if (!dst) { GlobalFree(mem); return false; }
    std::memcpy(dst, sel.c_str(), bytes);
    GlobalUnlock(mem);
    if (!OpenClipboard(owner)) { GlobalFree(mem); return false; }
    EmptyClipboard();
    // The clipboard owns `mem` only on success.
    if (!SetClipboardData(CF_UNICODETEXT, mem)) {
        CloseClipboard();
        GlobalFree(mem);
        return false;
    }
    CloseClipboard();
    pdf::TraceF("[PDFTool.View] copied %zu chars from page %d\n", sel.size(), selection_.page);
    return true;
}

D2D1_RECT_F PdfPageView::PageRectOnScreen(int page, float scale) const {
    const pdf::PageRectDip& r = layout_->PageRect(page);
    const float x0 = SnapToPixel(SnapToPixel(ContentLeft(), scale) + static_cast<float>(r.x), scale);
    const float y0 = SnapToPixel(
        bounds_.y + static_cast<float>(r.y) - SnapToPixel(scroll_.Offset(), scale), scale);
    return D2D1::RectF(x0, y0, x0 + static_cast<float>(r.width), y0 + static_cast<float>(r.height));
}

PdfPageView::PagePoint PdfPageView::HitPagePoint(float x, float y) const {
    PagePoint pt;
    if (!layout_ || layout_->PageCount() == 0) return pt;
    const double contentY = y - bounds_.y + scroll_.Offset();
    const int p = layout_->PageAt(contentY);  // nearest page: drags may leave a page
    if (p < 0) return pt;
    const D2D1_RECT_F rc = PageRectOnScreen(p, Context().dpiScale > 0 ? Context().dpiScale : 1.0f);
    const float w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return pt;
    pt.page = p;
    pt.ux = std::clamp((x - rc.left) / w, 0.0f, 1.0f);
    pt.uy = std::clamp((y - rc.top) / h, 0.0f, 1.0f);
    return pt;
}

void PdfPageView::RequestSelection() {
    // One job in flight: a fast drag would otherwise queue hundreds of them
    // ahead of page renders. Moves during the job just mark selDirty_.
    if (selPending_) { selDirty_ = true; return; }
    if (!engine_ || !app_ || DocumentId() == pdf::kNoDocument || selA_.page < 0) return;
    // A selection stays on the page where the drag started (multi-page text
    // selection is out of scope); the far end is clamped onto that page.
    PagePoint b = selB_;
    if (b.page != selA_.page) {
        b.ux = b.page > selA_.page ? 1.0 : 0.0;
        b.uy = b.page > selA_.page ? 1.0 : 0.0;
    }
    selPending_ = true;
    selDirty_ = false;
    const int ticket = selTicket_;
    const pdf::DocumentId id = DocumentId();
    const PagePoint a = selA_;
    pdf::Engine* engine = engine_;
    Application* app = app_;
    std::weak_ptr<int> alive = alive_;
    PdfPageView* self = this;
    engine_->Post([engine, app, id, a, b, ticket, alive, self] {
        pdf::TextSelection sel;
        if (pdf::Document* doc = engine->Documents().Get(id)) {
            auto r = pdf::SelectText(*doc, a.page, a.ux, a.uy, b.ux, b.uy);
            if (r) sel = std::move(r.value());
        }
        app->Post([sel = std::move(sel), ticket, alive, self]() mutable {
            if (alive.expired()) return;  // view destroyed
            self->OnSelectionResult(std::move(sel), ticket);
        });
    });
}

void PdfPageView::OnSelectionResult(pdf::TextSelection sel, int ticket) {
    selPending_ = false;
    if (ticket != selTicket_) {
        // Stale (cleared, new press), but a newer drag may be waiting on this
        // slot: send it now.
        if (selecting_ && selDirty_) RequestSelection();
        return;
    }
    selection_ = std::move(sel);
    Invalidate();
    if (selDirty_) RequestSelection();  // the pointer moved while we waited
}

void PdfPageView::RenderTextOverlays(const DrawingContext& dc, int first, int last) {
    const auto& colors = Theme().colors;
    const float scale = dc.DpiScale();
    auto fill = [&](int page, const std::vector<pdf::UnitRect>& units, const D2D1_COLOR_F& c) {
        const D2D1_RECT_F pr = PageRectOnScreen(page, scale);
        const float w = pr.right - pr.left, h = pr.bottom - pr.top;
        for (const pdf::UnitRect& u : units) {
            dc.FillRect(D2D1::RectF(pr.left + static_cast<float>(u.left) * w,
                                    pr.top + static_cast<float>(u.top) * h,
                                    pr.left + static_cast<float>(u.right) * w,
                                    pr.top + static_cast<float>(u.bottom) * h),
                        c);
        }
    };
    // Search hits in a warm wash, the current one in the accent at higher
    // alpha; both translucent so the text underneath stays readable.
    const D2D1_COLOR_F hit = WithAlphaScale(colors.severityWarningStroke, 0.35f);
    const D2D1_COLOR_F current = WithAlphaScale(colors.accent, 0.45f);
    for (int p = first; p <= last; ++p) {
        auto it = highlights_.find(p);
        if (it == highlights_.end()) continue;
        for (int i = 0; i < static_cast<int>(it->second.size()); ++i)
            fill(p, it->second[i], p == currentMatchPage_ && i == currentMatchIndex_ ? current : hit);
    }
    if (selection_.count > 0 && selection_.page >= first && selection_.page <= last)
        fill(selection_.page, selection_.units, WithAlphaScale(colors.accent, 0.3f));
}
