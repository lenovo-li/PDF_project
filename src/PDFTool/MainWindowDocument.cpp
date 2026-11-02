// MainWindowDocument.cpp — page menu, document edits, save.
//
// Every edit runs on the engine thread against the Document in the store,
// then sends back the new page sizes; the UI resets both views to them and
// marks the document dirty. Nothing is written to disk until Save.

#include "MainWindow.h"

#include "controls/PdfPageView.h"
#include "controls/ThumbnailStrip.h"

#include <FluentUI/controls/ContentDialog.h>
#include <FluentUI/dialogs/FileDialog.h>
#include <PDFCore/Document.h>
#include <PDFCore/Engine.h>
#include <PDFCore/FileIO.h>
#include <PDFCore/PageOps.h>
#include <PDFCore/Trace.h>

#include <algorithm>
#include <utility>
#include <vector>

using namespace fluent;

namespace {
// Builds a leaf menu item that calls `fn` on the window.
MenuItem Item(const wchar_t* text, std::function<void()> fn, const wchar_t* accel = L"") {
    MenuItem m;
    m.text = text;
    m.accelerator = accel;
    m.AddInvokeHandler(std::move(fn));
    return m;
}
}  // namespace

void MainWindow::BuildMenus(StackPanel* bar) {
    pageMenu_ = std::make_unique<MenuFlyout>();
    {
        std::vector<MenuItem> items;
        items.push_back(Item(L"向左旋转", [this] { RotateCurrent(-1); }));
        items.push_back(Item(L"向右旋转", [this] { RotateCurrent(+1); }));
        items.push_back(MenuItem::Sep());
        items.push_back(Item(L"上移一页", [this] { MoveCurrent(-1); }));
        items.push_back(Item(L"下移一页", [this] { MoveCurrent(+1); }));
        items.push_back(Item(L"在后面插入空白页", [this] { InsertBlankAfterCurrent(); }));
        items.push_back(Item(L"从文件插入页面…", [this] { InsertFromFile(); }));
        items.push_back(Item(L"删除当前页", [this] { DeleteCurrent(); }));
        items.push_back(MenuItem::Sep());
        items.push_back(Item(L"保存", [this] { Save(false); }, L"Ctrl+S"));
        items.push_back(Item(L"另存为…", [this] { Save(true); }, L"Ctrl+Shift+S"));
        pageMenu_->SetItems(std::move(items));
    }
    BuildAnnotMenu();
    toolMenu_ = std::make_unique<MenuFlyout>();
    {
        std::vector<MenuItem> items;
        items.push_back(Item(L"合并其他 PDF…", [this] { MergeWithOthers(); }));
        items.push_back(Item(L"提取页面…", [this] { ExtractCurrent(); }));
        items.push_back(Item(L"拆分…", [this] { SplitCurrent(); }));
        items.push_back(MenuItem::Sep());
        items.push_back(Item(L"加密…", [this] { EncryptCurrent(); }));
        items.push_back(Item(L"移除密码…", [this] { DecryptCurrent(); }));
        items.push_back(MenuItem::Sep());
        items.push_back(Item(L"填写表单…", [this] { FillForm(); }));
        items.push_back(MenuItem::Sep());
        items.push_back(Item(L"转为 Word…", [this] { ConvertToWord(); }));
        items.push_back(Item(L"取消转换", [this] { CancelConvert(); }));
        items.push_back(MenuItem::Sep());
        items.push_back(Item(L"压缩 / 优化…", [this] { CompressCurrent(); }));
        items.push_back(Item(L"当前页导出为 PNG…", [this] { ExportPagePng(); }));
        items.push_back(MenuItem::Sep());
        items.push_back(Item(L"设置…", [this] { ShowSettings(); }));
        toolMenu_->SetItems(std::move(items));
    }
    auto* pages = bar->Emplace<Button>();
    pages->SetText(L"页面 ▾");
    pages->SetVAlign(VAlign::Center);
    pages->SetFlyout(pageMenu_.get());
    auto* annots = bar->Emplace<Button>();
    annots->SetText(L"注释 ▾");
    annots->SetVAlign(VAlign::Center);
    annots->SetFlyout(annotMenu_.get());
    auto* tools = bar->Emplace<Button>();
    tools->SetText(L"工具 ▾");
    tools->SetVAlign(VAlign::Center);
    tools->SetFlyout(toolMenu_.get());
}

void MainWindow::SetDirty(bool dirty) {
    dirty_ = dirty;
    SetTitle((dirty ? L"* " : L"") + (docName_.empty() ? std::wstring(L"PDFTool")
                                                        : docName_ + L" - PDFTool"));
}

