// Document.h — RAII wrapper over an FPDF_DOCUMENT.
//
// THREADING: every member must be called on the engine thread (see Engine.h).
// PDFium keeps global state, so even two different Documents may not be used
// from two threads at once. The UI never holds a Document directly.
//
// Opening reads the whole file into memory with CreateFileW and loads it with
// FPDF_LoadMemDocument64. FPDF_LoadDocument takes an 8-bit path, which loses
// non-ASCII (e.g. Chinese) paths on Windows. The buffer must outlive the
// FPDF_DOCUMENT, so the Document owns it. A streaming FPDF_FILEACCESS loader
// can replace this later if very large files make the memory cost matter.
#pragma once

#include "PageGeometry.h"
#include "Render.h"
#include "Result.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// PDFium's FPDF_DOCUMENT and FPDF_FORMHANDLE, declared here so this header
// does not pull in fpdfview.h for every PDFCore user.
struct fpdf_document_t__;
struct fpdf_form_handle_t__;

namespace pdf {

// Hard limits applied to untrusted files before allocating anything.
constexpr std::uint64_t kMaxFileBytes = 2ull * 1024 * 1024 * 1024;  // 2 GiB
// A hostile /Count can claim billions of pages; the viewer allocates per page.
constexpr int kMaxPages = 100000;
// Largest displayed page side accepted from a file, in points. PDF 1.7 caps a
// MediaBox at 14400 pt (200 in); /UserUnit (PDF 1.6+) can legally enlarge
// that, so allow 10x before treating the size as hostile.
constexpr double kMaxPageSidePt = 144000.0;

class Document {
public:
    Document(Document&&) noexcept;
    Document& operator=(Document&&) noexcept;
    ~Document();

    static Result<Document> Open(const std::wstring& path,
                                 const std::string& password = {});
    static Result<Document> OpenMemory(std::vector<std::uint8_t> bytes,
                                       const std::string& password = {});
    static Result<Document> CreateEmpty();

    int PageCount() const;
    Result<PageGeometry> PageGeometryAt(int index) const;

    // Displayed size in points (CropBox, /Rotate applied) of every page, read
    // with FPDF_GetPageSizeByIndexF so no page content is parsed: opening a
    // 5000-page file must not load 5000 pages. A page whose size cannot be read
    // gets US Letter, so one broken page does not stop the document opening.
    // Fails if the document has more than kMaxPages pages.
    Result<std::vector<std::pair<double, double>>> DisplayedPageSizesPt() const;

    // Renders page `index` (with /Rotate applied, annotations and form fields
    // included) onto an opaque white widthPx x heightPx bitmap. Get the size
    // from RenderSizePx so the aspect ratio matches the displayed page.
    Result<Bitmap> RenderPage(int index, int widthPx, int heightPx) const;

    // Draws one line of `text` in Helvetica at (xPt, yPt), the baseline origin
    // in PDF user space, on page `index`. Used by tests to build PDFs with a
    // known text layer, and later by the editor for simple text insertion.
    Status AddTextLine(int index, const std::wstring& text, double xPt, double yPt,
                       double sizePt);

    // Sets page `index`'s /Rotate to `degrees` (any multiple of 90, normalized;
    // other values are an error). Changes the displayed size, so callers
    // re-read DisplayedPageSizesPt afterwards.
    Status SetPageRotation(int index, int degrees);

    // Appends a blank page of the given size in points (e.g. A4 = 595 x 842).
    Status AddBlankPage(double widthPt, double heightPt);

    // Serializes a full (non-incremental) copy.
    Result<std::vector<std::uint8_t>> SaveToMemory() const;

    // Writes to `path` through a temp file in the same directory plus an
    // atomic MoveFileExW, so a failure never damages an existing file.
    Status SaveAs(const std::wstring& path) const;

    // The underlying handle, for other PDFCore modules (text search, page
    // editing) that need PDFium calls this class does not wrap. Engine thread
    // only; never stored past the call, never handed to PDFTool.
    fpdf_document_t__* Raw() const;

    // The form-fill environment (FPDF_FORMHANDLE) for an AcroForm document,
    // created on first call and owned by the Document; nullptr if the document
    // has no AcroForm (or only XFA). For Forms.cpp and RenderPage. Any page
    // given to FORM_OnAfterLoadPage must get FORM_OnBeforeClosePage before
    // FPDF_ClosePage, within the same call.
    fpdf_form_handle_t__* Form() const;

private:
    struct Impl;
    explicit Document(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// True if opening failed because the file needs a (different) password, so
// the UI can prompt. Keeps FPDF_ERR_* out of PDFTool.
bool IsPasswordError(const Error& e);

// Maps FPDF_GetLastError() codes to a message for the user.
std::wstring DescribePdfiumError(unsigned long code);

}  // namespace pdf
