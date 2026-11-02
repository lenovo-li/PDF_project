// Engine.h — the single thread that owns PDFium.
//
// PDFium has process-global state and is not thread-safe, so exactly one
// thread calls FPDF_InitLibraryWithConfig, every FPDF_* function, and
// FPDF_DestroyLibrary. Everything else submits work here.
//
//   UI thread ── Post(job) ──▶ engine thread runs job (opens/renders/saves)
//                                   │
//   UI thread ◀── app->Post(result) ┘  (the job itself marshals its result back
//                                       with fluent::Application::Post; PDFCore
//                                       does not know about the UI)
//
// Jobs run in FIFO order. Long jobs should check a cancellation flag they
// capture. One Engine per process; construct it in wWinMain after
// fluent::Application and destroy it before (declaration order does this).
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace pdf {

class DocumentStore;

class Engine {
public:
    Engine();   // starts the thread and initializes PDFium on it
    ~Engine();  // drains queued jobs, destroys PDFium, joins

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Queue a job; returns immediately. Safe from any thread.
    //
    // A job that throws is stopped there: the exception is traced and the
    // engine goes on with the next job, instead of std::terminate taking the
    // process (and the user's unsaved edits) down. PDFCore reports expected
    // failures through Result, so this is a last line of defence. Whatever the
    // job would have done after the throw does not happen, including posting
    // its result back to the UI.
    void Post(std::function<void()> job);

    // Queue a job and block until it has run. For tests and shutdown paths
    // only; never call from the UI thread during normal operation. Returns
    // false if the job threw (the exception is traced, not rethrown).
    bool RunSync(std::function<void()> job);

    bool OnEngineThread() const { return std::this_thread::get_id() == engineId_.load(); }

    // The open documents. Engine thread only (call it from inside a job). It is
    // created after PDFium is initialized and destroyed before PDFium is, both
    // on the engine thread, so no document outlives the library.
    DocumentStore& Documents();

private:
    void ThreadMain();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    bool stopping_ = false;
    // Written by the engine thread itself on start. thread_.get_id() is not safe
    // to read from inside the thread before std::thread's constructor returns.
    std::atomic<std::thread::id> engineId_{};
    std::unique_ptr<DocumentStore> documents_;  // engine thread only
    std::thread thread_;  // last: starts after the members above exist
};

}  // namespace pdf
