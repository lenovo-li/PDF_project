// MainWindowTools.cpp — tools menu (qpdf file tools, PNG export) and the
// modal prompts they use.
//
// The qpdf tools work on the saved file on disk, so they refuse to run while
// there are unsaved edits (the user saves first). They run on the engine
// thread: qpdf does not touch PDFium, but serialising them there keeps the UI
// thread free of file I/O without adding another worker.

#include "MainWindow.h"

#include "controls/PdfPageView.h"

#include <FluentUI/controls/CheckBox.h>
#include <FluentUI/controls/ContentDialog.h>
#include <FluentUI/dialogs/FileDialog.h>
#include <PDFCore/Document.h>
#include <PDFCore/Engine.h>
#include <PDFCore/FileIO.h>
#include <PDFCore/ImageExport.h>
#include <PDFCore/PageRange.h>
#include <PDFCore/Render.h>

#include <algorithm>
#include <memory>
#include <utility>

using namespace fluent;

bool MainWindow::PromptText(const std::wstring& title, const std::wstring& label,
                            std::wstring* value, bool password) {
    ContentDialog dlg;
    dlg.SetTitle(title);
    auto body = std::make_unique<StackPanel>();
    body->SetSpacing(8.0f);
    auto* text = body->Emplace<TextBlock>();
    text->SetText(label);
    text->SetWrap(true);
    auto* box = body->Emplace<TextBox>();
    box->SetWidth(320.0f);
    box->SetText(*value);
    box->SetPassword(password);
    dlg.SetContent(std::move(body));
    dlg.AddButton(L"确定", DialogResult::Primary);
    dlg.AddButton(L"取消", DialogResult::Cancel);
    if (dlg.ShowDialog(*this) != DialogResult::Primary) return false;
    *value = box->Text();  // the dialog (and its tree) is still alive here
    return true;
}

bool MainWindow::PromptEncryption(std::string* user, std::string* owner,
                                  pdf::Permissions* perms) {
    // A DialogWindow is single-use (Start refuses an initialized dialog), so a
    // re-prompt after a missing owner password builds a fresh dialog.
    for (;;) {
        ContentDialog dlg;
        dlg.SetTitle(L"加密 PDF（AES-256）");
        auto body = std::make_unique<StackPanel>();
        body->SetSpacing(8.0f);
        auto label = [&](const wchar_t* t) {
            auto* l = body->Emplace<TextBlock>();
            l->SetText(t);
            l->SetWrap(true);
        };
        auto pw = [&]() {
            auto* b = body->Emplace<TextBox>();
            b->SetWidth(320.0f);
            b->SetPassword(true);
            return b;
        };
        auto check = [&](const wchar_t* t) {
            auto* c = body->Emplace<CheckBox>();
            c->SetText(t);
            c->SetChecked(true);
            return c;
        };
        label(L"打开密码（可留空：任何人都能打开，但受下面的权限限制）：");
        TextBox* userBox = pw();
        label(L"所有者密码（必填，拥有全部权限）：");
        TextBox* ownerBox = pw();
        label(L"用打开密码打开时允许：");
        CheckBox* print = check(L"打印");
        CheckBox* copy = check(L"复制文字和图片");
        CheckBox* modify = check(L"修改内容");
        CheckBox* annotate = check(L"添加注释、填写表单");
        CheckBox* assemble = check(L"插入、旋转、删除页面");
        dlg.SetContent(std::move(body));
        dlg.AddButton(L"加密", DialogResult::Primary);
        dlg.AddButton(L"取消", DialogResult::Cancel);
        if (dlg.ShowDialog(*this) != DialogResult::Primary) return false;
        if (ownerBox->Text().empty()) {
            ShowMessage(L"需要所有者密码", L"请设置所有者密码，否则无法限制权限。");
            continue;
        }
        *user = pdf::WideToUtf8(userBox->Text());
        *owner = pdf::WideToUtf8(ownerBox->Text());
        perms->print = print->IsChecked();
        perms->copy = copy->IsChecked();
        perms->modify = modify->IsChecked();
        perms->annotate = annotate->IsChecked();
        perms->assemble = assemble->IsChecked();
        return true;
    }
}

