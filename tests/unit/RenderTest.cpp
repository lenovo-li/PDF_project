// Render: pixel-size rules, PDFium rendering of a known page (including
// /Rotate), refusal of hostile sizes, and the bounded bitmap cache.
//
// The test PDF is written byte by byte here: a 200 x 100 pt page with a solid
// red rectangle filling its left half, so expected pixel colors are exact.

#include "../framework/Test.h"
#include "../framework/TestEngine.h"

#include <PDFCore/BitmapCache.h>
#include <PDFCore/Document.h>
#include <PDFCore/Render.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

using namespace pdf;

namespace {

constexpr std::uint32_t kRed = 0xFFFF0000;
constexpr std::uint32_t kWhite = 0xFFFFFFFF;

// Builds a one-page PDF with a correct xref table. `rotate` goes into /Rotate.
std::vector<std::uint8_t> RedHalfPagePdf(int rotate, const char* mediaBox = "0 0 200 100") {
    const std::string content = "1 0 0 rg 0 0 100 100 re f\n";
    const std::string objs[] = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [" + std::string(mediaBox) + "] /Rotate " +
            std::to_string(rotate) + " /Contents 4 0 R >>",
        "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content +
            "endstream",
    };
    std::string pdf = "%PDF-1.4\n";
    std::vector<std::size_t> offsets;
    for (int i = 0; i < 4; ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    const std::size_t xref = pdf.size();
    pdf += "xref\n0 5\n0000000000 65535 f \n";
    for (std::size_t off : offsets) {
        char line[24];
        std::snprintf(line, sizeof(line), "%010zu 00000 n \n", off);
        pdf += line;
    }
    pdf += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) +
           "\n%%EOF\n";
    return {pdf.begin(), pdf.end()};
}

std::shared_ptr<const Bitmap> FakeBitmap(int w, int h) {
    auto b = std::make_shared<Bitmap>();
    b->width = w;
    b->height = h;
    b->pixels.resize(static_cast<std::size_t>(w) * h * 4);
    return b;
}

}  // namespace

TEST(RenderSize, MapsPointsToPixelsAtDpi) {
    PageGeometry g;
    g.box = {0, 0, 612, 792};  // US Letter
    auto s = RenderSizePx(g, 96);
    EXPECT_TRUE(s.ok());
    if (s) {
        EXPECT_EQ(s.value().width, 816);   // 612 pt * 96/72
        EXPECT_EQ(s.value().height, 1056);
    }
    auto hi = RenderSizePx(g, 192);  // 200 % display
    EXPECT_TRUE(hi.ok());
    if (hi) EXPECT_EQ(hi.value().width, 1632);
}

TEST(RenderSize, RejectsNonFiniteAndOverflowingInput) {
    PageGeometry g;
    g.box = {0, 0, 100, 100};
    g.zoom = std::nan("");
    EXPECT_FALSE(RenderSizePx(g, 96).ok());
    g.zoom = INFINITY;
    EXPECT_FALSE(RenderSizePx(g, 96).ok());
    g.zoom = 1e12;  // far past INT_MAX pixels; must not reach the int cast
    EXPECT_FALSE(RenderSizePx(g, 96).ok());
    g.zoom = 1.0;
    EXPECT_FALSE(RenderSizePx(g, std::nan("")).ok());
    g.box = {0, 0, std::nan(""), 100};
    EXPECT_FALSE(RenderSizePx(g, 96).ok());
}

TEST(RenderSize, LimitsAreInclusiveAtExactBoundary) {
    EXPECT_TRUE(CheckRenderSize(8192, 8192).ok());   // exactly 256 MiB
    EXPECT_FALSE(CheckRenderSize(8192, 8193).ok());
    EXPECT_TRUE(CheckRenderSize(16384, 1).ok());     // exactly the side cap
    EXPECT_FALSE(CheckRenderSize(16385, 1).ok());
}

TEST(RenderSize, RotationSwapsSides) {
    PageGeometry g;
    g.box = {0, 0, 200, 100};
    g.rotation = 90;
    auto s = RenderSizePx(g, 72);
    EXPECT_TRUE(s.ok());
    if (s) {
        EXPECT_EQ(s.value().width, 100);
        EXPECT_EQ(s.value().height, 200);
    }
}

