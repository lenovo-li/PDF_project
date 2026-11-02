// PageBitmapSource.h — page bitmaps for one view, rendered on the engine thread.
//
// Shared by PdfPageView and ThumbnailStrip, which differ only in layout. The
// owner asks BitmapFor(page, wantedKey) while painting; this returns the D2D
// bitmap when the exact size is ready, a stale (stretched) one while a new
// size renders, or nullptr. A miss posts one render job.
//
// What keeps it bounded and cheap:
// - CPU bitmaps live in a byte-budgeted pdf::BitmapCache.
// - D2D bitmaps are kept only for pages near the visible range (SetVisible).
// - Jobs check a shared viewport + epoch on the engine thread and skip pages
//   that scrolled away or sizes that a zoom/DPI/document change made stale.
// - A size that failed is not retried until InvalidateSizes or device restore.
//
// Lifetime: results come back through Application::Post holding a weak
// generation token; destroying the source (or SetDocument) invalidates it, so
// a late result never touches freed memory.
//
// THREADING: UI thread only, except the job bodies it posts.
#pragma once

#include <FluentUI/FluentUI.h>
#include <PDFCore/BitmapCache.h>
#include <PDFCore/DocumentStore.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace pdf { class Engine; }

class PageBitmapSource {
public:
    // `onReady(page)` runs on the UI thread when a page inside the visible
    // range has a new bitmap; the owner repaints.
    PageBitmapSource(pdf::Engine* engine, fluent::Application* app, std::uint64_t cacheBytes,
                     std::uint64_t maxPageBytes, std::function<void(int page)> onReady);
    ~PageBitmapSource();
    PageBitmapSource(const PageBitmapSource&) = delete;
    PageBitmapSource& operator=(const PageBitmapSource&) = delete;

    // Drops everything for the old document; kNoDocument clears.
    void SetDocument(pdf::DocumentId id);
    pdf::DocumentId Document() const { return docId_; }

    // Every wanted size changed (zoom, DPI): skip queued jobs, retry failures.
    void InvalidateSizes();

    // The page's content changed (annotation edit): drop its cached bitmaps
    // and ignore renders already in flight for it. The current D2D bitmap is
    // kept and drawn until the new one arrives, so the page does not flash.
    void InvalidatePage(int page);
    // Every page's content may have changed (form fields can span pages). Same
    // rules as InvalidatePage, in O(1) of the page count.
    void InvalidateAllContent();

    // Publishes the visible range to queued jobs and frees D2D bitmaps of
    // pages more than kKeepAround outside it.
    void SetVisible(int first, int last);

    // Key for a page drawn at wDip x hDip in `ctx`'s window: pixel size at the
    // window's DPI, shrunk to the per-page budget and the device texture limit.
    pdf::BitmapCache::Key WantedKey(int page, double wDip, double hDip,
                                    const fluent::UIContext& ctx) const;

    ID2D1Bitmap* BitmapFor(const pdf::BitmapCache::Key& want, const fluent::UIContext& ctx);
    bool Failed(const pdf::BitmapCache::Key& key) const { return failed_.count(key) != 0; }

    void OnDeviceLost();
    void OnDeviceRestored();

    static constexpr int kKeepAround = 2;

private:
    struct KeyHash {
        std::size_t operator()(const pdf::BitmapCache::Key& k) const noexcept;
    };
    struct GpuPage {
        pdf::BitmapCache::Key key;
        fluent::ComPtr<ID2D1Bitmap> bitmap;
        long long contentGen = 0;  // ContentGen(page) when it was uploaded
    };
    // Per-page edits plus whole-document ones, as one number: bumping the
    // global part retires every page at once without touching N entries.
    // 64-bit: the multiply cannot overflow in any realistic number of edits.
    long long ContentGen(int page) const {
        auto it = contentGen_.find(page);
        return static_cast<long long>(allContentGen_) * 1000003 +
               (it == contentGen_.end() ? 0 : it->second);
    }
    // Read by engine jobs; written on the UI thread.
    struct SharedViewport {
        std::atomic<int> first{0};
        std::atomic<int> last{-1};
        std::atomic<unsigned> epoch{0};
    };

    void RequestRender(const pdf::BitmapCache::Key& key);
    void OnRendered(const pdf::BitmapCache::Key& key, std::shared_ptr<const pdf::Bitmap> bmp,
                    bool failed, long long contentGen);

    pdf::Engine* engine_;
    fluent::Application* app_;
    std::uint64_t maxPageBytes_;
    std::function<void(int)> onReady_;
    pdf::DocumentId docId_ = pdf::kNoDocument;

    pdf::BitmapCache cache_;
    std::unordered_map<int, GpuPage> gpu_;
    std::unordered_set<pdf::BitmapCache::Key, KeyHash> pending_;
    std::unordered_set<pdf::BitmapCache::Key, KeyHash> failed_;
    // Bumped per page by InvalidatePage; only pages ever edited have an entry.
    std::unordered_map<int, int> contentGen_;
    int allContentGen_ = 0;  // bumped by InvalidateAllContent
    std::shared_ptr<SharedViewport> viewport_ = std::make_shared<SharedViewport>();
    std::shared_ptr<int> generation_ = std::make_shared<int>(0);
};
