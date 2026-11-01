// Document: create, save, reopen through PDFium on the engine thread; error
// paths for missing and malformed files. Test PDFs are generated here so the
// expected content is known and no binary fixtures are needed.

#include "../framework/Test.h"
#include "../framework/TestEngine.h"

#include <PDFCore/Document.h>
#include <PDFCore/FileIO.h>

#include <fpdfview.h>
#include <windows.h>

using namespace pdf;

namespace {
std::wstring TempPath(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}
}  // namespace

TEST(Document, CreateSaveReopenKeepsPages) {
    pdftest::OnEngine([] {
        auto doc = Document::CreateEmpty();
        EXPECT_TRUE(doc.ok());
        if (!doc) return;
        EXPECT_TRUE(doc.value().AddBlankPage(595, 842).ok());  // A4
        EXPECT_TRUE(doc.value().AddBlankPage(842, 595).ok());  // A4 landscape
        auto bytes = doc.value().SaveToMemory();
        EXPECT_TRUE(bytes.ok());
        if (!bytes) return;

        auto reopened = Document::OpenMemory(std::move(bytes.value()));
        EXPECT_TRUE(reopened.ok());
        if (!reopened) return;
        EXPECT_EQ(reopened.value().PageCount(), 2);
        auto g = reopened.value().PageGeometryAt(1);
        EXPECT_TRUE(g.ok());
        if (g) {
            EXPECT_NEAR(g.value().box.Width(), 842, 0.01);
            EXPECT_NEAR(g.value().box.Height(), 595, 0.01);
        }
    });
}

TEST(Document, UnicodePathRoundTrip) {
    // Guards the reason Open() avoids FPDF_LoadDocument: non-ASCII paths.
    const std::wstring path = TempPath(L"PDFTool测试_文档.pdf");
    pdftest::OnEngine([&] {
        auto doc = Document::CreateEmpty();
        if (!doc) return;
        doc.value().AddBlankPage(300, 400);
        EXPECT_TRUE(doc.value().SaveAs(path).ok());
        auto reopened = Document::Open(path);
        EXPECT_TRUE(reopened.ok());
        if (reopened) EXPECT_EQ(reopened.value().PageCount(), 1);
    });
    DeleteFileW(path.c_str());
}

TEST(Document, MissingFileIsAnErrorNotACrash) {
    pdftest::OnEngine([] {
        auto r = Document::Open(TempPath(L"PDFTool_does_not_exist_9f3a.pdf"));
        EXPECT_FALSE(r.ok());
    });
}

TEST(Document, GarbageBytesReportFormatError) {
    pdftest::OnEngine([] {
        std::vector<std::uint8_t> junk = {'%', 'P', 'D', 'F', '-', 'x', 0, 1, 2, 3};
        auto r = Document::OpenMemory(std::move(junk));
        EXPECT_FALSE(r.ok());
        if (!r) EXPECT_EQ(r.error().code, static_cast<unsigned long>(FPDF_ERR_FORMAT));
    });
}

TEST(Document, PageIndexOutOfRangeIsRejected) {
    pdftest::OnEngine([] {
        auto doc = Document::CreateEmpty();
        if (!doc) return;
        doc.value().AddBlankPage(100, 100);
        EXPECT_FALSE(doc.value().PageGeometryAt(1).ok());
        EXPECT_FALSE(doc.value().PageGeometryAt(-1).ok());
    });
}

TEST(Document, InvalidPageSizeIsRejected) {
    pdftest::OnEngine([] {
        auto doc = Document::CreateEmpty();
        if (!doc) return;
        EXPECT_FALSE(doc.value().AddBlankPage(0, 100).ok());
        EXPECT_FALSE(doc.value().AddBlankPage(100, 1e9).ok());
        EXPECT_EQ(doc.value().PageCount(), 0);
    });
}

TEST(FileIO, AtomicWriteLeavesNoTempOnSuccess) {
    const std::wstring path = TempPath(L"PDFTool_atomic_test.bin");
    std::vector<std::uint8_t> data(1000, 0xAB);
    EXPECT_TRUE(WriteFileAtomic(path, data).ok());
    EXPECT_EQ(GetFileAttributesW((path + L".tmp~").c_str()), INVALID_FILE_ATTRIBUTES);
    auto back = ReadWholeFile(path, 1 << 20);
    EXPECT_TRUE(back.ok());
    if (back) EXPECT_EQ(back.value().size(), data.size());
    DeleteFileW(path.c_str());
}

TEST(FileIO, ReadRefusesOversizedFile) {
    const std::wstring path = TempPath(L"PDFTool_size_test.bin");
    std::vector<std::uint8_t> data(100, 1);
    WriteFileAtomic(path, data);
    EXPECT_FALSE(ReadWholeFile(path, 50).ok());
    DeleteFileW(path.c_str());
}
