// PageOps: rotate, delete, insert, move, import. Each page gets a unique
// width (100, 110, 120, ...) so the tests can tell which page is where.

#include "../framework/Test.h"
#include "../framework/TestEngine.h"

#include <PDFCore/Document.h>
#include <PDFCore/PageOps.h>

#include <vector>

using namespace pdf;

namespace {
Result<Document> PagesWithWidths(int n) {
    auto doc = Document::CreateEmpty();
    if (!doc) return doc.error();
    for (int i = 0; i < n; ++i)
        if (auto s = doc.value().AddBlankPage(100 + 10 * i, 300); !s) return s.error();
    return doc;
}

// Page order as the widths' "identity": 0 for width 100, 1 for 110, ...
std::vector<int> Order(const Document& doc) {
    std::vector<int> ids;
    auto sizes = doc.DisplayedPageSizesPt();
    if (!sizes) return ids;
    for (const auto& [w, h] : sizes.value()) {
        // Rotation 90/270 swaps the sides; the unique side is whichever is not 300.
        const double side = (h > 299 && h < 301) ? w : h;
        ids.push_back(static_cast<int>((side - 100) / 10 + 0.5));
    }
    return ids;
}
}  // namespace

TEST(PageOps, ValidPageListRejectsBadInput) {
    EXPECT_TRUE(ValidPageList({0, 2}, 3));
    EXPECT_FALSE(ValidPageList({}, 3));
    EXPECT_FALSE(ValidPageList({3}, 3));
    EXPECT_FALSE(ValidPageList({-1}, 3));
    EXPECT_FALSE(ValidPageList({1, 1}, 3));
}

TEST(PageOps, DeleteKeepsOrderOfTheRest) {
    pdftest::OnEngine([] {
        auto doc = PagesWithWidths(5);
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        EXPECT_TRUE(DeletePages(doc.value(), {1, 3}).ok());  // any order
        EXPECT_TRUE(Order(doc.value()) == std::vector<int>({0, 2, 4}));
        EXPECT_FALSE(DeletePages(doc.value(), {0, 1, 2}).ok());  // all pages
        EXPECT_FALSE(DeletePages(doc.value(), {7}).ok());
        EXPECT_EQ(doc.value().PageCount(), 3);  // refusals changed nothing
    });
}

TEST(PageOps, MoveFollowsPdfiumContractAndRefusesBadTargets) {
    pdftest::OnEngine([] {
        auto doc = PagesWithWidths(4);  // A B C D
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        EXPECT_TRUE(MovePages(doc.value(), {3, 2}, 1).ok());  // -> A D C B
        EXPECT_TRUE(Order(doc.value()) == std::vector<int>({0, 3, 2, 1}));
        EXPECT_FALSE(MovePages(doc.value(), {0, 1}, 3).ok());  // 3 + 2 > 4
        EXPECT_FALSE(MovePages(doc.value(), {2, 2}, 0).ok());  // duplicate
        EXPECT_TRUE(Order(doc.value()) == std::vector<int>({0, 3, 2, 1}));  // unchanged
    });
}

TEST(PageOps, RotateAddsQuarterTurns) {
    pdftest::OnEngine([] {
        auto doc = PagesWithWidths(2);
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        EXPECT_TRUE(RotatePages(doc.value(), {1}, 1).ok());
        EXPECT_TRUE(RotatePages(doc.value(), {1}, 1).ok());  // 180 total
        auto g = doc.value().PageGeometryAt(1);
        if (g) EXPECT_EQ(g.value().rotation, 180);
        EXPECT_TRUE(RotatePages(doc.value(), {0}, -1).ok());  // counter-clockwise
        auto g0 = doc.value().PageGeometryAt(0);
        if (g0) EXPECT_EQ(g0.value().rotation, 270);
        EXPECT_TRUE(Order(doc.value()) == std::vector<int>({0, 1}));  // sizes intact
    });
}

TEST(PageOps, InsertBlankAndImportFromAnotherDocument) {
    pdftest::OnEngine([] {
        auto doc = PagesWithWidths(2);   // ids 0, 1
        auto src = PagesWithWidths(4);   // ids 0..3
        if (!doc || !src) { EXPECT_TRUE(false); return; }
        EXPECT_TRUE(InsertBlankPage(doc.value(), 1, 150, 300).ok());  // id 5 between
        EXPECT_TRUE(Order(doc.value()) == std::vector<int>({0, 5, 1}));
        EXPECT_FALSE(InsertBlankPage(doc.value(), 9, 100, 100).ok());
        EXPECT_TRUE(ImportPages(doc.value(), src.value(), {3, 2}, 0).ok());
        EXPECT_TRUE(Order(doc.value()) == std::vector<int>({3, 2, 0, 5, 1}));
        EXPECT_FALSE(ImportPages(doc.value(), src.value(), {4}, 0).ok());
        // Survives a save/reopen round trip.
        auto bytes = doc.value().SaveToMemory();
        if (!bytes) { EXPECT_TRUE(false); return; }
        auto back = Document::OpenMemory(std::move(bytes.value()));
        if (back) EXPECT_TRUE(Order(back.value()) == std::vector<int>({3, 2, 0, 5, 1}));
    });
}
