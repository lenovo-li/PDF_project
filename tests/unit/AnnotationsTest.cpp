#include "../framework/Test.h"
#include "../framework/TestEngine.h"

// Annotations: add each kind, list (with unitRect), edit text, remove, survive save/reopen.

#include <PDFCore/Annotations.h>
#include <PDFCore/Document.h>
#include <PDFCore/PageOps.h>

#include <windows.h>

using namespace pdf;

namespace {
std::wstring TempPath(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}

// One blank 200 x 300 pt page. Call on the engine thread (inside OnEngine).
Result<Document> MakeSinglePage() {
    auto doc = Document::CreateEmpty();
    if (!doc) return doc.error();
    if (auto s = doc.value().AddBlankPage(200, 300); !s) return s.error();
    return doc;
}

// A page with text (for markup-from-chars tests).
Result<Document> MakePageWithText() {
    auto doc = Document::CreateEmpty();
    if (!doc) return doc.error();
    if (auto s = doc.value().AddBlankPage(595, 842); !s) return s.error();
    if (auto s = doc.value().AddTextLine(0, L"First line of text", 72, 700, 16); !s)
        return s.error();
    if (auto s = doc.value().AddTextLine(0, L"Second line", 72, 680, 16); !s) return s.error();
    return doc;
}
}  // namespace

TEST(Annotations, ListEmpty) {
    pdftest::OnEngine([] {
        auto doc = MakeSinglePage();
        EXPECT_TRUE(doc.ok());
        if (!doc) return;
        auto list = ListAnnotations(doc.value(), 0);
        EXPECT_TRUE(list.ok());
        if (list) {
            EXPECT_EQ(list.value().annots.size(), 0u);
            EXPECT_FALSE(list.value().truncated);
        }
    });
}

TEST(Annotations, AddAndListHighlight) {
    pdftest::OnEngine([] {
        auto doc = MakeSinglePage();
        EXPECT_TRUE(doc.ok());
        if (!doc) return;
        std::vector<BoxPt> rects{{10, 100, 50, 110}, {10, 90, 80, 100}};
        AnnotColor yellow{255, 220, 0, 255};
        EXPECT_TRUE(AddMarkup(doc.value(), 0, AnnotKind::Highlight, rects, yellow).ok());
        auto list = ListAnnotations(doc.value(), 0);
        EXPECT_TRUE(list.ok());
        if (list) {
            EXPECT_EQ(list.value().annots.size(), 1u);
            const AnnotInfo& a = list.value().annots[0];
            EXPECT_EQ(a.kind, AnnotKind::Highlight);
            EXPECT_EQ(a.color.r, 255);
            EXPECT_EQ(a.color.g, 220);
            EXPECT_EQ(a.color.b, 0);
            // unitRect should be non-empty (exact values depend on page transform).
            EXPECT_TRUE(a.unitRect.right > a.unitRect.left);
            EXPECT_TRUE(a.unitRect.bottom > a.unitRect.top);
        }
    });
}

TEST(Annotations, AddNoteAndRead) {
    pdftest::OnEngine([] {
        auto doc = MakeSinglePage();
        EXPECT_TRUE(doc.ok());
        if (!doc) return;
        EXPECT_TRUE(AddNote(doc.value(), 0, {50, 150}, L"测试注释", {255, 0, 0, 255}).ok());
        auto list = ListAnnotations(doc.value(), 0);
        EXPECT_TRUE(list.ok());
        if (list) {
            EXPECT_EQ(list.value().annots.size(), 1u);
            EXPECT_EQ(list.value().annots[0].kind, AnnotKind::Note);
            EXPECT_EQ(list.value().annots[0].contents, L"测试注释");
        }
    });
}

TEST(Annotations, SetAnnotationText) {
    pdftest::OnEngine([] {
        auto doc = MakeSinglePage();
        EXPECT_TRUE(doc.ok());
        if (!doc) return;
        EXPECT_TRUE(AddNote(doc.value(), 0, {10, 10}, L"原文", {255, 0, 0, 255}).ok());
        EXPECT_TRUE(SetAnnotationText(doc.value(), 0, 0, L"新文").ok());
        auto list = ListAnnotations(doc.value(), 0);
        EXPECT_TRUE(list.ok());
        if (list) {
            EXPECT_EQ(list.value().annots.size(), 1u);
            EXPECT_EQ(list.value().annots[0].contents, L"新文");
        }
    });
}

TEST(Annotations, AddSquareAndInk) {
    pdftest::OnEngine([] {
        auto doc = MakeSinglePage();
        EXPECT_TRUE(doc.ok());
        if (!doc) return;
        EXPECT_TRUE(AddSquare(doc.value(), 0, {20, 50, 80, 100}, {0, 0, 255, 255}, 2.0f).ok());
        std::vector<std::vector<PointF>> strokes{{{10, 10}, {20, 20}, {30, 15}}};
        EXPECT_TRUE(AddInk(doc.value(), 0, strokes, {0, 255, 0, 255}, 1.5f).ok());
        auto list = ListAnnotations(doc.value(), 0);
        EXPECT_TRUE(list.ok());
        if (list) EXPECT_EQ(list.value().annots.size(), 2u);
    });
}

