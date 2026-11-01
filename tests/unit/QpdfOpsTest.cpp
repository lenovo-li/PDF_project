// qpdf integration: PDFium-generated files read back by qpdf, and merge.

#include "../framework/Test.h"
#include "../framework/TestEngine.h"

#include <PDFCore/Document.h>
#include <PDFCore/QpdfOps.h>

#include <windows.h>

using namespace pdf;

namespace {
std::wstring TempPath(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}

// Writes a PDF with `pages` blank pages (on the engine thread).
bool MakePdf(const std::wstring& path, int pages, double widthPt) {
    bool ok = false;
    pdftest::OnEngine([&] {
        auto doc = Document::CreateEmpty();
        if (!doc) return;
        for (int i = 0; i < pages; ++i) doc.value().AddBlankPage(widthPt, 400);
        ok = doc.value().SaveAs(path).ok();
    });
    return ok;
}
}  // namespace

TEST(Qpdf, ReadsPdfiumOutput) {
    const std::wstring path = TempPath(L"PDFTool_qpdf_count.pdf");
    EXPECT_TRUE(MakePdf(path, 3, 300));
    auto n = QpdfPageCount(path);
    EXPECT_TRUE(n.ok());
    if (n) EXPECT_EQ(n.value(), 3);
    DeleteFileW(path.c_str());
}

TEST(Qpdf, MergeConcatenatesPagesInOrder) {
    const std::wstring a = TempPath(L"PDFTool_merge_a.pdf");
    const std::wstring b = TempPath(L"PDFTool_merge_b.pdf");
    const std::wstring out = TempPath(L"PDFTool_合并结果.pdf");
    EXPECT_TRUE(MakePdf(a, 2, 300));  // pages 0-1: 300 pt wide
    EXPECT_TRUE(MakePdf(b, 1, 500));  // page 2: 500 pt wide
    EXPECT_TRUE(MergeFiles({a, b}, out).ok());

    pdftest::OnEngine([&] {
        auto merged = Document::Open(out);
        EXPECT_TRUE(merged.ok());
        if (!merged) return;
        EXPECT_EQ(merged.value().PageCount(), 3);
        auto first = merged.value().PageGeometryAt(0);
        auto last = merged.value().PageGeometryAt(2);
        if (first) EXPECT_NEAR(first.value().box.Width(), 300, 0.01);
        if (last) EXPECT_NEAR(last.value().box.Width(), 500, 0.01);  // order kept
    });
    DeleteFileW(a.c_str());
    DeleteFileW(b.c_str());
    DeleteFileW(out.c_str());
}

TEST(Qpdf, GarbageInputIsAnErrorNotAnException) {
    const std::wstring path = TempPath(L"PDFTool_qpdf_garbage.pdf");
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w = 0;
    WriteFile(f, "not a pdf at all", 16, &w, nullptr);
    CloseHandle(f);
    EXPECT_FALSE(QpdfPageCount(path).ok());
    EXPECT_FALSE(MergeFiles({path}, TempPath(L"PDFTool_qpdf_never.pdf")).ok());
    DeleteFileW(path.c_str());
}
