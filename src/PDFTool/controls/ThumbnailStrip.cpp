#include "ThumbnailStrip.h"

#include <PDFCore/PageGeometry.h>
#include <PDFCore/Trace.h>

#include <FluentUI/graphics/DWriteContext.h>
#include <FluentUI/graphics/PixelSnapping.h>
#include <FluentUI/input/InputManager.h>

#include <algorithm>
#include <string>

using namespace fluent;

namespace {
// Each thumbnail fits a box this size; the label sits in the gap below it.
constexpr double kBoxW = 120.0;
constexpr double kBoxH = 150.0;
constexpr double kGapDip = 28.0;     // room for the page-number label
constexpr float kLabelH = 18.0f;
// Thumbnails are small: 48 MiB holds ~600 of them at 200 % display scale.
constexpr std::uint64_t kCacheBytes = 48ull * 1024 * 1024;
constexpr std::uint64_t kPageBytes = 2ull * 1024 * 1024;
}  // namespace

ThumbnailStrip::ThumbnailStrip(pdf::Engine* engine, Application* app)
    : pages_(engine, app, kCacheBytes, kPageBytes, [this](int) { Invalidate(); }) {
    scroll_.SetKeepVisibleWhenOverflow(false);
    SetWidth(kStripWidthDip);
}

ThumbnailStrip::~ThumbnailStrip() = default;

void ThumbnailStrip::SetDocument(pdf::DocumentId id,
                                 const std::vector<std::pair<double, double>>& sizesPt) {
    ClearDocument();
    pages_.SetDocument(id);
    if (!sizesPt.empty()) layout_.emplace(pdf::FitPagesInBox(sizesPt, kBoxW, kBoxH), 1.0, kGapDip);
    scroll_.SetContentHeight(layout_ ? static_cast<float>(layout_->ContentHeight()) : 0.0f);
    scroll_.SetOffset(0);
    current_ = layout_ ? 0 : -1;
    Invalidate();
}

void ThumbnailStrip::ClearDocument() {
    pages_.SetDocument(pdf::kNoDocument);
    layout_.reset();
    current_ = hover_ = -1;
    scroll_.SetContentHeight(0);
    Invalidate();
}

void ThumbnailStrip::SetCurrentPage(int page) {
    if (!layout_ || page < 0 || page >= layout_->PageCount() || page == current_) return;
    current_ = page;
    ScrollIntoView(page);
    Invalidate();
}

void ThumbnailStrip::ScrollIntoView(int page) {
    const pdf::PageRectDip& r = layout_->PageRect(page);
    const float top = static_cast<float>(r.y) - 8.0f;
    const float bottom = static_cast<float>(r.y + r.height) + kLabelH + 8.0f;
    if (top < scroll_.Offset()) {
        scroll_.AnimateTo(top);
    } else if (bottom > scroll_.Offset() + bounds_.h) {
        scroll_.AnimateTo(bottom - bounds_.h);
    }
}

int ThumbnailStrip::HitPage(float x, float y) const {
    if (!layout_ || x < bounds_.x || x >= bounds_.x + bounds_.w) return -1;
    // The label under a thumbnail counts as part of it: users click numbers too.
    // +2 matches where Render places the label (dest.bottom + 2).
    return layout_->PageWithTailAt(y - bounds_.y + scroll_.Offset(), kLabelH + 2.0);
}

void ThumbnailStrip::Measure(float availW, float availH) {
    FrameworkElement::Measure(availW, availH);  // SetWidth fixes the width
}

void ThumbnailStrip::OnBoundsChanged() {
    scroll_.SetBounds(bounds_);
    scroll_.SetContentHeight(layout_ ? static_cast<float>(layout_->ContentHeight()) : 0.0f);
}

void ThumbnailStrip::OnAttachedToTree() { scroll_.AttachToContext(Context()); }
void ThumbnailStrip::OnDetachedFromTree() { scroll_.DetachFromContext(); }

void ThumbnailStrip::OnDpiChanged(float) {
    pages_.InvalidateSizes();
    Invalidate();
}

void ThumbnailStrip::OnDeviceLost() { pages_.OnDeviceLost(); }

void ThumbnailStrip::OnDeviceRestored() {
    pages_.OnDeviceRestored();
    Invalidate();
}

// --- Rendering ---------------------------------------------------------------

