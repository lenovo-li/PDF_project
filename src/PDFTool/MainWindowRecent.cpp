// MainWindowRecent.cpp — recent files menu, settings dialog, file drop,
// command-line open.
//
// Config keys: "recent.0".."recent.9" (newest first; ConfigStore holds only
// scalars), "theme.mode" = "system" | "light" | "dark". Each change is saved
// at once.

#include "MainWindow.h"

#include <ConfigStore.h>
#include <FluentUI/Application.h>
#include <FluentUI/controls/CheckBox.h>
#include <FluentUI/controls/ComboBox.h>
#include <FluentUI/controls/ContentDialog.h>
#include <FluentUI/controls/TextBlock.h>
#include <PDFCore/RecentFiles.h>
#include <PDFCore/Trace.h>

#include <cwctype>
#include <string>
#include <utility>

using namespace fluent;

namespace {
std::string RecentKey(int i) { return "recent." + std::to_string(i); }

bool IsPdfPath(const std::wstring& p) {
    if (p.size() < 4) return false;
    std::wstring ext = p.substr(p.size() - 4);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext == L".pdf";
}
}  // namespace

void MainWindow::LoadRecent() {
    std::vector<std::wstring> list;
    if (cfg_) {
        for (int i = 0; i < pdf::kMaxRecentFiles; ++i)
            list.push_back(cfg_->GetString(RecentKey(i).c_str(), L""));
        // Older configs kept only the last file. Migrated once, then emptied
        // (ConfigStore cannot delete a key) so clearing the list sticks.
        const std::wstring last = cfg_->GetString("recent.last", L"");
        if (!last.empty()) {
            list.push_back(last);
            cfg_->SetString("recent.last", L"");
        }
    }
    recent_ = pdf::NormalizeRecent(std::move(list));
    RebuildRecentMenu();
}

void MainWindow::AddRecent(const std::wstring& path) {
    recent_ = pdf::PushRecent(std::move(recent_), path);
    RebuildRecentMenu();
    if (!cfg_) return;
    for (int i = 0; i < pdf::kMaxRecentFiles; ++i)
        cfg_->SetString(RecentKey(i).c_str(),
                        i < static_cast<int>(recent_.size()) ? recent_[static_cast<std::size_t>(i)] : L"");
    cfg_->Save();
}

void MainWindow::ClearRecent() {
    recent_.clear();
    AddRecent(L"");  // writes the empty list and rebuilds the menu
}

void MainWindow::RebuildRecentMenu() {
    if (!recentMenu_) return;
    std::vector<MenuItem> items;
    for (std::size_t i = 0; i < recent_.size(); ++i) {
        MenuItem m;
        m.text = pdf::RecentLabel(static_cast<int>(i), recent_[i]);
        const std::wstring path = recent_[i];
        m.AddInvokeHandler([this, path] { OpenFile(path); });
        items.push_back(std::move(m));
    }
    if (items.empty()) {
        MenuItem none;
        none.text = L"（没有最近打开的文件）";
        none.enabled = false;
        items.push_back(std::move(none));
    } else {
        items.push_back(MenuItem::Sep());
        MenuItem clear;
        clear.text = L"清除列表";
        clear.AddInvokeHandler([this] { ClearRecent(); });
        items.push_back(std::move(clear));
    }
    // A handler may run from inside the menu that is being replaced; SetItems
    // closes the menu first and MenuItem copies its handler list before calling.
    recentMenu_->SetItems(std::move(items));
}

void MainWindow::ApplyThemeSetting() {
    std::wstring mode = L"system";
    if (cfg_) {
        if (cfg_->Has("theme.mode"))
            mode = cfg_->GetString("theme.mode", L"system");
        else if (cfg_->Has("theme.dark"))  // older configs stored the last state
            mode = cfg_->GetBool("theme.dark", false) ? L"dark" : L"light";
    }
    if (mode == L"dark")
        SetDarkMode(true);
    else if (mode == L"light")
        SetDarkMode(false);
    else
        FollowSystemTheme();
    pdf::TraceF("[PDFTool.UI] theme mode=%ls dark=%d\n", mode.c_str(), IsDarkMode() ? 1 : 0);
}

void MainWindow::ShowSettings() {
    static const wchar_t* kModes[] = {L"system", L"light", L"dark"};
    std::wstring mode = cfg_ ? cfg_->GetString("theme.mode", L"system") : L"system";
    int current = 0;
    for (int i = 0; i < 3; ++i)
        if (mode == kModes[i]) current = i;

    ContentDialog dlg;
    dlg.SetTitle(L"设置");
    auto body = std::make_unique<StackPanel>();
    body->SetSpacing(8.0f);
    auto* themeLabel = body->Emplace<TextBlock>();
    themeLabel->SetText(L"主题");
    auto* theme = body->Emplace<ComboBox>();
    theme->SetWidth(240.0f);
    theme->SetItems({L"跟随系统（启动时读取）", L"浅色", L"深色"});
    theme->SetSelectedIndex(current);
    auto* clear = body->Emplace<CheckBox>();
    clear->SetText(L"清除最近打开的文件列表");
    auto* where = body->Emplace<TextBlock>();
    where->SetWrap(true);
    where->SetDimmed(true);
    where->SetMaxWidth(360.0f);
    if (cfg_)
        where->SetText(std::wstring(cfg_->Portable() ? L"便携模式，" : L"") + L"配置文件：" +
                       cfg_->Path());
    dlg.SetContent(std::move(body));
    dlg.AddButton(L"确定", DialogResult::Primary);
    dlg.AddButton(L"取消", DialogResult::Cancel);
    if (dlg.ShowDialog(*this) != DialogResult::Primary) return;

    const int sel = theme->SelectedIndex();
    if (sel >= 0 && sel < 3 && cfg_) {
        cfg_->SetString("theme.mode", kModes[sel]);
        cfg_->Save();
        ApplyThemeSetting();
    }
    if (clear->IsChecked()) ClearRecent();
}

void MainWindow::OnFilesDropped(std::vector<std::wstring> files) {
    std::wstring pdfFile;
    for (const std::wstring& f : files)
        if (IsPdfPath(f)) {
            pdfFile = f;
            break;
        }
    if (pdfFile.empty()) {
        ShowStatus(L"只能拖入 PDF 文件");
        return;
    }
    pdf::TraceF("[PDFTool.UI] file dropped (%zu files)\n", files.size());
    // Not from inside the OLE drop callback: OpenFile may show a modal dialog
    // (unsaved changes, password), and Explorer waits for the drop to return.
    std::weak_ptr<int> gen = generation_;
    Application* app = OwningApplication();
    if (!app) return;
    app->Post([this, gen, pdfFile] {
        if (gen.lock()) OpenFile(pdfFile);
    });
}

void MainWindow::OpenOnStart(const std::wstring& path) {
    pdf::TraceF("[PDFTool.UI] open from command line\n");
    OpenFile(path);
}
