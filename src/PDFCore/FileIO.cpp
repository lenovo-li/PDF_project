#include "FileIO.h"

#include "Trace.h"

#include <windows.h>

#include <cwctype>

namespace pdf {

namespace {
struct HandleCloser {
    HANDLE h;
    ~HandleCloser() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};

Error LastWin32Error(const wchar_t* what) {
    const DWORD code = GetLastError();
    TraceF("[PDFTool.FileIO] %ls failed, GetLastError=%lu\n", what, code);
    return Error{std::wstring(what) + L" 失败（Win32 错误 " + std::to_wstring(code) + L"）"};
}
}  // namespace

Result<std::vector<std::uint8_t>> ReadWholeFile(const std::wstring& path,
                                                std::uint64_t maxBytes) {
    HandleCloser f{CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (f.h == INVALID_HANDLE_VALUE) return LastWin32Error(L"打开文件");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f.h, &size)) return LastWin32Error(L"读取文件大小");
    if (static_cast<std::uint64_t>(size.QuadPart) > maxBytes)
        return Error{L"文件过大"};
    std::vector<std::uint8_t> bytes(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (bytes.size() - done) > (1u << 30) ? (1u << 30) : (bytes.size() - done));
        DWORD got = 0;
        if (!ReadFile(f.h, bytes.data() + done, chunk, &got, nullptr))
            return LastWin32Error(L"读取文件");
        if (got == 0) break;  // file shrank while reading
        done += got;
    }
    bytes.resize(done);
    return bytes;
}

Status WriteFileAtomic(const std::wstring& path, const std::vector<std::uint8_t>& bytes) {
    const std::wstring tmp = path + L".tmp~";
    {
        HandleCloser f{CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (f.h == INVALID_HANDLE_VALUE) return LastWin32Error(L"创建临时文件");
        size_t done = 0;
        while (done < bytes.size()) {
            const DWORD chunk = static_cast<DWORD>(
                (bytes.size() - done) > (1u << 30) ? (1u << 30) : (bytes.size() - done));
            DWORD wrote = 0;
            if (!WriteFile(f.h, bytes.data() + done, chunk, &wrote, nullptr) || wrote == 0) {
                Error e = LastWin32Error(L"写入临时文件");
                CloseHandle(f.h);
                f.h = INVALID_HANDLE_VALUE;
                DeleteFileW(tmp.c_str());
                return e;
            }
            done += wrote;
        }
        if (!FlushFileBuffers(f.h)) {
            Error e = LastWin32Error(L"刷新临时文件");
            CloseHandle(f.h);
            f.h = INVALID_HANDLE_VALUE;
            DeleteFileW(tmp.c_str());
            return e;
        }
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        Error e = LastWin32Error(L"替换目标文件");
        DeleteFileW(tmp.c_str());
        return e;
    }
    return Ok{};
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr,
                                      0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

std::wstring NumberedPath(const std::wstring& base, int n, int count) {
    std::wstring stem = base;
    if (stem.size() >= 4) {
        std::wstring ext = stem.substr(stem.size() - 4);
        for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
        if (ext == L".pdf") stem.resize(stem.size() - 4);
    }
    const int width = static_cast<int>(std::to_wstring(count < 1 ? 1 : count).size());
    std::wstring num = std::to_wstring(n);
    if (static_cast<int>(num.size()) < width) num.insert(0, width - num.size(), L'0');
    return stem + L"_" + num + L".pdf";
}

}  // namespace pdf
