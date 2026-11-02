// UnitMapping.h — PDF user space <-> fractions of the displayed page.
//
// Internal to PDFCore (it takes a loaded FPDF_PAGE). Both directions go
// through PDFium's own display matrix (FPDF_PageToDevice / FPDF_DeviceToPage),
// the one FPDF_RenderPageBitmap uses, so rotation and CropBox offset are
// handled exactly as the bitmap is drawn. TextSearch (highlights, selection)
// and Annotations (drawing, hit-test, creating from a drag) share it so the
// two can never disagree.
//
// THREADING: engine thread only.
#pragma once

#include "PageGeometry.h"
#include "TextSearch.h"  // UnitRect

#include <fpdfview.h>

#include <algorithm>

namespace pdf::detail {

// Virtual device size. FPDF_PageToDevice returns ints, so a large square
// keeps rounding below 1e-5 of the page.
constexpr int kUnitDevice = 100000;

// User-space box -> fraction of the displayed page, clamped to the page (a
// glyph or annotation can stick out past the CropBox).
inline UnitRect BoxToUnit(FPDF_PAGE page, const BoxPt& b) {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    FPDF_PageToDevice(page, 0, 0, kUnitDevice, kUnitDevice, 0, b.left, b.bottom, &x0, &y0);
    FPDF_PageToDevice(page, 0, 0, kUnitDevice, kUnitDevice, 0, b.right, b.top, &x1, &y1);
    const double k = 1.0 / kUnitDevice;
    auto c = [](double v) { return std::clamp(v, 0.0, 1.0); };
    return {c(std::min(x0, x1) * k), c(std::min(y0, y1) * k), c(std::max(x0, x1) * k),
            c(std::max(y0, y1) * k)};
}

// Fraction of the displayed page -> user space. The input is clamped to the
// page first. Returns false only if PDFium rejects the mapping.
inline bool UnitToPoint(FPDF_PAGE page, double ux, double uy, PointF* out) {
    const int dx = static_cast<int>(std::clamp(ux, 0.0, 1.0) * kUnitDevice);
    const int dy = static_cast<int>(std::clamp(uy, 0.0, 1.0) * kUnitDevice);
    double px = 0, py = 0;
    if (!FPDF_DeviceToPage(page, 0, 0, kUnitDevice, kUnitDevice, 0, dx, dy, &px, &py))
        return false;
    *out = {px, py};
    return true;
}

}  // namespace pdf::detail
