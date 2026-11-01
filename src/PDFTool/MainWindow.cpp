#include "MainWindow.h"

#include <ConfigStore.h>
#include <FluentUI/dialogs/FileDialog.h>
#include <PDFCore/Document.h>
#include <PDFCore/Engine.h>

#include <memory>

using namespace fluent;

MainWindow::MainWindow(ConfigStore* cfg, pdf::Engine* engine) : cfg_(cfg), engine_(engine) {
    SetTitle(L"PDFTool");
    SetClientSize(960.0f, 640.0f);
}

MainWindow::~MainWindow() { ++*generation_; }

void MainWindow::OnInitialize() {
    auto root = std::make_unique<StackPanel>();
    root->SetSpacing(12.0f);
    root->SetMargin(Thickness(24.0f));

    auto* heading = root->Emplace<TextBlock>();
    heading->SetText(L"PDFTool");
    heading->SetTypographyRole(TypographyRole::Title);
    heading->SetHitTestVisible(false);

    auto* open = root->Emplace<Button>();
    open->SetText(L"打开 PDF…");
    open->SetWidth(140.0f);
    open->SetHAlign(HAlign::Left);
    openSub_ = open->Click().Subscribe<&MainWindow::OnOpenClicked>(this);

    status_ = root->Emplace<TextBlock>();
    status_->SetText(L"未打开文档");

    SetRoot(std::move(root));
}

void MainWindow::OnUnloaded(WindowEventArgs&) {
    if (!cfg_) return;
    // Fires from inside OnDestroying while the HWND is still valid.
    WindowState s = CaptureWindowState();
    if (s.valid) {
        cfg_->SetInt("window.x", s.x);
        cfg_->SetInt("window.y", s.y);
        cfg_->SetInt("window.width", s.width);
        cfg_->SetInt("window.height", s.height);
        cfg_->SetBool("window.maximized", s.maximized);
        cfg_->SetInt("window.dpi", s.dpi);
    }
    cfg_->SetBool("theme.dark", IsDarkMode());
    cfg_->Save();
}

void MainWindow::OnOpenClicked(Button&, RoutedEventArgs&) {
    OpenFileDialog dlg;
    dlg.SetTitle(L"打开 PDF");
    dlg.AddFilter(L"PDF 文件", L"*.pdf");
    dlg.AddFilter(L"所有文件", L"*.*");
    const std::wstring path = dlg.ShowDialog(Hwnd());
    if (!path.empty()) OpenFile(path);
}

void MainWindow::OpenFile(const std::wstring& path) {
    Application* app = OwningApplication();
    if (!app || !engine_) return;
    ShowStatus(L"正在打开…");
    const int ticket = ++*generation_;
    std::weak_ptr<int> gen = generation_;
    engine_->Post([this, app, path, ticket, gen] {
        // Engine thread: all PDFium work happens here.
        auto doc = pdf::Document::Open(path);
        std::wstring text = doc ? L"已打开：" + path + L"（" +
                                      std::to_wstring(doc.value().PageCount()) + L" 页）"
                                : L"打开失败：" + doc.error().message;
        app->Post([this, ticket, gen, text = std::move(text)] {
            // UI thread. Drop the result if the window is gone or a newer open won.
            auto alive = gen.lock();
            if (!alive || *alive != ticket) return;
            ShowStatus(text);
        });
    });
    if (cfg_) {
        cfg_->SetString("recent.last", path);
        cfg_->Save();
    }
}

void MainWindow::ShowStatus(const std::wstring& text) {
    if (status_) status_->SetText(text);
    OutputDebugStringA("[PDFTool.UI] status updated\n");
}