void MainWindow::ShowMessage(const std::wstring& title, const std::wstring& text) {
    ContentDialog dlg;
    dlg.SetTitle(title);
    auto body = std::make_unique<TextBlock>();
    body->SetText(text);
    body->SetWrap(true);
    dlg.SetContent(std::move(body));
    dlg.AddButton(L"确定", DialogResult::Primary);
    dlg.ShowDialog(*this);
}

std::optional<MainWindow::ToolTarget> MainWindow::RequireSavedFile() {
    if (!view_ || view_->DocumentId() == pdf::kNoDocument || docPath_.empty()) {
        ShowStatus(L"请先打开一个 PDF");
        return std::nullopt;
    }
    if (dirty_ || pendingEdits_ > 0) {
        ShowMessage(L"有未保存的修改", L"这个工具处理磁盘上的文件。请先保存（Ctrl+S），再使用。");
        return std::nullopt;
    }
    return ToolTarget{view_->DocumentId(), docPath_, docName_, docPassword_};
}

bool MainWindow::StillCurrent(const ToolTarget& t) {
    // After the prompts: the same document, still saved, still the same file.
    if (view_ && view_->DocumentId() == t.id && docPath_ == t.path && !dirty_ &&
        pendingEdits_ == 0)
        return true;
    ShowMessage(L"文档已变化", L"在设置选项期间文档被修改或换成了其他文件，操作已取消。");
    return false;
}

bool MainWindow::OutputClashes(const std::wstring& out, const std::vector<std::wstring>& inputs) {
    // Paths come from the shell dialogs, so they are full paths; compare
    // case-insensitively like NTFS does.
    auto same = [&](const std::wstring& p) { return _wcsicmp(p.c_str(), out.c_str()) == 0; };
    if (!same(docPath_) && std::none_of(inputs.begin(), inputs.end(), same)) return false;
    ShowMessage(L"不能覆盖输入文件",
                L"输出文件和正在处理的文件相同。请选择另一个文件名，原文件保持不变。");
    return true;
}

void MainWindow::RunFileTool(const wchar_t* what, std::function<pdf::Status()> job) {
    Application* app = OwningApplication();
    if (!app || !engine_) return;
    std::weak_ptr<int> gen = generation_;
    const int ticket = *generation_;
    MainWindow* self = this;
    const std::wstring label = what;
    ShowStatus(label + L"…");
    engine_->Post([app, job = std::move(job), gen, ticket, self, label] {
        // Engine thread (qpdf work, file I/O; PNG export also renders).
        const pdf::Status s = job();
        std::wstring error = s ? std::wstring{} : s.error().message;
        app->Post([gen, ticket, self, label, error = std::move(error)] {
            auto alive = gen.lock();
            if (!alive) return;  // window closed meanwhile
            const std::wstring text = error.empty() ? label + L"完成" : label + L"失败：" + error;
            // Another file opened since: do not overwrite its status line, but
            // a failure still deserves to be seen.
            if (*alive != ticket) {
                if (!error.empty()) self->ShowMessage(label, text);
                return;
            }
            self->ShowStatus(text);
        });
    });
}

namespace {
std::wstring AskSavePath(HWND owner, const wchar_t* title, const std::wstring& suggested) {
    SaveFileDialog dlg;
    dlg.SetTitle(title);
    dlg.AddFilter(L"PDF 文件", L"*.pdf");
    dlg.SetDefaultExtension(L"pdf");
    dlg.SetFileName(suggested);
    return dlg.ShowDialog(owner);
}

std::wstring Stem(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}
}  // namespace

