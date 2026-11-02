// SearchRunner.h — whole-document text search, streamed and cancellable.
//
// The engine thread is a FIFO shared with page rendering. One job searching
// 5000 pages would freeze the view for its whole run, so the search is split
// into chunks of kPagesPerChunk pages. Each chunk's result is posted to the UI
// thread, which then queues the next chunk; render jobs posted meanwhile run
// in between. A new Start or Cancel bumps a generation token, so a chunk that
// is already queued finds itself stale and does nothing.
//
// THREADING: Start/Cancel and the callbacks run on the UI thread.
#pragma once

#include <FluentUI/FluentUI.h>
#include <PDFCore/DocumentStore.h>
#include <PDFCore/TextSearch.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pdf { class Engine; }

class SearchRunner {
public:
    static constexpr int kPagesPerChunk = 8;
    static constexpr int kMaxTotalMatches = 10000;

    struct Callbacks {
        // A batch of matches for consecutive pages, in page order.
        std::function<void(std::vector<pdf::TextMatch>&)> onMatches;
        // pagesDone of pageCount searched; called after every chunk.
        std::function<void(int pagesDone, int pageCount)> onProgress;
        // Search finished. capped: results are incomplete (a page or the
        // total hit its cap), so the count is shown as "N+".
        std::function<void(int total, bool capped)> onDone;
    };

    SearchRunner(pdf::Engine* engine, fluent::Application* app) : engine_(engine), app_(app) {}
    ~SearchRunner() { Cancel(); }
    SearchRunner(const SearchRunner&) = delete;
    SearchRunner& operator=(const SearchRunner&) = delete;

    void Start(pdf::DocumentId doc, int pageCount, const std::wstring& query,
               const pdf::SearchOptions& options, Callbacks callbacks);
    void Cancel();
    bool Running() const { return running_; }

private:
    void PostChunk(int firstPage);

    pdf::Engine* engine_;
    fluent::Application* app_;
    pdf::DocumentId doc_ = pdf::kNoDocument;
    int pageCount_ = 0;
    std::wstring query_;
    pdf::SearchOptions options_;
    Callbacks cb_;
    int found_ = 0;
    // Some page hit kMaxMatchesPerPage, or the total hit kMaxTotalMatches.
    bool truncated_ = false;
    bool running_ = false;
    // Read by chunk jobs on the engine thread while Start/Cancel bump it on the
    // UI thread, so it must be atomic (a plain int here is a data race).
    std::shared_ptr<std::atomic<int>> generation_ = std::make_shared<std::atomic<int>>(0);
};