TEST(Annotations, RemoveAnnotation) {
    pdftest::OnEngine([] {
        auto doc = MakeSinglePage();
        EXPECT_TRUE(doc.ok());
        if (!doc) return;
        EXPECT_TRUE(AddNote(doc.value(), 0, {10, 10}, L"first", {255, 0, 0, 255}).ok());
        EXPECT_TRUE(AddNote(doc.value(), 0, {30, 30}, L"second", {0, 255, 0, 255}).ok());
        auto before = ListAnnotations(doc.value(), 0);
        EXPECT_TRUE(before.ok());
        if (before) EXPECT_EQ(before.value().annots.size(), 2u);
        EXPECT_TRUE(RemoveAnnotation(doc.value(), 0, 0).ok());
        auto after = ListAnnotations(doc.value(), 0);
        EXPECT_TRUE(after.ok());
        if (after) {
            EXPECT_EQ(after.value().annots.size(), 1u);
            EXPECT_EQ(after.value().annots[0].contents, L"second");
        }
    });
}

TEST(Annotations, AnnotationsSurviveSave) {
    const std::wstring path = TempPath(L"PDFTool_annot_save.pdf");
    pdftest::OnEngine([&] {
        auto doc = MakeSinglePage();
        EXPECT_TRUE(doc.ok());
        if (!doc) return;
        EXPECT_TRUE(AddNote(doc.value(), 0, {50, 50}, L"saved note", {255, 0, 0, 255}).ok());
        EXPECT_TRUE(doc.value().SaveAs(path).ok());
    });
    pdftest::OnEngine([&] {
        auto reopen = Document::Open(path);
        EXPECT_TRUE(reopen.ok());
        if (!reopen) return;
        auto list = ListAnnotations(reopen.value(), 0);
        EXPECT_TRUE(list.ok());
        if (list) {
            EXPECT_EQ(list.value().annots.size(), 1u);
            EXPECT_EQ(list.value().annots[0].contents, L"saved note");
        }
    });
    DeleteFileW(path.c_str());
}

// The viewer draws annotations through RenderPage (FPDF_ANNOT). An annotation
// with no appearance stream would be saved but invisible, so check pixels.
// A box drawn in the viewer must come back where it was drawn, on every
// rotation: unit rect -> user space (UnitRectToPage) -> annotation ->
// ListAnnotations' unitRect. The input is asymmetric in x and y, so a missed
// y flip or a rotation applied the wrong way moves it and fails.
TEST(Annotations, UnitRectRoundTripsOnEveryRotation) {
    for (int turns = 0; turns < 4; ++turns) {
        pdftest::OnEngine([turns] {
            auto doc = MakeSinglePage();  // 200 x 300 pt
            if (!doc) { EXPECT_TRUE(doc.ok()); return; }
            if (turns) EXPECT_TRUE(RotatePages(doc.value(), {0}, turns).ok());
            const UnitRect u{0.10, 0.15, 0.40, 0.30};
            auto box = UnitRectToPage(doc.value(), 0, u);
            if (!box) { EXPECT_TRUE(box.ok()); return; }
            EXPECT_TRUE(AddSquare(doc.value(), 0, box.value(), {255, 0, 0, 255}, 1.0f).ok());
            auto list = ListAnnotations(doc.value(), 0);
            if (!list || list.value().annots.size() != 1u) {
                EXPECT_TRUE(list.ok() && list.value().annots.size() == 1u);
                return;
            }
            const UnitRect& got = list.value().annots[0].unitRect;
            EXPECT_NEAR(got.left, u.left, 1e-3);
            EXPECT_NEAR(got.top, u.top, 1e-3);
            EXPECT_NEAR(got.right, u.right, 1e-3);
            EXPECT_NEAR(got.bottom, u.bottom, 1e-3);
        });
    }
}

TEST(Annotations, AddMarkupForChars) {
    pdftest::OnEngine([] {
        auto doc = MakePageWithText();
        EXPECT_TRUE(doc.ok());
        if (!doc) return;
        // Highlight characters 0..5 ("First ").
        EXPECT_TRUE(AddMarkupForChars(doc.value(), 0, AnnotKind::Highlight, 0, 6,
                                      {255, 220, 0, 255}).ok());
        auto list = ListAnnotations(doc.value(), 0);
        EXPECT_TRUE(list.ok());
        if (list) {
            EXPECT_EQ(list.value().annots.size(), 1u);
            EXPECT_EQ(list.value().annots[0].kind, AnnotKind::Highlight);
        }
    });
}

