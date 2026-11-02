// MainWindowAnnot.cpp — annotation menu and gesture handling.
//
// Gestures come from PdfPageView (drags, clicks, text-selection markup); the
// menu switches tools or applies markup to the current text selection. All
// document edits run on the engine thread with dirty tracking.

#include "MainWindow.h"

#include "controls/PdfPageView.h"
#include "controls/ThumbnailStrip.h"

#include <PDFCore/Annotations.h>
#include <PDFCore/Document.h>
#include <PDFCore/Engine.h>
#include <PDFCore/Trace.h>

#include <FluentUI/controls/ContentDialog.h>

using namespace fluent;

namespace {
MenuItem Item(const wchar_t* text, std::function<void()> fn, const wchar_t* accel = L"") {
    MenuItem m;
    m.text = text;
    m.accelerator = accel;
    m.AddInvokeHandler(std::move(fn));
    return m;
}

constexpr pdf::AnnotColor kYellow{255, 220, 0, 255};
constexpr pdf::AnnotColor kBlue{50, 120, 255, 255};
constexpr pdf::AnnotColor kRed{255, 50, 50, 255};
constexpr pdf::AnnotColor kBlack{0, 0, 0, 255};
}  // namespace

void MainWindow::BuildAnnotMenu() {
    annotMenu_ = std::make_unique<MenuFlyout>();
    std::vector<MenuItem> items;
    auto markupItem = [&](const wchar_t* text, pdf::AnnotKind kind, pdf::AnnotColor color) {
        return Item(text, [this, kind, color] {
            if (!view_ || !engine_) return;
            const pdf::TextSelection* sel = view_->GetTextSelection();
            if (!sel || sel->count == 0) return;
            const pdf::DocumentId id = view_->DocumentId();
            const int page = sel->page, first = sel->first, count = sel->count;
            Application* app = OwningApplication();
            if (!app) return;
            std::weak_ptr<int> gen = generation_;
            const int ticket = *generation_;
            pdf::Engine* eng = engine_;
            MainWindow* self = this;
            const std::wstring what =
                kind == pdf::AnnotKind::Highlight   ? L"高亮"
                : kind == pdf::AnnotKind::Underline ? L"下划线"
                                                    : L"删除线";
            ShowStatus(what + L"…");
            ++pendingEdits_;
            eng->Post([eng, app, id, page, first, count, kind, color, what, gen, ticket, self] {
                std::wstring error;
                if (pdf::Document* doc = eng->Documents().Get(id)) {
                    if (auto s = pdf::AddMarkupForChars(*doc, page, kind, first, count, color); !s)
                        error = s.error().message;
                } else {
                    error = L"文档已关闭";
                }
                app->Post([self, gen, ticket, id, page, what, error = std::move(error)] {
                    auto alive = gen.lock();
                    if (!alive) return;
                    if (self->pendingEdits_ > 0) --self->pendingEdits_;
                    if (*alive != ticket || self->view_->DocumentId() != id) return;
                    if (!error.empty()) {
                        self->ShowStatus(what + L"失败：" + error);
                        return;
                    }
                    self->PageContentChanged(page);
                    self->ShowStatus(what + L"完成");
                });
            });
        });
    };
    items.push_back(markupItem(L"高亮选中文字", pdf::AnnotKind::Highlight, kYellow));
    items.push_back(markupItem(L"下划线", pdf::AnnotKind::Underline, kBlue));
    items.push_back(markupItem(L"删除线", pdf::AnnotKind::StrikeOut, kRed));
    items.push_back(MenuItem::Sep());
    auto toolItem = [&](const wchar_t* text, AnnotTool tool) {
        MenuItem m;
        m.text = text;
        m.AddInvokeHandler([this, tool] {
            if (view_) view_->SetAnnotationTool(tool);
        });
        return m;
    };
    items.push_back(toolItem(L"便签", AnnotTool::Note));
    items.push_back(toolItem(L"文字框", AnnotTool::FreeText));
    items.push_back(toolItem(L"矩形", AnnotTool::Square));
    items.push_back(toolItem(L"手绘", AnnotTool::Ink));
    items.push_back(Item(L"选择（Esc）", [this] {
        if (view_) view_->SetAnnotationTool(AnnotTool::None);
    }, L"Esc"));
    items.push_back(MenuItem::Sep());
    items.push_back(Item(L"删除选中注释", [this] {
        if (!view_) return;
        auto [page, index] = view_->SelectedAnnotation();
        if (page < 0) return;
        AnnotationGestureArgs args;
        args.type = AnnotationGestureArgs::Type::Delete;
        args.page = page;
        args.annotIndex = index;
        OnAnnotationGesture(*view_, args);
    }, L"Del"));
    annotMenu_->SetItems(std::move(items));
}

void MainWindow::PageContentChanged(int page) {
    if (!view_ || page < 0) return;
    view_->PageContentChanged(page);
    if (thumbs_) thumbs_->PageContentChanged(page);
    SetDirty(true);
}

