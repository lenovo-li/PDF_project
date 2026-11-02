#include "MainWindow.h"

#include "controls/PdfPageView.h"
#include "controls/ThumbnailStrip.h"

#include <ConfigStore.h>
#include <FluentUI/dialogs/FileDialog.h>
#include <FluentUI/input/DragDrop.h>
#include <FluentUI/controls/StatusBar.h>
#include <FluentUI/layout/DockPanel.h>
#include <PDFCore/Document.h>
#include <PDFCore/Engine.h>
#include <PDFCore/FileIO.h>
#include <PDFCore/Trace.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace fluent;

MainWindow::MainWindow(ConfigStore* cfg, pdf::Engine* engine) : cfg_(cfg), engine_(engine) {
    SetTitle(L"PDFTool");
    SetClientSize(960.0f, 640.0f);
}

MainWindow::~MainWindow() { ++*generation_; }

void MainWindow::OnInitialize() {
    auto root = std::make_unique<DockPanel>();

    // The toolbar IS the title bar: the window draws its caption buttons
    // (min/max/close) in a band TitleBarHeight() tall at the top right, and
    // the toolbar shares that band. Matching its height and centring the
    // buttons lines them up with the caption buttons; the right margin keeps
    // the search box clear of them. Empty space in the band still drags the
    // window. Status text lives in the StatusBar at the bottom.
    constexpr float kCaptionButtonsWidthDip = 3 * 46.0f;  // FluentUI caption buttons
    auto* bar = root->Emplace<StackPanel>();
    DockPanel::SetDock(bar, Dock::Top);
    bar->SetOrientation(StackPanel::Orientation::Horizontal);
    bar->SetHeight(TitleBarHeight());
    bar->SetSpacing(8.0f);
    bar->SetMargin(Thickness(12.0f, 0.0f, kCaptionButtonsWidthDip, 0.0f));

    auto addButton = [bar](const wchar_t* text) {
        auto* b = bar->Emplace<Button>();
        b->SetText(text);
        b->SetVAlign(VAlign::Center);
        return b;
    };
    openSub_ = addButton(L"打开 PDF…")->Click().Subscribe<&MainWindow::OnOpenClicked>(this);
    recentMenu_ = std::make_unique<MenuFlyout>();
    addButton(L"最近 ▾")->SetFlyout(recentMenu_.get());
    LoadRecent();  // fills recentMenu_
    BuildMenus(bar);
    zoomOutSub_ = addButton(L"缩小")->Click().Subscribe<&MainWindow::OnZoomOutClicked>(this);
    zoomInSub_ = addButton(L"放大")->Click().Subscribe<&MainWindow::OnZoomInClicked>(this);
    fitSub_ = addButton(L"适合宽度")->Click().Subscribe<&MainWindow::OnFitWidthClicked>(this);

    // Page jump: "第 [ 12 ] / 340 页". Digits only; Enter commits. KeyDown
    // subscribers run before the TextBox's own key handling, and the TextBox
    // ignores Enter, so subscribing here is the commit hook.
    auto* pageLabel = bar->Emplace<TextBlock>();
    pageLabel->SetText(L"第");
    pageLabel->SetVAlign(VAlign::Center);
    pageLabel->SetHitTestVisible(false);
    pageBox_ = bar->Emplace<TextBox>();
    pageBox_->SetWidth(64.0f);
    pageBox_->SetVAlign(VAlign::Center);
    pageBox_->SetMaxLength(6);  // kMaxPages is 100000
    pageBox_->SetInputFilter([](wchar_t c) { return c >= L'0' && c <= L'9'; });
    pageKeySub_ = pageBox_->KeyDown().Subscribe<&MainWindow::OnPageBoxKeyDown>(this);
    pageTextSub_ = pageBox_->TextChanged().Subscribe<&MainWindow::OnPageBoxTextChanged>(this);
    pageTotal_ = bar->Emplace<TextBlock>();
    pageTotal_->SetText(L"/ 0 页");
    pageTotal_->SetVAlign(VAlign::Center);
    pageTotal_->SetHitTestVisible(false);

    // Search: box (Enter = next, Shift+Enter = previous), prev/next, count.
    searchBox_ = bar->Emplace<TextBox>();
    searchBox_->SetWidth(180.0f);
    searchBox_->SetVAlign(VAlign::Center);
    searchBox_->SetPlaceholder(L"搜索（Ctrl+F）");
    searchBox_->SetMaxLength(pdf::kMaxQueryChars);
    searchKeySub_ = searchBox_->KeyDown().Subscribe<&MainWindow::OnSearchBoxKeyDown>(this);
    searchPrevSub_ = addButton(L"↑")->Click().Subscribe<&MainWindow::OnSearchPrevClicked>(this);
    searchNextSub_ = addButton(L"↓")->Click().Subscribe<&MainWindow::OnSearchNextClicked>(this);
    searchCount_ = bar->Emplace<TextBlock>();
    searchCount_->SetVAlign(VAlign::Center);
    searchCount_->SetHitTestVisible(false);

    // Status bar along the bottom: messages (progress, results, errors) on
    // the left, the open document and zoom on the right. Docked before the
    // thumbnails so it spans the full width under them.
    status_ = root->Emplace<StatusBar>();
    DockPanel::SetDock(status_, Dock::Bottom);
    status_->SetText(L"未打开文档");
    {
        auto info = std::make_unique<TextBlock>();
        info->SetTypographyRole(TypographyRole::Caption);
        info->SetDimmed(true);
        info->SetHitTestVisible(false);
        docInfo_ = info.get();
        status_->SetRightContent(std::move(info));
    }

    thumbs_ = root->Emplace<ThumbnailStrip>(engine_, OwningApplication());
    DockPanel::SetDock(thumbs_, Dock::Left);
    thumbSub_ = thumbs_->PageClicked().Subscribe<&MainWindow::OnThumbnailClicked>(this);

    view_ = root->Emplace<PdfPageView>(engine_, OwningApplication());  // fills the rest
    viewSub_ = view_->ViewChanged().Subscribe<&MainWindow::OnViewChanged>(this);
    annotGestureSub_ = view_->AnnotationGesture().Subscribe<&MainWindow::OnAnnotationGesture>(this);
    if (cfg_) {
        const double z = cfg_->GetDouble("view.zoom", 1.0);
        view_->SetZoom(std::isfinite(z) ? z : 1.0);
    }

    search_ = std::make_unique<SearchRunner>(engine_, OwningApplication());
    convert_ = std::make_unique<ConvertRunner>(engine_, OwningApplication());
    // Files dropped anywhere on the window (no child accepts drops).
    root->SetDropTarget(MakeFileDropTarget(
        [this](std::vector<std::wstring> files) { OnFilesDropped(std::move(files)); }));
    rootKeySub_ = root->PreviewKeyDown().Subscribe<&MainWindow::OnRootPreviewKeyDown>(this);

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
    // Zoom changes continuously (Ctrl+wheel); saving once here is the debounce.
    if (view_) cfg_->SetDouble("view.zoom", view_->Zoom());
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

void MainWindow::OnZoomInClicked(Button&, RoutedEventArgs&) {
    if (view_) view_->ZoomBy(1.25);
}
void MainWindow::OnZoomOutClicked(Button&, RoutedEventArgs&) {
    if (view_) view_->ZoomBy(0.8);
}
void MainWindow::OnFitWidthClicked(Button&, RoutedEventArgs&) {
    if (view_) view_->FitWidth();
}

void MainWindow::OnViewChanged(PdfPageView&, PageViewChangedArgs& e) {
    ShowPageNumber(e.currentPage, e.pageCount);
    if (e.pageCount <= 0) return;
    if (thumbs_) thumbs_->SetCurrentPage(e.currentPage);
    // Right side only: scrolling and zooming must not wipe a message on the left.
    if (docInfo_)
        docInfo_->SetText(docName_ + L"  ·  " + std::to_wstring(e.pageCount) + L" 页  ·  " +
                          std::to_wstring(static_cast<int>(std::lround(e.zoom * 100))) + L"%");
}

void MainWindow::OnThumbnailClicked(ThumbnailStrip&, PageClickedArgs& e) {
    if (view_) view_->ScrollToPage(e.page);
}

void MainWindow::OnPageBoxKeyDown(UIElement&, KeyEventArgs& e) {
    if (e.vk != VK_RETURN || !view_ || !pageBox_) return;
    e.handled = true;
    const int count = view_->PageCount();
    const std::wstring& text = pageBox_->Text();
    // Digits only (input filter) and at most 6 of them, so stoi cannot throw
    // out_of_range; empty text simply restores the current number.
    const int wanted = text.empty() ? 0 : std::stoi(text);
    if (count > 0 && wanted >= 1) view_->ScrollToPage(std::min(wanted, count) - 1);
    // Show what the view actually landed on (clamped, or unchanged if empty).
    // Enter ends the edit, so overwrite the box even though it still has focus.
    pageBoxEditing_ = false;
    ShowPageNumber(view_->CurrentPage(), count);
}

void MainWindow::OnPageBoxTextChanged(TextEditBase&, std::wstring&) {
    // TextChanged fires for user edits only (not SetText), so this marks the
    // start of typing; scrolling must not clobber a half-typed number.
    pageBoxEditing_ = true;
}

void MainWindow::ShowPageNumber(int currentPage, int pageCount) {
    if (pageTotal_) pageTotal_->SetText(L"/ " + std::to_wstring(pageCount) + L" 页");
    if (pageBox_ && !pageBoxEditing_)
        pageBox_->SetText(currentPage >= 0 ? std::to_wstring(currentPage + 1) : L"");
}

void MainWindow::CloseCurrentDocument() {
    if (!view_ || view_->DocumentId() == pdf::kNoDocument) return;
    const pdf::DocumentId old = view_->DocumentId();
    if (search_) search_->Cancel();
    if (convert_) convert_->Cancel();  // its pages belong to the old document
    searchQuery_.clear();
    matches_.clear();
    perPage_.clear();
    currentMatch_ = -1;
    ShowSearchCount();
    if (thumbs_) thumbs_->ClearDocument();
    if (docInfo_) docInfo_->SetText(L"");
    view_->ClearDocument();  // views first: they drop in-flight results for `old`
    pdf::Engine* engine = engine_;
    engine_->Post([engine, old] { engine->Documents().Close(old); });
    pdf::TraceF("[PDFTool.UI] closed document id=%llu\n", static_cast<unsigned long long>(old));
}

void MainWindow::OpenFile(const std::wstring& path, const std::string& password) {
    Application* app = OwningApplication();
    if (!app || !engine_) return;
    // Only on the first attempt: a password retry re-enters here after the
    // user already confirmed.
    if (password.empty() && !ConfirmDiscard(L"打开其他文件")) return;
    ShowStatus(L"正在打开…");
    const int ticket = ++*generation_;
    std::weak_ptr<int> gen = generation_;
    pdf::Engine* engine = engine_;
    engine_->Post([this, app, engine, path, password, ticket, gen] {
        // Engine thread: open, read page sizes, keep the document in the store.
        pdf::DocumentId id = pdf::kNoDocument;
        std::vector<std::pair<double, double>> sizes;
        std::wstring error;
        bool needPassword = false;
        auto doc = pdf::Document::Open(path, password);
        if (!doc) {
            error = doc.error().message;
            needPassword = pdf::IsPasswordError(doc.error());
        } else if (auto s = doc.value().DisplayedPageSizesPt(); !s) {
            error = s.error().message;
        } else {
            sizes = std::move(s.value());
            id = engine->Documents().Add(std::move(doc.value()));
        }
        app->Post([this, engine, ticket, gen, id, path, password, needPassword,
                   sizes = std::move(sizes), error = std::move(error)] {
            // UI thread. The window is alive iff the generation token is.
            auto alive = gen.lock();
            if (!alive) return;  // window gone; the Engine closes the document on exit
            if (*alive != ticket) {  // a newer open won: close this one unseen
                if (id != pdf::kNoDocument)
                    engine->Post([engine, id] { engine->Documents().Close(id); });
                return;
            }
            if (id == pdf::kNoDocument) {
                if (needPassword) {
                    // Encrypted: ask, then retry. A wrong password lands here
                    // again with the prompt repeated; Cancel stops.
                    std::wstring pw;
                    const std::wstring label = password.empty() ? L"此文档受密码保护，请输入密码："
                                                                : L"密码错误，请重新输入：";
                    if (PromptText(L"打开受保护的 PDF", label, &pw, /*password*/ true)) {
                        OpenFile(path, pdf::WideToUtf8(pw));
                        return;
                    }
                    ShowStatus(L"已取消打开");
                    return;
                }
                ShowStatus(L"打开失败：" + error);
                return;
            }
            CloseCurrentDocument();
            docPath_ = path;
            docPassword_ = password;
            const size_t slash = path.find_last_of(L"\\/");
            docName_ = slash == std::wstring::npos ? path : path.substr(slash + 1);
            pendingEdits_ = 0;
            SetDirty(false);  // after docName_: it sets the title
            AddRecent(path);
            thumbs_->SetDocument(id, sizes);
            view_->SetDocument(id, sizes);
            // Keyboard shortcuts (Ctrl+F, F3, PgDn) tunnel from the focused
            // element; with nothing focused they would do nothing.
            SetFocusElement(view_);
            // A 0-page file is valid PDF; ViewChanged has nothing to report for
            // it, so say so here instead of leaving "正在打开…" up.
            ShowStatus(sizes.empty() ? L"文档没有页面" : L"就绪");
        });
    });
}

void MainWindow::ShowStatus(const std::wstring& text) {
    if (status_) status_->SetText(text);
}
