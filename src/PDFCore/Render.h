// Render.h — page bitmaps and the size rules for rendering untrusted pages.
//
// A page's size comes from the file, so a hostile PDF can ask for a page of
// 14400 x 14400 pt; at 400 % zoom on a 200 % display that is a multi-gigabyte
// bitmap. Every render size goes through RenderSizePx / CheckRenderSize, which
// compute in 64-bit and refuse anything over the limits below before a single
// byte is allocated.
#pragma once

#include "PageGeometry.h"
#include "Result.h"

#include <cstdint>
#include <vector>

namespace pdf {

// 16384 px per side matches the D3D11 texture limit, so any bitmap that passes
// can be uploaded by the viewer without tiling.
constexpr int kMaxRenderSidePx = 16384;
// 256 MiB of BGRA = 64 Mpx, e.g. 8000 x 8000. Large enough for printing a
// poster-sized page at 300 dpi, small enough to survive on a 4 GB machine.
constexpr std::uint64_t kMaxRenderBytes = 256ull * 1024 * 1024;

// BGRA, top-down rows, stride = width * 4. Pages render onto opaque white, so
// every alpha is 255 and the data is valid as premultiplied too; the viewer can
// copy `pixels` straight into a DXGI_FORMAT_B8G8R8A8_UNORM bitmap.
struct Bitmap {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;

    int Stride() const { return width * 4; }
    std::uint64_t Bytes() const { return pixels.size(); }
    // Pixel at (x, y) as 0xAARRGGBB; for tests and diagnostics.
    std::uint32_t At(int x, int y) const;
};

// Fails with a message if w x h is non-positive or over the limits above.
Status CheckRenderSize(int widthPx, int heightPx);

// Pixel size of the displayed page (rotation and zoom applied) at `dpi`
// (96 = 100 % display scale). Rounds to the nearest pixel, at least 1 px per
// side, and fails if the result breaks the limits or the geometry is invalid.
struct SizePx {
    int width = 0;
    int height = 0;
};
Result<SizePx> RenderSizePx(const PageGeometry& g, double dpi);

// The largest size with the same aspect ratio as `wantW x wantH` that passes
// CheckRenderSize, uses at most `maxBytes` (capped at kMaxRenderBytes), and is
// no larger than wanted. The viewer uses it at high
// zoom: it renders the capped bitmap and lets D2D stretch it, a little soft
// instead of blank. Returns {0, 0} for non-positive or non-finite input.
SizePx FitRenderSize(double wantW, double wantH, std::uint64_t maxBytes = kMaxRenderBytes);

}  // namespace pdf
