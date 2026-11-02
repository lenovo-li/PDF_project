#include "QpdfOps.h"

#include "Document.h"  // kMaxFileBytes
#include "FileIO.h"
#include "Trace.h"

#include <qpdf/Buffer.hh>
#include <qpdf/QPDFExc.hh>
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

Result<std::unique_ptr<LoadedPdf>> Load(const std::wstring& path,
                                        const std::string& password = {}) {
    auto bytes = ReadWholeFile(path, kMaxFileBytes);
    if (!bytes) return bytes.error();
    auto pdf = std::make_unique<LoadedPdf>();
    pdf->bytes = std::move(bytes.value());
    pdf->qpdf->setSuppressWarnings(true);
    pdf->qpdf->processMemoryFile("input", reinterpret_cast<const char*>(pdf->bytes.data()),
                                 pdf->bytes.size(), password.empty() ? nullptr : password.c_str());
    return pdf;
}

// Serializes `q` with `configure` applied to the writer and writes atomically.
template <class Configure>
Status WriteOut(QPDF& q, const std::wstring& output, Configure configure) {
    QPDFWriter writer(q);
    writer.setOutputMemory();
    configure(writer);
    writer.write();
    std::unique_ptr<Buffer> buf(writer.getBuffer());
    std::vector<std::uint8_t> bytes(buf->getBuffer(), buf->getBuffer() + buf->getSize());
    return WriteFileAtomic(output, bytes);
}

// New document holding `pages` of `src`, in order.
Status WritePages(QPDF& src, const std::vector<int>& pages, const std::wstring& output) {
    auto all = QPDFPageDocumentHelper(src).getAllPages();
    QPDF out;
    out.emptyPDF();
    QPDFPageDocumentHelper outPages(out);
    for (int p : pages) {
        if (p < 0 || p >= static_cast<int>(all.size())) return Error{L"页码超出范围"};
        outPages.addPage(all[static_cast<std::size_t>(p)], false);
    }
    // Decrypted on read; written without encryption (the user extracted it).
    return WriteOut(out, output, [](QPDFWriter&) {});
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

Status MergeFiles(const std::vector<std::wstring>& inputs, const std::wstring& output,
                  const std::string& firstPassword) {
    if (inputs.empty()) return Error{L"没有输入文件"};
    try {
        QPDF out;
        out.emptyPDF();
        QPDFPageDocumentHelper outPages(out);
        // Sources stay alive until the writer has run: copied pages reference
        // their stream data lazily.
        std::vector<std::unique_ptr<LoadedPdf>> sources;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const std::string& pw = (i == 0) ? firstPassword : std::string{};
            auto pdf = Load(inputs[i], pw);
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

Status ExtractPages(const std::wstring& input, const std::vector<int>& pages,
                    const std::wstring& output, const std::string& password) {
    if (pages.empty()) return Error{L"没有选择页面"};
    try {
        auto pdf = Load(input, password);
        if (!pdf) return pdf.error();
        return WritePages(*pdf.value()->qpdf, pages, output);
    } catch (const std::exception& e) {
        return FromException("ExtractPages", e);
    }
}

Status SplitFile(const std::wstring& input, const std::vector<std::vector<int>>& chunks,
                 const std::vector<std::wstring>& outputs, const std::string& password) {
    if (chunks.empty() || chunks.size() != outputs.size()) return Error{L"拆分参数无效"};
    try {
        auto pdf = Load(input, password);
        if (!pdf) return pdf.error();
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            if (auto s = WritePages(*pdf.value()->qpdf, chunks[i], outputs[i]); !s) return s;
        }
        TraceF("[PDFTool.Qpdf] split into %zu files\n", chunks.size());
        return Ok{};
    } catch (const std::exception& e) {
        return FromException("SplitFile", e);
    }
}

Status EncryptPdf(const std::wstring& input, const std::wstring& output,
                   const std::string& userPassword, const std::string& ownerPassword,
                   const Permissions& perms, const std::string& inputPassword) {
    if (ownerPassword.empty()) return Error{L"需要设置所有者密码"};
    try {
        auto pdf = Load(input, inputPassword);
        if (!pdf) return pdf.error();
        return WriteOut(*pdf.value()->qpdf, output, [&](QPDFWriter& w) {
            w.setPreserveEncryption(false);
            w.setR6EncryptionParameters(userPassword.c_str(), ownerPassword.c_str(),
                                        /*accessibility*/ true, perms.copy, perms.assemble,
                                        perms.annotate, perms.annotate, perms.modify,
                                        perms.print ? qpdf_r3p_full : qpdf_r3p_none,
                                        /*encrypt_metadata*/ true);
        });
    } catch (const std::exception& e) {
        return FromException("EncryptPdf", e);
    }
}

Status DecryptPdf(const std::wstring& input, const std::wstring& output,
                   const std::string& password) {
    try {
        auto pdf = Load(input, password);
        if (!pdf) return pdf.error();
        return WriteOut(*pdf.value()->qpdf, output,
                        [](QPDFWriter& w) { w.setPreserveEncryption(false); });
    } catch (const std::exception& e) {
        return FromException("DecryptPdf", e);
    }
}

Status CompressFile(const std::wstring& input, const std::wstring& output,
                    const CompressOptions& options, const std::string& password) {
    try {
        auto pdf = Load(input, password);
        if (!pdf) return pdf.error();
        return WriteOut(*pdf.value()->qpdf, output, [&](QPDFWriter& w) {
            w.setObjectStreamMode(qpdf_o_generate);
            w.setCompressStreams(true);
            w.setRecompressFlate(true);
            w.setLinearization(options.linearize);
        });
    } catch (const std::exception& e) {
        return FromException("CompressFile", e);
    }
}

Result<EncryptionInfo> QueryEncryption(const std::wstring& input) {
    EncryptionInfo info;
    try {
        auto pdf = Load(input);
        if (!pdf) return pdf.error();
        info.encrypted = pdf.value()->qpdf->isEncrypted();
        return info;
    } catch (const QPDFExc& e) {
        if (e.getErrorCode() == qpdf_e_password) {
            info.encrypted = info.needsPassword = true;
            return info;
        }
        return FromException("QueryEncryption", e);
    } catch (const std::exception& e) {
        return FromException("QueryEncryption", e);
    }
}

}  // namespace pdf
