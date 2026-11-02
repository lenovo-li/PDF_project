// PdfPageView.h — continuous-scroll page viewer.
//
// Pages are stacked vertically (pdf::PageLayout). Only pages intersecting the
// viewport are drawn or requested, so cost is O(visible) whatever the page
// count. Rendering, the bounded caches and stale-job skipping live in
// PageBitmapSource; a missing page is drawn as a placeholder, or as a stretched
// stale bitmap while a new zoom renders.
//
// Bitmaps are rendered at layout size × Context().dpiScale, so a DPI change
// (monitor switch) or zoom change simply asks for new pixel sizes.
#pragma once

#include "PageBitmapSource.h"

#include <FluentUI/FluentUI.h>
#include <PDFCore/Annotations.h>
#include <PDFCore/DocumentStore.h>
#include <PDFCore/PageLayout.h>
#include <PDFCore/TextSearch.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pdf { class Engine; }

struct PageViewChangedArgs : fluent::RoutedEventArgs {
    int currentPage = -1;  // 0-based, -1 when no document
    int pageCount = 0;
    double zoom = 1.0;
};

// Pointer tools. Text markup (highlight, underline, strike-out) is not a tool:
// it applies to the current text selection from the menu.
enum class AnnotTool {
    None,      // select annotations or text
    Note,      // click to place
    FreeText,  // drag a box
    Square,    // drag a box
    Ink        // drag a stroke
};

struct AnnotationGestureArgs : fluent::RoutedEventArgs {
    enum class Type { EditText, Delete, AddNote, AddFreeText, AddSquare, AddInk };
    Type type = Type::EditText;
    int page = -1;
    int annotIndex = -1;                // EditText / Delete
    pdf::UnitRect unitRect;             // AddNote (top-left used), AddFreeText, AddSquare
    std::vector<pdf::PointF> inkStroke; // AddInk, unit coords of the displayed page
};

class PdfPageView final : public fluent::FrameworkElement {
public:
    static constexpr double kMinZoom = 0.1;
    static constexpr double kMaxZoom = 8.0;

    PdfPageView(pdf::Engine* engine, fluent::Application* app);
    ~PdfPageView() override;

    // `sizesPt` are the displayed page sizes in points (rotation applied), from
    // Document::DisplayedPageSizesPt. The view does not own the document; the
    // caller closes it in the engine's DocumentStore after ClearDocument().
    void SetDocument(pdf::DocumentId id, const std::vector<std::pair<double, double>>& sizesPt);
    void ClearDocument();
    pdf::DocumentId DocumentId() const { return pages_.Document(); }

    void SetZoom(double zoom);  // clamped to [kMinZoom, kMaxZoom]
    double Zoom() const { return zoom_; }
    void ZoomBy(double factor) { SetZoom(zoom_ * factor); }
    // Zoom so the widest page fits the viewport width.
    void FitWidth();

    int CurrentPage() const { return currentPage_; }
    int PageCount() const { return layout_ ? layout_->PageCount() : 0; }
    void ScrollToPage(int index);

    // Current page or zoom changed (scrolling, zooming, new document).
    fluent::Event<PdfPageView, PageViewChangedArgs>& ViewChanged() { return viewChanged_; }

    // Annotation mode: which tool creates an annotation on the next drag/click.
    void SetAnnotationTool(AnnotTool tool);
    AnnotTool GetAnnotationTool() const { return annotTool_; }

    // Page content changed on the engine side (annotation added, removed or
    // edited): drop its bitmaps and annotation list so both reload. Page sizes
    // are unchanged, so the layout, scroll position and search stay.
    void PageContentChanged(int page);
    // Same for every page at once (form edits may touch any page).
    void AllContentChanged();

    // Returns (page, annotIndex) of the selected annotation, or (-1, -1).
    std::pair<int, int> SelectedAnnotation() const { return {selectedAnnotPage_, selectedAnnotIndex_}; }
    // Kind and text of the selected annotation, if it is loaded.
    const pdf::AnnotInfo* SelectedAnnotationInfo() const;
    void ClearAnnotationSelection();

    // Annotation gestures. The view only reports them; MainWindow edits the
    // document on the engine thread (dirty state, pending edits, prompts).
    fluent::Event<PdfPageView, AnnotationGestureArgs>& AnnotationGesture() { return annotGesture_; }