void ThumbnailStrip::Render(const DrawingContext& dc) {
    const auto& colors = Theme().colors;
    const D2D1_RECT_F all =
        D2D1::RectF(bounds_.x, bounds_.y, bounds_.x + bounds_.w, bounds_.y + bounds_.h);
    auto clip = dc.PushClip(all);
    dc.FillRect(all, colors.cardFill);
    // Separator against the page view.
    dc.FillRect(D2D1::RectF(all.right - 1, all.top, all.right, all.bottom),
                colors.controlStrokeDefault);
    if (!layout_) {
        scroll_.Render(dc);
        return;
    }

    const float scale = dc.DpiScale();
    const float offset = SnapToPixel(scroll_.Offset(), scale);
    // Widen the query upward by the label height: a page whose thumbnail has
    // scrolled just above the top still owns the label showing below it.
    const auto range = layout_->VisibleRange(offset - (kLabelH + 2.0f), bounds_.h + kLabelH + 2.0f);
    pages_.SetVisible(range.first, range.last);

    IDWriteTextFormat* fmt =
        Context().dwrite ? Context().dwrite->Format(Theme().typography.captionSize) : nullptr;
    const float colLeft =
        bounds_.x + (bounds_.w - static_cast<float>(layout_->ContentWidth())) * 0.5f;

    for (int p = range.first; p <= range.last; ++p) {
        const pdf::PageRectDip& r = layout_->PageRect(p);
        const float x0 = SnapToPixel(colLeft + static_cast<float>(r.x), scale);
        const float y0 = SnapToPixel(bounds_.y + static_cast<float>(r.y) - offset, scale);
        const D2D1_RECT_F dest = D2D1::RectF(x0, y0, x0 + static_cast<float>(r.width),
                                             y0 + static_cast<float>(r.height));

        if (p == current_ || p == hover_) {
            // Accent wash behind thumbnail + label for the current page, a faint
            // text-colour wash for hover; same alphas as ListBox.
            const D2D1_COLOR_F wash =
                WithAlphaScale(p == current_ ? colors.accent : colors.textPrimary,
                               p == current_ ? 0.15f : 0.05f);
            dc.FillRoundedRect(
                D2D1::RoundedRect(D2D1::RectF(dest.left - 6, dest.top - 6, dest.right + 6,
                                              dest.bottom + kLabelH + 4),
                                  4.0f, 4.0f),
                wash);
        }

        const pdf::BitmapCache::Key want = pages_.WantedKey(p, r.width, r.height, Context());
        if (ID2D1Bitmap* bmp = pages_.BitmapFor(want, Context())) {
            dc.Dc()->DrawBitmap(bmp, dest, dc.Opacity(), D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            // Paper is white whatever the theme (see PdfPageView).
            dc.FillRect(dest, pages_.Failed(want) ? colors.severityErrorFill
                                                  : D2D1::ColorF(D2D1::ColorF::White));
        }
        dc.DrawRect(dest, p == current_ ? colors.accent : colors.controlStrokeDefault,
                    p == current_ ? 2.0f : 1.0f);

        if (fmt) {
            const std::wstring label = std::to_wstring(p + 1);
            dc.DrawText(label.c_str(), static_cast<UINT32>(label.size()), fmt,
                        D2D1::RectF(bounds_.x, dest.bottom + 2, bounds_.x + bounds_.w,
                                    dest.bottom + 2 + kLabelH),
                        p == current_ ? colors.textPrimary : colors.textSecondary);
        }
    }
    scroll_.Render(dc);
}

// --- Input -------------------------------------------------------------------

void ThumbnailStrip::OnPointerWheelChanged(PointerEventArgs& e) {
    if (!layout_ || scroll_.MaxOffset() <= 0.0f) return;
    scroll_.AnimateBy(e.WheelScrollLines() * 40.0f);
    // Content moves under a still pointer; drop the stale hover until the
    // pointer moves again rather than recomputing it on every tick.
    hover_ = -1;
    Invalidate();
    e.handled = true;
}

void ThumbnailStrip::OnPointerPressed(PointerEventArgs& e) {
    if (e.button != PointerButton::Left) return;
    const float x = e.position.x, y = e.position.y;
    if (scroll_.HitBarRegion(x, y)) {
        if (!scroll_.HitThumb(x, y)) {  // jump so the thumb centres on the pointer
            const RectDip thumb = scroll_.ThumbRect();
            const float track = std::max(1.0f, bounds_.h - thumb.h);
            scroll_.SetOffset((y - thumb.h * 0.5f - bounds_.y) / track * scroll_.MaxOffset());
        }
        scroll_.BeginDrag(y);
        if (Context().input) Context().input->CapturePointer(this);
        Invalidate();
        e.handled = true;
        return;
    }
    const int p = HitPage(x, y);
    if (p < 0) return;
    current_ = p;
    Invalidate();
    PageClickedArgs args;
    args.page = p;
    pageClicked_.Raise(*this, args);
    e.handled = true;
}

void ThumbnailStrip::OnPointerMoved(PointerEventArgs& e) {
    const float x = e.position.x, y = e.position.y;
    if (scroll_.IsDragging()) {
        scroll_.DragTo(y);
        Invalidate();
        e.handled = true;
        return;
    }
    const bool wasBar = scroll_.IsBarHovered();
    scroll_.SetBarHover(scroll_.HitBarRegion(x, y));
    const bool barChanged = scroll_.IsBarHovered() != wasBar;
    const int p = scroll_.IsBarHovered() ? -1 : HitPage(x, y);
    // Wake only for the scrollbar: Wake starts its fade cycle, which ticks
    // frames for over a second; a thumbnail hover change needs one repaint.
    if (barChanged) scroll_.Wake();
    if (p != hover_ || barChanged) {
        hover_ = p;
        Invalidate();
    }
}

void ThumbnailStrip::OnPointerReleased(PointerEventArgs& e) {
    if (e.button != PointerButton::Left || !scroll_.IsDragging()) return;
    scroll_.EndDrag();
    if (Context().input && Context().input->Captured() == this)
        Context().input->ReleaseCapture(this);
    Invalidate();
    e.handled = true;
}

void ThumbnailStrip::OnPointerLeft() {
    if (hover_ < 0 && !scroll_.IsBarHovered()) return;
    hover_ = -1;
    scroll_.SetBarHover(false);
    Invalidate();
}

void ThumbnailStrip::OnAnimationTick(float dtSec) {
    scroll_.Tick(dtSec);
    Invalidate();  // scroll_ is an embedded model; it cannot request frames
}
