// PdfPageViewAnnot.cpp — annotation display and pointer tools for PdfPageView.
// Split from PdfPageView.cpp to keep the annotation code separate.
//
// The page bitmap already contains the annotations (RenderPage draws them
// with FPDF_ANNOT); this file only keeps each visible page's annotation list
// for hit-testing and the selection outline, and turns pointer gestures into
// AnnotationGesture events. All document edits happen in MainWindow.

#include "PdfPageView.h"

#include <PDFCore/Annotations.h>
#include <PDFCore/Engine.h>
#include <PDFCore/Trace.h>

#include <FluentUI/input/InputManager.h>

#include <algorithm>
#include <cmath>

using namespace fluent;

namespace {
// Pages kept loaded beyond the visible range, like PageBitmapSource.
constexpr int kAnnotKeepAround = 2;
// A drag shorter than this (fraction of the page) is a click, not a box.
constexpr double kMinDragUnit = 0.01;
// Ink points closer than this to the previous one are dropped: a slow drag
// would otherwise send a point per pixel and hit kMaxInkPoints quickly.
constexpr double kInkMinStepUnit = 0.002;
}  // namespace

void PdfPageView::SetAnnotationTool(AnnotTool tool) {
    if (tool == annotTool_) return;
    annotTool_ = tool;
    if (annotDragging_) {
        annotDragging_ = false;
        inkStroke_.clear();
        if (Context().input && Context().input->Captured() == this)
            Context().input->ReleaseCapture(this);
    }
    if (tool != AnnotTool::None) {
        ClearSelection();
        ClearAnnotationSelection();
    }
    Invalidate();
}

void PdfPageView::ResetAnnotations() {
    annotations_.clear();
    annotRequested_.clear();
    // Keep counting up rather than clearing annotGen_: a result from the old
    // document still in flight must not match a fresh generation 0.
    for (auto& [page, gen] : annotGen_) ++gen;
    selectedAnnotPage_ = selectedAnnotIndex_ = -1;
    if (annotDragging_) {
        annotDragging_ = false;
        inkStroke_.clear();
        if (Context().input && Context().input->Captured() == this)
            Context().input->ReleaseCapture(this);
    }
}

void PdfPageView::PageContentChanged(int page) {
    if (!layout_ || page < 0 || page >= layout_->PageCount()) return;
    pages_.InvalidatePage(page);
    ++annotGen_[page];
    annotations_.erase(page);
    annotRequested_.erase(page);
    // Indices on this page may have shifted; the old selection could now name
    // a different annotation.
    if (selectedAnnotPage_ == page) selectedAnnotPage_ = selectedAnnotIndex_ = -1;
    Invalidate();  // the next paint re-requests both bitmap and list
}

void PdfPageView::AllContentChanged() {
    if (!layout_) return;
    pages_.InvalidateAllContent();
    ResetAnnotations();  // lists reload lazily for the visible pages
    Invalidate();
}

void PdfPageView::EnsureAnnotations(int first, int last) {
    // Drop lists far from view so memory stays O(visible).
    for (auto it = annotations_.begin(); it != annotations_.end();) {
        const bool keep = it->first >= first - kAnnotKeepAround && it->first <= last + kAnnotKeepAround;
        if (!keep) annotRequested_.erase(it->first);
        it = keep ? std::next(it) : annotations_.erase(it);
    }
    if (!engine_ || !app_ || DocumentId() == pdf::kNoDocument) return;
    const pdf::DocumentId id = DocumentId();
    pdf::Engine* engine = engine_;
    Application* app = app_;
    std::weak_ptr<int> alive = alive_;
    PdfPageView* self = this;
    for (int page = first; page <= last; ++page) {
        if (annotRequested_.count(page)) continue;
        annotRequested_[page] = true;
        const int gen = annotGen_[page];
        engine_->Post([engine, app, id, page, gen, alive, self] {
            // Engine thread.
            std::vector<pdf::AnnotInfo> annots;
            if (pdf::Document* doc = engine->Documents().Get(id)) {
                if (auto list = pdf::ListAnnotations(*doc, page); list) {
                    annots = std::move(list.value().annots);
                } else {
                    pdf::TraceF("[PDFTool.Annot] list failed, page=%d err=%lu\n", page,
                                list.error().code);
                }
            }
            app->Post([page, annots = std::move(annots), gen, alive, self]() mutable {
                if (alive.expired()) return;  // view destroyed
                self->OnAnnotationsLoaded(page, std::move(annots), gen);
            });
        });
    }
}

