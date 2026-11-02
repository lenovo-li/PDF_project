#include "ConvertRunner.h"

#include <PDFCore/Engine.h>
#include <PDFCore/FileIO.h>
#include <PDFCore/Trace.h>
#include <PDFCore/convert/Convert.h>
#include <PDFCore/convert/Extract.h>

#include <algorithm>
#include <new>
#include <utility>

using namespace fluent;

void ConvertRunner::Start(pdf::DocumentId doc, int pageCount, const std::wstring& outPath,
                          Callbacks cb) {
    Cancel();
    if (!engine_ || !app_ || doc == pdf::kNoDocument || pageCount <= 0) return;
    doc_ = doc;
    pageCount_ = pageCount;
    out_ = outPath;
    cb_ = std::move(cb);
    pages_ = std::make_shared<Pages>();
    running_ = true;
    truncated_ = false;
    pdf::TraceF("[PDFTool.Convert] start: %d pages\n", pageCount);
    PostChunk(0);
}

void ConvertRunner::Cancel() {
    generation_->fetch_add(1);  // queued jobs and in-flight results become stale
    running_ = false;
    pages_.reset();  // a job still holding it frees it when it ends
}

void ConvertRunner::PostChunk(int firstPage) {
    const int ticket = generation_->load();
    std::weak_ptr<std::atomic<int>> gen = generation_;
    const int last = std::min(pageCount_, firstPage + kPagesPerChunk);
    pdf::Engine* engine = engine_;
    Application* app = app_;
    const pdf::DocumentId id = doc_;
    std::shared_ptr<Pages> pages = pages_;
    ConvertRunner* self = this;
    engine_->Post([engine, app, id, firstPage, last, pages, ticket, gen, self] {
        {
            auto alive = gen.lock();
            if (!alive || alive->load() != ticket) return;
        }
        std::wstring error;
        bool truncated = false;
        pdf::Document* doc = engine->Documents().Get(id);
        if (!doc) {
            error = L"文档已关闭";
        } else {
            std::uint64_t images = 0;
            for (const pdf::ExtractedPage& p : *pages) images += pdf::ImageBytes(p);
            try {
                for (int p = firstPage; p < last && error.empty(); ++p) {
                    auto r = pdf::ExtractPage(*doc, p);
                    if (!r) {
                        error = L"第 " + std::to_wstring(p + 1) + L" 页：" + r.error().message;
                        pdf::TraceF("[PDFTool.Convert] page %d failed: %ls (code %lu)\n", p,
                                    r.error().message.c_str(), r.error().code);
                        break;
                    }
                    images += pdf::ImageBytes(r.value());
                    truncated = truncated || r.value().truncated;
                    if (images > pdf::kMaxConvertImageBytes)
                        error = L"扫描页图片过多，超出转换上限";
                    pages->push_back(std::move(r.value()));
                }
            } catch (const std::bad_alloc&) {
                error = L"内存不足";
            }
        }
        app->Post([last, ticket, gen, self, truncated, error = std::move(error)] {
            auto ok = gen.lock();
            if (!ok || ok->load() != ticket) return;  // runner gone or cancelled
            self->truncated_ = self->truncated_ || truncated;
            if (!error.empty()) {
                self->running_ = false;
                self->pages_.reset();
                if (self->cb_.onDone) self->cb_.onDone(error, self->truncated_);
                return;
            }
            if (self->cb_.onProgress) self->cb_.onProgress(last, self->pageCount_);
            if (last >= self->pageCount_)
                self->PostFinish();
            else
                self->PostChunk(last);
        });
    });
}

void ConvertRunner::PostFinish() {
    const int ticket = generation_->load();
    std::weak_ptr<std::atomic<int>> gen = generation_;
    Application* app = app_;
    std::shared_ptr<Pages> pages = std::move(pages_);  // the job is the last user
    const std::wstring out = out_;
    ConvertRunner* self = this;
    engine_->Post([app, pages, out, ticket, gen, self] {
        {
            auto alive = gen.lock();
            if (!alive || alive->load() != ticket) return;
        }
        std::wstring error;
        auto docx = pdf::FinishDocx(std::move(*pages));
        // Analyze + write can take seconds on a big document; a Cancel in that
        // time must not leave a file behind (or replace an existing one).
        if (auto alive = gen.lock(); !alive || alive->load() != ticket) {
            pdf::TraceF("[PDFTool.Convert] cancelled before writing\n");
            return;
        }
        if (!docx) {
            error = docx.error().message;
        } else if (auto s = pdf::WriteFileAtomic(out, docx.value()); !s) {
            error = s.error().message;
        }
        pdf::TraceF("[PDFTool.Convert] finish: %s\n", error.empty() ? "ok" : "failed");
        app->Post([ticket, gen, self, error = std::move(error)] {
            auto ok = gen.lock();
            if (!ok || ok->load() != ticket) return;
            self->running_ = false;
            if (self->cb_.onDone) self->cb_.onDone(error, self->truncated_);
        });
    });
}