TEST(RenderSize, RejectsHostileAndInvalidSizes) {
    PageGeometry g;
    g.box = {0, 0, 14400, 14400};  // largest legal page
    g.zoom = 4.0;
    EXPECT_FALSE(RenderSizePx(g, 192).ok());
    PageGeometry empty;
    EXPECT_FALSE(RenderSizePx(empty, 96).ok());
    PageGeometry ok;
    ok.box = {0, 0, 100, 100};
    EXPECT_FALSE(RenderSizePx(ok, 0).ok());
    EXPECT_FALSE(RenderSizePx(ok, -96).ok());
    // Under the per-side cap but over the byte cap: 16000 x 16000 x 4 > 256 MiB.
    EXPECT_FALSE(CheckRenderSize(16000, 16000).ok());
    EXPECT_FALSE(CheckRenderSize(16385, 1).ok());
    EXPECT_TRUE(CheckRenderSize(8000, 8000).ok());
}

TEST(RenderSize, FitKeepsAspectAndLimits) {
    auto s = FitRenderSize(800, 600);  // already fits: unchanged
    EXPECT_EQ(s.width, 800);
    EXPECT_EQ(s.height, 600);
    s = FitRenderSize(40000, 20000);   // over the side cap
    EXPECT_TRUE(CheckRenderSize(s.width, s.height).ok());
    EXPECT_NEAR(static_cast<double>(s.width) / s.height, 2.0, 0.01);
    s = FitRenderSize(12000, 12000);   // under the side cap, over the byte cap
    EXPECT_TRUE(CheckRenderSize(s.width, s.height).ok());
    EXPECT_EQ(s.width, s.height);
    EXPECT_TRUE(s.width >= 8191);      // uses nearly the whole budget
    s = FitRenderSize(1e9, 1);         // extreme aspect: still valid, never 0
    EXPECT_TRUE(CheckRenderSize(s.width, s.height).ok());
    s = FitRenderSize(4000, 4000, 16ull * 1024 * 1024);  // viewer's smaller budget
    EXPECT_TRUE(static_cast<std::uint64_t>(s.width) * s.height * 4 <= 16ull * 1024 * 1024);
    EXPECT_TRUE(s.width >= 2047);
    EXPECT_EQ(FitRenderSize(10, 10, 0).width, 0);
    s = FitRenderSize(std::nan(""), 10);
    EXPECT_EQ(s.width, 0);
    s = FitRenderSize(-5, 10);
    EXPECT_EQ(s.width, 0);
}

TEST(RenderPage, DrawsKnownContent) {
    pdftest::OnEngine([] {
        auto doc = Document::OpenMemory(RedHalfPagePdf(0));
        EXPECT_TRUE(doc.ok());
        if (!doc) return;
        auto bmp = doc.value().RenderPage(0, 200, 100);  // 1 px per point
        EXPECT_TRUE(bmp.ok());
        if (!bmp) return;
        const Bitmap& b = bmp.value();
        EXPECT_EQ(b.width, 200);
        EXPECT_EQ(b.height, 100);
        EXPECT_EQ(b.pixels.size(), static_cast<std::size_t>(200 * 100 * 4));
        EXPECT_EQ(b.At(10, 50), kRed);     // left half
        EXPECT_EQ(b.At(190, 50), kWhite);  // right half
    });
}

TEST(RenderPage, AppliesPageRotation) {
    // /Rotate 90 turns the page clockwise: its left half becomes the top half
    // of a 100 x 200 display.
    pdftest::OnEngine([] {
        auto doc = Document::OpenMemory(RedHalfPagePdf(90));
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto g = doc.value().PageGeometryAt(0);
        auto size = g ? RenderSizePx(g.value(), 72) : Result<SizePx>(g.error());
        EXPECT_TRUE(size.ok());
        if (!size) return;
        EXPECT_EQ(size.value().width, 100);
        EXPECT_EQ(size.value().height, 200);
        auto bmp = doc.value().RenderPage(0, size.value().width, size.value().height);
        EXPECT_TRUE(bmp.ok());
        if (!bmp) return;
        EXPECT_EQ(bmp.value().At(50, 10), kRed);
        EXPECT_EQ(bmp.value().At(50, 190), kWhite);
    });
}

TEST(Document, DisplayedPageSizesApplyRotationWithoutLoadingPages) {
    pdftest::OnEngine([] {
        auto doc = Document::OpenMemory(RedHalfPagePdf(90));
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto sizes = doc.value().DisplayedPageSizesPt();
        EXPECT_TRUE(sizes.ok());
        if (!sizes || sizes.value().size() != 1) { EXPECT_TRUE(false); return; }
        // MediaBox 200 x 100 with /Rotate 90 displays as 100 x 200.
        EXPECT_NEAR(sizes.value()[0].first, 100, 0.01);
        EXPECT_NEAR(sizes.value()[0].second, 200, 0.01);
    });
}