void MainWindow::OnAnnotationGesture(PdfPageView&, AnnotationGestureArgs& e) {
    if (!view_ || !engine_ || view_->DocumentId() == pdf::kNoDocument) return;
    // Block new gestures while an annotation edit is pending: rapid deletes
    // would capture stale indices after the first removal shifts the array.
    if (pendingEdits_ > 0) {
        ShowStatus(L"正在处理上一个操作…");
        return;
    }
    // Captured before any prompt: a prompt pumps messages, so another file can
    // open while it is up, and the edit must not land on that document.
    const pdf::DocumentId id = view_->DocumentId();
    Application* app = OwningApplication();
    if (!app) return;

    std::wstring what;
    std::function<pdf::Status(pdf::Document&)> job;

    switch (e.type) {
        case AnnotationGestureArgs::Type::AddNote: {
            std::wstring text;
            if (!PromptText(L"添加便签", L"便签文字（可选）：", &text, false)) return;
            if (view_->DocumentId() != id) return;  // document changed during modal
            what = L"添加便签";
            job = [e, text](pdf::Document& doc) -> pdf::Status {
                auto box = pdf::UnitRectToPage(doc, e.page, e.unitRect);
                if (!box) return box.error();
                // Click at top-left of the 20 pt icon (y-up: top is higher).
                pdf::PointF at{box.value().left, box.value().top - 20};
                return pdf::AddNote(doc, e.page, at, text, kYellow);
            };
            break;
        }
        case AnnotationGestureArgs::Type::AddFreeText: {
            std::wstring text;
            if (!PromptText(L"添加文字框", L"文字框内容：", &text, false)) return;
            if (view_->DocumentId() != id) return;  // document changed during modal
            if (text.empty()) {
                ShowMessage(L"文字框不能为空", L"请输入文字后再试。");
                return;
            }
            what = L"添加文字框";
            job = [e, text](pdf::Document& doc) -> pdf::Status {
                auto box = pdf::UnitRectToPage(doc, e.page, e.unitRect);
                if (!box) return box.error();
                return pdf::AddFreeText(doc, e.page, box.value(), text, kBlack);
            };
            break;
        }
        case AnnotationGestureArgs::Type::AddSquare: {
            what = L"添加矩形";
            job = [e](pdf::Document& doc) -> pdf::Status {
                auto box = pdf::UnitRectToPage(doc, e.page, e.unitRect);
                if (!box) return box.error();
                return pdf::AddSquare(doc, e.page, box.value(), kRed, 2.0f);
            };
            break;
        }
        case AnnotationGestureArgs::Type::AddInk: {
            what = L"添加手绘";
            job = [e](pdf::Document& doc) -> pdf::Status {
                auto pts = pdf::UnitPointsToPage(doc, e.page, e.inkStroke);
                if (!pts) return pts.error();
                std::vector<std::vector<pdf::PointF>> strokes{std::move(pts.value())};
                return pdf::AddInk(doc, e.page, strokes, kBlue, 2.0f);
            };
            break;
        }
        case AnnotationGestureArgs::Type::EditText: {
            const pdf::AnnotInfo* info = view_->SelectedAnnotationInfo();
            if (!info) return;
            std::wstring text = info->contents;
            const wchar_t* title = info->kind == pdf::AnnotKind::Note ? L"编辑便签" : L"编辑文字框";
            if (!PromptText(title, L"内容：", &text, false)) return;
            if (view_->DocumentId() != id) return;  // document changed during modal
            what = title;
            job = [e, text](pdf::Document& doc) -> pdf::Status {
                return pdf::SetAnnotationText(doc, e.page, e.annotIndex, text);
            };
            break;
        }
        case AnnotationGestureArgs::Type::Delete: {
            what = L"删除注释";
            job = [e](pdf::Document& doc) -> pdf::Status {
                return pdf::RemoveAnnotation(doc, e.page, e.annotIndex);
            };
            view_->ClearAnnotationSelection();
            break;
        }
        default:
            return;
    }

    const int page = e.page;
    std::weak_ptr<int> gen = generation_;
    const int ticket = *generation_;
    pdf::Engine* eng = engine_;
    MainWindow* self = this;
    ShowStatus(what + L"…");
    ++pendingEdits_;

    eng->Post([eng, app, id, page, job = std::move(job), what, gen, ticket, self] {
        std::wstring error;
        if (pdf::Document* doc = eng->Documents().Get(id)) {
            if (auto s = job(*doc); !s) {
                error = s.error().message;
                pdf::TraceF("[PDFTool.Annot] %ls failed: %ls\n", what.c_str(), error.c_str());
            }
        } else {
            error = L"文档已关闭";
        }
        app->Post([self, gen, ticket, id, page, what, error = std::move(error)] {
            auto alive = gen.lock();
            if (!alive) return;
            if (self->pendingEdits_ > 0) --self->pendingEdits_;
            if (*alive != ticket || self->view_->DocumentId() != id) return;
            if (!error.empty()) {
                self->ShowStatus(what + L"失败：" + error);
                return;
            }
            self->PageContentChanged(page);
            self->ShowStatus(what + L"完成");
        });
    });
}
