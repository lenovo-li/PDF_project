#include "../framework/FormFixture.h"
#include "../framework/Test.h"
#include "../framework/TestEngine.h"

// Forms: list fields, edit each kind, read-only is left alone, edits render and
// survive a save/reopen. The PDF is built by FormFixture.h.

#include <PDFCore/Document.h>
#include <PDFCore/Forms.h>

#include <windows.h>

using namespace pdf;

namespace {
const FormField* Find(const FormFieldList& l, const wchar_t* name) {
    for (const FormField& f : l.fields)
        if (f.name == name) return &f;
    return nullptr;
}

// Dark pixels inside a PDF-space box on a 300 x 300 pt page rendered 1:1.
int DarkPixels(const Bitmap& b, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            if ((b.At(x, 300 - y) & 0xFF) < 0x80) ++n;
    return n;
}
}  // namespace

TEST(Forms, BlankDocumentHasNoForm) {
    pdftest::OnEngine([] {
        auto doc = Document::CreateEmpty();
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        EXPECT_TRUE(doc.value().AddBlankPage(100, 100).ok());
        EXPECT_FALSE(HasForm(doc.value()));
        auto list = ListFormFields(doc.value());
        EXPECT_TRUE(list.ok());
        if (list) EXPECT_EQ(list.value().fields.size(), 0u);
        EXPECT_FALSE(ApplyFormEdits(doc.value(), {{L"x", L"y"}}).ok());
    });
}

TEST(Forms, ListsEveryFieldWithTypeValueAndOptions) {
    pdftest::OnEngine([] {
        auto doc = Document::OpenMemory(pdftest::MakeFormPdf());
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        EXPECT_TRUE(HasForm(doc.value()));
        auto list = ListFormFields(doc.value());
        if (!list) { EXPECT_TRUE(list.ok()); return; }
        EXPECT_EQ(list.value().fields.size(), 4u);
        EXPECT_FALSE(list.value().truncated);
        const FormField* name = Find(list.value(), L"name");
        const FormField* agree = Find(list.value(), L"agree");
        const FormField* color = Find(list.value(), L"color");
        const FormField* locked = Find(list.value(), L"locked");
        EXPECT_TRUE(name && agree && color && locked);
        if (!name || !agree || !color || !locked) return;
        EXPECT_EQ(name->type, FieldType::Text);
        EXPECT_EQ(name->value, L"Alice");
        EXPECT_FALSE(name->readOnly);
        EXPECT_EQ(agree->type, FieldType::CheckBox);
        EXPECT_FALSE(agree->checked);
        EXPECT_EQ(color->type, FieldType::ComboBox);
        EXPECT_EQ(color->value, L"Green");
        EXPECT_EQ(color->options.size(), 3u);
        if (color->options.size() == 3u) EXPECT_EQ(color->options[2], L"Blue");
        EXPECT_TRUE(locked->readOnly);
        EXPECT_EQ(name->page, 0);
    });
}

TEST(Forms, FieldCapTruncates) {
    pdftest::OnEngine([] {
        auto doc = Document::OpenMemory(pdftest::MakeFormPdf());
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto list = ListFormFields(doc.value(), 100000, 2);
        if (!list) { EXPECT_TRUE(list.ok()); return; }
        EXPECT_EQ(list.value().fields.size(), 2u);
        EXPECT_TRUE(list.value().truncated);
    });
}

TEST(Forms, EditsTextCheckBoxAndComboAndSkipsReadOnly) {
    pdftest::OnEngine([] {
        auto doc = Document::OpenMemory(pdftest::MakeFormPdf());
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        std::vector<FieldEdit> edits;
        edits.push_back({L"name", L"张三 Bob"});
        FieldEdit agree;
        agree.name = L"agree";
        agree.newChecked = true;
        edits.push_back(agree);
        edits.push_back({L"color", L"Blue"});
        edits.push_back({L"locked", L"changed"});
        auto n = ApplyFormEdits(doc.value(), edits);
        if (!n) { EXPECT_TRUE(n.ok()); return; }
        EXPECT_EQ(n.value(), 3);  // the read-only field is not counted
        auto list = ListFormFields(doc.value());
        if (!list) { EXPECT_TRUE(list.ok()); return; }
        const FormField* name = Find(list.value(), L"name");
        const FormField* a = Find(list.value(), L"agree");
        const FormField* color = Find(list.value(), L"color");
        const FormField* locked = Find(list.value(), L"locked");
        if (!name || !a || !color || !locked) { EXPECT_TRUE(false); return; }
        EXPECT_EQ(name->value, L"张三 Bob");
        EXPECT_TRUE(a->checked);
        EXPECT_EQ(color->value, L"Blue");
        EXPECT_EQ(locked->value, L"fixed");
    });
}

