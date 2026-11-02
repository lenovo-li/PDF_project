// PageLayout: stacking, centring, visible-range and page-at queries, and
// tolerance of garbage sizes from malformed files.

#include "../framework/Test.h"

#include <PDFCore/PageLayout.h>

#include <cmath>

using namespace pdf;

namespace {
// Three pages: 100x200, 50x100, 100x200 at zoom 1, gap 10.
// Tops: 10, 220, 330. Content height 540. Content width 120.
PageLayout ThreePages(double zoom = 1.0) {
    return PageLayout({{100, 200}, {50, 100}, {100, 200}}, zoom, 10);
}
}  // namespace

TEST(PageLayout, StacksAndCentresPages) {
    auto l = ThreePages();
    EXPECT_EQ(l.PageCount(), 3);
    EXPECT_NEAR(l.ContentWidth(), 120, 1e-9);
    EXPECT_NEAR(l.ContentHeight(), 540, 1e-9);
    EXPECT_NEAR(l.PageRect(0).y, 10, 1e-9);
    EXPECT_NEAR(l.PageRect(1).y, 220, 1e-9);
    EXPECT_NEAR(l.PageRect(2).y, 330, 1e-9);
    EXPECT_NEAR(l.PageRect(0).x, 10, 1e-9);
    EXPECT_NEAR(l.PageRect(1).x, 35, 1e-9);  // narrower page is centred
}

TEST(PageLayout, ZoomScalesEverything) {
    auto l = ThreePages(2.0);
    EXPECT_NEAR(l.PageRect(1).y, 10 + 400 + 10, 1e-9);
    EXPECT_NEAR(l.PageRect(1).height, 200, 1e-9);
    EXPECT_NEAR(l.ContentWidth(), 220, 1e-9);
}

TEST(PageLayout, VisibleRangeFindsIntersectingPagesOnly) {
    auto l = ThreePages();
    auto r = l.VisibleRange(0, 5);  // top gap only
    EXPECT_TRUE(r.first > r.last);
    r = l.VisibleRange(0, 50);
    EXPECT_EQ(r.first, 0);
    EXPECT_EQ(r.last, 0);
    r = l.VisibleRange(205, 20);  // bottom of page 0 .. top of page 1
    EXPECT_EQ(r.first, 0);
    EXPECT_EQ(r.last, 1);
    r = l.VisibleRange(210, 10);  // exactly the gap between pages 0 and 1
    EXPECT_TRUE(r.first > r.last);
    r = l.VisibleRange(0, 10000);
    EXPECT_EQ(r.first, 0);
    EXPECT_EQ(r.last, 2);
    r = l.VisibleRange(600, 100);  // past the end
    EXPECT_TRUE(r.first > r.last);
}

TEST(PageLayout, PageAtPicksNearestInGaps) {
    auto l = ThreePages();
    EXPECT_EQ(l.PageAt(-50), 0);
    EXPECT_EQ(l.PageAt(100), 0);
    EXPECT_EQ(l.PageAt(212), 0);  // gap 210..220, closer to page 0
    EXPECT_EQ(l.PageAt(218), 1);
    EXPECT_EQ(l.PageAt(400), 2);
    EXPECT_EQ(l.PageAt(10000), 2);
    PageLayout empty({}, 1.0, 10);
    EXPECT_EQ(empty.PageAt(0), -1);
    EXPECT_NEAR(empty.ContentHeight(), 0, 1e-9);
}

TEST(PageLayout, GarbageSizesTakeNoSpace) {
    PageLayout l({{std::nan(""), 100}, {-5, INFINITY}, {100, 100}}, 1.0, 10);
    // Each dimension is sanitized on its own: a NaN width zeroes the width only.
    EXPECT_NEAR(l.PageRect(0).width, 0, 1e-9);
    EXPECT_NEAR(l.PageRect(0).height, 100, 1e-9);
    EXPECT_NEAR(l.PageRect(1).width, 0, 1e-9);
    EXPECT_NEAR(l.PageRect(1).height, 0, 1e-9);
    EXPECT_NEAR(l.PageRect(2).y, 10 + 100 + 10 + 0 + 10, 1e-9);
    EXPECT_NEAR(l.ContentWidth(), 120, 1e-9);
    EXPECT_TRUE(std::isfinite(l.ContentHeight()));
}

TEST(PageLayout, FitPagesInBoxKeepsAspectWithinBox) {
    auto fit = FitPagesInBox({{612, 792}, {792, 612}, {10, 200}, {std::nan(""), 5}}, 100, 130);
    EXPECT_EQ(fit.size(), static_cast<std::size_t>(4));
    // Portrait Letter: height-bound would be 130/792; width-bound 100/612 is smaller.
    EXPECT_NEAR(fit[0].first, 100, 1e-6);
    EXPECT_NEAR(fit[0].second, 792 * 100.0 / 612, 1e-6);
    // Landscape: width-bound.
    EXPECT_NEAR(fit[1].first, 100, 1e-6);
    EXPECT_NEAR(fit[1].second, 612 * 100.0 / 792, 1e-6);
    // Tall banner: height-bound, so it stays one box tall.
    EXPECT_NEAR(fit[2].second, 130, 1e-6);
    EXPECT_NEAR(fit[2].first, 6.5, 1e-6);
    EXPECT_NEAR(fit[3].first, 0, 1e-9);
    auto none = FitPagesInBox({{100, 100}}, 0, 100);
    EXPECT_NEAR(none[0].first, 0, 1e-9);
}

TEST(PageLayout, PageWithTailAtCoversLabelBelowEachPage) {
    // Tops 10, 220, 330 with 10-DIP gaps (see ThreePages). Tail 8 = label.
    auto l = ThreePages();
    EXPECT_EQ(l.PageWithTailAt(5, 8), -1);      // top padding
    EXPECT_EQ(l.PageWithTailAt(100, 8), 0);
    EXPECT_EQ(l.PageWithTailAt(215, 8), 0);     // label under page 0
    EXPECT_EQ(l.PageWithTailAt(217.9, 8), 0);   // bottom of that label (PageAt says 1)
    EXPECT_EQ(l.PageWithTailAt(219, 8), -1);    // rest of the gap
    EXPECT_EQ(l.PageWithTailAt(220, 8), 1);
    EXPECT_EQ(l.PageWithTailAt(10000, 8), -1);
    EXPECT_EQ(l.PageWithTailAt(std::nan(""), 8), -1);
    PageLayout empty({}, 1.0, 10);
    EXPECT_EQ(empty.PageWithTailAt(0, 8), -1);
}