void MainWindow::EditDocument(const wchar_t* what,
                              std::function<pdf::Status(pdf::Document&, int)> edit,
                              int pageAfter) {
    if (!view_ || view_->DocumentId() == pdf::kNoDocument) return;
    Application* app = OwningApplication();
    if (!app) return;
    const pdf::DocumentId id = view_->DocumentId();
    const int current = std::max(0, view_->CurrentPage());
    std::weak_ptr<int> gen = generation_;
    const int ticket = *generation_;
    pdf::Engine* engine = engine_;
    const std::wstring label = what;
    ShowStatus(label + L"…");
    ++pendingEdits_;
    MainWindow* self = this;  // used only on the UI thread, after the token check
    engine_->Post([engine, app, id, current, edit = std::move(edit), gen, ticket, label,
                   pageAfter, self] {
        // Engine thread.
        std::wstring error;
        std::vector<std::pair<double, double>> sizes;
        if (pdf::Document* doc = engine->Documents().Get(id)) {
            if (auto s = edit(*doc, current); !s) {
                error = s.error().message;
            } else if (auto z = doc->DisplayedPageSizesPt(); !z) {
                error = z.error().message;
            } else {
                sizes = std::move(z.value());
            }
        } else {
            error = L"文档已关闭";
        }
        app->Post([self, gen, ticket, id, label, error = std::move(error),
                   sizes = std::move(sizes), current, pageAfter]() mutable {
            auto alive = gen.lock();
            if (!alive) return;  // window gone
            if (self->pendingEdits_ > 0) --self->pendingEdits_;
            if (*alive != ticket) return;  // another file opened meanwhile
            self->OnEditDone(id, label, error, sizes, current, pageAfter);
        });
    });
}

void MainWindow::OnEditDone(pdf::DocumentId id, const std::wstring& label,
                            const std::wstring& error,
                            const std::vector<std::pair<double, double>>& sizes, int current,
                            int pageAfter) {
    if (view_->DocumentId() != id) return;
    if (!error.empty()) {
        ShowStatus(label + L"失败：" + error);
        return;
    }
    // The page list changed: results and caches for the old one are
    // wrong. Same document id, so the engine-side document stays.
    if (search_) search_->Cancel();
    // Pages extracted so far may no longer exist. Cancel is silent, so say it.
    const bool convertStopped = convert_ && convert_->Running();
    if (convert_) convert_->Cancel();
    searchQuery_.clear();
    matches_.clear();
    currentMatch_ = -1;
    ShowSearchCount();
    const double zoom = view_->Zoom();
    view_->SetDocument(id, sizes);
    view_->SetZoom(zoom);
    thumbs_->SetDocument(id, sizes);
    const int target = std::clamp(pageAfter < 0 ? current : pageAfter, 0,
                                  std::max(0, static_cast<int>(sizes.size()) - 1));
    view_->ScrollToPage(target);
    SetDirty(true);
    ShowStatus(label + L"完成" + (convertStopped ? L"；页面已变化，转为 Word 已取消" : L""));
}

void MainWindow::RotateCurrent(int quarterTurns) {
    EditDocument(L"旋转", [quarterTurns](pdf::Document& d, int cur) {
        return pdf::RotatePages(d, {cur}, quarterTurns);
    }, -1);
}

void MainWindow::DeleteCurrent() {
    if (!view_ || view_->PageCount() <= 1) {
        ShowStatus(L"不能删除唯一的页面");
        return;
    }
    const int cur = std::max(0, view_->CurrentPage());
    EditDocument(L"删除页面", [](pdf::Document& d, int c) { return pdf::DeletePages(d, {c}); },
                 std::max(0, cur - (cur >= view_->PageCount() - 1 ? 1 : 0)));
}

void MainWindow::InsertBlankAfterCurrent() {
    const int cur = view_ ? std::max(0, view_->CurrentPage()) : 0;
    EditDocument(L"插入空白页", [](pdf::Document& d, int c) {
        // Same size as the current page, so a blank page fits the document.
        auto g = d.PageGeometryAt(c);
        const double w = g ? g.value().box.Width() : 595, h = g ? g.value().box.Height() : 842;
        return pdf::InsertBlankPage(d, c + 1, w, h);
    }, cur + 1);
}

void MainWindow::MoveCurrent(int delta) {
    if (!view_) return;
    const int cur = std::max(0, view_->CurrentPage());
    const int dest = cur + delta;
    if (dest < 0 || dest >= view_->PageCount()) return;
    EditDocument(L"移动页面", [dest](pdf::Document& d, int c) {
        return pdf::MovePages(d, {c}, dest);
    }, dest);
}

