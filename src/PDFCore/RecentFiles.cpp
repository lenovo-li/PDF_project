#include "RecentFiles.h"

#include <cwchar>

namespace pdf {

namespace {
bool SamePath(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}
}  // namespace

std::vector<std::wstring> NormalizeRecent(std::vector<std::wstring> list) {
    std::vector<std::wstring> out;
    for (std::wstring& p : list) {
        if (p.empty()) continue;
        bool dup = false;
        for (const std::wstring& q : out) dup = dup || SamePath(p, q);
        if (dup) continue;
        out.push_back(std::move(p));
        if (static_cast<int>(out.size()) == kMaxRecentFiles) break;
    }
    return out;
}

std::vector<std::wstring> PushRecent(std::vector<std::wstring> list, const std::wstring& path) {
    if (path.empty()) return NormalizeRecent(std::move(list));
    list.insert(list.begin(), path);
    return NormalizeRecent(std::move(list));  // drops the older copy of `path`
}

std::vector<std::wstring> RemoveRecent(std::vector<std::wstring> list, const std::wstring& path) {
    std::vector<std::wstring> out;
    for (std::wstring& p : list)
        if (!SamePath(p, path)) out.push_back(std::move(p));
    return NormalizeRecent(std::move(out));
}

std::wstring RecentLabel(int index, const std::wstring& path, std::size_t maxFolderChars) {
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    std::wstring folder = slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
    if (folder.size() > maxFolderChars && maxFolderChars > 1)
        folder = L"…" + folder.substr(folder.size() - (maxFolderChars - 1));
    std::wstring label = std::to_wstring(index + 1) + L"  " + name;
    if (!folder.empty()) label += L"  —  " + folder;
    return label;
}

}  // namespace pdf
