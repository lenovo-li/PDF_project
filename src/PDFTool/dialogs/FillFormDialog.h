// FillFormDialog.h — modal dialog for filling AcroForm fields.
//
// Lists the fields from pdf::ListFormFields with one control each: a text area
// (or text box, for single-line fields) for text fields, a check box, a combo box for choices and radio groups.
// Read-only fields are shown disabled; push buttons and signatures are left
// out. The dialog never touches the document: Run returns the edits, and the
// caller applies them on the engine thread.
#pragma once

#include <PDFCore/Forms.h>

#include <FluentUI/controls/ContentDialog.h>

#include <string>
#include <vector>

namespace fluent {
class NativeWindowHost;
class UIElement;
}  // namespace fluent

class FillFormDialog {
public:
    explicit FillFormDialog(const pdf::FormFieldList& fields);

    // Shows the dialog modally. Returns the fields the user changed (empty on
    // cancel or when nothing changed). Unchanged fields are left out so their
    // appearance streams are not regenerated for nothing.
    std::vector<pdf::FieldEdit> Run(fluent::NativeWindowHost& owner);

    // Fields the dialog shows (editable kinds only), for the caller's message.
    int ShownCount() const { return static_cast<int>(controls_.size()); }

private:
    struct FieldControl {
        pdf::FormField field;               // as listed, to tell what changed
        fluent::UIElement* control = nullptr;  // owned by the dialog's tree
    };

    fluent::ContentDialog dlg_;
    std::vector<FieldControl> controls_;
};
