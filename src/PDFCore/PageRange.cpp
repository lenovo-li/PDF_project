#include "PageRange.h"

#include <cwctype>

namespace pdf {

namespace {
// Parses a positive decimal page number; false on empty, non-digit or > limit.
bool ParseNumber(const std::wstring& s, int limit, int* out) {
    if (s.empty() || s.size() > 9) return false;  // 9 digits cannot overflow int
    int v = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
        v = v * 10 + (c - L'0');
    }
    if (v < 1 || v > limit) return false;
    *out = v;
    return true;
}

std::wstring Trim(const std::wstring& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::iswspace(s[a])) ++a;
    while (b > a && std::iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}
}  // namespace

Result<std::vector<int>> ParsePageRange(const std::wstring& text, int pageCount) {
    if (pageCount <= 0) return Error{L"文档没有页面"};
    std::vector<int> pages;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t comma = text.find(L',', start);
        if (comma == std::wstring::npos) comma = text.size();
        const std::wstring token = Trim(text.substr(start, comma - start));
        start = comma + 1;
        if (token.empty()) {
            if (comma == text.size() && !pages.empty()) break;  // trailing comma
            return Error{L"页码范围为空或有多余的逗号"};
        }
        const std::size_t dash = token.find(L'-');
        int a = 0, b = 0;
        const bool ok = dash == std::wstring::npos
                            ? ParseNumber(token, pageCount, &a) && (b = a, true)
                            : ParseNumber(Trim(token.substr(0, dash)), pageCount, &a) &&
                                  ParseNumber(Trim(token.substr(dash + 1)), pageCount, &b);
        if (!ok) return Error{L"无效的页码：" + token + L"（范围 1-" + std::to_wstring(pageCount) + L"）"};
        const int step = a <= b ? 1 : -1;
        for (int p = a;; p += step) {
            // Cap: "1-N,1-N,..." repeated must not grow without bound.
            if (static_cast<int>(pages.size()) >= pageCount * 4 + 16)
                return Error{L"页码范围过长"};
            pages.push_back(p - 1);
            if (p == b) break;
        }
        if (comma == text.size()) break;
    }
    return pages;
}

Result<int> ParsePositiveInt(const std::wstring& text, int max) {
    int v = 0;
    if (max < 1 || !ParseNumber(Trim(text), max, &v))
        return Error{L"请输入 1 到 " + std::to_wstring(max < 1 ? 1 : max) + L" 之间的整数"};
    return v;
}

std::vector<std::vector<int>> SplitEvery(int pageCount, int pagesPerFile) {
    std::vector<std::vector<int>> out;
    if (pageCount <= 0 || pagesPerFile <= 0) return out;
    for (int first = 0; first < pageCount; first += pagesPerFile) {
        std::vector<int> chunk;
        for (int p = first; p < pageCount && p < first + pagesPerFile; ++p) chunk.push_back(p);
        out.push_back(std::move(chunk));
    }
    return out;
}

}  // namespace pdf
