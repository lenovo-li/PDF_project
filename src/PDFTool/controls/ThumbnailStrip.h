// ThumbnailStrip.h — vertical strip of page thumbnails with page numbers.
//
// Each page is scaled into a fixed box (pdf::FitPagesInBox) and stacked with
// pdf::PageLayout, so drawing, hit-testing and rendering touch only the
// visible thumbnails: O(visible) for any page count. Bitmaps come from its own
// PageBitmapSource with a smaller cache than the main view, so scrolling the
// strip never evicts the large page bitmaps the reader is looking at.
//
// Clicking a thumbnail raises PageClicked; the owner scrolls the main view.
// SetCurrentPage highlights a page and scrolls it into view.
#pragma once

#include "PageBitmapSource.h"

#include <FluentUI/FluentUI.h>
#include <PDFCore/DocumentStore.h>
#include <PDFCore/PageLayout.h>

#include <optional>
#include <utility>
#include <vector>

namespace pdf { class Engine; }

struct PageClickedArgs : fluent::RoutedEventArgs {
    int page = -1;  // 0-based
};

class ThumbnailStrip final : public fluent::FrameworkElement {
public:
    static constexpr float kStripWidthDip = 168.0f;

    ThumbnailStrip(pdf::Engine* engine, fluent::Application* app);
    ~ThumbnailStrip() override;

    // Same inputs as PdfPageView::SetDocument; the strip does not own the doc.
    void SetDocument(pdf::DocumentId id, const std::vector<std::pair<double, double>>& sizesPt);
    void ClearDocument();

    void SetCurrentPage(int page);
    int CurrentPage() const { return current_; }

    // Page content changed (annotation edit): drop its thumbnail bitmap.
    void PageContentChanged(int page) { pages_.InvalidatePage(page); Invalidate(); }
    void AllContentChanged() { pages_.InvalidateAllContent(); Invalidate(); }

    fluent::Event<ThumbnailStrip, PageClickedArgs>& PageClicked() { return pageClicked_; }

    void Measure(float availW, float availH) override;
    void Render(const fluent::DrawingContext& dc) override;

protected:
    void OnBoundsChanged() override;
    void OnAttachedToTree() override;
    void OnDetachedFromTree() override;
    void OnDpiChanged(float dpiScale) override;
    void OnDeviceLost() override;
    void OnDeviceRestored() override;
    void OnThemeChanged() override { Invalidate(); }

    void OnPointerWheelChanged(fluent::PointerEventArgs& e) override;
    void OnPointerPressed(fluent::PointerEventArgs& e) override;
    void OnPointerMoved(fluent::PointerEventArgs& e) override;
    void OnPointerReleased(fluent::PointerEventArgs& e) override;
    void OnPointerLeft() override;

    bool WantsAnimationTick() const override { return scroll_.NeedsTick(); }
    void OnAnimationTick(float dtSec) override;

private:
    // Page index under a point in window DIPs, or -1 (gaps, labels excluded).
    int HitPage(float x, float y) const;
    void ScrollIntoView(int page);

    std::optional<pdf::PageLayout> layout_;
    int current_ = -1;
    int hover_ = -1;
    fluent::ScrollViewer scroll_;
    PageBitmapSource pages_;
    fluent::Event<ThumbnailStrip, PageClickedArgs> pageClicked_;
};