TEST(Forms, UncheckingACheckedBox) {
    pdftest::OnEngine([] {
        auto doc = Document::OpenMemory(pdftest::MakeFormPdf());
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        FieldEdit on;
        on.name = L"agree";
        on.newChecked = true;
        EXPECT_TRUE(ApplyFormEdits(doc.value(), {on}).ok());
        FieldEdit off = on;
        off.newChecked = false;
        EXPECT_TRUE(ApplyFormEdits(doc.value(), {off}).ok());
        auto list = ListFormFields(doc.value());
        if (!list) { EXPECT_TRUE(list.ok()); return; }
        const FormField* a = Find(list.value(), L"agree");
        EXPECT_TRUE(a && !a->checked);
    });
}

// The viewer shows fields through RenderPage, so edits must change pixels:
// a value drawn in the text field, the check box's on-appearance.
TEST(Forms, FieldsAndEditsAreRendered) {
    pdftest::OnEngine([] {
        auto doc = Document::OpenMemory(pdftest::MakeFormPdf());
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto before = doc.value().RenderPage(0, 300, 300);
        if (!before) { EXPECT_TRUE(before.ok()); return; }
        EXPECT_TRUE(DarkPixels(before.value(), 22, 242, 278, 268) > 20);  // "Alice"
        EXPECT_TRUE(DarkPixels(before.value(), 22, 202, 38, 218) < 5);    // box off
        FieldEdit agree;
        agree.name = L"agree";
        agree.newChecked = true;
        EXPECT_TRUE(ApplyFormEdits(doc.value(), {{L"name", L""}, agree}).ok());
        auto after = doc.value().RenderPage(0, 300, 300);
        if (!after) { EXPECT_TRUE(after.ok()); return; }
        EXPECT_TRUE(DarkPixels(after.value(), 22, 242, 278, 268) < 5);    // emptied
        EXPECT_TRUE(DarkPixels(after.value(), 22, 202, 38, 218) > 20);    // box on
    });
}

TEST(Forms, EditsSurviveSaveAndReopen) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    const std::wstring path = std::wstring(dir) + L"PDFTool_form_save.pdf";
    pdftest::OnEngine([&] {
        auto doc = Document::OpenMemory(pdftest::MakeFormPdf());
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        FieldEdit agree;
        agree.name = L"agree";
        agree.newChecked = true;
        EXPECT_TRUE(ApplyFormEdits(doc.value(), {{L"name", L"Saved"}, agree,
                                                 {L"color", L"Red"}}).ok());
        EXPECT_TRUE(doc.value().SaveAs(path).ok());
    });
    pdftest::OnEngine([&] {
        auto doc = Document::Open(path);
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto list = ListFormFields(doc.value());
        if (!list) { EXPECT_TRUE(list.ok()); return; }
        const FormField* name = Find(list.value(), L"name");
        const FormField* a = Find(list.value(), L"agree");
        const FormField* color = Find(list.value(), L"color");
        EXPECT_TRUE(name && name->value == L"Saved");
        EXPECT_TRUE(a && a->checked);
        EXPECT_TRUE(color && color->value == L"Red");
    });
    DeleteFileW(path.c_str());
}

TEST(Forms, UnknownFieldAndUnknownOptionChangeNothing) {
    pdftest::OnEngine([] {
        auto doc = Document::OpenMemory(pdftest::MakeFormPdf());
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto n = ApplyFormEdits(doc.value(), {{L"nope", L"x"}, {L"color", L"Purple"}});
        if (!n) { EXPECT_TRUE(n.ok()); return; }
        EXPECT_EQ(n.value(), 0);
        auto list = ListFormFields(doc.value());
        if (!list) { EXPECT_TRUE(list.ok()); return; }
        const FormField* color = Find(list.value(), L"color");
        EXPECT_TRUE(color && color->value == L"Green");
    });
}

