#include "Engine.h"

#include "Trace.h"

#include <fpdfview.h>

#include <future>

namespace pdf {

Engine::Engine() : thread_([this] { ThreadMain(); }) {}

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

void Engine::RunSync(std::function<void()> job) {
    if (OnEngineThread()) {  // re-entrant call from a job: run inline, else deadlock
        job();
        return;
    }
    std::promise<void> done;
    auto finished = done.get_future();
    Post([&] {
        job();
        done.set_value();
    });
    finished.wait();
}

void Engine::ThreadMain() {
    engineId_.store(std::this_thread::get_id());
    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;  // fields up to m_v8EmbedderSlot; no V8 in this build
    FPDF_InitLibraryWithConfig(&config);
    TraceF("[PDFTool.Engine] PDFium initialized on thread %lu\n", GetCurrentThreadId());

    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (jobs_.empty()) break;  // stopping and drained
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        job();
    }

    FPDF_DestroyLibrary();
    TraceF("[PDFTool.Engine] PDFium destroyed\n");
}

}  // namespace pdf
