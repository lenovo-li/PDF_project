#include "QpdfOps.h"

#include "Document.h"  // kMaxFileBytes
#include "FileIO.h"
#include "Trace.h"

#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <windows.h>

#include <exception>
#include <memory>

namespace pdf {

namespace {

// qpdf's file APIs take 8-bit paths; read through our Unicode-safe reader and
// hand qpdf a memory buffer instead. The buffer must outlive the QPDF object.
struct LoadedPdf {
    std::vector<std::uint8_t> bytes;
    std::unique_ptr<QPDF> qpdf = std::make_unique<QPDF>();
};

Result<std::unique_ptr<LoadedPdf>> Load(const std::wstring& path) {
    auto bytes = ReadWholeFile(path, kMaxFileBytes);
    if (!bytes) return bytes.error();
    auto pdf = std::make_unique<LoadedPdf>();
    pdf->bytes = std::move(bytes.value());
    pdf->qpdf->setSuppressWarnings(true);
    pdf->qpdf->processMemoryFile("input", reinterpret_cast<const char*>(pdf->bytes.data()),
                                 pdf->bytes.size());
    return pdf;
}

Error FromException(const char* op, const std::exception& e) {
    TraceF("[PDFTool.Qpdf] %s failed: %s\n", op, e.what());
    std::wstring msg(L"PDF 处理失败：");
    // qpdf messages are ASCII/UTF-8; widen for display.
    const int n = MultiByteToWideChar(CP_UTF8, 0, e.what(), -1, nullptr, 0);
    if (n > 1) {
        std::wstring w(static_cast<size_t>(n - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, e.what(), -1, w.data(), n);
        msg += w;
    }
    return Error{msg};
}

}  // namespace

Result<int> QpdfPageCount(const std::wstring& path) {
    try {
        auto pdf = Load(path);
        if (!pdf) return pdf.error();
        return static_cast<int>(
            QPDFPageDocumentHelper(*pdf.value()->qpdf).getAllPages().size());
    } catch (const std::exception& e) {
        return FromException("QpdfPageCount", e);
    }
}

Status MergeFiles(const std::vector<std::wstring>& inputs, const std::wstring& output) {
    if (inputs.empty()) return Error{L"没有输入文件"};
    try {
        QPDF out;
        out.emptyPDF();
        QPDFPageDocumentHelper outPages(out);
        // Sources stay alive until the writer has run: copied pages reference
        // their stream data lazily.
        std::vector<std::unique_ptr<LoadedPdf>> sources;
        for (const auto& path : inputs) {
            auto pdf = Load(path);
            if (!pdf) return pdf.error();
            for (auto& page : QPDFPageDocumentHelper(*pdf.value()->qpdf).getAllPages())
                outPages.addPage(page, false);
            sources.push_back(std::move(pdf.value()));
        }
        QPDFWriter writer(out);
        writer.setOutputMemory();
        writer.write();
        std::unique_ptr<Buffer> buf(writer.getBuffer());
        std::vector<std::uint8_t> bytes(buf->getBuffer(), buf->getBuffer() + buf->getSize());
        return WriteFileAtomic(output, bytes);
    } catch (const std::exception& e) {
        return FromException("MergeFiles", e);
    }
}

}  // namespace pdf