TEST(Forms, RadioGroupListsEachExportValueOnce) {
    pdftest::OnEngine([] {
        auto doc = Document::OpenMemory(pdftest::MakeFormPdf2());
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto list = ListFormFields(doc.value());
        if (!list) { EXPECT_TRUE(list.ok()); return; }
        EXPECT_EQ(list.value().fields.size(), 2u);
        const FormField* size = Find(list.value(), L"size");
        const FormField* shared = Find(list.value(), L"shared");
        if (!size || !shared) { EXPECT_TRUE(false); return; }
        EXPECT_EQ(size->type, FieldType::RadioButton);
        EXPECT_EQ(size->value, L"L");
        EXPECT_TRUE(size->checked);
        EXPECT_EQ(size->options.size(), 2u);
        if (size->options.size() == 2u) {
            EXPECT_EQ(size->options[0], L"S");
            EXPECT_EQ(size->options[1], L"L");
        }
        EXPECT_EQ(shared->page, 0);  // first widget's page
        EXPECT_EQ(shared->value, L"one");
    });
}

TEST(Forms, RadioEditSelectsTheMatchingButton) {
    pdftest::OnEngine([] {
        auto doc = Document::OpenMemory(pdftest::MakeFormPdf2());
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto n = ApplyFormEdits(doc.value(), {{L"size", L"S"}});
        if (!n) { EXPECT_TRUE(n.ok()); return; }
        EXPECT_EQ(n.value(), 1);
        auto list = ListFormFields(doc.value());
        if (!list) { EXPECT_TRUE(list.ok()); return; }
        const FormField* size = Find(list.value(), L"size");
        EXPECT_TRUE(size && size->value == L"S");
        // Rendered: the S button is filled now, the L button is empty.
        auto bmp = doc.value().RenderPage(0, 300, 300);
        if (!bmp) { EXPECT_TRUE(bmp.ok()); return; }
        EXPECT_TRUE(DarkPixels(bmp.value(), 24, 244, 36, 256) > 20);
        EXPECT_TRUE(DarkPixels(bmp.value(), 64, 244, 76, 256) < 5);
    });
}

// One field, a widget on each page: editing it through the first widget must
// redraw the second page's widget too.
TEST(Forms, FieldSpanningPagesUpdatesEveryWidget) {
    pdftest::OnEngine([] {
        auto doc = Document::OpenMemory(pdftest::MakeFormPdf2());
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto before = doc.value().RenderPage(1, 300, 300);
        if (!before) { EXPECT_TRUE(before.ok()); return; }
        EXPECT_TRUE(DarkPixels(before.value(), 22, 102, 278, 128) > 10);  // "one"
        auto n = ApplyFormEdits(doc.value(), {{L"shared", L""}});
        if (!n) { EXPECT_TRUE(n.ok()); return; }
        EXPECT_EQ(n.value(), 1);
        auto after = doc.value().RenderPage(1, 300, 300);
        if (!after) { EXPECT_TRUE(after.ok()); return; }
        EXPECT_TRUE(DarkPixels(after.value(), 22, 102, 278, 128) < 5);
    });
}

TEST(Forms, TextBudgetTruncatesTheList) {
    pdftest::OnEngine([] {
        // A text field whose value is 60000 chars, repeated 12 times: over the
        // 512 Ki-unit total budget well before the field cap.
        const std::string big(60000, 'x');
        std::vector<std::string> objs;
        std::string annots, fields;
        objs.push_back("<< /Type /Catalog /Pages 2 0 R /AcroForm 4 0 R >>");
        objs.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
        for (int i = 0; i < 12; ++i) {
            const std::string ref = std::to_string(5 + i) + " 0 R ";
            annots += ref;
            fields += ref;
        }
        objs.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Annots [" + annots +
                       "] /Resources << >> >>");
        objs.push_back("<< /Fields [" + fields + "] /DA (/Helv 12 Tf 0 g) >>");
        for (int i = 0; i < 12; ++i)
            objs.push_back("<< /Type /Annot /Subtype /Widget /FT /Tx /T (f" + std::to_string(i) +
                           ") /V (" + big + ") /Rect [0 0 10 10] /P 3 0 R >>");
        auto doc = Document::OpenMemory(pdftest::BuildPdf(objs));
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto list = ListFormFields(doc.value());
        if (!list) { EXPECT_TRUE(list.ok()); return; }
        EXPECT_TRUE(list.value().truncated);
        EXPECT_TRUE(list.value().fields.size() < 12u);
        EXPECT_TRUE(list.value().fields.size() >= 8u);
    });
}

