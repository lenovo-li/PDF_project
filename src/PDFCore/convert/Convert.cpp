#include "Convert.h"

#include "Analyze.h"
#include "DocxWriter.h"
#include "Extract.h"
#include "../Document.h"

#include <new>
#include <utility>

namespace pdf {

Result<std::vector<std::uint8_t>> FinishDocx(std::vector<ExtractedPage> pages) {
    try {
        return WriteDocx(Analyze(std::move(pages)));
    } catch (const std::bad_alloc&) {
        return Error{L"内存不足，无法生成 Word 文档"};
    }
}

Result<std::vector<std::uint8_t>> ConvertToDocx(const Document& doc) {
    const int n = doc.PageCount();
    if (n <= 0) return Error{L"文档没有页面"};
    if (n > kMaxConvertPages) return Error{L"页数超过转换上限"};
    std::vector<ExtractedPage> pages;
    try {
        pages.reserve(static_cast<std::size_t>(n));
    } catch (const std::bad_alloc&) {
        return Error{L"内存不足"};
    }
    std::uint64_t imageBytes = 0, chars = 0;
    for (int i = 0; i < n; ++i) {
        auto p = ExtractPage(doc, i);
        if (!p) return p.error();
        imageBytes += ImageBytes(p.value());
        chars += p.value().chars.size();
        if (imageBytes > kMaxConvertImageBytes) return Error{L"扫描页图片过多，超出转换上限"};
        if (chars > kMaxConvertChars) return Error{L"文字太多，超出转换上限"};
        pages.push_back(std::move(p.value()));
    }
    return FinishDocx(std::move(pages));
}

}  // namespace pdf
