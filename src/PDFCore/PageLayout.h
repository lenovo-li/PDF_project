// PageLayout.h — vertical continuous-scroll layout of pages, in DIPs.
//
// The viewer stacks pages top to bottom, each centred horizontally, with a
// fixed gap. For a 5000-page document the viewer must not touch every page on
// each scroll or paint, so the layout is built once per zoom change (O(n)) and
// queries are O(log n + visible): VisibleRange binary-searches the prefix of
// page tops. Pure data, no PDFium, no UI; built from PageSizeDip results the
// engine reports once after opening.
#pragma once

#include <utility>
#include <vector>

namespace pdf {

struct PageRectDip {
    double x = 0, y = 0, width = 0, height = 0;
};

class PageLayout {
public:
    // `sizesDip` are the displayed page sizes at zoom 1.0 (rotation applied).
    // The layout scales them by `zoom` and centres each in a column as wide as
    // the widest page. `gapDip` separates pages and pads the top and bottom.
    PageLayout(const std::vector<std::pair<double, double>>& sizesDip, double zoom,
               double gapDip);

    int PageCount() const { return static_cast<int>(rects_.size()); }
    double ContentWidth() const { return contentWidth_; }
    double ContentHeight() const { return contentHeight_; }
    const PageRectDip& PageRect(int index) const { return rects_[index]; }

    // Pages intersecting [top, top + height); `first > last` when none.
    struct Range {
        int first = 0;
        int last = -1;
    };
    Range VisibleRange(double top, double height) const;

    // Page whose area contains `y`, or the nearest one when `y` is in a gap.
    // -1 for an empty document. Used for the page-number indicator.
    int PageAt(double y) const;

    // Page whose area extended `tail` DIPs below its bottom edge contains `y`,
    // or -1. For hit-testing a page together with a label drawn under it:
    // PageAt splits a gap at its midpoint, which would hand the lower part of
    // a label to the next page. `tail` should not exceed the gap.
    int PageWithTailAt(double y, double tail) const;

private:
    std::vector<PageRectDip> rects_;
    double contentWidth_ = 0;
    double contentHeight_ = 0;
};

// Scales each page to fit a boxW x boxH box, keeping its aspect ratio, for a
// thumbnail strip: a portrait page fills the height, a landscape page the
// width, and a 1:20 banner page does not become 20 boxes tall. Feed the result
// to PageLayout with zoom 1.0. Garbage sizes come out as 0 x 0 (PageLayout
// already gives those no space).
std::vector<std::pair<double, double>> FitPagesInBox(
    const std::vector<std::pair<double, double>>& sizes, double boxW, double boxH);

}  // namespace pdf
