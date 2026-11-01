#include "PageGeometry.h"

namespace pdf {

int NormalizeRotation(int degrees) {
    if (degrees % 90 != 0) return 0;
    int r = degrees % 360;
    return r < 0 ? r + 360 : r;
}

void PageSizeDip(const PageGeometry& g, double* widthDip, double* heightDip) {
    const double s = kDipPerPoint * g.zoom;
    const double w = g.box.Width() * s;
    const double h = g.box.Height() * s;
    const bool swap = NormalizeRotation(g.rotation) % 180 != 0;
    *widthDip = swap ? h : w;
    *heightDip = swap ? w : h;
}

// Step 1 moves the box origin to (0,0) and flips y so (u,v) is top-left based,
// in unrotated DIPs. Step 2 rotates the unrotated page clockwise into the
// displayed frame. W/H are the unrotated page size in DIPs.
PointF PageToView(const PageGeometry& g, PointF p) {
    const double s = kDipPerPoint * g.zoom;
    const double W = g.box.Width() * s;
    const double H = g.box.Height() * s;
    const double u = (p.x - g.box.left) * s;
    const double v = (g.box.top - p.y) * s;
    switch (NormalizeRotation(g.rotation)) {
        case 90:  return {H - v, u};
        case 180: return {W - u, H - v};
        case 270: return {v, W - u};
        default:  return {u, v};
    }
}

PointF ViewToPage(const PageGeometry& g, PointF q) {
    const double s = kDipPerPoint * g.zoom;
    const double W = g.box.Width() * s;
    const double H = g.box.Height() * s;
    double u, v;
    switch (NormalizeRotation(g.rotation)) {
        case 90:  u = q.y;     v = H - q.x; break;
        case 180: u = W - q.x; v = H - q.y; break;
        case 270: u = W - q.y; v = q.x;     break;
        default:  u = q.x;     v = q.y;     break;
    }
    return {g.box.left + u / s, g.box.top - v / s};
}

}  // namespace pdf
