#include "Document.h"

#include "FileIO.h"
#include "Trace.h"

#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdfview.h>

namespace pdf {

struct Document::Impl {
    std::vector<std::uint8_t> bytes;  // must outlive doc (LoadMemDocument)
    FPDF_DOCUMENT doc = nullptr;
    ~Impl() {
        if (doc) FPDF_CloseDocument(doc);
    }
};

Document::Document(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Document::Document(Document&&) noexcept = default;
Document& Document::operator=(Document&&) noexcept = default;
Document::~Document() = default;

std::wstring DescribePdfiumError(unsigned long code) {
    switch (code) {
        case FPDF_ERR_SUCCESS:  return L"成功";
        case FPDF_ERR_FILE:     return L"文件无法打开";
        case FPDF_ERR_FORMAT:   return L"不是有效的 PDF 或文件已损坏";
        case FPDF_ERR_PASSWORD: return L"需要密码或密码错误";
        case FPDF_ERR_SECURITY: return L"不支持的加密方式";
        case FPDF_ERR_PAGE:     return L"页面不存在或内容错误";
        default:                return L"未知错误";
    }
}

Result<Document> Document::OpenMemory(std::vector<std::uint8_t> bytes,
                                      const std::string& password) {
    auto impl = std::make_unique<Impl>();
    impl->bytes = std::move(bytes);
    impl->doc = FPDF_LoadMemDocument64(impl->bytes.data(), impl->bytes.size(),
                                       password.empty() ? nullptr : password.c_str());
    if (!impl->doc) {
        const unsigned long code = FPDF_GetLastError();
        TraceF("[PDFTool.Document] FPDF_LoadMemDocument64 failed, err=%lu, bytes=%zu\n",
               code, impl->bytes.size());
        return Error{DescribePdfiumError(code), code};
    }
    return Document(std::move(impl));
}

Result<Document> Document::Open(const std::wstring& path, const std::string& password) {
    auto bytes = ReadWholeFile(path, kMaxFileBytes);
    if (!bytes) return bytes.error();
    return OpenMemory(std::move(bytes.value()), password);
}

Result<Document> Document::CreateEmpty() {
    auto impl = std::make_unique<Impl>();
    impl->doc = FPDF_CreateNewDocument();
    if (!impl->doc) return Error{L"无法创建新文档"};
    return Document(std::move(impl));
}

int Document::PageCount() const { return FPDF_GetPageCount(impl_->doc); }

Result<PageGeometry> Document::PageGeometryAt(int index) const {
    if (index < 0 || index >= PageCount()) return Error{L"页码超出范围", FPDF_ERR_PAGE};
    FPDF_PAGE page = FPDF_LoadPage(impl_->doc, index);
    if (!page) return Error{L"无法加载页面", FPDF_GetLastError()};
    PageGeometry g;
    // Displayed area is the CropBox, which PDFium reports as the MediaBox when
    // absent. FPDF_GetPageBoundingBox returns exactly that.
    FS_RECTF r{};
    if (FPDF_GetPageBoundingBox(page, &r)) {
        g.box = {r.left, r.bottom, r.right, r.top};
    } else {
        g.box = {0, 0, FPDF_GetPageWidthF(page), FPDF_GetPageHeightF(page)};
    }
    // Returns 0..3 quarter turns, or -1 on error (treated as unrotated).
    const int quarterTurns = FPDFPage_GetRotation(page);
    g.rotation = quarterTurns > 0 ? quarterTurns * 90 : 0;
    FPDF_ClosePage(page);
    return g;
}

Status Document::AddBlankPage(double widthPt, double heightPt) {
    if (!(widthPt > 0 && heightPt > 0 && widthPt <= 14400 && heightPt <= 14400))
        return Error{L"页面尺寸无效"};  // 14400 pt = 200 in, the PDF 1.7 limit
    FPDF_PAGE page = FPDFPage_New(impl_->doc, PageCount(), widthPt, heightPt);
    if (!page) return Error{L"无法新建页面"};
    FPDF_ClosePage(page);
    return Ok{};
}

namespace {
struct MemoryWriter : FPDF_FILEWRITE {
    std::vector<std::uint8_t>* out;
};
int WriteBlockToVector(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
    auto* w = static_cast<MemoryWriter*>(self);
    const auto* p = static_cast<const std::uint8_t*>(data);
    w->out->insert(w->out->end(), p, p + size);
    return 1;
}
}  // namespace

Result<std::vector<std::uint8_t>> Document::SaveToMemory() const {
    std::vector<std::uint8_t> out;
    MemoryWriter w{};
    w.version = 1;
    w.WriteBlock = &WriteBlockToVector;
    w.out = &out;
    if (!FPDF_SaveAsCopy(impl_->doc, &w, FPDF_NO_INCREMENTAL))
        return Error{L"保存失败"};
    return out;
}

Status Document::SaveAs(const std::wstring& path) const {
    auto bytes = SaveToMemory();
    if (!bytes) return bytes.error();
    return WriteFileAtomic(path, bytes.value());
}

}  // namespace pdf
