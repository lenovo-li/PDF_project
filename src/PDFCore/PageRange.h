// PageRange.h — parse "1,3,5-7" style page ranges typed by the user.
//
// Pure function so the dialog and the tests share one parser. Pages are
// 1-based in the text (what users see) and 0-based in the result. Untrusted
// text: the result size is capped at pageCount (a range like "1-999999999"
// cannot allocate a billion ints), and any malformed token is an error that
// names the token, rather than silently skipping it.
#pragma once

#include "Result.h"

#include <string>
#include <vector>

namespace pdf {

// Accepts digits, ',', '-', and spaces. "5-3" is a descending range (5,4,3).
// "-" alone, "3-", "0", and pages above pageCount are errors. Duplicates are
// kept in order (extract "1,1" makes two copies), but the total is capped.
Result<std::vector<int>> ParsePageRange(const std::wstring& text, int pageCount);

// A whole number in [1, max] typed by the user (surrounding spaces allowed).
Result<int> ParsePositiveInt(const std::wstring& text, int max);

// Splits [0, pageCount) into consecutive chunks of `pagesPerFile` pages.
std::vector<std::vector<int>> SplitEvery(int pageCount, int pagesPerFile);

}  // namespace pdf
