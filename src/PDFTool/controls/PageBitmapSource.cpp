#include "PageBitmapSource.h"

#include <PDFCore/Document.h>
#include <PDFCore/Engine.h>
#include <PDFCore/Render.h>
#include <PDFCore/Trace.h>

#include <FluentUI/graphics/D2DContext.h>
#include <FluentUI/window/WindowServices.h>

#include <algorithm>
#include <cmath>

using namespace fluent;

std::size_t PageBitmapSource::KeyHash::operator()(const pdf::BitmapCache::Key& k) const noexcept {
    return (static_cast<std::size_t>(k.page) * 1000003u) ^
           (static_cast<std::size_t>(k.width) << 20) ^ static_cast<std::size_t>(k.height) ^
           (static_cast<std::size_t>(k.rotation) << 40);
}

PageBitmapSource::PageBitmapSource(pdf::Engine* engine, Application* app,
                                   std::uint64_t cacheBytes, std::uint64_t maxPageBytes,
                                   std::function<void(int)> onReady)
    : engine_(engine), app_(app), maxPageBytes_(maxPageBytes), onReady_(std::move(onReady)),
      cache_(cacheBytes) {}

PageBitmapSource::~PageBitmapSource() { ++*generation_; }

void PageBitmapSource::SetDocument(pdf::DocumentId id) {
    ++*generation_;  // drop results still in flight for the old document
    viewport_->epoch.fetch_add(1);  // and skip its renders still queued
    docId_ = id;
    cache_.Clear();
    gpu_.clear();
    pending_.clear();
    failed_.clear();
    contentGen_.clear();
    viewport_->first = 0;
    viewport_->last = -1;
}

void PageBitmapSource::InvalidateSizes() {
    viewport_->epoch.fetch_add(1);
    failed_.clear();  // a size-related failure may not recur at the new size
}

void PageBitmapSource::InvalidateAllContent() {
    ++allContentGen_;
    cache_.Clear();
    pending_.clear();  // in-flight results carry the old generation and are dropped
    failed_.clear();
}

void PageBitmapSource::InvalidatePage(int page) {
    ++contentGen_[page];
    cache_.InvalidatePage(page);
    // A render in flight for the old content will be dropped by its content
    // generation; forget it here so the next paint can request the new one.
    for (auto it = pending_.begin(); it != pending_.end();)
        it = it->page == page ? pending_.erase(it) : std::next(it);
    for (auto it = failed_.begin(); it != failed_.end();)
        it = it->page == page ? failed_.erase(it) : std::next(it);
}

void PageBitmapSource::SetVisible(int first, int last) {
    viewport_->first = first;
    viewport_->last = last;
    // Keep D2D bitmaps only near the viewport, so GPU memory is bounded by what
    // is on screen, not by how far the user has scrolled.
    for (auto it = gpu_.begin(); it != gpu_.end();) {
        const bool keep = it->first >= first - kKeepAround && it->first <= last + kKeepAround;
        it = keep ? std::next(it) : gpu_.erase(it);
    }
}

pdf::BitmapCache::Key PageBitmapSource::WantedKey(int page, double wDip, double hDip,
                                                  const UIContext& ctx) const {
    const double s = ctx.dpiScale > 0 ? ctx.dpiScale : 1.0;
    double w = std::round(wDip * s);
    double h = std::round(hDip * s);
    // The device's texture limit can be below FitRenderSize's 16384 (8192 on
    // feature level 10, less on WARP). Past it CreateBitmap fails, so shrink to
    // fit and let the bitmap stretch instead of showing an error.
    if (ID2D1DeviceContext* d2d = ctx.window ? ctx.window->D2D().DC() : nullptr) {
        const double cap = static_cast<double>(d2d->GetMaximumBitmapSize());
        if (cap > 0 && (w > cap || h > cap)) {
            const double k = std::min(cap / w, cap / h);
            w = std::floor(w * k);
            h = std::floor(h * k);
        }
    }
    const pdf::SizePx px = pdf::FitRenderSize(w, h, maxPageBytes_);
    return {page, px.width, px.height, 0};
}

void PageBitmapSource::OnDeviceLost() {
    pdf::TraceF("[PDFTool.View] device lost, dropping %zu D2D bitmaps\n", gpu_.size());
    gpu_.clear();  // the CPU cache survives and refills them after restore
}

void PageBitmapSource::OnDeviceRestored() {
    // CreateBitmap fails while the device is gone; those were not page
    // failures, so give every page one more try on the new device.
    failed_.clear();
}

