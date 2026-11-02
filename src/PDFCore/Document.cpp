#include "Document.h"

#include "FileIO.h"
#include "Trace.h"

#include <fpdf_edit.h>
#include <fpdf_formfill.h>
#include <fpdf_save.h>
#include <fpdfview.h>

#include <new>

namespace pdf {

struct Document::Impl {
    std::vector<std::uint8_t> bytes;  // must outlive doc (LoadMemDocument)
    FPDF_DOCUMENT doc = nullptr;
    // Form-fill environment, created on first use for documents with an
    // AcroForm. PDFium keeps a pointer to formInfo for the handle's lifetime;
    // Impl lives on the heap, so moving a Document never moves it.
    FPDF_FORMFILLINFO formInfo{};
    FPDF_FORMHANDLE form = nullptr;
    bool formTried = false;
    ~Impl() {
        // The form environment refers to the document: exit it first.
        if (form) FPDFDOC_ExitFormFillEnvironment(form);
        if (doc) FPDF_CloseDocument(doc);
    }
};

fpdf_form_handle_t__* Document::Form() const {
    if (impl_->formTried) return impl_->form;
    impl_->formTried = true;
    const int type = FPDF_GetFormType(impl_->doc);
    // Hybrid XFA documents also carry an AcroForm; without XFA in this build
    // PDFium uses that, so only "no form at all" is skipped.
    if (type == FORMTYPE_NONE) return nullptr;
    // Version 1, every callback null: no JavaScript, no XFA, no timers. PDFium
    // checks each callback before calling it. The engine thread draws and
    // edits synchronously, so invalidation callbacks are not needed.
    impl_->formInfo.version = 1;
    impl_->form = FPDFDOC_InitFormFillEnvironment(impl_->doc, &impl_->formInfo);
    if (!impl_->form) {
        TraceF("[PDFTool.Forms] FPDFDOC_InitFormFillEnvironment failed\n");
        return nullptr;
    }
    // Field backgrounds are drawn by FFLDraw; no highlight so the rendered page
    // (and an exported PNG) looks like the printed form.
    FPDF_SetFormFieldHighlightAlpha(impl_->form, 0);
    TraceF("[PDFTool.Forms] form environment ready\n");
    return impl_->form;
}

Document::Document(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Document::Document(Document&&) noexcept = default;
Document& Document::operator=(Document&&) noexcept = default;
Document::~Document() = default;

bool IsPasswordError(const Error& e) { return e.code == FPDF_ERR_PASSWORD; }

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

FPDF_DOCUMENT Document::Raw() const { return impl_->doc; }

Result<std::vector<std::pair<double, double>>> Document::DisplayedPageSizesPt() const {
    const int n = PageCount();
    if (n < 0 || n > kMaxPages) {
        TraceF("[PDFTool.Document] refused page count %d (limit %d)\n", n, kMaxPages);
        return Error{L"页数超出上限"};
    }
    std::vector<std::pair<double, double>> sizes;
    sizes.reserve(static_cast<std::size_t>(n));
    int broken = 0;
    for (int i = 0; i < n; ++i) {
        FS_SIZEF s{};
        // Upper bound too: one page claiming /MediaBox [0 0 1e30 1e30] would
        // otherwise widen the layout column until every other page is drawn
        // far off screen. Written so NaN fails.
        const bool ok = FPDF_GetPageSizeByIndexF(impl_->doc, i, &s) &&
                        s.width > 0 && s.width <= kMaxPageSidePt &&
                        s.height > 0 && s.height <= kMaxPageSidePt;
        if (ok) {
            sizes.emplace_back(s.width, s.height);
        } else {
            sizes.emplace_back(612.0, 792.0);
            ++broken;
        }
    }
    if (broken) TraceF("[PDFTool.Document] %d of %d page sizes unreadable or out of range\n",
                       broken, n);
    return sizes;
}

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

namespace {
struct PageCloser {
    void operator()(fpdf_page_t__* p) const { FPDF_ClosePage(p); }
};
struct BitmapDestroyer {
    void operator()(fpdf_bitmap_t__* b) const { FPDFBitmap_Destroy(b); }
};
using PagePtr = std::unique_ptr<fpdf_page_t__, PageCloser>;
using PdfBitmapPtr = std::unique_ptr<fpdf_bitmap_t__, BitmapDestroyer>;
}  // namespace

Result<Bitmap> Document::RenderPage(int index, int widthPx, int heightPx) const {
    if (index < 0 || index >= PageCount()) return Error{L"页码超出范围", FPDF_ERR_PAGE};
    auto sizeOk = CheckRenderSize(widthPx, heightPx);
    if (!sizeOk) {
        TraceF("[PDFTool.Render] page %d: refused size %dx%d\n", index, widthPx, heightPx);
        return sizeOk.error();
    }

    Bitmap out;
    out.width = widthPx;
    out.height = heightPx;
    try {
        // CheckRenderSize capped this at kMaxRenderBytes, but a fragmented or
        // low-memory process can still fail the allocation.
        out.pixels.resize(static_cast<std::size_t>(widthPx) * heightPx * 4);
    } catch (const std::bad_alloc&) {
        TraceF("[PDFTool.Render] page %d: out of memory for %dx%d\n", index, widthPx, heightPx);
        return Error{L"内存不足，无法渲染页面"};
    }

    PagePtr page(FPDF_LoadPage(impl_->doc, index));
    if (!page) {
        const unsigned long code = FPDF_GetLastError();
        TraceF("[PDFTool.Render] FPDF_LoadPage failed, page=%d, err=%lu\n", index, code);
        return Error{L"无法加载页面", code};
    }
    // PDFium draws into our buffer directly (external-buffer form), so there
    // is no second copy of the pixels.
    PdfBitmapPtr bmp(FPDFBitmap_CreateEx(widthPx, heightPx, FPDFBitmap_BGRA,
                                         out.pixels.data(), out.Stride()));
    if (!bmp) {
        TraceF("[PDFTool.Render] FPDFBitmap_CreateEx failed, page=%d, %dx%d\n", index,
               widthPx, heightPx);
        return Error{L"无法创建页面位图"};
    }
    FPDFBitmap_FillRect(bmp.get(), 0, 0, widthPx, heightPx, 0xFFFFFFFF);
    // rotate = 0: PDFium already applies the page's /Rotate when it maps the
    // page into the display rectangle; passing it again would double-rotate.
    FPDF_RenderPageBitmap(bmp.get(), page.get(), 0, 0, widthPx, heightPx, 0, FPDF_ANNOT);
    // Form widgets are not part of FPDF_RenderPageBitmap; FFLDraw adds them on
    // top. The page is attached to the form only for this call.
    if (FPDF_FORMHANDLE form = Form()) {
        FORM_OnAfterLoadPage(page.get(), form);
        FPDF_FFLDraw(form, bmp.get(), page.get(), 0, 0, widthPx, heightPx, 0, FPDF_ANNOT);
        FORM_OnBeforeClosePage(page.get(), form);
    }
    // Destroy the PDFium bitmap (it does not own the buffer) before the page,
    // children before parents.
    bmp.reset();
    page.reset();
    return out;
}

Status Document::AddBlankPage(double widthPt, double heightPt) {
    if (!(widthPt > 0 && heightPt > 0 && widthPt <= 14400 && heightPt <= 14400))
        return Error{L"页面尺寸无效"};  // 14400 pt = 200 in, the PDF 1.7 limit
    FPDF_PAGE page = FPDFPage_New(impl_->doc, PageCount(), widthPt, heightPt);
    if (!page) return Error{L"无法新建页面"};
    FPDF_ClosePage(page);
    return Ok{};
}

Status Document::SetPageRotation(int index, int degrees) {
    if (index < 0 || index >= PageCount()) return Error{L"页码超出范围", FPDF_ERR_PAGE};
    if (degrees % 90 != 0) return Error{L"旋转角度必须是 90 的倍数"};
    PagePtr page(FPDF_LoadPage(impl_->doc, index));
    if (!page) return Error{L"无法加载页面", FPDF_GetLastError()};
    FPDFPage_SetRotation(page.get(), NormalizeRotation(degrees) / 90);  // 0..3 quarter turns
    return Ok{};
}

Status Document::AddTextLine(int index, const std::wstring& text, double xPt, double yPt,
                             double sizePt) {
    if (index < 0 || index >= PageCount()) return Error{L"页码超出范围", FPDF_ERR_PAGE};
    if (text.empty()) return Error{L"文字为空"};
    if (!(sizePt > 0 && sizePt <= 1000)) return Error{L"字号无效"};
    PagePtr page(FPDF_LoadPage(impl_->doc, index));
    if (!page) return Error{L"无法加载页面", FPDF_GetLastError()};
    FPDF_PAGEOBJECT obj =
        FPDFPageObj_NewTextObj(impl_->doc, "Helvetica", static_cast<float>(sizePt));
    if (!obj) return Error{L"无法创建文字对象"};
    static_assert(sizeof(wchar_t) == sizeof(unsigned short));
    if (!FPDFText_SetText(obj, reinterpret_cast<FPDF_WIDESTRING>(text.c_str()))) {
        FPDFPageObj_Destroy(obj);  // not yet owned by the page
        return Error{L"无法设置文字"};
    }
    FPDFPageObj_Transform(obj, 1, 0, 0, 1, xPt, yPt);
    FPDFPage_InsertObject(page.get(), obj);  // the page owns it from here
    if (!FPDFPage_GenerateContent(page.get())) return Error{L"无法生成页面内容"};
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
