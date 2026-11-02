// CommandLine.h — the file to open from the process arguments, pure.
//
// "PDFTool.exe file.pdf" comes from Explorer's "Open with" and file
// associations. Arguments starting with '-' or '/' are switches (none are
// defined yet) and are skipped; a Windows path never starts with either. The
// first remaining argument is the file.
#pragma once

#include <string>
#include <vector>

namespace pdf {

// `args` includes argv[0] (the exe). Empty when no file was given.
inline std::wstring FileFromArgs(const std::vector<std::wstring>& args) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        if (a.empty() || a[0] == L'-' || a[0] == L'/') continue;
        return a;
    }
    return {};
}

}  // namespace pdf
