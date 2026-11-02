#include "PdfPageView.h"

#include <PDFCore/Document.h>
#include <PDFCore/Engine.h>
#include <PDFCore/PageGeometry.h>
#include <PDFCore/Trace.h>

#include <FluentUI/graphics/D2DContext.h>
#include <FluentUI/graphics/PixelSnapping.h>
#include <FluentUI/input/InputManager.h>
#include <FluentUI/window/WindowServices.h>

#include <algorithm>
#include <cmath>

using namespace fluent;

namespace {
constexpr double kGapDip = 12.0;
// CPU cache budget: about a dozen full-screen pages at 200 % display scale.
constexpr std::uint64_t kCacheBytes = 192ull * 1024 * 1024;
// Per-page render budget: a single page never costs more than this, even at
// 800 % zoom; past it the bitmap is stretched (slightly soft, never blank).
constexpr std::uint64_t kPageBytes = 64ull * 1024 * 1024;
}  // namespace

PdfPageView::PdfPageView(pdf::Engine* engine, Application* app)
    // `this` outlives pages_ (a member), so the callback never dangles; it
    // fires only on the UI thread.
    : pages_(engine, app, kCacheBytes, kPageBytes, [this](int) { Invalidate(); }),
      engine_(engine),
      app_(app) {
    scroll_.SetKeepVisibleWhenOverflow(true);
    SetFocusable(true);
}

PdfPageView::~PdfPageView() = default;

void PdfPageView::SetDocument(pdf::DocumentId id,
                              const std::vector<std::pair<double, double>>& sizesPt) {
    ClearDocument();
    pages_.SetDocument(id);
    sizesDip_.reserve(sizesPt.size());
    for (const auto& [w, h] : sizesPt)
        sizesDip_.emplace_back(w * pdf::kDipPerPoint, h * pdf::kDipPerPoint);
    pdf::TraceF("[PDFTool.View] document set: id=%llu, %zu pages\n",
                static_cast<unsigned long long>(id), sizesDip_.size());
    RebuildLayout(false);
    scroll_.SetOffset(0);
    scroll_.SetOffsetX(0);
    UpdateCurrentPage();
    Invalidate();
}

void PdfPageView::ClearDocument() {
    pages_.SetDocument(pdf::kNoDocument);  // drops in-flight and queued work
    ClearHighlights();
    ClearSelection();
    ResetAnnotations();
    sizesDip_.clear();
    layout_.reset();
    UpdateExtents();
    UpdateCurrentPage();
    Invalidate();
}

void PdfPageView::SetZoom(double zoom) {
    if (!std::isfinite(zoom)) return;
    zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    if (std::fabs(zoom - zoom_) < 1e-6) return;
    zoom_ = zoom;
    pages_.InvalidateSizes();  // new pixel sizes: skip queued renders, retry failures
    RebuildLayout(true);
    UpdateCurrentPage(/*force=*/true);  // zoom is part of the event payload
    Invalidate();
}

void PdfPageView::FitWidth() {
    double widest = 0;
    for (const auto& [w, h] : sizesDip_) widest = std::max(widest, w);
    const double avail = bounds_.w - 2 * kGapDip - 16.0;  // leave room for the bar
    if (widest > 0 && avail > 0) SetZoom(avail / widest);
}

void PdfPageView::ScrollToPage(int index) {
    if (!layout_ || index < 0 || index >= layout_->PageCount()) return;
    scroll_.SetOffset(static_cast<float>(layout_->PageRect(index).y - kGapDip));
    // An explicit jump makes the target the current page, even when the
    // 30 %-line rule would pick a neighbour: near the end the offset clamps,
    // and short pages put several on that line. Clicking thumbnail 5 must not
    // highlight 4. Normal scrolling goes back to the 30 % rule.
    if (index != currentPage_) {
        currentPage_ = index;
        PageViewChangedArgs args;
        args.currentPage = currentPage_;
        args.pageCount = PageCount();
        args.zoom = zoom_;
        viewChanged_.Raise(*this, args);
    }
    Invalidate();
}

