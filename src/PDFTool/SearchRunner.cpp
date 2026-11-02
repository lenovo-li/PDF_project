#include "SearchRunner.h"

#include <PDFCore/Engine.h>
#include <PDFCore/Trace.h>

#include <algorithm>
#include <utility>

using namespace fluent;

void SearchRunner::Start(pdf::DocumentId doc, int pageCount, const std::wstring& query,
                         const pdf::SearchOptions& options, Callbacks callbacks) {
    Cancel();
    if (!engine_ || !app_ || doc == pdf::kNoDocument || pageCount <= 0 || query.empty()) return;
    doc_ = doc;
    pageCount_ = pageCount;
    query_ = query.substr(0, pdf::kMaxQueryChars);
    options_ = options;
    cb_ = std::move(callbacks);
    found_ = 0;
    truncated_ = false;
    running_ = true;
    pdf::TraceF("[PDFTool.Search] start: %d pages, %zu-char query\n", pageCount, query_.size());
    PostChunk(0);
}

void SearchRunner::Cancel() {
    generation_->fetch_add(1);  // queued chunks and in-flight results become stale
    running_ = false;
}

void SearchRunner::PostChunk(int firstPage) {
    const int ticket = generation_->load();
    std::weak_ptr<std::atomic<int>> gen = generation_;
    const int last = std::min(pageCount_, firstPage + kPagesPerChunk);
    pdf::Engine* engine = engine_;
    Application* app = app_;
    const pdf::DocumentId id = doc_;
    const std::wstring query = query_;
    const pdf::SearchOptions options = options_;
    SearchRunner* self = this;
    engine_->Post([engine, app, id, firstPage, last, query, options, ticket, gen, self] {
        // Engine thread. A stale chunk (new search, cancel, view gone) skips
        // the PDFium work; that is the whole point of chunking.
        auto alive = gen.lock();
        if (!alive || alive->load() != ticket) return;
        alive.reset();  // never keep the UI-owned token alive past this check
        std::vector<pdf::TextMatch> matches;
        bool pageCapped = false;
        if (pdf::Document* doc = engine->Documents().Get(id)) {
            for (int p = firstPage; p < last; ++p) {
                auto r = pdf::SearchPage(*doc, p, query, options);
                if (!r) continue;  // a broken page is skipped, not fatal
                pageCapped = pageCapped || r.value().truncated;
                for (auto& m : r.value().matches) matches.push_back(std::move(m));
            }
        }
        app->Post([matches = std::move(matches), pageCapped, last, ticket, gen, self]() mutable {
            auto ok = gen.lock();
            if (!ok || ok->load() != ticket) return;  // runner gone or search replaced
            const int room = kMaxTotalMatches - self->found_;
            const bool capped = static_cast<int>(matches.size()) > room;
            if (capped) matches.resize(static_cast<std::size_t>(room));
            self->found_ += static_cast<int>(matches.size());
            self->truncated_ = self->truncated_ || pageCapped || capped;
            if (!matches.empty() && self->cb_.onMatches) self->cb_.onMatches(matches);
            if (self->cb_.onProgress) self->cb_.onProgress(last, self->pageCount_);
            if (capped || last >= self->pageCount_) {
                self->running_ = false;
                pdf::TraceF("[PDFTool.Search] done: %d matches%s\n", self->found_,
                            capped ? " (capped)" : "");
                if (self->cb_.onDone) self->cb_.onDone(self->found_, self->truncated_);
                return;
            }
            self->PostChunk(last);
        });
    });
}
