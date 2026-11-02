// qpdf integration: PDFium-generated files read back by qpdf, and merge.

#include "../framework/Test.h"
#include "../framework/TestEngine.h"

#include <PDFCore/Document.h>
#include <PDFCore/QpdfOps.h>

#include <windows.h>

#include <string>
#include <vector>

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

namespace {
// Page widths of a file as PDFium reads it, in order (for checking extracts).
std::vector<int> Widths(const std::wstring& path, const std::string& password = {}) {
    std::vector<int> w;
    pdftest::OnEngine([&] {
        auto doc = Document::Open(path, password);
        if (!doc) return;
        auto sizes = doc.value().DisplayedPageSizesPt();
        if (!sizes) return;
        for (const auto& s : sizes.value()) w.push_back(static_cast<int>(s.first + 0.5));
    });
    return w;
}

// Pages of widths 100, 110, 120, ... so extracted order is visible.
bool MakeNumberedPdf(const std::wstring& path, int pages) {
    bool ok = false;
    pdftest::OnEngine([&] {
        auto doc = Document::CreateEmpty();
        if (!doc) return;
        for (int i = 0; i < pages; ++i) doc.value().AddBlankPage(100 + 10 * i, 400);
        ok = doc.value().SaveAs(path).ok();
    });
    return ok;
}
}  // namespace

TEST(Qpdf, ExtractKeepsRequestedOrder) {
    const std::wstring in = TempPath(L"PDFTool_ex_in.pdf"), out = TempPath(L"PDFTool_ex_out.pdf");
    EXPECT_TRUE(MakeNumberedPdf(in, 5));
    EXPECT_TRUE(ExtractPages(in, {4, 0, 2}, out).ok());
    EXPECT_TRUE(Widths(out) == std::vector<int>({140, 100, 120}));
    EXPECT_FALSE(ExtractPages(in, {5}, out).ok());
    EXPECT_FALSE(ExtractPages(in, {}, out).ok());
    DeleteFileW(in.c_str());
    DeleteFileW(out.c_str());
}

TEST(Qpdf, SplitWritesOneFilePerChunk) {
    const std::wstring in = TempPath(L"PDFTool_sp_in.pdf");
    const std::wstring a = TempPath(L"PDFTool_sp_a.pdf"), b = TempPath(L"PDFTool_sp_b.pdf");
    EXPECT_TRUE(MakeNumberedPdf(in, 3));
    EXPECT_TRUE(SplitFile(in, {{0, 1}, {2}}, {a, b}).ok());
    EXPECT_TRUE(Widths(a) == std::vector<int>({100, 110}));
    EXPECT_TRUE(Widths(b) == std::vector<int>({120}));
    EXPECT_FALSE(SplitFile(in, {{0}}, {a, b}).ok());  // mismatched lists
    for (const auto& p : {in, a, b}) DeleteFileW(p.c_str());
}

TEST(Qpdf, EncryptThenDecryptRoundTrip) {
    const std::wstring in = TempPath(L"PDFTool_enc_in.pdf"), enc = TempPath(L"PDFTool_enc.pdf"),
                       dec = TempPath(L"PDFTool_dec.pdf");
    EXPECT_TRUE(MakeNumberedPdf(in, 2));
    Permissions perms;
    perms.print = false;
    EXPECT_TRUE(EncryptPdf(in, enc, "user1", "owner1", perms).ok());
    EXPECT_FALSE(EncryptPdf(in, enc, "user1", "", perms).ok());  // owner required

    auto info = QueryEncryption(enc);
    EXPECT_TRUE(info.ok());
    if (info) {
        EXPECT_TRUE(info.value().encrypted);
        EXPECT_TRUE(info.value().needsPassword);
    }
    EXPECT_TRUE(Widths(enc).empty());                                    // no password: refused
    EXPECT_TRUE(Widths(enc, "user1") == std::vector<int>({100, 110}));   // PDFium opens it
    EXPECT_FALSE(DecryptPdf(enc, dec, "wrong").ok());
    EXPECT_TRUE(DecryptPdf(enc, dec, "owner1").ok());
    auto plain = QueryEncryption(dec);
    if (plain) EXPECT_FALSE(plain.value().encrypted);
    EXPECT_TRUE(Widths(dec) == std::vector<int>({100, 110}));
    for (const auto& p : {in, enc, dec}) DeleteFileW(p.c_str());
}