namespace {
// One page, the given AcroForm extras and field objects (objects 5.. in order,
// each a widget on page 3). Font is object 4.
std::vector<std::uint8_t> OnePageForm(const std::string& acroExtra,
                                      const std::vector<std::string>& widgets) {
    std::vector<std::string> objs;
    std::string refs;
    for (std::size_t i = 0; i < widgets.size(); ++i) refs += std::to_string(5 + i) + " 0 R ";
    objs.push_back("<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [" + refs +
                   "] /DA (/Helv 12 Tf 0 g) /DR << /Font << /Helv 4 0 R >> >> " + acroExtra +
                   " >> >>");
    objs.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objs.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Annots [" + refs +
                   "] /Resources << >> >>");
    objs.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    for (const std::string& w : widgets) objs.push_back(w);
    return pdftest::BuildPdf(objs);
}
}  // namespace

// A value over the 64 Ki-char read cap is not read, so the field must not be
// editable: applying the dialog would otherwise wipe the real text.
TEST(Forms, OverlongValueIsReadOnly) {
    pdftest::OnEngine([] {
        const std::string big(70000, 'x');
        auto doc = Document::OpenMemory(OnePageForm("", {
            "<< /Type /Annot /Subtype /Widget /FT /Tx /T (long) /V (" + big +
            ") /Rect [10 10 290 40] /P 3 0 R >>"}));
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto list = ListFormFields(doc.value());
        if (!list || list.value().fields.size() != 1u) { EXPECT_TRUE(false); return; }
        EXPECT_TRUE(list.value().fields[0].readOnly);
        EXPECT_TRUE(list.value().fields[0].value.empty());
    });
}

// A hybrid document (AcroForm plus /XFA): this build has no XFA, so the
// AcroForm fields must still be listed and editable.
TEST(Forms, HybridXfaDocumentUsesItsAcroForm) {
    pdftest::OnEngine([] {
        auto doc = Document::OpenMemory(OnePageForm("/XFA [(template) 5 0 R]", {
            "<< /Type /Annot /Subtype /Widget /FT /Tx /T (t) /V (a) /Rect [10 10 290 40] /P 3 0 R >>"}));
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto list = ListFormFields(doc.value());
        if (!list) { EXPECT_TRUE(list.ok()); return; }
        EXPECT_EQ(list.value().fields.size(), 1u);
        auto n = ApplyFormEdits(doc.value(), {{L"t", L"b"}});
        EXPECT_TRUE(n.ok() && n.value() == 1);
    });
}

TEST(Forms, MultiSelectListKeepsOnlyTheNewValue) {
    pdftest::OnEngine([] {
        // Ff bit 22: multi-select. A and C selected.
        auto doc = Document::OpenMemory(OnePageForm("", {
            "<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 2097152 /T (pick) "
            "/Opt [(A) (B) (C)] /V [(A) (C)] /I [0 2] /Rect [10 10 290 100] /P 3 0 R "
            "/DA (/Helv 12 Tf 0 g) >>"}));
        if (!doc) { EXPECT_TRUE(doc.ok()); return; }
        auto n = ApplyFormEdits(doc.value(), {{L"pick", L"B"}});
        EXPECT_TRUE(n.ok() && n.value() == 1);
        auto list = ListFormFields(doc.value());
        if (!list || list.value().fields.size() != 1u) { EXPECT_TRUE(false); return; }
        EXPECT_EQ(list.value().fields[0].type, FieldType::ListBox);
        EXPECT_EQ(list.value().fields[0].value, std::wstring(L"B"));
    });
}
