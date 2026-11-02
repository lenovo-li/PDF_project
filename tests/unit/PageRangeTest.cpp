// PageRange: parsing user-typed ranges, caps, and splitting.

#include "../framework/Test.h"

#include <PDFCore/PageRange.h>

#include <vector>

using namespace pdf;

TEST(PageRange, ParsesListsAndRanges) {
    auto r = ParsePageRange(L"1, 3,5-7", 10);
    EXPECT_TRUE(r.ok());
    if (r) EXPECT_TRUE(r.value() == std::vector<int>({0, 2, 4, 5, 6}));
    auto d = ParsePageRange(L"4-2", 10);  // descending
    if (d) EXPECT_TRUE(d.value() == std::vector<int>({3, 2, 1}));
    auto t = ParsePageRange(L"2,", 10);  // trailing comma tolerated
    if (t) EXPECT_TRUE(t.value() == std::vector<int>({1}));
    auto dup = ParsePageRange(L"1,1", 3);  // duplicates kept
    if (dup) EXPECT_TRUE(dup.value() == std::vector<int>({0, 0}));
}

TEST(PageRange, RejectsMalformedAndOutOfRange) {
    for (const wchar_t* bad : {L"", L"0", L"11", L"a", L"1-", L"-3", L"1,,2", L"1-2-3", L"1.5",
                               L"9999999999"}) {
        EXPECT_FALSE(ParsePageRange(bad, 10).ok());
    }
    EXPECT_FALSE(ParsePageRange(L"1", 0).ok());
}

TEST(PageRange, CapsRepeatedRanges) {
    std::wstring big;
    for (int i = 0; i < 100; ++i) big += L"1-10,";
    EXPECT_FALSE(ParsePageRange(big, 10).ok());  // 1000 entries > 4*10+16
    EXPECT_TRUE(ParsePageRange(L"1-10,1-10,1-10", 10).ok());
}

TEST(PageRange, SplitEveryCoversAllPagesOnce) {
    auto s = SplitEvery(7, 3);
    EXPECT_EQ(s.size(), static_cast<std::size_t>(3));
    if (s.size() == 3) {
        EXPECT_TRUE(s[0] == std::vector<int>({0, 1, 2}));
        EXPECT_TRUE(s[2] == std::vector<int>({6}));
    }
    EXPECT_TRUE(SplitEvery(0, 3).empty());
    EXPECT_TRUE(SplitEvery(5, 0).empty());
}

TEST(PageRange, ParsePositiveInt) {
    auto a = ParsePositiveInt(L" 3 ", 10);
    EXPECT_TRUE(a.ok());
    if (a) EXPECT_EQ(a.value(), 3);
    EXPECT_FALSE(ParsePositiveInt(L"0", 10).ok());
    EXPECT_FALSE(ParsePositiveInt(L"11", 10).ok());
    EXPECT_FALSE(ParsePositiveInt(L"1-2", 10).ok());
    EXPECT_FALSE(ParsePositiveInt(L"", 10).ok());
    EXPECT_FALSE(ParsePositiveInt(L"5", 0).ok());
}
