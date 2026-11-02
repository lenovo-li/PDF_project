---
name: pattern-engine-thread-contract
description: Recurring review patterns in PDFTool - engine-thread rule, generation tokens, unbounded sizes, stale state across modals, output paths, engine exceptions, per-item-only caps, UI-normalized change checks, null required PDFium callbacks, fixture gaps
metadata:
  type: project
---

The "every FPDF_* call on the engine thread" rule is documented (src/PDFCore/Document.h, Engine.h, DocumentStore.h) but not asserted at runtime. `Engine::OnEngineThread()` is only used in Engine.cpp for RunSync re-entrancy.

**Why:** a UI-thread call would silently work in tests (tests wrap bodies in pdftest::OnEngine) and only race in the real app.

**How to apply:** when reviewing new PDFCore entry points, trace every caller to an Engine::Post/RunSync job. Suggesting a debug assert is a recommendation, not a defect.

Recurring defect shapes seen here:
- Stale-result guards (`std::weak_ptr<int>` generation tokens) are checked only in the UI-side `app->Post` lambda, so the engine still does the stale work. Examples: PdfPageView.cpp RequestRender, PdfPageViewText.cpp RequestSelection. Check whether the engine side can bail early (`gen.expired()` is thread-safe).
- The opposite variant: the engine side does check, but reads the `int` value (`*gen.lock() != ticket`) while the UI thread does `++*generation_` - a non-atomic data race (first seen in SearchRunner.cpp PostChunk, since fixed with shared_ptr<atomic<int>>). Only `expired()` / a fresh shared_ptr per run, or `std::atomic<int>`, is safe cross-thread.
- Sanity checks written as `v > 0 && isfinite(v)` (PageLayout.cpp `Sane`, Document.cpp DisplayedPageSizesPt `> 0`) accept finite huge values; look for missing upper bounds on file-supplied sizes.
- Failure sets (`failed_`) cleared on zoom/doc change but not on device restore.
- "Window-wide" shortcuts via root `PreviewKeyDown` only fire when some element has focus: FluentUI InputManager::KeyDown routes from the focused element only, and click on a non-focusable spot clears focus. Seen in MainWindowSearch.cpp (Ctrl+F/F3).
- Code paths that set `selecting_ = false` (Esc, ClearSelection) without releasing pointer capture; OnPointerReleased then skips ReleaseCapture.
- std::function callbacks that can re-enter Start() (which reassigns cb_) while the callback is executing - check SearchRunner-style runners.
- Tests: `if (r) EXPECT_...` without a preceding `EXPECT_TRUE(r.ok())` passes silently; recurs in each new test file (TextSearchTest.cpp too). DocumentStoreTest OnePageDoc still calls `.value()` unchecked.
- UI-control logic (zoom anchor, FitWidth, stale-result dropping, StepMatch wrap) tends to live inside the control/window instead of pure PDFCore functions, so it is untested.
- Precondition checked before a modal (FileDialog/ContentDialog), state captured after it. Modals pump messages, so engine callbacks (open result, edit done) can change docPath_/dirty_/doc id in between. Seen in MainWindowTools.cpp (RequireSavedFile then docPath_ captured after AskSavePath).
- UI flags updated only in the completion callback (dirty_ set in OnEditDone), so an in-flight edit is invisible to guards like RequireSavedFile.
- Output path never compared with the open file or other inputs (MainWindowTools.cpp tools; split NumberedPath outputs bypass the save dialog's overwrite prompt). No unsaved-changes prompt on open/close.
- Engine::ThreadMain runs job() with no try/catch, so any uncaught bad_alloc on the engine thread is std::terminate. Check new engine code that allocates outside RenderPage's catch (ImageExport.cpp SavePng copy).
- Caps are per item only (per string, per option list, per field count) with no total byte budget; capped results are then copied again on the UI thread outside any catch. Seen in Forms.cpp ListFormFields (radio options uncapped) -> FillFormDialog copies.
- Loop caps named for one thing but counting another (Forms.cpp kMaxWidgetsScanned counts every annot), and hitting a cap returns success with a partial count, so the UI reports "done".
- "Changed?" comparisons done after a UI control normalizes the value (TextArea turns newlines into '\n') against the raw engine value. Seen in FillFormDialog::Run.
- PDFium FPDF_FORMFILLINFO with callbacks the header marks "Required: yes" left null (FFI_GetPage): fine for crashes (guarded), but features that need the callback fail silently. Check header requirement lines when reviewing form/JS code.
- Fixtures cover only the easy kinds; whole branches (radio groups, multi-page fields) have no test. Check each switch case in new engine code against the fixture.
- Per-item caps again in convert/Extract.cpp (kMaxCharsPerPage per page, only images have a doc-wide budget): check every new pipeline for a total budget across pages.
- `std::clamp(v, constant, pageSize / k)` where page size comes from the file: lo > hi on tiny pages is UB (MSVC debug asserts). Seen in convert/Analyze.cpp margins.
- Raw PDFium byte strings (FPDFText_GetFontInfo BaseFont) passed into XML after only XmlEscape, with no UTF-8/XML-char validation. Seen in convert/DocxWriter.cpp RunProps.
- Runners check the cancel ticket only at job start; a long final job (FinishDocx + WriteFileAtomic) still writes after Cancel. Seen in ConvertRunner.cpp PostFinish.
- Gating on `FPDF_GetFormType == FORMTYPE_ACRO_FORM` excludes XFA-foreground hybrids that do have AcroForm fields (Document.cpp Form()).
- Tests that only feed a hand-built value into a one-line predicate (DocumentTest IsPasswordError with Error{.., FPDF_ERR_PASSWORD}) instead of driving the real path (Document::Open on an encrypted file).
