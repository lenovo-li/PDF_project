// DocxWriter.h — AnalyzedDocument → .docx bytes (WordprocessingML, ECMA-376
// Transitional), pure: no PDFium, no file I/O.
//
// Package: [Content_Types].xml, _rels/.rels, word/document.xml,
// word/styles.xml, word/_rels/document.xml.rels, word/media/imageN.png.
// One w:sectPr per Section (page size, orientation, margins); paragraphs carry
// alignment, indents, spacing and page breaks; runs carry font, size, bold,
// italic and colour. Scanned pages become inline pictures.
#pragma once

#include "Model.h"
#include "../Result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pdf {

Result<std::vector<std::uint8_t>> WriteDocx(const AnalyzedDocument& doc);

// Exposed for tests.
std::string ToUtf8(const std::u32string& s);
std::string XmlEscape(const std::string& utf8);  // & < > " '
// PDF font name ("ABCDEF+TimesNewRomanPS-BoldMT") → a name Word knows
// ("Times New Roman"); unknown names lose style suffixes ("Foo-Bold" → "Foo").
std::string WordFontName(const std::string& pdfName);

}  // namespace pdf
