// FormFixture.h — builds a small AcroForm PDF in memory for form tests.
//
// PDFium cannot create form fields, so the test PDF is written by hand. The
// xref offsets are computed while writing, so the fixture is a valid PDF
// rather than one PDFium has to repair.
//
// Page: 300 x 300 pt. Fields (all on page 0):
//   "name"   text field     rect [20 240 280 270], value "Alice"
//   "agree"  check box      rect [20 200 40 220],  off, on-state "Yes"
//   "color"  combo box      rect [20 150 200 180], options Red/Green/Blue, value "Green"
//   "locked" text field     rect [20 100 280 130], value "fixed", read-only (Ff bit 1)
//
// MakeFormPdf2: two 300 x 300 pt pages.
//   "size"   radio group (parent with two kids on page 0): "S" rect [20 240 40 260],
//            "L" rect [60 240 80 260]; value /L
//   "shared" text field with one widget per page, rect [20 100 280 130], value "one"
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pdftest {

// Serializes objects 1..N (objs[i] is object i + 1, without "N 0 obj") with a
// correct xref table; object 1 is the catalog.
inline std::vector<std::uint8_t> BuildPdf(const std::vector<std::string>& objs) {
    std::string out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objs.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    for (std::size_t off : offsets) {
        char line[24];
        snprintf(line, sizeof(line), "%010zu 00000 n \n", off);
        out += line;
    }
    out += "trailer\n<< /Size " + std::to_string(objs.size() + 1) +
           " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return {out.begin(), out.end()};
}

inline std::vector<std::uint8_t> MakeFormPdf() {
    std::vector<std::string> objs;  // objs[i] is object i + 1, without "N 0 obj"
    objs.push_back("<< /Type /Catalog /Pages 2 0 R /AcroForm 4 0 R >>");                 // 1
    objs.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");                          // 2
    objs.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] "
                   "/Annots [5 0 R 6 0 R 7 0 R 8 0 R] /Resources << >> >>");               // 3
    objs.push_back("<< /Fields [5 0 R 6 0 R 7 0 R 8 0 R] /NeedAppearances true "
                   "/DA (/Helv 12 Tf 0 g) /DR << /Font << /Helv 9 0 R >> >> >>");          // 4
    objs.push_back("<< /Type /Annot /Subtype /Widget /FT /Tx /T (name) /V (Alice) "
                   "/Rect [20 240 280 270] /P 3 0 R /F 4 /DA (/Helv 12 Tf 0 g) >>");       // 5
    objs.push_back("<< /Type /Annot /Subtype /Widget /FT /Btn /T (agree) /V /Off /AS /Off "
                   "/Rect [20 200 40 220] /P 3 0 R /F 4 /MK << /CA (4) >> "
                   "/DA (/ZaDb 12 Tf 0 g) /AP << /N << /Yes 10 0 R /Off 11 0 R >> >> >>"); // 6
    objs.push_back("<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 131072 /T (color) "
                   "/Opt [(Red) (Green) (Blue)] /V (Green) /Rect [20 150 200 180] /P 3 0 R "
                   "/F 4 /DA (/Helv 12 Tf 0 g) >>");                                       // 7
    objs.push_back("<< /Type /Annot /Subtype /Widget /FT /Tx /Ff 1 /T (locked) /V (fixed) "
                   "/Rect [20 100 280 130] /P 3 0 R /F 4 /DA (/Helv 12 Tf 0 g) >>");       // 8
    objs.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                   "/Encoding /WinAnsiEncoding >>");                                       // 9
    // Check box appearances: a filled square when on, nothing when off.
    const std::string on = "0 g 3 3 14 14 re f";
    objs.push_back("<< /Type /XObject /Subtype /Form /BBox [0 0 20 20] /Length " +
                   std::to_string(on.size()) + " >>\nstream\n" + on + "\nendstream");     // 10
    objs.push_back("<< /Type /XObject /Subtype /Form /BBox [0 0 20 20] /Length 0 >>\n"
                   "stream\n\nendstream");                                                 // 11

    return BuildPdf(objs);
}

// Radio appearances: on = filled square, off = empty.
inline std::vector<std::uint8_t> MakeFormPdf2() {
    const std::string on = "0 g 4 4 12 12 re f";
    const std::string onObj = "<< /Type /XObject /Subtype /Form /BBox [0 0 20 20] /Length " +
                              std::to_string(on.size()) + " >>\nstream\n" + on + "\nendstream";
    const std::string offObj =
        "<< /Type /XObject /Subtype /Form /BBox [0 0 20 20] /Length 0 >>\nstream\n\nendstream";
    std::vector<std::string> objs;
    objs.push_back("<< /Type /Catalog /Pages 2 0 R /AcroForm 5 0 R >>");                 // 1
    objs.push_back("<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>");                    // 2
    objs.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] "
                   "/Annots [7 0 R 8 0 R 10 0 R] /Resources << >> >>");                   // 3
    objs.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] "
                   "/Annots [11 0 R] /Resources << >> >>");                               // 4
    objs.push_back("<< /Fields [6 0 R 9 0 R] /NeedAppearances true "
                   "/DA (/Helv 12 Tf 0 g) /DR << /Font << /Helv 12 0 R >> >> >>");        // 5
    // Radio parent: Ff bit 16 (radio) + bit 15 (no toggle to off).
    objs.push_back("<< /FT /Btn /Ff 49152 /T (size) /V /L /Kids [7 0 R 8 0 R] >>");       // 6
    objs.push_back("<< /Type /Annot /Subtype /Widget /Parent 6 0 R /AS /Off "
                   "/Rect [20 240 40 260] /P 3 0 R /F 4 /MK << /CA (l) >> "
                   "/DA (/ZaDb 12 Tf 0 g) /AP << /N << /S 13 0 R /Off 14 0 R >> >> >>");  // 7
    objs.push_back("<< /Type /Annot /Subtype /Widget /Parent 6 0 R /AS /L "
                   "/Rect [60 240 80 260] /P 3 0 R /F 4 /MK << /CA (l) >> "
                   "/DA (/ZaDb 12 Tf 0 g) /AP << /N << /L 15 0 R /Off 16 0 R >> >> >>");  // 8
    objs.push_back("<< /FT /Tx /T (shared) /V (one) /DA (/Helv 12 Tf 0 g) "
                   "/Kids [10 0 R 11 0 R] >>");                                           // 9
    objs.push_back("<< /Type /Annot /Subtype /Widget /Parent 9 0 R "
                   "/Rect [20 100 280 130] /P 3 0 R /F 4 >>");                            // 10
    objs.push_back("<< /Type /Annot /Subtype /Widget /Parent 9 0 R "
                   "/Rect [20 100 280 130] /P 4 0 R /F 4 >>");                            // 11
    objs.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                   "/Encoding /WinAnsiEncoding >>");                                      // 12
    objs.push_back(onObj);   // 13
    objs.push_back(offObj);  // 14
    objs.push_back(onObj);   // 15
    objs.push_back(offObj);  // 16
    return BuildPdf(objs);
}

}  // namespace pdftest
