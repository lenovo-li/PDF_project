#include "../framework/Test.h"

#include <PDFCore/CommandLine.h>
#include <PDFCore/RecentFiles.h>

#include <string>
#include <vector>

using namespace pdf;

TEST(RecentFiles, PushMovesToFrontWithoutDuplicates) {
    std::vector<std::wstring> l = {L"C:\\a.pdf", L"C:\\b.pdf", L"C:\\c.pdf"};
    l = PushRecent(l, L"c:\\B.PDF");  // same file, other case
    EXPECT_EQ(l.size(), 3u);
    EXPECT_EQ(l[0], std::wstring(L"c:\\B.PDF"));
    EXPECT_EQ(l[1], std::wstring(L"C:\\a.pdf"));
    EXPECT_EQ(l[2], std::wstring(L"C:\\c.pdf"));
}

TEST(RecentFiles, CapDropsTheOldest) {
    std::vector<std::wstring> l;
    for (int i = 0; i < kMaxRecentFiles + 3; ++i)
        l = PushRecent(l, L"C:\\f" + std::to_wstring(i) + L".pdf");
    EXPECT_EQ(static_cast<int>(l.size()), kMaxRecentFiles);
    EXPECT_EQ(l.front(), L"C:\\f" + std::to_wstring(kMaxRecentFiles + 2) + L".pdf");
    EXPECT_EQ(l.back(), std::wstring(L"C:\\f3.pdf"));
}

TEST(RecentFiles, RemoveAndNormalize) {
    std::vector<std::wstring> l = {L"", L"C:\\a.pdf", L"C:\\A.pdf", L"C:\\b.pdf"};
    l = NormalizeRecent(l);
    EXPECT_EQ(l.size(), 2u);
    l = RemoveRecent(l, L"c:\\a.PDF");
    EXPECT_EQ(l.size(), 1u);
    if (!l.empty()) EXPECT_EQ(l[0], std::wstring(L"C:\\b.pdf"));
    EXPECT_EQ(PushRecent(l, L"").size(), 1u);  // an empty path is never added
}

TEST(RecentFiles, LabelShortensTheFolderFromTheLeft) {
    EXPECT_EQ(RecentLabel(0, L"C:\\docs\\x.pdf"), std::wstring(L"1  x.pdf  —  C:\\docs"));
    EXPECT_EQ(RecentLabel(2, L"x.pdf"), std::wstring(L"3  x.pdf"));
    const std::wstring label = RecentLabel(0, L"C:\\aaaa\\bbbb\\cccc\\x.pdf", 8);
    EXPECT_EQ(label, std::wstring(L"1  x.pdf  —  …bb\\cccc"));
}

TEST(CommandLine, FirstNonSwitchArgumentIsTheFile) {
    EXPECT_EQ(FileFromArgs({L"PDFTool.exe"}), std::wstring());
    EXPECT_EQ(FileFromArgs({}), std::wstring());
    EXPECT_EQ(FileFromArgs({L"PDFTool.exe", L"-x", L"/y", L"C:\\a b.pdf", L"c.pdf"}),
              std::wstring(L"C:\\a b.pdf"));
}