void PdfPageView::OnAnnotationsLoaded(int page, std::vector<pdf::AnnotInfo> annots, int gen) {
    auto it = annotGen_.find(page);
    if (it == annotGen_.end() || it->second != gen) return;  // page edited or document changed
    if (!annotRequested_.count(page)) return;  // scrolled away and dropped meanwhile
    // Only what the view can act on: links, form widgets and popups are not
    // selectable (deleting a widget would break the form).
    annots.erase(std::remove_if(annots.begin(), annots.end(),
                                [](const pdf::AnnotInfo& a) { return !pdf::IsEditableKind(a.kind); }),
                 annots.end());
    annotations_[page] = std::move(annots);
    // Repaint only if a selection could be drawn on it; hit-testing reads the
    // list directly, so nothing else depends on a frame.
    if (page == selectedAnnotPage_) Invalidate();
}

const pdf::AnnotInfo* PdfPageView::SelectedAnnotationInfo() const {
    if (selectedAnnotPage_ < 0) return nullptr;
    auto it = annotations_.find(selectedAnnotPage_);
    if (it == annotations_.end()) return nullptr;
    for (const pdf::AnnotInfo& a : it->second)
        if (a.index == selectedAnnotIndex_) return &a;
    return nullptr;
}

void PdfPageView::ClearAnnotationSelection() {
    if (selectedAnnotPage_ < 0) return;
    selectedAnnotPage_ = selectedAnnotIndex_ = -1;
    Invalidate();
}

std::pair<int, int> PdfPageView::HitAnnotation(float x, float y) const {
    if (!layout_ || layout_->PageCount() == 0) return {-1, -1};
    const double contentY = y - bounds_.y + scroll_.Offset();
    const int p = layout_->PageAt(contentY);
    if (p < 0) return {-1, -1};
    auto it = annotations_.find(p);
    if (it == annotations_.end()) return {-1, -1};
    const D2D1_RECT_F pr = PageRectOnScreen(p, Context().dpiScale > 0 ? Context().dpiScale : 1.0f);
    const float w = pr.right - pr.left, h = pr.bottom - pr.top;
    if (w <= 0 || h <= 0) return {-1, -1};
    if (x < pr.left || x > pr.right || y < pr.top || y > pr.bottom) return {-1, -1};
    const double ux = (x - pr.left) / w, uy = (y - pr.top) / h;
    // A few DIPs of slack so thin markup (an underline) is still clickable.
    const double slackX = 3.0 / w, slackY = 3.0 / h;
    // Last in /Annots is drawn on top, so test from the end.
    for (int i = static_cast<int>(it->second.size()) - 1; i >= 0; --i) {
        const pdf::UnitRect& u = it->second[i].unitRect;
        if (ux >= u.left - slackX && ux <= u.right + slackX && uy >= u.top - slackY &&
            uy <= u.bottom + slackY)
            return {p, it->second[i].index};
    }
    return {-1, -1};
}

// --- Pointer tools -------------------------------------------------------------

bool PdfPageView::AnnotPointerPressed(const PointerEventArgs& e) {
    const float x = e.position.x, y = e.position.y;
    if (annotTool_ == AnnotTool::None) {
        // Select tool: a press on an annotation selects it (double-click edits
        // a note or free text); anywhere else falls through to text selection.
        const auto [page, index] = HitAnnotation(x, y);
        if (page < 0) {
            ClearAnnotationSelection();
            return false;
        }
        ClearSelection();
        selectedAnnotPage_ = page;
        selectedAnnotIndex_ = index;
        Invalidate();
        if (e.clickCount >= 2) {
            const pdf::AnnotInfo* info = SelectedAnnotationInfo();
            if (info && (info->kind == pdf::AnnotKind::Note || info->kind == pdf::AnnotKind::FreeText)) {
                AnnotationGestureArgs args;
                args.type = AnnotationGestureArgs::Type::EditText;
                args.page = page;
                args.annotIndex = index;
                annotGesture_.Raise(*this, args);
            }
        }
        return true;
    }
    const PagePoint pt = HitPagePoint(x, y);
    if (pt.page < 0) return true;  // a tool is active: never start a text selection
    // HitPagePoint snaps to the nearest page; a press in the gap between pages
    // should not start a shape on the page above.
    const D2D1_RECT_F pr = PageRectOnScreen(pt.page, Context().dpiScale > 0 ? Context().dpiScale : 1.0f);
    if (x < pr.left || x > pr.right || y < pr.top || y > pr.bottom) return true;
    if (annotTool_ == AnnotTool::Note) {
        AnnotationGestureArgs args;
        args.type = AnnotationGestureArgs::Type::AddNote;
        args.page = pt.page;
        args.unitRect = {pt.ux, pt.uy, pt.ux, pt.uy};
        annotGesture_.Raise(*this, args);
        return true;
    }
    annotDragging_ = true;
    annotDragStart_ = annotDragEnd_ = pt;
    inkStroke_.clear();
    if (annotTool_ == AnnotTool::Ink) inkStroke_.push_back({pt.ux, pt.uy});
    if (Context().input) Context().input->CapturePointer(this);
    Invalidate();
    return true;
}