    // Search highlights, as fractions of the displayed page (pdf::UnitRect).
    // AddHighlights appends a batch for pages as search results stream in;
    // only pages in view are ever drawn, so the total does not cost per frame.
    void AddHighlights(const std::vector<pdf::TextMatch>& matches);
    void ClearHighlights();
    // Emphasise one match and scroll it into view; (-1, -1) clears.
    void SetCurrentMatch(int page, int matchOnPage);

    // Drag-to-select text. The view sends the drag's end points to the engine
    // (one job in flight at a time) and draws what comes back. Ctrl+C copies.
    const std::wstring& SelectedText() const { return selection_.text; }
    const pdf::TextSelection* GetTextSelection() const {
        return selection_.count > 0 ? &selection_ : nullptr;
    }
    void ClearSelection();
    bool CopySelection() const;

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
    void OnKeyDownRouted(fluent::KeyEventArgs& e) override;

    bool WantsAnimationTick() const override { return scroll_.NeedsTick(); }
    void OnAnimationTick(float dtSec) override;

private:
    void RebuildLayout(bool keepAnchor);
    void UpdateExtents();
    void UpdateCurrentPage(bool force = false);
    float ContentLeft() const;  // x of content column in window DIPs

    // Window DIPs -> (page, fraction of that page); page -1 when not on one.
    struct PagePoint {
        int page = -1;
        double ux = 0, uy = 0;
    };
    PagePoint HitPagePoint(float x, float y) const;
    D2D1_RECT_F PageRectOnScreen(int page, float scale) const;
    void RequestSelection();
    void OnSelectionResult(pdf::TextSelection sel, int ticket);
    void RenderTextOverlays(const fluent::DrawingContext& dc, int first, int last);
    void RenderAnnotations(const fluent::DrawingContext& dc, int first, int last);
    // Returns (page, annotIndex) if (x, y) hits an annotation, else (-1, -1).
    std::pair<int, int> HitAnnotation(float x, float y) const;
    void EnsureAnnotations(int first, int last);
    void OnAnnotationsLoaded(int page, std::vector<pdf::AnnotInfo> annots, int gen);
    void ResetAnnotations();
    // Annotation tool input; return true if the event was consumed.
    bool AnnotPointerPressed(const fluent::PointerEventArgs& e);
    bool AnnotPointerMoved(const fluent::PointerEventArgs& e);
    bool AnnotPointerReleased(const fluent::PointerEventArgs& e);

    std::vector<std::pair<double, double>> sizesDip_;  // zoom 1.0

    // page -> its matches' rectangles; one inner vector per match.
    std::unordered_map<int, std::vector<std::vector<pdf::UnitRect>>> highlights_;
    int currentMatchPage_ = -1;
    int currentMatchIndex_ = -1;

    // Annotations: page -> list of annots with their screen-draw unitRects.
    std::unordered_map<int, std::vector<pdf::AnnotInfo>> annotations_;
    // Loaded lazily for visible pages (EnsureAnnotations from Render) and
    // dropped for pages far outside the view, so memory is O(visible).
    // annotGen_[page] is bumped when a page is refreshed: a list result that
    // was already in flight from before an edit carries the old number and is
    // dropped instead of overwriting the fresh list.
    std::unordered_map<int, int> annotGen_;
    std::unordered_map<int, bool> annotRequested_;
    int selectedAnnotPage_ = -1;
    int selectedAnnotIndex_ = -1;  // AnnotInfo::index (the /Annots position)
    AnnotTool annotTool_ = AnnotTool::None;
    bool annotDragging_ = false;
    PagePoint annotDragStart_, annotDragEnd_;
    std::vector<pdf::PointF> inkStroke_;  // unit coords of the stroke being drawn

    // Selection drag state. selPending_ keeps at most one engine job in
    // flight; a move during it just updates selB_ and re-sends on return.
    bool selecting_ = false;
    PagePoint selA_, selB_;
    bool selPending_ = false;
    bool selDirty_ = false;
    int selTicket_ = 0;
    pdf::TextSelection selection_;
    std::optional<pdf::PageLayout> layout_;
    double zoom_ = 1.0;
    int currentPage_ = -1;

    fluent::ScrollViewer scroll_;
    // Rendering, caching and stale-job skipping; shared design with the
    // thumbnail strip (see PageBitmapSource.h).
    PageBitmapSource pages_;

    pdf::Engine* engine_ = nullptr;
    fluent::Application* app_ = nullptr;
    // Lifetime token for selection jobs: expires with the view, so a late
    // result never touches it.
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);

    fluent::Event<PdfPageView, PageViewChangedArgs> viewChanged_;
    fluent::Event<PdfPageView, AnnotationGestureArgs> annotGesture_;
};