TEST(Qpdf, CompressAndLinearizeKeepPages) {
    const std::wstring in = TempPath(L"PDFTool_cz_in.pdf"), out = TempPath(L"PDFTool_cz_out.pdf");
    EXPECT_TRUE(MakeNumberedPdf(in, 4));
    CompressOptions opt;
    opt.linearize = true;
    EXPECT_TRUE(CompressFile(in, out, opt).ok());
    EXPECT_TRUE(Widths(out) == std::vector<int>({100, 110, 120, 130}));
    DeleteFileW(in.c_str());
    DeleteFileW(out.c_str());
}

// Documents what PDFium does when an encrypted document is edited and saved:
// the save path must not silently strip a user's password. If this test
// starts failing after a PDFium update, revisit MainWindow's save flow.
TEST(Qpdf, PdfiumSaveOfEncryptedDocumentKeepsPassword) {
    const std::wstring in = TempPath(L"PDFTool_pe_in.pdf"), enc = TempPath(L"PDFTool_pe_enc.pdf"),
                       out = TempPath(L"PDFTool_pe_out.pdf");
    EXPECT_TRUE(MakeNumberedPdf(in, 2));
    EXPECT_TRUE(EncryptPdf(in, enc, "u", "o", Permissions{}).ok());
    bool saved = false;
    pdftest::OnEngine([&] {
        auto doc = Document::Open(enc, "u");
        if (!doc) return;
        doc.value().AddBlankPage(200, 200);  // an edit
        saved = doc.value().SaveAs(out).ok();
    });
    EXPECT_TRUE(saved);
    auto info = QueryEncryption(out);
    EXPECT_TRUE(info.ok());
    if (info) EXPECT_TRUE(info.value().needsPassword);  // still protected
    EXPECT_EQ(Widths(out, "u").size(), static_cast<std::size_t>(3));
    for (const auto& p : {in, enc, out}) DeleteFileW(p.c_str());
}

// The open-time password prompt depends on PDFium reporting FPDF_ERR_PASSWORD
// for an encrypted file opened with no password or a wrong one.
TEST(Qpdf, OpeningEncryptedFileReportsPasswordError) {
    const std::wstring in = TempPath(L"PDFTool_pwe_in.pdf"), enc = TempPath(L"PDFTool_pwe.pdf");
    EXPECT_TRUE(MakeNumberedPdf(in, 1));
    EXPECT_TRUE(EncryptPdf(in, enc, "user", "owner", Permissions{}).ok());
    pdftest::OnEngine([&] {
        auto none = Document::Open(enc);
        EXPECT_FALSE(none.ok());
        if (!none) EXPECT_TRUE(IsPasswordError(none.error()));
        auto wrong = Document::Open(enc, "nope");
        EXPECT_FALSE(wrong.ok());
        if (!wrong) EXPECT_TRUE(IsPasswordError(wrong.error()));
        auto right = Document::Open(enc, "user");
        EXPECT_TRUE(right.ok());
        auto garbage = Document::OpenMemory({'%', 'P', 'D', 'F', '-', 'x'});
        if (!garbage) EXPECT_FALSE(IsPasswordError(garbage.error()));  // not a password case
    });
    DeleteFileW(in.c_str());
    DeleteFileW(enc.c_str());
}

TEST(Qpdf, MergeOpensEncryptedFirstInputWithItsPassword) {
    const std::wstring a = TempPath(L"PDFTool_mg_a.pdf"), enc = TempPath(L"PDFTool_mg_enc.pdf"),
                       b = TempPath(L"PDFTool_mg_b.pdf"), out = TempPath(L"PDFTool_mg_out.pdf");
    EXPECT_TRUE(MakeNumberedPdf(a, 2));
    EXPECT_TRUE(MakeNumberedPdf(b, 1));
    EXPECT_TRUE(EncryptPdf(a, enc, "u", "o", Permissions{}).ok());
    EXPECT_FALSE(MergeFiles({enc, b}, out).ok());  // no password: refused
    EXPECT_TRUE(MergeFiles({enc, b}, out, "u").ok());
    EXPECT_TRUE(Widths(out) == std::vector<int>({100, 110, 100}));  // output is not encrypted
    for (const auto& p : {a, enc, b, out}) DeleteFileW(p.c_str());
}
