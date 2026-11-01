// PageGeometry.h — the single page-space → view-space transform.
//
// PDF user space is in points (1/72 inch), origin at the bottom-left of the
// MediaBox, y pointing up; the page may carry /Rotate (0/90/180/270, clockwise
// when displayed) and the MediaBox origin need not be (0,0). The view is in DIPs
// (1/96 inch), origin top-left, y pointing down. Every caller that maps between
// the two goes through these functions so rotation and offset are handled in
// exactly one place, and they are pure so tests cover every rotation headless.
#pragma once

namespace pdf {

struct PointF {
    double x = 0, y = 0;
};

struct BoxPt {  // a PDF rectangle in points: left, bottom, right, top
    double left = 0, bottom = 0, right = 0, top = 0;
    double Width() const { return right - left; }
    double Height() const { return top - bottom; }
};

struct PageGeometry {
    BoxPt box;         // MediaBox or CropBox, whichever is displayed
    int rotation = 0;  // 0, 90, 180, 270 (normalized)
    double zoom = 1.0; // 1.0 = 100 %
};

// Normalizes any multiple of 90 (including negative) to 0/90/180/270. Values
// that are not multiples of 90 are invalid in PDF and are treated as 0.
int NormalizeRotation(int degrees);

constexpr double kDipPerPoint = 96.0 / 72.0;

// Displayed page size in DIPs, after rotation and zoom.
void PageSizeDip(const PageGeometry& g, double* widthDip, double* heightDip);

// Page space (points) → view space (DIPs, relative to the page's top-left).
PointF PageToView(const PageGeometry& g, PointF pagePt);

// View space → page space; the exact inverse of PageToView.
PointF ViewToPage(const PageGeometry& g, PointF viewDip);

}  // namespace pdf
