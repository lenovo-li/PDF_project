#include "Render.h"

#include <algorithm>
#include <cmath>

namespace pdf {

std::uint32_t Bitmap::At(int x, int y) const {
    if (x < 0 || y < 0 || x >= width || y >= height) return 0;
    const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
    return (std::uint32_t{pixels[i + 3]} << 24) | (std::uint32_t{pixels[i + 2]} << 16) |
           (std::uint32_t{pixels[i + 1]} << 8) | std::uint32_t{pixels[i]};
}

Status CheckRenderSize(int widthPx, int heightPx) {
    if (widthPx <= 0 || heightPx <= 0) return Error{L"渲染尺寸无效"};
    if (widthPx > kMaxRenderSidePx || heightPx > kMaxRenderSidePx)
        return Error{L"页面渲染尺寸超出上限"};
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(widthPx) * static_cast<std::uint64_t>(heightPx) * 4;
    if (bytes > kMaxRenderBytes) return Error{L"页面渲染尺寸超出上限"};
    return Ok{};
}

Result<SizePx> RenderSizePx(const PageGeometry& g, double dpi) {
    if (!(dpi > 0 && std::isfinite(dpi))) return Error{L"DPI 无效"};
    double wDip = 0, hDip = 0;
    PageSizeDip(g, &wDip, &hDip);
    // Written so NaN fails: a NaN box or zoom compares false everywhere.
    if (!(wDip > 0 && hDip > 0 && std::isfinite(wDip) && std::isfinite(hDip)))
        return Error{L"页面尺寸无效"};
    // A sliver page (e.g. 0.1 pt wide) still gets one pixel rather than failing.
    const double w = std::max(1.0, std::round(wDip * dpi / 96.0));
    const double h = std::max(1.0, std::round(hDip * dpi / 96.0));
    // Compare as double before the int cast, which would overflow.
    if (w > kMaxRenderSidePx || h > kMaxRenderSidePx) return Error{L"页面渲染尺寸超出上限"};
    SizePx s{static_cast<int>(w), static_cast<int>(h)};
    auto ok = CheckRenderSize(s.width, s.height);
    if (!ok) return ok.error();
    return s;
}

SizePx FitRenderSize(double wantW, double wantH, std::uint64_t maxBytes) {
    if (!(wantW > 0 && wantH > 0 && std::isfinite(wantW) && std::isfinite(wantH))) return {};
    const double maxPixels = static_cast<double>(std::min(maxBytes, kMaxRenderBytes) / 4);
    if (maxPixels < 1) return {};
    double scale = 1.0;
    scale = std::min(scale, kMaxRenderSidePx / wantW);
    scale = std::min(scale, kMaxRenderSidePx / wantH);
    scale = std::min(scale, std::sqrt(maxPixels / wantW / wantH));
    // floor keeps the result inside the limits after rounding; max(1) keeps a
    // very thin page visible.
    const double w = std::max(1.0, std::floor(wantW * scale));
    const double h = std::max(1.0, std::floor(wantH * scale));
    return {static_cast<int>(w), static_cast<int>(h)};
}

}  // namespace pdf