bool PdfPageView::AnnotPointerMoved(const PointerEventArgs& e) {
    if (!annotDragging_) return false;
    PagePoint pt = HitPagePoint(e.position.x, e.position.y);
    // A shape belongs to the page it started on: clamp onto that page.
    if (pt.page != annotDragStart_.page) {
        const D2D1_RECT_F pr =
            PageRectOnScreen(annotDragStart_.page, Context().dpiScale > 0 ? Context().dpiScale : 1.0f);
        const float w = pr.right - pr.left, h = pr.bottom - pr.top;
        if (w <= 0 || h <= 0) return true;
        pt.page = annotDragStart_.page;
        pt.ux = std::clamp((e.position.x - pr.left) / w, 0.0f, 1.0f);
        pt.uy = std::clamp((e.position.y - pr.top) / h, 0.0f, 1.0f);
    }
    annotDragEnd_ = pt;
    if (annotTool_ == AnnotTool::Ink) {
        const pdf::PointF& last = inkStroke_.back();
        const bool farEnough = std::hypot(pt.ux - last.x, pt.uy - last.y) >= kInkMinStepUnit;
        if (farEnough && inkStroke_.size() < static_cast<std::size_t>(pdf::kMaxInkPoints))
            inkStroke_.push_back({pt.ux, pt.uy});
    }
    Invalidate();
    return true;
}

bool PdfPageView::AnnotPointerReleased(const PointerEventArgs& e) {
    if (!annotDragging_) return false;
    (void)e;
    annotDragging_ = false;
    if (Context().input && Context().input->Captured() == this) Context().input->ReleaseCapture(this);
    const PagePoint a = annotDragStart_, b = annotDragEnd_;
    AnnotationGestureArgs args;
    args.page = a.page;
    if (annotTool_ == AnnotTool::Ink) {
        if (inkStroke_.size() >= 2) {
            args.type = AnnotationGestureArgs::Type::AddInk;
            args.inkStroke = std::move(inkStroke_);
            annotGesture_.Raise(*this, args);
        }
        inkStroke_.clear();
    } else {
        const pdf::UnitRect u{std::min(a.ux, b.ux), std::min(a.uy, b.uy), std::max(a.ux, b.ux),
                              std::max(a.uy, b.uy)};
        if (u.right - u.left >= kMinDragUnit && u.bottom - u.top >= kMinDragUnit) {
            args.type = annotTool_ == AnnotTool::Square ? AnnotationGestureArgs::Type::AddSquare
                                                        : AnnotationGestureArgs::Type::AddFreeText;
            args.unitRect = u;
            annotGesture_.Raise(*this, args);
        }
    }
    Invalidate();
    return true;
}

// --- Drawing -------------------------------------------------------------------

void PdfPageView::RenderAnnotations(const DrawingContext& dc, int first, int last) {
    if (!layout_) return;
    EnsureAnnotations(first, last);
    const auto& colors = Theme().colors;
    const float scale = dc.DpiScale();
    auto toScreen = [&](int page, double ux, double uy) {
        const D2D1_RECT_F pr = PageRectOnScreen(page, scale);
        return D2D1::Point2F(pr.left + static_cast<float>(ux) * (pr.right - pr.left),
                             pr.top + static_cast<float>(uy) * (pr.bottom - pr.top));
    };
    // The annotations themselves are in the page bitmap; only the selection
    // outline is drawn here.
    if (const pdf::AnnotInfo* sel = SelectedAnnotationInfo();
        sel && selectedAnnotPage_ >= first && selectedAnnotPage_ <= last) {
        const D2D1_POINT_2F tl = toScreen(selectedAnnotPage_, sel->unitRect.left, sel->unitRect.top);
        const D2D1_POINT_2F br = toScreen(selectedAnnotPage_, sel->unitRect.right, sel->unitRect.bottom);
        dc.DrawRect(D2D1::RectF(tl.x - 2, tl.y - 2, br.x + 2, br.y + 2), colors.accent, 2.0f);
    }
    if (!annotDragging_ || annotDragStart_.page < first || annotDragStart_.page > last) return;
    const int p = annotDragStart_.page;
    if (annotTool_ == AnnotTool::Ink) {
        for (std::size_t i = 1; i < inkStroke_.size(); ++i)
            dc.DrawLine(toScreen(p, inkStroke_[i - 1].x, inkStroke_[i - 1].y),
                        toScreen(p, inkStroke_[i].x, inkStroke_[i].y), colors.accent, 2.0f);
    } else {
        const D2D1_POINT_2F a = toScreen(p, annotDragStart_.ux, annotDragStart_.uy);
        const D2D1_POINT_2F b = toScreen(p, annotDragEnd_.ux, annotDragEnd_.uy);
        dc.DrawRect(D2D1::RectF(std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x),
                                std::max(a.y, b.y)),
                    colors.accent, 1.5f);
    }
}