void MainWindow::MergeWithOthers() {
    auto t = RequireSavedFile();
    if (!t) return;
    OpenFileDialog dlg;
    dlg.SetTitle(L"选择要合并到后面的 PDF（可多选）");
    dlg.AddFilter(L"PDF 文件", L"*.pdf");
    dlg.SetMultiSelect(true);
    std::vector<std::wstring> others = dlg.ShowMulti(Hwnd());
    if (others.empty()) return;
    if (!StillCurrent(t.value())) return;
    const std::wstring out = AskSavePath(Hwnd(), L"合并后保存为", Stem(t->name) + L"_合并.pdf");
    if (out.empty()) return;
    std::vector<std::wstring> inputs{t->path};
    inputs.insert(inputs.end(), others.begin(), others.end());
    if (OutputClashes(out, inputs)) return;
    const std::string pw = t->password;  // for the first input
    RunFileTool(L"合并", [inputs, out, pw] { return pdf::MergeFiles(inputs, out, pw); });
}

void MainWindow::ExtractCurrent() {
    auto t = RequireSavedFile();
    if (!t) return;
    std::wstring range = std::to_wstring(std::max(0, view_->CurrentPage()) + 1);
    if (!PromptText(L"提取页面", L"要提取的页码，例如 1,3,5-7：", &range)) return;
    auto pages = pdf::ParsePageRange(range, view_->PageCount());
    if (!pages) {
        ShowMessage(L"页码无效", pages.error().message);
        return;
    }
    if (!StillCurrent(t.value())) return;
    const std::wstring out = AskSavePath(Hwnd(), L"提取的页面保存为", Stem(t->name) + L"_提取.pdf");
    if (out.empty()) return;
    if (OutputClashes(out, {t->path})) return;
    RunFileTool(L"提取页面", [in = t->path, p = pages.value(), out, pw = t->password] {
        return pdf::ExtractPages(in, p, out, pw);
    });
}

void MainWindow::SplitCurrent() {
    // More files than this in one go is almost certainly a mistake (and would
    // keep the engine busy for a long time without a way to cancel).
    constexpr int kMaxSplitFiles = 1000;
    auto t = RequireSavedFile();
    if (!t) return;
    std::wstring every = L"1";
    if (!PromptText(L"拆分", L"每个文件包含几页：", &every)) return;
    auto n = pdf::ParsePositiveInt(every, std::max(1, view_->PageCount()));
    if (!n) {
        ShowMessage(L"输入无效", n.error().message);
        return;
    }
    if (!StillCurrent(t.value())) return;
    const auto chunks = pdf::SplitEvery(view_->PageCount(), n.value());
    if (static_cast<int>(chunks.size()) > kMaxSplitFiles) {
        ShowMessage(L"文件太多", L"这样会生成 " + std::to_wstring(chunks.size()) +
                                     L" 个文件，上限是 " + std::to_wstring(kMaxSplitFiles) +
                                     L"。请增大每个文件的页数。");
        return;
    }
    // The dialog only names the series; its own overwrite prompt would ask
    // about the base name, which is never written. Check the real outputs.
    SaveFileDialog dlg;
    dlg.SetTitle(L"拆分文件的保存位置和名称（会加上 _1、_2…）");
    dlg.AddFilter(L"PDF 文件", L"*.pdf");
    dlg.SetDefaultExtension(L"pdf");
    dlg.SetFileName(Stem(t->name) + L"_拆分.pdf");
    dlg.SetOverwritePrompt(false);
    const std::wstring base = dlg.ShowDialog(Hwnd());
    if (base.empty()) return;
    std::vector<std::wstring> outs;
    int existing = 0;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        outs.push_back(
            pdf::NumberedPath(base, static_cast<int>(i) + 1, static_cast<int>(chunks.size())));
        if (OutputClashes(outs.back(), {t->path})) return;
        if (GetFileAttributesW(outs.back().c_str()) != INVALID_FILE_ATTRIBUTES) ++existing;
    }
    if (existing > 0) {
        ContentDialog ask;
        ask.SetTitle(L"文件已存在");
        auto body = std::make_unique<TextBlock>();
        body->SetText(std::to_wstring(existing) + L" 个输出文件已经存在（例如 " + outs[0] +
                      L"）。要覆盖吗？");
        body->SetWrap(true);
        ask.SetContent(std::move(body));
        ask.AddButton(L"覆盖", DialogResult::Primary);
        ask.AddButton(L"取消", DialogResult::Cancel);
        if (ask.ShowDialog(*this) != DialogResult::Primary) return;
    }
    RunFileTool(L"拆分", [in = t->path, chunks, outs, pw = t->password] {
        return pdf::SplitFile(in, chunks, outs, pw);
    });
}

