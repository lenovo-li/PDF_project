#include "Engine.h"

#include "DocumentStore.h"

#include "Trace.h"

#include <fpdfview.h>

#include <objbase.h>

#include <exception>
#include <future>

#pragma comment(lib, "ole32.lib")

namespace pdf {

namespace {
// Runs `job`, turning any exception into a trace line. True if it finished.
bool RunGuarded(const std::function<void()>& job) {
    try {
        job();
        return true;
    } catch (const std::bad_alloc&) {
        TraceF("[PDFTool.Engine] job failed: out of memory\n");
    } catch (const std::exception& e) {
        TraceF("[PDFTool.Engine] job failed: %s\n", e.what());
    } catch (...) {
        TraceF("[PDFTool.Engine] job failed: unknown exception\n");
    }
    return false;
}
}  // namespace

Engine::Engine() : thread_([this] { ThreadMain(); }) {}

DocumentStore& Engine::Documents() { return *documents_; }

Engine::~Engine() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void Engine::Post(std::function<void()> job) {
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    wake_.notify_one();
}

bool Engine::RunSync(std::function<void()> job) {
    // Re-entrant call from a job: run inline, else deadlock.
    if (OnEngineThread()) return RunGuarded(job);
    // The guard is inside the posted job so the promise is always fulfilled;
    // a throw that skipped set_value would leave this thread waiting forever.
    std::promise<bool> done;
    auto finished = done.get_future();
    Post([&] { done.set_value(RunGuarded(job)); });
    return finished.get();
}

void Engine::ThreadMain() {
    engineId_.store(std::this_thread::get_id());
    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;  // fields up to m_v8EmbedderSlot; no V8 in this build
    FPDF_InitLibraryWithConfig(&config);
    TraceF("[PDFTool.Engine] PDFium initialized on thread %lu\n", GetCurrentThreadId());
    // COM for WIC (image export) on this thread. MTA: the engine thread has no
    // message loop, which an STA would need. Balanced at thread exit.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com))
        TraceF("[PDFTool.Engine] CoInitializeEx failed, hr=0x%08lX (image export disabled)\n",
               static_cast<unsigned long>(com));
    documents_ = std::make_unique<DocumentStore>();

    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (jobs_.empty()) break;  // stopping and drained
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        RunGuarded(job);
    }

    // Close every document still open before tearing PDFium down.
    TraceF("[PDFTool.Engine] closing %zu open document(s)\n", documents_->Count());
    documents_.reset();
    if (SUCCEEDED(com)) CoUninitialize();
    FPDF_DestroyLibrary();
    TraceF("[PDFTool.Engine] PDFium destroyed\n");
}

}  // namespace pdf
