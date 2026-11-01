// QpdfOps.h — structural operations implemented with qpdf.
//
// qpdf handles whole-file transformations that PDFium does not do well:
// merge, split, encrypt/decrypt, linearize, repair. These functions do not
// touch PDFium and keep no global state, so they may run on any worker
// thread; each call owns its own QPDF objects. qpdf reports failures by
// throwing; every function here catches and converts to a Result so no
// exception crosses the PDFCore boundary.
#pragma once

#include "Result.h"

#include <string>
#include <vector>

namespace pdf {

// Number of pages according to qpdf (used to cross-check PDFium in tests).
Result<int> QpdfPageCount(const std::wstring& path);

// Concatenates all pages of `inputs` in order into `output` (atomic write).
Status MergeFiles(const std::vector<std::wstring>& inputs, const std::wstring& output);

}  // namespace pdf