TEST(Annotations, CreatedAnnotationsAreVisibleWhenRendered) {
    pdftest::OnEngine([] {
        auto doc = MakeSinglePage();  // 200 x 300 pt, white
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        EXPECT_TRUE(AddMarkup(doc.value(), 0, AnnotKind::Highlight, {{20, 250, 120, 270}},
                              {255, 220, 0, 255}).ok());
        EXPECT_TRUE(AddSquare(doc.value(), 0, {20, 100, 120, 200}, {0, 0, 255, 255}, 4.0f).ok());
        EXPECT_TRUE(AddFreeText(doc.value(), 0, {20, 20, 180, 60}, L"WWWWWWWW",
                                {0, 0, 0, 255}).ok());
        auto bmp = doc.value().RenderPage(0, 200, 300);  // 1 px per point
        if (!bmp) { EXPECT_TRUE(bmp.ok()); return; }
        const Bitmap& b = bmp.value();
        // Pixel rows are top-down: PDF y maps to row 300 - y.
        auto at = [&](int x, int yPdf) { return b.At(x, 300 - yPdf); };
        EXPECT_NE(at(70, 260), 0xFFFFFFFFu);  // inside the highlight
        EXPECT_NE(at(21, 150), 0xFFFFFFFFu);  // on the square's left border
        EXPECT_EQ(at(70, 150), 0xFFFFFFFFu);  // square is an outline, inside is white
        bool freeTextInk = false;              // some dark pixel in the free text box
        for (int y = 22; y < 58 && !freeTextInk; ++y)
            for (int x = 22; x < 178 && !freeTextInk; ++x)
                if ((at(x, y) & 0xFF) < 0x80) freeTextInk = true;
        EXPECT_TRUE(freeTextInk);
    });
}

TEST(Annotations, RemoveAnnotationWithPopup) {
    pdftest::OnEngine([] {
        auto doc = MakeSinglePage();
        EXPECT_TRUE(doc.ok());
        if (!doc) return;
        EXPECT_TRUE(AddNote(doc.value(), 0, {50, 50}, L"note", {255, 0, 0, 255}).ok());
        auto before = ListAnnotations(doc.value(), 0);
        EXPECT_TRUE(before.ok());
        if (before) EXPECT_EQ(before.value().annots.size(), 1u);
        EXPECT_TRUE(RemoveAnnotation(doc.value(), 0, 0).ok());
        auto after = ListAnnotations(doc.value(), 0);
        EXPECT_TRUE(after.ok());
        if (after) EXPECT_EQ(after.value().annots.size(), 0u);
    });
}

TEST(Annotations, EditAnnotationText) {
    pdftest::OnEngine([] {
        auto doc = MakeSinglePage();
        EXPECT_TRUE(doc.ok());
        if (!doc) return;
        EXPECT_TRUE(AddFreeText(doc.value(), 0, {20, 100, 180, 140}, L"原始文本",
                                {0, 0, 0, 255}).ok());
        EXPECT_TRUE(SetAnnotationText(doc.value(), 0, 0, L"修改后").ok());
        auto list = ListAnnotations(doc.value(), 0);
        EXPECT_TRUE(list.ok());
        if (list) {
            EXPECT_EQ(list.value().annots.size(), 1u);
            EXPECT_EQ(list.value().annots[0].contents, L"修改后");
        }
    });
}

// Editing text clears the appearance stream so the new text is rendered, not
// the old one. Check by adding text, rendering, editing to blank, and
// confirming that the previously-inked region is now clear (except the border).
TEST(Annotations, EditedFreeTextRendersNewText) {
    pdftest::OnEngine([] {
        auto doc = MakeSinglePage();  // 200 x 300 pt
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        // Black text on white (no background fill, since FreeText skips /C).
        EXPECT_TRUE(AddFreeText(doc.value(), 0, {20, 200, 180, 240}, L"WWWWWWWW",
                                {0, 0, 0, 255}).ok());
        auto darkPixels = [&]() {
            auto bmp = doc.value().RenderPage(0, 200, 300);  // 1 px/pt
            if (!bmp) return -1;
            int n = 0;
            // Inner rect, away from the border that AddFreeText draws.
            for (int y = 202; y < 238; ++y)
                for (int x = 22; x < 178; ++x)
                    if ((bmp.value().At(x, 300 - y) & 0xFF) < 0x80) ++n;
            return n;
        };
        const int before = darkPixels();
        EXPECT_TRUE(before > 100);  // "WWWWWWWW" should ink many pixels
        EXPECT_TRUE(SetAnnotationText(doc.value(), 0, 0, L" ").ok());
        const int after = darkPixels();
        EXPECT_TRUE(after < 50);  // a space or empty should leave almost none
    });
}
