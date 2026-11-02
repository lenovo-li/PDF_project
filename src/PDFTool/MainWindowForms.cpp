// MainWindowForms.cpp — "Fill Form..." dialog and ApplyFormEdits on engine thread.

#include "MainWindow.h"

#include "controls/PdfPageView.h"
#include "controls/ThumbnailStrip.h"
#include "dialogs/FillFormDialog.h"

#include <PDFCore/Document.h>
#include <PDFCore/Engine.h>
#include <PDFCore/Forms.h>
#include <PDFCore/Trace.h>

#include <FluentUI/Application.h>

using namespace fluent;

void MainWindow::FillForm() {
    if (!view_ || view_->DocumentId() == pdf::kNoDocument) {
        ShowMessage(L"没有打开的文档", L"请先打开一个 PDF 文件。");
        return;
    }
    if (pendingEdits_ > 0) {
        ShowMessage(L"操作进行中", L"请等待当前操作完成。");
        return;
    }
    const pdf::DocumentId id = view_->DocumentId();
    pdf::Engine* eng = engine_;
    Application* app = OwningApplication();
    MainWindow* self = this;
    std::weak_ptr<int> gen = generation_;
    const int ticket = *generation_;
    ShowStatus(L"正在读取表单字段…");

    eng->Post([eng, app, id, self, gen, ticket] {
        pdf::FormFieldList list;
        if (pdf::Document* doc = eng->Documents().Get(id)) {
            if (!pdf::HasForm(*doc)) {
                app->Post([self, gen, ticket, id] {
                    auto alive = gen.lock();
                    if (!alive || *alive != ticket || self->view_->DocumentId() != id) return;
                    self->ShowStatus(L"文档没有可填写的表单");
                    self->ShowMessage(L"没有表单", L"此文档不包含 AcroForm 表单字段。");
                });
                return;
            }
            auto r = pdf::ListFormFields(*doc);
            if (!r) {
                app->Post([self, gen, ticket, id, error = r.error().message] {
                    auto alive = gen.lock();
                    if (!alive || *alive != ticket || self->view_->DocumentId() != id) return;
                    self->ShowStatus(L"读取表单字段失败：" + error);
                });
                return;
            }
            list = std::move(r.value());
        }
        app->Post([self, app, eng, gen, ticket, id, list = std::move(list)] {
            auto alive = gen.lock();
            if (!alive || *alive != ticket || self->view_->DocumentId() != id) return;
            self->ShowStatus(L"");
            if (list.fields.empty()) {
                self->ShowMessage(L"没有字段", L"此表单不包含可填写的字段。");
                return;
            }
            FillFormDialog dlg(list);
            if (dlg.ShownCount() == 0) {
                self->ShowMessage(L"没有可填字段", L"此表单只有签名、按钮或只读字段。");
                return;
            }
            const auto edits = dlg.Run(*self);
            if (edits.empty()) {
                self->ShowStatus(L"没有修改");
                return;
            }
            self->ShowStatus(L"正在应用表单编辑…");
            std::weak_ptr<int> gen2 = self->generation_;
            const int ticket2 = *self->generation_;
            ++self->pendingEdits_;
            eng->Post([eng, app, id, edits, gen2, ticket2, self] {
                int applied = 0;
                std::wstring error;
                if (pdf::Document* doc = eng->Documents().Get(id)) {
                    auto r = pdf::ApplyFormEdits(*doc, edits);
                    if (r) {
                        applied = r.value();
                    } else {
                        error = r.error().message;
                        pdf::TraceF("[PDFTool.Forms] ApplyFormEdits failed: %ls\n", error.c_str());
                    }
                }
                const int wanted = static_cast<int>(edits.size());
                app->Post([self, gen2, ticket2, id, applied, wanted, error = std::move(error)] {
                    auto alive = gen2.lock();
                    if (!alive) return;
                    if (self->pendingEdits_ > 0) --self->pendingEdits_;
                    if (*alive != ticket2 || self->view_->DocumentId() != id) return;
                    if (!error.empty()) {
                        self->ShowStatus(L"应用表单编辑失败：" + error);
                        return;
                    }
                    // Field edits may span multiple pages; invalidate all.
                    if (applied > 0) {
                        self->view_->AllContentChanged();
                        if (self->thumbs_) self->thumbs_->AllContentChanged();
                        self->SetDirty(true);
                    }
                    if (applied == wanted)
                        self->ShowStatus(L"已应用 " + std::to_wstring(applied) + L" 个字段的编辑");
                    else
                        self->ShowStatus(L"已应用 " + std::to_wstring(applied) + L" / " +
                                         std::to_wstring(wanted) + L" 个字段的编辑，其余未能写入");
                });
            });
        });
    });
}