void MainWindow::InsertFromFile() {
    if (!view_ || view_->DocumentId() == pdf::kNoDocument) return;
    OpenFileDialog dlg;
    dlg.SetTitle(L"选择要插入的 PDF");
    dlg.AddFilter(L"PDF 文件", L"*.pdf");
    const std::wstring path = dlg.ShowDialog(Hwnd());
    if (path.empty()) return;
    const int cur = std::max(0, view_->CurrentPage());
    // File I/O and parsing happen inside the edit, on the engine thread. The
    // source is closed when the lambda returns; imported pages are deep copies.
    EditDocument(L"插入页面", [path](pdf::Document& d, int c) -> pdf::Status {
        auto src = pdf::Document::Open(path);
        if (!src) return src.error();
        return pdf::ImportPages(d, src.value(), {}, c + 1);
    }, cur + 1);
}

void MainWindow::Save(bool saveAs) {
    if (!view_ || view_->DocumentId() == pdf::kNoDocument) return;
    std::wstring path = docPath_;
    if (saveAs || path.empty()) {
        SaveFileDialog dlg;
        dlg.SetTitle(L"另存为");
        dlg.AddFilter(L"PDF 文件", L"*.pdf");
        dlg.SetDefaultExtension(L"pdf");
        dlg.SetFileName(docName_);
        path = dlg.ShowDialog(Hwnd());
        if (path.empty()) return;
    }
    Application* app = OwningApplication();
    if (!app) return;
    const pdf::DocumentId id = view_->DocumentId();
    std::weak_ptr<int> gen = generation_;
    const int ticket = *generation_;
    pdf::Engine* engine = engine_;
    MainWindow* self = this;
    ShowStatus(L"正在保存…");
    engine_->Post([engine, app, id, path, gen, ticket, self] {
        // Engine thread. SaveAs writes a temp file then renames it over the
        // target, so overwriting the open file is safe (the document is in
        // memory) and a failure leaves the old file intact.
        std::wstring error;
        if (pdf::Document* doc = engine->Documents().Get(id)) {
            if (auto s = doc->SaveAs(path); !s) error = s.error().message;
        } else {
            error = L"文档已关闭";
        }
        app->Post([self, gen, ticket, id, path, error = std::move(error)] {
            auto alive = gen.lock();
            if (!alive || *alive != ticket || self->view_->DocumentId() != id) return;
            if (!error.empty()) {
                self->ShowStatus(L"保存失败：" + error);
                return;
            }
            self->docPath_ = path;
            const size_t slash = path.find_last_of(L"\\/");
            self->docName_ = slash == std::wstring::npos ? path : path.substr(slash + 1);
            self->SetDirty(false);
            self->AddRecent(path);
            self->ShowStatus(L"已保存");
            pdf::TraceF("[PDFTool.UI] saved document id=%llu\n",
                        static_cast<unsigned long long>(id));
        });
    });
}

bool MainWindow::ConfirmDiscard(const std::wstring& action) {
    if (!dirty_ && pendingEdits_ == 0) return true;
    ContentDialog dlg;
    dlg.SetTitle(L"有未保存的修改");
    auto body = std::make_unique<TextBlock>();
    body->SetText(L"“" + docName_ + L"”有未保存的修改。" + action + L"之前要保存吗？");
    body->SetWrap(true);
    dlg.SetContent(std::move(body));
    dlg.AddButton(L"保存", DialogResult::Primary);
    dlg.AddButton(L"不保存", DialogResult::Secondary);
    dlg.AddButton(L"取消", DialogResult::Cancel);
    const DialogResult r = dlg.ShowDialog(*this);
    if (r == DialogResult::Secondary) return true;
    if (r != DialogResult::Primary) return false;
    std::wstring path = docPath_;
    if (path.empty()) {
        SaveFileDialog sd;
        sd.SetTitle(L"保存");
        sd.AddFilter(L"PDF 文件", L"*.pdf");
        sd.SetDefaultExtension(L"pdf");
        sd.SetFileName(docName_);
        path = sd.ShowDialog(Hwnd());
        if (path.empty()) return false;
    }
    if (!SaveNow(path)) {
        ShowMessage(L"保存失败", L"文件没有保存，操作已取消。");
        return false;
    }
    return true;
}

bool MainWindow::SaveNow(const std::wstring& path) {
    if (!view_ || view_->DocumentId() == pdf::kNoDocument || !engine_) return false;
    const pdf::DocumentId id = view_->DocumentId();
    bool ok = false;
    pdf::Engine* engine = engine_;
    // RunSync blocks the UI thread; acceptable only here, where the user asked
    // to save before the window closes. FIFO order means every queued edit has
    // been applied before this runs.
    engine_->RunSync([engine, id, &path, &ok] {
        if (pdf::Document* doc = engine->Documents().Get(id)) ok = doc->SaveAs(path).ok();
    });
    if (ok) {
        docPath_ = path;
        pendingEdits_ = 0;
        SetDirty(false);
    }
    return ok;
}

void MainWindow::OnClosing(WindowClosingArgs& e) {
    if (!ConfirmDiscard(L"关闭")) e.cancel = true;
}
