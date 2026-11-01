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
#include "Result.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pdf {

// Hard limits applied to untrusted files before allocating anything.
constexpr std::uint64_t kMaxFileBytes = 2ull * 1024 * 1024 * 1024;  // 2 GiB

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

    // Appends a blank page of the given size in points (e.g. A4 = 595 x 842).
    Status AddBlankPage(double widthPt, double heightPt);

    // Serializes a full (non-incremental) copy.
    Result<std::vector<std::uint8_t>> SaveToMemory() const;

    // Writes to `path` through a temp file in the same directory plus an
    // atomic MoveFileExW, so a failure never damages an existing file.
    Status SaveAs(const std::wstring& path) const;

private:
    struct Impl;
    explicit Document(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Maps FPDF_GetLastError() codes to a message for the user.
std::wstring DescribePdfiumError(unsigned long code);

}  // namespace pdf
