// MainWindowConvert.cpp — "Convert to Word": save dialog, ConvertRunner,
// progress in the status line.

#include "MainWindow.h"

#include "controls/PdfPageView.h"

#include <PDFCore/Trace.h>
#include <PDFCore/convert/Convert.h>

#include <FluentUI/Application.h>
#include <FluentUI/dialogs/FileDialog.h>

using namespace fluent;

void MainWindow::ConvertToWord() {
    if (!view_ || view_->DocumentId() == pdf::kNoDocument) {
        ShowMessage(L"没有打开的文档", L"请先打开一个 PDF 文件。");
        return;
    }
    const int pages = view_->PageCount();
    if (pages <= 0) {
        ShowMessage(L"无法转换", L"文档没有页面。");
        return;
    }
    if (pages > pdf::kMaxConvertPages) {
        ShowMessage(L"无法转换", L"文档超过 " + std::to_wstring(pdf::kMaxConvertPages) +
                                     L" 页，超出转换上限。");
        return;
    }
    if (convert_ && convert_->Running()) {
        ShowMessage(L"正在转换", L"上一个转换还没有完成。可以在“工具→取消转换”中停止它。");
        return;
    }
    if (pendingEdits_ > 0) {
        ShowMessage(L"操作进行中", L"请等待当前操作完成。");
        return;
    }
    const pdf::DocumentId id = view_->DocumentId();
    SaveFileDialog dlg;
    dlg.SetTitle(L"转为 Word 文档");
    dlg.AddFilter(L"Word 文档", L"*.docx");
    dlg.SetDefaultExtension(L"docx");
    const size_t dot = docName_.find_last_of(L'.');
    dlg.SetFileName((dot == std::wstring::npos ? docName_ : docName_.substr(0, dot)) + L".docx");
    const std::wstring out = dlg.ShowDialog(Hwnd());
    if (out.empty()) return;
    // The dialog is modal: a different file may have been opened meanwhile.
    if (view_->DocumentId() != id) return;
    if (OutputClashes(out, {})) return;

    ShowStatus(L"正在转为 Word…");
    MainWindow* self = this;
    ConvertRunner::Callbacks cb;
    cb.onProgress = [self](int done, int total) {
        self->ShowStatus(L"正在转为 Word… " + std::to_wstring(done) + L" / " +
                         std::to_wstring(total) + L" 页");
    };
    // The status line is narrow (it shares the title bar): name only.
    const size_t slash = out.find_last_of(L"\\/");
    const std::wstring outName = slash == std::wstring::npos ? out : out.substr(slash + 1);
    cb.onDone = [self, outName](const std::wstring& error, bool truncated) {
        if (error.empty()) {
            self->ShowStatus(L"已转为 Word：" + outName);
            if (truncated)
                self->ShowMessage(L"部分文字未转换",
                                  L"有页面的文字超过每页 20 万字符的上限，超出部分没有写入 Word 文档。");
        } else {
            self->ShowStatus(L"转为 Word 失败：" + error);
            self->ShowMessage(L"转为 Word 失败", error);
        }
    };
    // Works on the in-memory document, so unsaved edits are included.
    convert_->Start(id, pages, out, std::move(cb));
}

void MainWindow::CancelConvert() {
    if (!convert_ || !convert_->Running()) {
        ShowStatus(L"没有正在进行的转换");
        return;
    }
    convert_->Cancel();
    pdf::TraceF("[PDFTool.Convert] cancelled\n");
    ShowStatus(L"已取消转为 Word");
}
