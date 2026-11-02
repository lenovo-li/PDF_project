#include "Forms.h"

#include "Document.h"
#include "Trace.h"

#include <fpdf_annot.h>
#include <fpdf_formfill.h>
#include <fpdfview.h>

#include <algorithm>
#include <memory>
#include <new>
#include <unordered_map>

namespace pdf {

namespace {
struct PageCloser {
    void operator()(fpdf_page_t__* p) const { FPDF_ClosePage(p); }
};
struct AnnotCloser {
    void operator()(fpdf_annotation_t__* a) const { FPDFPage_CloseAnnot(a); }
};
using PagePtr = std::unique_ptr<fpdf_page_t__, PageCloser>;
using AnnotPtr = std::unique_ptr<fpdf_annotation_t__, AnnotCloser>;

// Hostile files: a page may carry tens of thousands of widgets, a choice field
// thousands of options, a value megabytes of text.
// Every annotation counts (links and notes too): the cap bounds the work of
// a scan, whatever the page holds.
constexpr int kMaxAnnotsScanned = 20000;
constexpr int kMaxOptions = 1000;  // per choice field or radio group
constexpr std::size_t kMaxFieldChars = 64 * 1024;
// Total UTF-16 units across every listed name, value and option (1 MiB): the
// list is copied to the UI thread and into the dialog's controls.
constexpr std::size_t kMaxListChars = 512 * 1024;

// A page attached to the form environment for the duration of one scope, so
// FORM_OnBeforeClosePage always runs before FPDF_ClosePage (Document.h).
class FormPage {
public:
    FormPage(FPDF_DOCUMENT doc, FPDF_FORMHANDLE form, int index)
        : form_(form), page_(FPDF_LoadPage(doc, index)) {
        if (page_) FORM_OnAfterLoadPage(page_.get(), form_);
    }
    ~FormPage() {
        if (page_) FORM_OnBeforeClosePage(page_.get(), form_);
    }
    FormPage(const FormPage&) = delete;
    FormPage& operator=(const FormPage&) = delete;
    FPDF_PAGE get() const { return page_.get(); }
    explicit operator bool() const { return page_ != nullptr; }

private:
    FPDF_FORMHANDLE form_;
    PagePtr page_;  // destroyed after the destructor body: detach, then close
};

FieldType TypeOf(int t) {
    switch (t) {
        case FPDF_FORMFIELD_PUSHBUTTON:  return FieldType::PushButton;
        case FPDF_FORMFIELD_CHECKBOX:    return FieldType::CheckBox;
        case FPDF_FORMFIELD_RADIOBUTTON: return FieldType::RadioButton;
        case FPDF_FORMFIELD_TEXTFIELD:   return FieldType::Text;
        case FPDF_FORMFIELD_COMBOBOX:    return FieldType::ComboBox;
        case FPDF_FORMFIELD_LISTBOX:     return FieldType::ListBox;
        case FPDF_FORMFIELD_SIGNATURE:   return FieldType::Signature;
        default:                         return FieldType::Unknown;
    }
}

// Size-then-fill UTF-16 read shared by the form string getters; capped.
template <typename Fn>
std::wstring ReadString(Fn fn) {
    const unsigned long bytes = fn(nullptr, 0);
    if (bytes < 2 || bytes > (kMaxFieldChars + 1) * 2) return {};
    std::wstring buf(bytes / 2, L'\0');
    fn(reinterpret_cast<FPDF_WCHAR*>(buf.data()), bytes);
    buf.resize(bytes / 2 - 1);
    return buf;
}

std::wstring FieldName(FPDF_FORMHANDLE f, FPDF_ANNOTATION a) {
    return ReadString([&](FPDF_WCHAR* b, unsigned long n) {
        return FPDFAnnot_GetFormFieldName(f, a, b, n);
    });
}
// True when the value is longer than kMaxFieldChars, so FieldValue returned
// "" for it: such a field must not be offered for editing, or applying the
// dialog would replace the real text with nothing.
bool ValueTooLong(FPDF_FORMHANDLE f, FPDF_ANNOTATION a) {
    return FPDFAnnot_GetFormFieldValue(f, a, nullptr, 0) > (kMaxFieldChars + 1) * 2;
}
std::wstring FieldValue(FPDF_FORMHANDLE f, FPDF_ANNOTATION a) {
    return ReadString([&](FPDF_WCHAR* b, unsigned long n) {
        return FPDFAnnot_GetFormFieldValue(f, a, b, n);
    });
}
std::wstring ExportValue(FPDF_FORMHANDLE f, FPDF_ANNOTATION a) {
    return ReadString([&](FPDF_WCHAR* b, unsigned long n) {
        return FPDFAnnot_GetFormFieldExportValue(f, a, b, n);
    });
}
std::wstring OptionLabel(FPDF_FORMHANDLE f, FPDF_ANNOTATION a, int i) {
    return ReadString([&](FPDF_WCHAR* b, unsigned long n) {
        return FPDFAnnot_GetOptionLabel(f, a, i, b, n);
    });
}

bool IsWidget(FPDF_ANNOTATION a) { return FPDFAnnot_GetSubtype(a) == FPDF_ANNOT_WIDGET; }

// A left click in the middle of the widget, as a user would toggle it.
bool ClickCenter(FPDF_FORMHANDLE form, FPDF_PAGE page, FPDF_ANNOTATION a) {
    FS_RECTF r{};
    if (!FPDFAnnot_GetRect(a, &r)) return false;
    const double x = (r.left + r.right) / 2, y = (r.top + r.bottom) / 2;
    FORM_OnMouseMove(form, page, 0, x, y);  // the widget is "entered" first, as with a real pointer
    FORM_OnLButtonDown(form, page, 0, x, y);
    FORM_OnLButtonUp(form, page, 0, x, y);
    return true;
}

// Replaces a text widget's whole value: focus, select all, type the new text.
// Focus is killed by the caller after all edits on this page finish.
bool ReplaceText(FPDF_FORMHANDLE form, FPDF_PAGE page, FPDF_ANNOTATION a,
                 const std::wstring& text) {
    if (!FORM_SetFocusedAnnot(form, a)) return false;
    FORM_SelectAllText(form, page);  // false on an empty field; replace still inserts
    FORM_ReplaceSelection(form, page, reinterpret_cast<FPDF_WIDESTRING>(text.c_str()));
    return true;
}
}  // namespace

bool HasForm(const Document& doc) { return FPDF_GetFormType(doc.Raw()) != FORMTYPE_NONE; }

Result<FormFieldList> ListFormFields(const Document& doc, int pageLimit, int fieldCap) {
    FormFieldList out;
    const int type = FPDF_GetFormType(doc.Raw());
    out.hasXfa = type == FORMTYPE_XFA_FULL || type == FORMTYPE_XFA_FOREGROUND;
    FPDF_FORMHANDLE form = doc.Form();
    if (!form) return out;  // no AcroForm: an empty list, not an error
    try {
        std::unordered_map<std::wstring, std::size_t> byName;
        const int pages = std::min(doc.PageCount(), std::max(0, pageLimit));
        int scanned = 0;
        std::size_t chars = 0;
        // Charges `n` units to the budget; false (and truncated) when over.
        auto charge = [&](std::size_t n) {
            chars += n;
            if (chars > kMaxListChars) out.truncated = true;
            return !out.truncated;
        };
        for (int p = 0; p < pages && !out.truncated; ++p) {
            // Attached to the form, not a bare FPDF_LoadPage: without the page
            // view FPDFAnnot_IsChecked reports a box ticked through the form
            // as unchecked (found by Forms.EditsTextCheckBoxAndCombo...).
            FormPage page(doc.Raw(), form, p);
            if (!page) continue;
            const int n = FPDFPage_GetAnnotCount(page.get());
            for (int i = 0; i < n; ++i) {
                if (++scanned > kMaxAnnotsScanned) {
                    out.truncated = true;
                    break;
                }
                AnnotPtr a(FPDFPage_GetAnnot(page.get(), i));
                if (!a || !IsWidget(a.get())) continue;
                const int ft = FPDFAnnot_GetFormFieldType(form, a.get());
                if (ft < 0) continue;  // a widget that is not in this form's field tree
                std::wstring name = FieldName(form, a.get());
                if (name.empty()) continue;
                const bool on = FPDFAnnot_IsChecked(form, a.get());
                auto it = byName.find(name);
                if (it != byName.end()) {
                    // Another widget of a known field. For a radio group the
                    // checked one carries the group's value.
                    FormField& f = out.fields[it->second];
                    if (f.type == FieldType::RadioButton) {
                        if (on) f.checked = true;
                        if (f.options.size() < static_cast<std::size_t>(kMaxOptions)) {
                            std::wstring v = ExportValue(form, a.get());
                            if (!charge(v.size())) break;
                            // Kids sharing an export value toggle together:
                            // one entry is enough.
                            if (std::find(f.options.begin(), f.options.end(), v) == f.options.end())
                                f.options.push_back(std::move(v));
                        }
                    }
                    continue;
                }
                if (static_cast<int>(out.fields.size()) >= fieldCap) {
                    out.truncated = true;
                    break;
                }
                FormField f;
                f.name = std::move(name);
                f.type = TypeOf(ft);
                f.value = FieldValue(form, a.get());
                f.checked = on;
                const int flags = FPDFAnnot_GetFormFieldFlags(form, a.get());
                f.readOnly = (flags & FPDF_FORMFLAG_READONLY) != 0 || ValueTooLong(form, a.get());
                f.multiline = f.type == FieldType::Text && (flags & FPDF_FORMFLAG_TEXT_MULTILINE);
                f.page = p;
                f.annotIndex = i;
                if (f.type == FieldType::ComboBox || f.type == FieldType::ListBox) {
                    const int count = std::min(FPDFAnnot_GetOptionCount(form, a.get()), kMaxOptions);
                    for (int k = 0; k < count && !out.truncated; ++k) {
                        std::wstring label = OptionLabel(form, a.get(), k);
                        if (charge(label.size())) f.options.push_back(std::move(label));
                    }
                } else if (f.type == FieldType::RadioButton) {
                    std::wstring v = ExportValue(form, a.get());
                    if (charge(v.size())) f.options.push_back(std::move(v));
                }
                if (!charge(f.name.size() + f.value.size())) break;
                byName.emplace(f.name, out.fields.size());
                out.fields.push_back(std::move(f));
            }
        }
    } catch (const std::bad_alloc&) {
        TraceF("[PDFTool.Forms] ListFormFields out of memory\n");
        return Error{L"内存不足"};
    }
    return out;
}

Result<int> ApplyFormEdits(Document& doc, const std::vector<FieldEdit>& edits) {
    FPDF_FORMHANDLE form = doc.Form();
    if (!form) return Error{L"文档没有可填写的表单"};
    if (edits.empty()) return 0;
    for (const FieldEdit& e : edits)
        if (e.name.empty() || e.newValue.size() > kMaxFieldChars) return Error{L"表单值无效"};
    std::unordered_map<std::wstring, const FieldEdit*> want;
    for (const FieldEdit& e : edits) want[e.name] = &e;
    // A field's value is shared by all its widgets, so each field is edited
    // once, through its first widget, and the others follow (PDFium rebuilds
    // every widget's appearance). A radio group is the exception: the edit
    // clicks the one widget whose export value matches.
    std::unordered_map<std::wstring, bool> done;
    int applied = 0, scanned = 0;
    const int pages = doc.PageCount();
    for (int p = 0; p < pages && done.size() < want.size(); ++p) {
        FormPage page(doc.Raw(), form, p);
        if (!page) continue;
        const int n = FPDFPage_GetAnnotCount(page.get());
        for (int i = 0; i < n; ++i) {
            if (++scanned > kMaxAnnotsScanned) {
                TraceF("[PDFTool.Forms] ApplyFormEdits: widget scan cap reached\n");
                return applied;
            }
            AnnotPtr a(FPDFPage_GetAnnot(page.get(), i));
            if (!a || !IsWidget(a.get())) continue;
            const std::wstring name = FieldName(form, a.get());
            auto w = want.find(name);
            if (w == want.end() || done.count(name)) continue;
            const FieldEdit& e = *w->second;
            if (FPDFAnnot_GetFormFieldFlags(form, a.get()) & FPDF_FORMFLAG_READONLY) {
                done[name] = true;  // never edit a read-only field
                continue;
            }
            bool ok = false;
            switch (TypeOf(FPDFAnnot_GetFormFieldType(form, a.get()))) {
                case FieldType::Text:
                    ok = ReplaceText(form, page.get(), a.get(), e.newValue);
                    FORM_ForceToKillFocus(form);
                    done[name] = true;
                    break;
                case FieldType::CheckBox:
                    // The click can miss (a widget covered by another, a zero
                    // rect): count the edit only if the state really changed.
                    if ((FPDFAnnot_IsChecked(form, a.get()) != 0) != e.newChecked)
                        ClickCenter(form, page.get(), a.get());
                    ok = (FPDFAnnot_IsChecked(form, a.get()) != 0) == e.newChecked;
                    FORM_ForceToKillFocus(form);
                    done[name] = true;
                    break;
                case FieldType::RadioButton:
                    // Only the widget with the wanted export value is clicked;
                    // the group's other widgets turn off by themselves.
                    if (ExportValue(form, a.get()) == e.newValue) {
                        if (!FPDFAnnot_IsChecked(form, a.get()))
                            ClickCenter(form, page.get(), a.get());
                        ok = FPDFAnnot_IsChecked(form, a.get()) != 0;
                        FORM_ForceToKillFocus(form);
                        done[name] = true;
                    }
                    break;
                case FieldType::ComboBox:
                case FieldType::ListBox: {
                    const int count = std::min(FPDFAnnot_GetOptionCount(form, a.get()), kMaxOptions);
                    int index = -1;
                    for (int k = 0; k < count && index < 0; ++k)
                        if (OptionLabel(form, a.get(), k) == e.newValue) index = k;
                    if (index >= 0 && FORM_SetFocusedAnnot(form, a.get())) {
                        // A multi-select list box keeps earlier selections:
                        // the dialog edits one value, so clear the others.
                        if (FPDFAnnot_GetFormFieldFlags(form, a.get()) & FPDF_FORMFLAG_CHOICE_MULTI_SELECT)
                            for (int k = 0; k < count; ++k)
                                if (k != index && FORM_IsIndexSelected(form, page.get(), k))
                                    FORM_SetIndexSelected(form, page.get(), k, false);
                        ok = FORM_SetIndexSelected(form, page.get(), index, true);
                        FORM_ForceToKillFocus(form);
                    } else if (index < 0 && (FPDFAnnot_GetFormFieldFlags(form, a.get()) &
                                             FPDF_FORMFLAG_CHOICE_EDIT)) {
                        ok = ReplaceText(form, page.get(), a.get(), e.newValue);  // editable combo
                        FORM_ForceToKillFocus(form);
                    }
                    done[name] = true;
                    break;
                }
                default:
                    done[name] = true;  // push buttons, signatures: nothing to set
                    break;
            }
            if (ok) ++applied;
        }
    }
    TraceF("[PDFTool.Forms] applied %d of %zu field edits\n", applied, edits.size());
    return applied;
}

}  // namespace pdf
