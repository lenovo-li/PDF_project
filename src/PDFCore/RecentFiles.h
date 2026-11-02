// RecentFiles.h — most-recently-used file list, pure.
//
// ConfigStore holds scalars only, so the list is stored as numbered string
// keys ("recent.0" = newest). These functions keep the list rules in one
// testable place: newest first, no duplicates (paths compare
// case-insensitively, as NTFS does), at most kMaxRecentFiles entries, no
// empty strings.
#pragma once

#include <string>
#include <vector>

namespace pdf {

constexpr int kMaxRecentFiles = 10;

// `path` moved (or added) to the front; the oldest entry falls off past the cap.
std::vector<std::wstring> PushRecent(std::vector<std::wstring> list, const std::wstring& path);

// `path` removed (e.g. the file no longer opens).
std::vector<std::wstring> RemoveRecent(std::vector<std::wstring> list, const std::wstring& path);

// Cleans a list read back from config (hand-edited or from an older version):
// drops empties and duplicates, keeps the first kMaxRecentFiles.
std::vector<std::wstring> NormalizeRecent(std::vector<std::wstring> list);

// Label for a menu: "N  name  —  folder", with the folder shortened from the
// left to at most `maxFolderChars` characters ("…\b\c").
std::wstring RecentLabel(int index, const std::wstring& path, std::size_t maxFolderChars = 40);

}  // namespace pdf
