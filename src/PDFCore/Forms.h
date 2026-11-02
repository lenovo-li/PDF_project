// Forms.h — AcroForm field inspection and editing.
//
// THREADING: all functions must be called on the engine thread (like Document).
//
// LIMITATIONS:
// - XFA forms are not supported (pdfium build without XFA).
// - JavaScript is disabled (FPDF_FORMFILLINFO callbacks minimally stubbed).
// - Signature fields are listed but cannot be edited.
// - Radio button groups: each button is a separate widget; setting the field
//   value clicks the widget whose export value matches.
// - Field names are qualified (Parent.Child); widgets with the same name share
//   a value, so editing one updates all.
#pragma once

#include "Result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pdf {

class Document;

enum class FieldType {
    Unknown,
    PushButton,  // button with no persistent state (just an action)
    CheckBox,
    RadioButton,
    Text,
    ComboBox,   // drop-down, may be editable
    ListBox,    // multi-select allowed
    Signature
};

struct FormField {
    std::wstring name;     // qualified name (T key, with parent chain)
    FieldType type = FieldType::Unknown;
    std::wstring value;    // current value (V key); for check/radio: on-state or "Off"
    bool checked = false;  // for check/radio: whether value != "Off"
    bool readOnly = false; // Ff bit 1, or a value over 64 Ki characters (not read)
    bool multiline = false;  // text field with Ff bit 13 (otherwise single-line)
    std::vector<std::wstring> options;  // for combo/list: Opt array entries
    int page = -1;         // first widget's page (widgets may span multiple pages)
    int annotIndex = -1;   // FPDFPage_GetAnnot index of the first widget
};

struct FormFieldList {
    std::vector<FormField> fields;
    // Hit a cap: field count, annotations scanned, or the total text budget.
    // The list is still usable; the fields after the cap are left out.
    bool truncated = false;
    bool hasXfa = false;     // document uses XFA (unsupported; fields may be incomplete)
};

// Field edit: apply to the field (by name), which updates all its widgets.
struct FieldEdit {
    std::wstring name;
    std::wstring newValue;  // for text/combo/list/radio: the value to set
    bool newChecked = false;  // for checkbox: the state to set
};

// Check if the document has an AcroForm. Cheap (no page scan).
bool HasForm(const Document& doc);

// Scan pages [0, pageLimit) for form fields, grouped by qualified name. At
// most `fieldCap` fields are returned; widgets beyond that are ignored. Also
// capped: 20000 annotations scanned, 1000 options per field, 1 MiB of text
// (names, values, options) in total. Any cap sets `truncated`.
Result<FormFieldList> ListFormFields(const Document& doc, int pageLimit = 100000,
                                     int fieldCap = 500);

// Apply edits to the document's form. Each edit names a field; all widgets of
// that field are updated. Text fields: focus, select all, replace. Check boxes:
// click if state differs. Combo/list: set selected index. Read-only fields are
// skipped. Returns the count of fields successfully edited; fewer than
// edits.size() when some were read-only, missing, or beyond the 20000
// annotation scan cap.
//
// The document must be saved afterward to persist changes (uncommitted edits
// are visible in rendering but not written until SaveAs/SaveToMemory).
Result<int> ApplyFormEdits(Document& doc, const std::vector<FieldEdit>& edits);

}  // namespace pdf
