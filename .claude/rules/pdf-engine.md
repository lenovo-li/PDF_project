---
paths:
  - "src/PDFCore/**"
  - "tests/**"
---

## PDF engine rules

**One engine thread.** PDFium is not thread-safe, including across different
documents (`FPDF_InitLibrary` state is global). `PDFCore` owns a single worker
thread; every `FPDF_*` call is queued onto it. The UI thread submits a job and
gets the result back through a callback that the UI layer marshals to its own
thread. Never call PDFium from the UI thread "just for a quick page count".
qpdf objects are not shared between threads either; a `QPDF` instance is used
by one job at a time.

**Lifetime.** Wrap every handle in RAII (`FPDF_DOCUMENT` → `FPDF_CloseDocument`,
`FPDF_PAGE` → `FPDF_ClosePage`, `FPDF_TEXTPAGE` → `FPDFText_ClosePage`,
`FPDF_BITMAP` → `FPDFBitmap_Destroy`). Close children before parents: text
page, then page, then document. A wrapper outliving its document is a bug.

**Untrusted input.** Every PDF can be malformed or hostile. Check page index
ranges, cap rendered bitmap size (width × height × 4 computed in 64-bit and
compared against a limit), cap loop counts that come from the file, and turn
failures into `Result` errors with `FPDF_GetLastError()` in the message. Never
`assert` on file content.

**Never damage the user's file.** Save to a temp file in the same directory,
then `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)`. On any failure the
original stays untouched. Passwords and decrypted text are never written to
the trace.

**Coordinates.** PDF user space is points (1/72 in), origin bottom-left,
y up, and pages can have `/Rotate` and a non-zero MediaBox origin. Keep the
page → DIP transform in one pure function with tests covering rotation 0/90/
180/270 and offset boxes; everything else uses it.

**Strings.** PDFium returns UTF-16LE through a size-then-fill protocol
(first call with a null buffer returns the byte count including the NUL).
Convert once at the wrapper boundary to `std::wstring`.

**Tests** run headless against small PDFs in `tests/data/`. Generate test
PDFs with PDFium or qpdf in the test itself when possible, so the expected
content is known.
