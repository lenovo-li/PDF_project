// TextSearch: matches, case/whole-word options, highlight rectangles, caps,
// page text extraction. The PDFs are built here with Document::AddTextLine so
// the text layer is known exactly.

#include "../framework/Test.h"
#include "../framework/TestEngine.h"

#include <PDFCore/Document.h>
#include <PDFCore/TextSearch.h>

#include <cmath>
#include <string>

using namespace pdf;

namespace {
// One A4 page with two lines: "Hello World hello" at y=700, "second line" at y=600.
Result<Document> TwoLineDoc() {
    auto doc = Document::CreateEmpty();
    if (!doc) return doc.error();
    if (auto s = doc.value().AddBlankPage(595, 842); !s) return s.error();
    if (auto s = doc.value().AddTextLine(0, L"Hello World hello", 72, 700, 24); !s) return s.error();
    if (auto s = doc.value().AddTextLine(0, L"second line", 72, 600, 24); !s) return s.error();
    // Round-trip so search reads a parsed content stream, as for real files.
    auto bytes = doc.value().SaveToMemory();
    if (!bytes) return bytes.error();
    return Document::OpenMemory(std::move(bytes.value()));
}
}  // namespace

TEST(TextSearch, FindsAllMatchesCaseInsensitiveByDefault) {
    pdftest::OnEngine([] {
        auto doc = TwoLineDoc();
        EXPECT_TRUE(doc.ok());
        if (!doc) return;
        auto r = SearchPage(doc.value(), 0, L"hello", {});
        EXPECT_TRUE(r.ok());
        if (!r) return;
        EXPECT_EQ(r.value().matches.size(), static_cast<std::size_t>(2));
        EXPECT_FALSE(r.value().truncated);
        for (const TextMatch& m : r.value().matches) {
            EXPECT_EQ(m.charCount, 5);
            EXPECT_FALSE(m.rects.empty());
        }
    });
}

TEST(TextSearch, MatchCaseAndWholeWord) {
    pdftest::OnEngine([] {
        auto doc = TwoLineDoc();
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        SearchOptions cs;
        cs.matchCase = true;
        auto r = SearchPage(doc.value(), 0, L"hello", cs);
        if (r) EXPECT_EQ(r.value().matches.size(), static_cast<std::size_t>(1));
        SearchOptions ww;
        ww.wholeWord = true;
        auto w = SearchPage(doc.value(), 0, L"lin", ww);  // only a prefix of "line"
        if (w) EXPECT_EQ(w.value().matches.size(), static_cast<std::size_t>(0));
        auto partial = SearchPage(doc.value(), 0, L"lin", {});
        if (partial) EXPECT_EQ(partial.value().matches.size(), static_cast<std::size_t>(1));
    });
}

TEST(TextSearch, RectanglesSitOnTheMatchedLine) {
    pdftest::OnEngine([] {
        auto doc = TwoLineDoc();
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto r = SearchPage(doc.value(), 0, L"second", {});
        if (!r || r.value().matches.size() != 1) { EXPECT_TRUE(false); return; }
        const TextMatch& m = r.value().matches[0];
        if (m.rects.empty()) { EXPECT_TRUE(false); return; }
        const BoxPt& b = m.rects[0];
        // Baseline 600, 24 pt text: the box spans roughly 595..625, starts near x=72.
        EXPECT_TRUE(b.bottom > 590 && b.bottom < 605);
        EXPECT_TRUE(b.top > 610 && b.top < 630);
        EXPECT_TRUE(b.left > 70 && b.left < 76);
        EXPECT_TRUE(b.right > b.left + 40);  // "second" at 24 pt is ~70 pt wide
    });
}

TEST(TextSearch, RejectsBadInputAndHandlesPagesWithoutText) {
    pdftest::OnEngine([] {
        auto doc = TwoLineDoc();
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        EXPECT_FALSE(SearchPage(doc.value(), 0, L"", {}).ok());
        EXPECT_FALSE(SearchPage(doc.value(), 0, std::wstring(kMaxQueryChars + 1, L'a'), {}).ok());
        EXPECT_FALSE(SearchPage(doc.value(), 1, L"x", {}).ok());
        EXPECT_FALSE(SearchPage(doc.value(), -1, L"x", {}).ok());
        auto blank = Document::CreateEmpty();
        if (!blank) return;
        blank.value().AddBlankPage(100, 100);  // no text layer, like a scan
        auto r = SearchPage(blank.value(), 0, L"x", {});
        EXPECT_TRUE(r.ok());
        if (r) EXPECT_EQ(r.value().matches.size(), static_cast<std::size_t>(0));
    });
}

TEST(TextSearch, CapsMatchesPerPage) {
    pdftest::OnEngine([] {
        auto doc = Document::CreateEmpty();
        if (!doc) return;
        doc.value().AddBlankPage(595, 842);
        // 1200 single-letter matches on one line, more than kMaxMatchesPerPage.
        doc.value().AddTextLine(0, std::wstring(1200, L'a'), 10, 400, 1);
        auto r = SearchPage(doc.value(), 0, L"a", {});
        EXPECT_TRUE(r.ok());
        if (!r) return;
        EXPECT_EQ(static_cast<int>(r.value().matches.size()), kMaxMatchesPerPage);
        EXPECT_TRUE(r.value().truncated);
    });
}

