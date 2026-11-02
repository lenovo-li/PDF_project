// ConvertRunner.h — PDF → Word in the background, chunked and cancellable.
//
// Same scheme as SearchRunner: the engine FIFO is shared with page rendering,
// so extraction runs kPagesPerChunk pages per job and the UI thread queues the
// next chunk when one finishes; render jobs run in between. The extracted
// pages live in a shared buffer that only engine jobs touch. The last job
// analyzes, writes the .docx and saves it atomically.
//
// A new Start or Cancel bumps the generation token; queued jobs then see a
// stale ticket and do nothing.
//
// THREADING: Start/Cancel and the callbacks run on the UI thread.
#pragma once

#include <FluentUI/FluentUI.h>
#include <PDFCore/DocumentStore.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pdf {
class Engine;
struct ExtractedPage;
}  // namespace pdf

class ConvertRunner {
public:
    static constexpr int kPagesPerChunk = 4;

    struct Callbacks {
        std::function<void(int pagesDone, int pageCount)> onProgress;
        // error empty on success; truncated: some page hit the per-page
        // character cap, so text is missing. Not called after Cancel.
        std::function<void(const std::wstring& error, bool truncated)> onDone;
    };

    ConvertRunner(pdf::Engine* engine, fluent::Application* app) : engine_(engine), app_(app) {}
    ~ConvertRunner() { Cancel(); }
    ConvertRunner(const ConvertRunner&) = delete;
    ConvertRunner& operator=(const ConvertRunner&) = delete;

    // Converts document `doc` (pageCount pages, as the view knows them) and
    // writes `outPath`. Cancels a running conversion first.
    void Start(pdf::DocumentId doc, int pageCount, const std::wstring& outPath, Callbacks cb);
    void Cancel();
    bool Running() const { return running_; }

private:
    using Pages = std::vector<pdf::ExtractedPage>;
    void PostChunk(int firstPage);
    void PostFinish();

    pdf::Engine* engine_;
    fluent::Application* app_;
    pdf::DocumentId doc_ = pdf::kNoDocument;
    int pageCount_ = 0;
    std::wstring out_;
    Callbacks cb_;
    bool running_ = false;
    bool truncated_ = false;  // a page hit ExtractPage's character cap
    // Engine-thread only (created on the UI thread before the first job).
    std::shared_ptr<Pages> pages_;
    // Read by engine jobs while the UI thread bumps it: atomic.
    std::shared_ptr<std::atomic<int>> generation_ = std::make_shared<std::atomic<int>>(0);
};