ID2D1Bitmap* PageBitmapSource::BitmapFor(const pdf::BitmapCache::Key& want, const UIContext& ctx) {
    const int page = want.page;
    auto it = gpu_.find(page);
    const long long gen = ContentGen(page);
    if (it != gpu_.end() && it->second.key == want && it->second.contentGen == gen)
        return it->second.bitmap.Get();

    if (want.width > 0 && want.height > 0 && !failed_.count(want)) {
        if (auto cpu = cache_.Get(want)) {
            ID2D1DeviceContext* d2d = ctx.window ? ctx.window->D2D().DC() : nullptr;
            if (d2d) {
                D2D1_BITMAP_PROPERTIES props = {};
                props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
                // Opaque (alpha 255 everywhere), so premultiplied is exact.
                props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
                props.dpiX = props.dpiY = 96.0f;
                ComPtr<ID2D1Bitmap> bmp;
                const HRESULT hr = d2d->CreateBitmap(
                    D2D1::SizeU(static_cast<UINT32>(cpu->width), static_cast<UINT32>(cpu->height)),
                    cpu->pixels.data(), static_cast<UINT32>(cpu->Stride()), props,
                    bmp.GetAddressOf());
                if (SUCCEEDED(hr)) {
                    gpu_[page] = GpuPage{want, bmp, gen};
                    return bmp.Get();
                }
                pdf::TraceF("[PDFTool.View] CreateBitmap failed hr=0x%08lX page=%d %dx%d\n",
                            static_cast<unsigned long>(hr), page, cpu->width, cpu->height);
                failed_.insert(want);
            }
        } else {
            RequestRender(want);
        }
    }
    // Stale bitmap (old zoom, DPI or content), drawn until the right one arrives.
    it = gpu_.find(page);
    return it != gpu_.end() ? it->second.bitmap.Get() : nullptr;
}

void PageBitmapSource::RequestRender(const pdf::BitmapCache::Key& key) {
    if (!engine_ || !app_ || docId_ == pdf::kNoDocument || pending_.count(key)) return;
    pending_.insert(key);
    pdf::Engine* engine = engine_;
    Application* app = app_;
    const pdf::DocumentId id = docId_;
    const int ticket = *generation_;
    std::weak_ptr<int> gen = generation_;
    std::shared_ptr<SharedViewport> vp = viewport_;
    const unsigned epoch = viewport_->epoch.load();
    const long long contentGen = ContentGen(key.page);
    PageBitmapSource* self = this;
    engine_->Post([engine, app, id, key, ticket, gen, vp, epoch, contentGen, self] {
        // Engine thread. Skip jobs made stale while queued: the page scrolled
        // well away, or zoom/DPI/document changed so this size is not wanted.
        // A skipped job still reports back so pending_ is cleared.
        std::shared_ptr<const pdf::Bitmap> out;
        bool failed = false;
        const bool wanted = vp->epoch.load() == epoch &&
                            key.page >= vp->first.load() - kKeepAround &&
                            key.page <= vp->last.load() + kKeepAround;
        if (wanted) {
            if (pdf::Document* doc = engine->Documents().Get(id)) {
                auto r = doc->RenderPage(key.page, key.width, key.height);
                if (r) {
                    out = std::make_shared<const pdf::Bitmap>(std::move(r.value()));
                } else {
                    failed = true;
                    pdf::TraceF("[PDFTool.Render] page %d %dx%d failed, err=%lu\n", key.page,
                                key.width, key.height, r.error().code);
                }
            }
        }
        app->Post([key, out = std::move(out), failed, ticket, gen, contentGen,
                   self]() mutable {
            // UI thread. Dropped if the owner is gone or shows another document.
            auto alive = gen.lock();
            if (!alive || *alive != ticket) return;
            self->OnRendered(key, std::move(out), failed, contentGen);
        });
    });
}

void PageBitmapSource::OnRendered(const pdf::BitmapCache::Key& key,
                                  std::shared_ptr<const pdf::Bitmap> bmp, bool failed,
                                  long long contentGen) {
    // Rendered before the page was edited: the pixels are outdated, and
    // pending_ for this key may already belong to the newer request.
    if (contentGen != ContentGen(key.page)) return;
    pending_.erase(key);
    if (failed) failed_.insert(key);
    // Put may refuse a bitmap bigger than the whole budget; the page then
    // stays a placeholder, which is the bounded-memory trade-off.
    if (bmp) cache_.Put(key, std::move(bmp));
    // Repaint whenever the page is on screen, including when the job was
    // skipped as stale. The skip cleared pending_ above, and only the next
    // paint can re-request the page: if the user scrolled away and back while
    // the job was queued, the page would otherwise stay blank until something
    // unrelated repainted. Off-screen results just sit in the cache.
    const bool visible = key.page >= viewport_->first && key.page <= viewport_->last;
    if (visible && onReady_) onReady_(key.page);
}