TEST(TextSearch, PageTextReturnsTheTextLayer) {
    pdftest::OnEngine([] {
        auto doc = TwoLineDoc();
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto t = PageText(doc.value(), 0);
        EXPECT_TRUE(t.ok());
        if (!t) return;
        EXPECT_TRUE(t.value().find(L"Hello World hello") != std::wstring::npos);
        EXPECT_TRUE(t.value().find(L"second line") != std::wstring::npos);
        auto part = PageText(doc.value(), 0, 6, 5);
        if (part) EXPECT_TRUE(part.value() == L"World");
        EXPECT_FALSE(PageText(doc.value(), 0, 100000, 1).ok());
    });
}

TEST(TextSearch, UnitRectsMatchDisplayedPageAndRotation) {
    pdftest::OnEngine([] {
        // A4 portrait, "second" on the baseline at y=600 starting at x=72.
        auto doc = TwoLineDoc();
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto r = SearchPage(doc.value(), 0, L"second", {});
        if (!r || r.value().matches.empty() || r.value().matches[0].units.empty()) {
            EXPECT_TRUE(false);
            return;
        }
        const UnitRect u = r.value().matches[0].units[0];
        // Unrotated: x = 72/595, y from the top = (842 - ~620)/842.
        EXPECT_NEAR(u.left, 72.0 / 595, 0.01);
        EXPECT_TRUE(u.top > 0.24 && u.top < 0.29);
        EXPECT_TRUE(u.bottom > u.top && u.right > u.left);

        // Rotate 90 clockwise: the page's left edge becomes the displayed top,
        // so the match's small x turns into a small y.
        EXPECT_TRUE(doc.value().SetPageRotation(0, 90).ok());
        auto rr = SearchPage(doc.value(), 0, L"second", {});
        if (!rr || rr.value().matches.empty() || rr.value().matches[0].units.empty()) {
            EXPECT_TRUE(false);
            return;
        }
        const UnitRect v = rr.value().matches[0].units[0];
        EXPECT_NEAR(v.top, 72.0 / 595, 0.01);
        EXPECT_TRUE(v.right > 0.7);  // high on the page (y=600 of 842) -> far right
        EXPECT_TRUE(v.bottom - v.top > v.right - v.left);  // the word now runs downward
    });
}

TEST(TextSearch, SelectTextBetweenTwoPointsInEitherDirection) {
    pdftest::OnEngine([] {
        auto doc = TwoLineDoc();
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        // Locate "World" through search, then drag across it by its unit rect.
        auto r = SearchPage(doc.value(), 0, L"World", {});
        if (!r || r.value().matches.empty() || r.value().matches[0].units.empty()) {
            EXPECT_TRUE(false);
            return;
        }
        const UnitRect w = r.value().matches[0].units[0];
        const double midY = (w.top + w.bottom) / 2;
        const double inset = (w.right - w.left) * 0.05;
        auto fwd = SelectText(doc.value(), 0, w.left + inset, midY, w.right - inset, midY);
        EXPECT_TRUE(fwd.ok());
        if (fwd) {
            EXPECT_TRUE(fwd.value().text == L"World");
            EXPECT_FALSE(fwd.value().units.empty());
        }
        auto back = SelectText(doc.value(), 0, w.right - inset, midY, w.left + inset, midY);
        if (back) EXPECT_TRUE(back.value().text == L"World");  // reading order kept

        auto none = SelectText(doc.value(), 0, 0.95, 0.95, 0.96, 0.96);  // empty corner
        EXPECT_TRUE(none.ok());
        if (none) EXPECT_EQ(none.value().count, 0);
        EXPECT_FALSE(SelectText(doc.value(), 0, std::nan(""), 0, 1, 1).ok());
        EXPECT_FALSE(SelectText(doc.value(), 5, 0, 0, 1, 1).ok());
    });
}

TEST(TextSearch, DragFromMarginPastLastLineSelectsToTheEnd) {
    pdftest::OnEngine([] {
        auto doc = TwoLineDoc();
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto r = SearchPage(doc.value(), 0, L"World", {});
        if (!r || r.value().matches.empty() || r.value().matches[0].units.empty()) {
            EXPECT_TRUE(false);
            return;
        }
        const UnitRect w = r.value().matches[0].units[0];
        // Start inside "World", end far below every line (bottom of the page):
        // the far end snaps to the page end, so the selection reaches line 2.
        auto sel = SelectText(doc.value(), 0, w.left + 0.001, (w.top + w.bottom) / 2, 0.5, 0.99);
        EXPECT_TRUE(sel.ok());
        if (!sel) return;
        EXPECT_TRUE(sel.value().text.find(L"World") != std::wstring::npos);
        EXPECT_TRUE(sel.value().text.find(L"second line") != std::wstring::npos);
        EXPECT_TRUE(sel.value().units.size() >= 2);  // spans two lines
        for (const UnitRect& u : sel.value().units) {
            EXPECT_TRUE(u.left >= 0 && u.right <= 1 && u.top >= 0 && u.bottom <= 1);
            EXPECT_TRUE(u.right > u.left && u.bottom > u.top);
        }
    });
}
