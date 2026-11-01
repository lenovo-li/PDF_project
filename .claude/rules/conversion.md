---
paths:
  - "src/PDFCore/convert/**"
  - "tests/**"
---

## Conversion rules (PDF → Word and similar)

Neither PDFium nor qpdf converts to DOCX; this project implements it. Keep
the stages separate so each can be tested alone:

1. **Extract** (PDFium): characters with font, size, color and bounding box
   (`FPDFText_*`), images and vector paths (`FPDFPageObj_*`), page size and
   rotation. Output is a plain data model with no PDFium handles.
2. **Analyze** (pure functions): characters → words → lines → paragraphs,
   columns, tables, headers/footers, reading order. Deterministic, headless,
   tested on hand-built inputs.
3. **Write** (pure functions + a zip writer): the analyzed model →
   WordprocessingML. Produce a minimal valid package; validate structure in
   tests by reading the generated XML back.

Pages without a text layer (scans) need OCR; that is a separate feature and a
new dependency, so ask the user before adding it.

**Fidelity is measured, not eyeballed.** For each sample in `tests/data/`
keep an expected-structure fixture (paragraph count, table shapes, text per
paragraph) and assert on it. Visual comparison in Word is a user check on
real hardware.

**Licensing.** pdf2docx, PyMuPDF, MuPDF and Poppler are GPL/AGPL; do not use or
port their code. Algorithms from papers and specs are fine.