void PdfPageView::RebuildLayout(bool keepAnchor) {
    // Anchor: the page at the top of the viewport and how far into it we are,
    // so zooming keeps the same content in view instead of jumping.
    int anchorPage = -1;
    double anchorFrac = 0;
    if (keepAnchor && layout_ && layout_->PageCount() > 0) {
        anchorPage = layout_->PageAt(scroll_.Offset());
        const pdf::PageRectDip& r = layout_->PageRect(anchorPage);
        if (r.height > 0) anchorFrac = (scroll_.Offset() - r.y) / r.height;
    }
    if (sizesDip_.empty()) {
        layout_.reset();
    } else {
        layout_.emplace(sizesDip_, zoom_, kGapDip);
    }
    UpdateExtents();
    if (anchorPage >= 0 && layout_) {
        const pdf::PageRectDip& r = layout_->PageRect(anchorPage);
        scroll_.SetOffset(static_cast<float>(r.y + anchorFrac * r.height));
    }
}

void PdfPageView::UpdateExtents() {
    scroll_.SetBounds(bounds_);
    scroll_.SetContentHeight(layout_ ? static_cast<float>(layout_->ContentHeight()) : 0.0f);
    scroll_.SetContentWidth(layout_ ? static_cast<float>(layout_->ContentWidth()) : 0.0f);
}

void PdfPageView::UpdateCurrentPage(bool force) {
    // The page under a line 30 % down the viewport reads as "current" better
    // than the one barely peeking in at the top.
    const int page = (layout_ && layout_->PageCount() > 0)
                         ? layout_->PageAt(scroll_.Offset() + bounds_.h * 0.3)
                         : -1;
    if (page == currentPage_ && !force) return;
    currentPage_ = page;
    PageViewChangedArgs args;
    args.currentPage = currentPage_;
    args.pageCount = PageCount();
    args.zoom = zoom_;
    viewChanged_.Raise(*this, args);
}

float PdfPageView::ContentLeft() const {
    const float contentW = layout_ ? static_cast<float>(layout_->ContentWidth()) : 0.0f;
    // Narrower than the viewport: centre. Wider: scroll horizontally.
    if (contentW <= bounds_.w) return bounds_.x + (bounds_.w - contentW) * 0.5f;
    return bounds_.x - scroll_.OffsetX();
}

void PdfPageView::Measure(float availW, float availH) {
    // Fill whatever the parent offers; the content scrolls inside.
    FrameworkElement::Measure(availW, availH);
}

void PdfPageView::OnBoundsChanged() {
    UpdateExtents();
    UpdateCurrentPage();
}

void PdfPageView::OnAttachedToTree() { scroll_.AttachToContext(Context()); }
void PdfPageView::OnDetachedFromTree() { scroll_.DetachFromContext(); }

void PdfPageView::OnDpiChanged(float dpiScale) {
    // Pixel sizes depend on DPI, so every visible page now wants a new key.
    // Old D2D bitmaps stay and are stretched until the new ones arrive.
    pdf::TraceF("[PDFTool.View] DPI scale now %.2f\n", dpiScale);
    pages_.InvalidateSizes();
    Invalidate();
}

void PdfPageView::OnDeviceLost() { pages_.OnDeviceLost(); }

void PdfPageView::OnDeviceRestored() {
    pages_.OnDeviceRestored();
    Invalidate();
}

// --- Rendering ---------------------------------------------------------------

