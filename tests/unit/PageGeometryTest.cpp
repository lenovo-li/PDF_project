// Page → view transform: every rotation, offset boxes, zoom, round trip.

#include "../framework/Test.h"

#include <PDFCore/PageGeometry.h>

using namespace pdf;

namespace {
// US Letter with a non-zero origin, the case that breaks naive "y = H - y".
PageGeometry Letter(int rotation) {
    PageGeometry g;
    g.box = {10, 20, 10 + 612, 20 + 792};
    g.rotation = rotation;
    return g;
}
constexpr double kTol = 1e-9;
}  // namespace

TEST(Geometry, NormalizeRotation) {
    EXPECT_EQ(NormalizeRotation(0), 0);
    EXPECT_EQ(NormalizeRotation(450), 90);
    EXPECT_EQ(NormalizeRotation(-90), 270);
    EXPECT_EQ(NormalizeRotation(45), 0);  // invalid in PDF -> unrotated
}

TEST(Geometry, SizeSwapsOnQuarterTurns) {
    double w = 0, h = 0;
    PageSizeDip(Letter(0), &w, &h);
    EXPECT_NEAR(w, 612 * kDipPerPoint, kTol);
    EXPECT_NEAR(h, 792 * kDipPerPoint, kTol);
    PageSizeDip(Letter(90), &w, &h);
    EXPECT_NEAR(w, 792 * kDipPerPoint, kTol);
    EXPECT_NEAR(h, 612 * kDipPerPoint, kTol);
}

TEST(Geometry, TopLeftCornerOfPageMapsPerRotation) {
    // The page's own top-left corner (left, top) in PDF space.
    const PointF topLeft{10, 20 + 792};
    double w = 0, h = 0;

    PointF v = PageToView(Letter(0), topLeft);
    EXPECT_NEAR(v.x, 0, kTol);
    EXPECT_NEAR(v.y, 0, kTol);

    // Rotated 90 clockwise, the old top-left ends up at the top-right.
    PageSizeDip(Letter(90), &w, &h);
    v = PageToView(Letter(90), topLeft);
    EXPECT_NEAR(v.x, w, kTol);
    EXPECT_NEAR(v.y, 0, kTol);

    PageSizeDip(Letter(180), &w, &h);
    v = PageToView(Letter(180), topLeft);
    EXPECT_NEAR(v.x, w, kTol);
    EXPECT_NEAR(v.y, h, kTol);

    PageSizeDip(Letter(270), &w, &h);
    v = PageToView(Letter(270), topLeft);
    EXPECT_NEAR(v.x, 0, kTol);
    EXPECT_NEAR(v.y, h, kTol);
}

TEST(Geometry, ZoomScalesLinearly) {
    PageGeometry g = Letter(0);
    g.zoom = 2.0;
    const PointF v = PageToView(g, {10 + 72, 20 + 792 - 72});  // 1 inch in from top-left
    EXPECT_NEAR(v.x, 192, kTol);  // 1 inch = 96 DIP, x2 zoom
    EXPECT_NEAR(v.y, 192, kTol);
}

TEST(Geometry, ViewToPageInvertsPageToView) {
    const PointF p{123.5, 456.25};
    for (int rot : {0, 90, 180, 270}) {
        PageGeometry g = Letter(rot);
        g.zoom = 1.75;
        const PointF back = ViewToPage(g, PageToView(g, p));
        EXPECT_NEAR(back.x, p.x, 1e-9);
        EXPECT_NEAR(back.y, p.y, 1e-9);
    }
}
