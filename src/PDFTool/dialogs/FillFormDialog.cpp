#include "FillFormDialog.h"

#include <FluentUI/controls/CheckBox.h>
#include <FluentUI/controls/ComboBox.h>
#include <FluentUI/controls/TextArea.h>
#include <FluentUI/controls/TextBox.h>
#include <FluentUI/controls/TextBlock.h>
#include <FluentUI/layout/ScrollPanel.h>
#include <FluentUI/layout/StackPanel.h>

#include <memory>

using namespace fluent;

namespace {
int IndexOf(const std::vector<std::wstring>& options, const std::wstring& value) {
    for (std::size_t i = 0; i < options.size(); ++i)
        if (options[i] == value) return static_cast<int>(i);
    return -1;
}

// The edit controls store newlines as '\n'; PDF values often use "\r\n" or
// '\r'. Both sides are compared in this form, so an untouched multi-line
// field is not reported as changed.
std::wstring NormalizeNewlines(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\r') {
            out += L'\n';
            if (i + 1 < s.size() && s[i + 1] == L'\n') ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}
}  // namespace

FillFormDialog::FillFormDialog(const pdf::FormFieldList& fields) {
    dlg_.SetTitle(L"填写表单");
    auto scroll = std::make_unique<ScrollPanel>();
    scroll->SetMaxHeight(520.0f);
    auto* stack = scroll->Emplace<StackPanel>();
    stack->SetOrientation(StackPanel::Orientation::Vertical);
    stack->SetSpacing(12.0f);
    stack->SetMargin(Thickness(4.0f, 4.0f, 16.0f, 4.0f));  // room for the scroll bar

    auto note = [&](const wchar_t* text) {
        auto* t = stack->Emplace<TextBlock>();
        t->SetText(text);
        t->SetWrap(true);
        t->SetDimmed(true);
    };
    if (fields.hasXfa) note(L"此文档含 XFA 表单，只能填写其中的 AcroForm 字段。");

    for (const pdf::FormField& f : fields.fields) {
        if (f.type == pdf::FieldType::PushButton || f.type == pdf::FieldType::Signature ||
            f.type == pdf::FieldType::Unknown)
            continue;
        auto* row = stack->Emplace<StackPanel>();
        row->SetOrientation(StackPanel::Orientation::Vertical);
        row->SetSpacing(4.0f);
        auto* label = row->Emplace<TextBlock>();
        label->SetText(f.readOnly ? f.name + L"（只读）" : f.name);
        label->SetDimmed(f.readOnly);

        FieldControl fc;
        fc.field = f;
        switch (f.type) {
            case pdf::FieldType::Text: {
                TextEditBase* t = nullptr;
                if (f.multiline) {
                    auto* area = row->Emplace<TextArea>();
                    area->SetHeight(72.0f);
                    t = area;
                } else {
                    t = row->Emplace<TextBox>();
                }
                t->SetWidth(300.0f);  // fits the dialog's content width
                t->SetText(NormalizeNewlines(f.value));
                t->SetReadOnly(f.readOnly);
                fc.control = t;
                break;
            }
            case pdf::FieldType::CheckBox: {
                auto* c = row->Emplace<CheckBox>();
                c->SetText(L"选中");
                c->SetChecked(f.checked);
                c->SetEnabled(!f.readOnly);
                fc.control = c;
                break;
            }
            case pdf::FieldType::RadioButton:
            case pdf::FieldType::ComboBox:
            case pdf::FieldType::ListBox: {
                auto* c = row->Emplace<ComboBox>();
                c->SetWidth(300.0f);
                c->SetItems(f.options);
                // A radio group's value is its checked button's export value,
                // or "Off" when none is checked: select nothing then.
                c->SetSelectedIndex(IndexOf(f.options, f.value));
                c->SetEnabled(!f.readOnly);
                fc.control = c;
                break;
            }
            default:
                continue;
        }
        controls_.push_back(std::move(fc));
    }
    if (fields.truncated) note(L"表单字段或内容太多，只显示了一部分。");
    if (controls_.empty()) note(L"没有可以填写的字段。");

    dlg_.SetContent(std::move(scroll));
    dlg_.AddButton(L"应用", DialogResult::Primary);
    dlg_.AddButton(L"取消", DialogResult::Cancel);
}

std::vector<pdf::FieldEdit> FillFormDialog::Run(NativeWindowHost& owner) {
    std::vector<pdf::FieldEdit> edits;
    if (dlg_.ShowDialog(owner) != DialogResult::Primary) return edits;
    // The dialog's element tree is still alive here (owned by dlg_).
    for (const FieldControl& fc : controls_) {
        const pdf::FormField& f = fc.field;
        if (f.readOnly) continue;
        pdf::FieldEdit e;
        e.name = f.name;
        bool changed = false;
        switch (f.type) {
            case pdf::FieldType::Text:
                e.newValue = static_cast<TextEditBase*>(fc.control)->Text();
                changed = e.newValue != NormalizeNewlines(f.value);
                break;
            case pdf::FieldType::CheckBox:
                e.newChecked = static_cast<CheckBox*>(fc.control)->IsChecked();
                changed = e.newChecked != f.checked;
                break;
            case pdf::FieldType::RadioButton:
            case pdf::FieldType::ComboBox:
            case pdf::FieldType::ListBox: {
                const int i = static_cast<ComboBox*>(fc.control)->SelectedIndex();
                if (i < 0 || i >= static_cast<int>(f.options.size())) break;
                e.newValue = f.options[static_cast<std::size_t>(i)];
                changed = e.newValue != f.value;
                break;
            }
            default:
                break;
        }
        if (changed) edits.push_back(std::move(e));
    }
    return edits;
}