void MainWindow::EncryptCurrent() {
    auto t = RequireSavedFile();
    if (!t) return;
    std::string user, owner;
    pdf::Permissions perms;
    if (!PromptEncryption(&user, &owner, &perms)) return;
    if (!StillCurrent(t.value())) return;
    const std::wstring out = AskSavePath(Hwnd(), L"加密后保存为", Stem(t->name) + L"_加密.pdf");
    if (out.empty()) return;
    if (OutputClashes(out, {t->path})) return;
    RunFileTool(L"加密", [in = t->path, out, user, owner, perms, pw = t->password] {
        return pdf::EncryptPdf(in, out, user, owner, perms, pw);
    });
}

void MainWindow::DecryptCurrent() {
    auto t = RequireSavedFile();
    if (!t) return;
    // Owner-only encrypted files have an empty docPassword_, and qpdf accepts
    // an empty password for them. If the file is not encrypted, qpdf just
    // writes a copy, which is harmless.
    const std::wstring out = AskSavePath(Hwnd(), L"无密码副本保存为", Stem(t->name) + L"_无密码.pdf");
    if (out.empty()) return;
    if (OutputClashes(out, {t->path})) return;
    RunFileTool(L"移除密码",
                [in = t->path, out, pw = t->password] { return pdf::DecryptPdf(in, out, pw); });
}

void MainWindow::CompressCurrent() {
    auto t = RequireSavedFile();
    if (!t) return;
    std::wstring answer = L"否";
    if (!PromptText(L"压缩 / 优化",
                    L"同时做线性化（网页快速查看，文件可能略大）？输入 是 或 否：", &answer))
        return;
    if (!StillCurrent(t.value())) return;
    pdf::CompressOptions opt;
    opt.linearize = answer.find(L'是') != std::wstring::npos ||
                    answer.find(L'y') != std::wstring::npos ||
                    answer.find(L'Y') != std::wstring::npos;
    const std::wstring out = AskSavePath(Hwnd(), L"优化后保存为", Stem(t->name) + L"_优化.pdf");
    if (out.empty()) return;
    if (OutputClashes(out, {t->path})) return;
    RunFileTool(L"压缩", [in = t->path, out, opt, pw = t->password] {
        return pdf::CompressFile(in, out, opt, pw);
    });
}

void MainWindow::ExportPagePng() {
    if (!view_ || view_->DocumentId() == pdf::kNoDocument) return;
    const int page = std::max(0, view_->CurrentPage());
    SaveFileDialog dlg;
    dlg.SetTitle(L"导出当前页为 PNG");
    dlg.AddFilter(L"PNG 图片", L"*.png");
    dlg.SetDefaultExtension(L"png");
    dlg.SetFileName(Stem(docName_) + L"_" + std::to_wstring(page + 1) + L".png");
    const std::wstring out = dlg.ShowDialog(Hwnd());
    if (out.empty()) return;
    const pdf::DocumentId id = view_->DocumentId();
    pdf::Engine* engine = engine_;
    // Works on the in-memory document, so unsaved edits are exported too.
    RunFileTool(L"导出 PNG", [engine, id, page, out]() -> pdf::Status {
        pdf::Document* doc = engine->Documents().Get(id);
        if (!doc) return pdf::Error{L"文档已关闭"};
        auto g = doc->PageGeometryAt(page);
        if (!g) return g.error();
        auto size = pdf::RenderSizePx(g.value(), 200.0);  // 200 dpi: print quality
        if (!size) return size.error();
        auto bmp = doc->RenderPage(page, size.value().width, size.value().height);
        if (!bmp) return bmp.error();
        try {
            return pdf::SavePng(bmp.value(), out);
        } catch (const std::bad_alloc&) {
            return pdf::Error{L"内存不足"};
        }
    });
}