TEST(Document, HostilePageSizesFallBackToLetter) {
    // One giant page must not widen the layout column for every other page.
    for (const char* box : {"0 0 1e30 1e30", "0 0 200000 100", "0 0 3e38 3e38"}) {
        pdftest::OnEngine([box] {
            auto doc = Document::OpenMemory(RedHalfPagePdf(0, box));
            if (!doc) return;  // PDFium may reject the file outright: also fine
            auto sizes = doc.value().DisplayedPageSizesPt();
            EXPECT_TRUE(sizes.ok());
            if (!sizes || sizes.value().empty()) return;
            EXPECT_NEAR(sizes.value()[0].first, 612, 0.01);
            EXPECT_NEAR(sizes.value()[0].second, 792, 0.01);
        });
    }
}

TEST(Document, LargeLegalPageSizeIsKept) {
    pdftest::OnEngine([] {
        auto doc = Document::OpenMemory(RedHalfPagePdf(0, "0 0 14400 7200"));
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto sizes = doc.value().DisplayedPageSizesPt();
        if (!sizes || sizes.value().size() != 1) { EXPECT_TRUE(false); return; }
        EXPECT_NEAR(sizes.value()[0].first, 14400, 0.01);
        EXPECT_NEAR(sizes.value()[0].second, 7200, 0.01);
    });
}

TEST(RenderPage, RejectsBadIndexAndSize) {
    pdftest::OnEngine([] {
        auto doc = Document::OpenMemory(RedHalfPagePdf(0));
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        EXPECT_FALSE(doc.value().RenderPage(1, 10, 10).ok());
        EXPECT_FALSE(doc.value().RenderPage(-1, 10, 10).ok());
        EXPECT_FALSE(doc.value().RenderPage(0, 0, 10).ok());
        EXPECT_FALSE(doc.value().RenderPage(0, 16000, 16000).ok());
    });
}

TEST(BitmapCache, EvictsLeastRecentlyUsedWithinBudget) {
    BitmapCache cache(3 * 400);  // room for three 10x10 bitmaps
    cache.Put({0, 10, 10}, FakeBitmap(10, 10));
    cache.Put({1, 10, 10}, FakeBitmap(10, 10));
    cache.Put({2, 10, 10}, FakeBitmap(10, 10));
    EXPECT_TRUE(cache.Get({0, 10, 10}) != nullptr);  // 0 becomes most recent
    cache.Put({3, 10, 10}, FakeBitmap(10, 10));       // evicts 1, the LRU
    EXPECT_TRUE(cache.Get({1, 10, 10}) == nullptr);
    EXPECT_TRUE(cache.Get({0, 10, 10}) != nullptr);
    EXPECT_TRUE(cache.Get({3, 10, 10}) != nullptr);
    EXPECT_EQ(cache.Count(), static_cast<std::size_t>(3));
    EXPECT_EQ(cache.Bytes(), static_cast<std::uint64_t>(1200));
}

TEST(BitmapCache, RotationIsPartOfTheKey) {
    // 0° and 180° have the same pixel size; the cache must still tell them apart.
    BitmapCache cache(1u << 20);
    cache.Put({0, 10, 20, 0}, FakeBitmap(10, 20));
    EXPECT_TRUE(cache.Get({0, 10, 20, 180}) == nullptr);
    EXPECT_TRUE(cache.Get({0, 10, 20, 0}) != nullptr);
}

TEST(BitmapCache, RefusesEntryLargerThanBudget) {
    BitmapCache cache(1000);
    cache.Put({0, 10, 10}, FakeBitmap(10, 10));
    EXPECT_FALSE(cache.Put({1, 20, 20}, FakeBitmap(20, 20)));  // 1600 bytes
    EXPECT_TRUE(cache.Get({0, 10, 10}) != nullptr);  // not flushed
}

TEST(BitmapCache, ReplaceAndInvalidateKeepByteCountExact) {
    BitmapCache cache(1u << 20);
    cache.Put({0, 10, 10}, FakeBitmap(10, 10));
    cache.Put({0, 10, 10}, FakeBitmap(10, 10));  // replace, not duplicate
    cache.Put({0, 20, 20}, FakeBitmap(20, 20));  // same page, other zoom
    cache.Put({1, 10, 10}, FakeBitmap(10, 10));
    EXPECT_EQ(cache.Bytes(), static_cast<std::uint64_t>(400 + 1600 + 400));
    cache.InvalidatePage(0);
    EXPECT_EQ(cache.Count(), static_cast<std::size_t>(1));
    EXPECT_EQ(cache.Bytes(), static_cast<std::uint64_t>(400));
    cache.Clear();
    EXPECT_EQ(cache.Bytes(), static_cast<std::uint64_t>(0));
}