void PdfPageView::Render(const DrawingContext& dc) {
    const auto& colors = Theme().colors;
    auto clip = dc.PushClip(D2D1::RectF(bounds_.x, bounds_.y, bounds_.x + bounds_.w,
                                        bounds_.y + bounds_.h));
    dc.FillRect(D2D1::RectF(bounds_.x, bounds_.y, bounds_.x + bounds_.w, bounds_.y + bounds_.h),
                colors.layerFill);
    if (!layout_) {
        scroll_.Render(dc);
        return;
    }

    const float scale = dc.DpiScale();
    const float offset = SnapToPixel(scroll_.Offset(), scale);
    const float left = SnapToPixel(ContentLeft(), scale);
    const auto range = layout_->VisibleRange(offset, bounds_.h);
    pages_.SetVisible(range.first, range.last);

    for (int p = range.first; p <= range.last; ++p) {
        const pdf::PageRectDip& r = layout_->PageRect(p);
        const float x0 = SnapToPixel(left + static_cast<float>(r.x), scale);
        const float y0 = SnapToPixel(bounds_.y + static_cast<float>(r.y) - offset, scale);
        const D2D1_RECT_F dest = D2D1::RectF(x0, y0, x0 + static_cast<float>(r.width),
                                             y0 + static_cast<float>(r.height));
        dc.FillRect(D2D1::RectF(dest.left - 1, dest.top - 1, dest.right + 1, dest.bottom + 1),
                    colors.controlStrokeDefault);  // page edge

        const pdf::BitmapCache::Key want = pages_.WantedKey(p, r.width, r.height, Context());
        if (ID2D1Bitmap* bmp = pages_.BitmapFor(want, Context())) {
            dc.Dc()->DrawBitmap(bmp, dest, dc.Opacity(), D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            // Paper is white whatever the theme: it stands for the page itself,
            // which PDFium renders onto white. Not a UI-chrome colour.
            dc.FillRect(dest, pages_.Failed(want) ? colors.severityErrorFill
                                                  : D2D1::ColorF(D2D1::ColorF::White));
        }
    }
    RenderTextOverlays(dc, range.first, range.last);
    RenderAnnotations(dc, range.first, range.last);
    scroll_.Render(dc);
}

// --- Input -------------------------------------------------------------------

void PdfPageView::OnPointerWheelChanged(PointerEventArgs& e) {
    if (!layout_) return;
    if (HasModifier(e.modifiers, ModifierKeys::Ctrl)) {
        ZoomBy(std::pow(1.1, e.wheelDelta / static_cast<double>(WHEEL_DELTA)));
    } else if (HasModifier(e.modifiers, ModifierKeys::Shift)) {
        scroll_.SetOffsetX(scroll_.OffsetX() + e.WheelScrollLines() * 40.0f);
        Invalidate();
    } else {
        scroll_.AnimateBy(e.WheelScrollLines() * 40.0f);
        Invalidate();
    }
    e.handled = true;
}

void PdfPageView::OnPointerPressed(PointerEventArgs& e) {
    if (e.button != PointerButton::Left) return;
    const float x = e.position.x, y = e.position.y;
    if (scroll_.HitBarRegion(x, y)) {
        if (!scroll_.HitThumb(x, y)) {  // jump so the thumb centres on the pointer
            const RectDip thumb = scroll_.ThumbRect();
            const float track = std::max(1.0f, bounds_.h - thumb.h);
            scroll_.SetOffset((y - thumb.h * 0.5f - bounds_.y) / track * scroll_.MaxOffset());
        }
        scroll_.BeginDrag(y);
    } else if (scroll_.HitHBarRegion(x, y)) {
        scroll_.BeginHDrag(x);
    } else {
        // An annotation tool, or a press on an annotation, takes the press.
        if (AnnotPointerPressed(e)) {
            e.handled = true;
            return;
        }
        // Otherwise a press on the page area starts a text selection.
        const PagePoint pt = HitPagePoint(x, y);
        ClearSelection();
        if (pt.page < 0) return;
        selecting_ = true;
        selA_ = selB_ = pt;
    }
    if (Context().input) Context().input->CapturePointer(this);
    UpdateCurrentPage();
    Invalidate();
    e.handled = true;
}

void PdfPageView::OnPointerMoved(PointerEventArgs& e) {
    const float x = e.position.x, y = e.position.y;
    if (scroll_.IsDragging()) {
        scroll_.DragTo(y);
    } else if (scroll_.IsHDragging()) {
        scroll_.HDragTo(x);
    } else if (AnnotPointerMoved(e)) {
        e.handled = true;
        return;
    } else if (selecting_) {
        const PagePoint pt = HitPagePoint(x, y);
        if (pt.page >= 0) {
            selB_ = pt;
            RequestSelection();  // repaint happens when the result arrives
        }
        e.handled = true;
        return;
    } else {
        // Repaint only when the bar's hover state actually changes, so plain
        // pointer movement over the page costs nothing.
        const bool wasHover = scroll_.IsBarHovered();
        scroll_.SetBarHover(scroll_.HitBarRegion(x, y));
        scroll_.SetHBarHover(scroll_.HitHBarRegion(x, y));
        if (scroll_.IsBarHovered() != wasHover) {
            scroll_.Wake();
            Invalidate();
        }
        return;
    }
    UpdateCurrentPage();
    Invalidate();
    e.handled = true;
}

void PdfPageView::OnPointerReleased(PointerEventArgs& e) {
    if (e.button != PointerButton::Left) return;
    if (AnnotPointerReleased(e)) {
        e.handled = true;
        return;
    }
    if (selecting_) {
        selecting_ = false;  // keep the selection; a click without drag selects nothing
        if (Context().input && Context().input->Captured() == this)
            Context().input->ReleaseCapture(this);
        e.handled = true;
        return;
    }
    if (!scroll_.IsDragging() && !scroll_.IsHDragging()) return;
    scroll_.EndDrag();
    scroll_.EndHDrag();
    if (Context().input && Context().input->Captured() == this)
        Context().input->ReleaseCapture(this);
    Invalidate();
    e.handled = true;
}

void PdfPageView::OnKeyDownRouted(KeyEventArgs& e) {
    if (!layout_) return;
    const bool ctrl = HasModifier(e.modifiers, ModifierKeys::Ctrl);
    const float page = std::max(40.0f, bounds_.h - 40.0f);
    switch (e.vk) {
        case VK_NEXT:  scroll_.AnimateBy(page); break;
        case VK_PRIOR: scroll_.AnimateBy(-page); break;
        case VK_DOWN:  scroll_.AnimateBy(40.0f); break;
        case VK_UP:    scroll_.AnimateBy(-40.0f); break;
        case VK_HOME:  scroll_.AnimateTo(0.0f); break;
        case VK_END:   scroll_.AnimateTo(scroll_.MaxOffset()); break;
        case VK_OEM_PLUS:
        case VK_ADD:      if (!ctrl) return; ZoomBy(1.25); break;
        case VK_OEM_MINUS:
        case VK_SUBTRACT: if (!ctrl) return; ZoomBy(0.8); break;
        case '0':         if (!ctrl) return; SetZoom(1.0); break;
        case 'C':         if (!ctrl) return; CopySelection(); break;
        case VK_ESCAPE:
            // Innermost first: a drawing gesture, then the tool, then selections.
            if (annotDragging_) {
                annotDragging_ = false;
                inkStroke_.clear();
                if (Context().input && Context().input->Captured() == this)
                    Context().input->ReleaseCapture(this);
            } else if (annotTool_ != AnnotTool::None) {
                SetAnnotationTool(AnnotTool::None);
            } else if (selectedAnnotPage_ >= 0) {
                ClearAnnotationSelection();
            } else if (selection_.count > 0) {
                ClearSelection();
            } else {
                return;
            }
            break;
        case VK_DELETE: {
            if (selectedAnnotPage_ < 0) return;
            AnnotationGestureArgs args;
            args.type = AnnotationGestureArgs::Type::Delete;
            args.page = selectedAnnotPage_;
            args.annotIndex = selectedAnnotIndex_;
            annotGesture_.Raise(*this, args);
            break;
        }
        default: return;
    }
    Invalidate();
    e.handled = true;
}

void PdfPageView::OnAnimationTick(float dtSec) {
    scroll_.Tick(dtSec);
    UpdateCurrentPage();
    // scroll_ is an embedded model, not a tree node: it cannot request frames.
    Invalidate();
}
