#include "PageLayout.h"

#include <algorithm>
#include <cmath>

namespace pdf {

namespace {
// Guards against NaN or negative sizes and zoom from a malformed file or a
// bad caller: such a page takes no space instead of corrupting every offset.
double Sane(double v) { return (v > 0 && std::isfinite(v)) ? v : 0.0; }
}  // namespace

PageLayout::PageLayout(const std::vector<std::pair<double, double>>& sizesDip, double zoom,
                       double gapDip) {
    zoom = Sane(zoom);
    gapDip = Sane(gapDip);
    rects_.reserve(sizesDip.size());
    double widest = 0;
    for (const auto& [w, h] : sizesDip) widest = std::max(widest, Sane(w) * zoom);
    contentWidth_ = widest + 2 * gapDip;

    double y = gapDip;
    for (const auto& [w, h] : sizesDip) {
        PageRectDip r;
        r.width = Sane(w) * zoom;
        r.height = Sane(h) * zoom;
        r.x = gapDip + (widest - r.width) / 2;
        r.y = y;
        rects_.push_back(r);
        y += r.height + gapDip;
    }
    contentHeight_ = rects_.empty() ? 0 : y;
}

PageLayout::Range PageLayout::VisibleRange(double top, double height) const {
    Range r;
    if (rects_.empty() || !(height > 0)) return r;
    const double bottom = top + height;
    // First page whose bottom edge is below `top`.
    auto first = std::partition_point(rects_.begin(), rects_.end(), [&](const PageRectDip& p) {
        return p.y + p.height <= top;
    });
    // First page whose top edge is at or below `bottom`; everything before it
    // (from `first`) intersects.
    auto end = std::partition_point(first, rects_.end(),
                                    [&](const PageRectDip& p) { return p.y < bottom; });
    if (first == end) return r;
    r.first = static_cast<int>(first - rects_.begin());
    r.last = static_cast<int>(end - rects_.begin()) - 1;
    return r;
}

int PageLayout::PageAt(double y) const {
    if (rects_.empty()) return -1;
    auto it = std::partition_point(rects_.begin(), rects_.end(),
                                   [&](const PageRectDip& p) { return p.y + p.height <= y; });
    if (it == rects_.end()) return PageCount() - 1;
    const int i = static_cast<int>(it - rects_.begin());
    // `y` is in the gap above page i: pick whichever neighbour is closer.
    if (y < it->y && i > 0) {
        const PageRectDip& prev = rects_[i - 1];
        if (y - (prev.y + prev.height) < it->y - y) return i - 1;
    }
    return i;
}

}  // namespace pdf

namespace pdf {

std::vector<std::pair<double, double>> FitPagesInBox(
    const std::vector<std::pair<double, double>>& sizes, double boxW, double boxH) {
    std::vector<std::pair<double, double>> out;
    out.reserve(sizes.size());
    const bool boxOk = boxW > 0 && boxH > 0 && std::isfinite(boxW) && std::isfinite(boxH);
    for (const auto& [w, h] : sizes) {
        if (!boxOk || !(w > 0 && h > 0 && std::isfinite(w) && std::isfinite(h))) {
            out.emplace_back(0.0, 0.0);
            continue;
        }
        const double k = std::min(boxW / w, boxH / h);
        out.emplace_back(w * k, h * k);
    }
    return out;
}

}  // namespace pdf

namespace pdf {

int PageLayout::PageWithTailAt(double y, double tail) const {
    if (rects_.empty() || !std::isfinite(y)) return -1;
    if (!(tail >= 0)) tail = 0;
    // First page whose extended bottom is below y; it is the only candidate,
    // because extended areas do not overlap while tail <= gap.
    auto it = std::partition_point(rects_.begin(), rects_.end(), [&](const PageRectDip& p) {
        return p.y + p.height + tail <= y;
    });
    if (it == rects_.end() || y < it->y) return -1;
    return static_cast<int>(it - rects_.begin());
}

}  // namespace pdf
