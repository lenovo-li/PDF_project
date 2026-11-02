// BitmapCache.h — bounded LRU cache of rendered pages.
//
// The viewer keeps recently rendered pages so scrolling back does not re-run
// PDFium. A document can have thousands of pages, so the cache is bounded by
// total pixel bytes, not entry count: one 400 % page costs as much as dozens
// of thumbnails. Inserting past the budget evicts least-recently-used entries.
//
// Entries are shared_ptr<const Bitmap> so an evicted bitmap the UI is still
// drawing stays alive until the UI drops it.
//
// THREADING: not synchronized. Owned and used by one thread (the UI thread,
// after render results are marshalled back). No PDFium calls.
#pragma once

#include "Render.h"

#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>

namespace pdf {

class BitmapCache {
public:
    struct Key {
        int page = 0;
        // Pixel size covers zoom and DPI but not rotation: 0° and 180° (and
        // any turn of a square page) give the same size. Rotation is part of
        // the key so a rotated page never returns the old bitmap. Content
        // edits still need InvalidatePage.
        int width = 0;
        int height = 0;
        int rotation = 0;
        bool operator==(const Key&) const = default;
    };

    explicit BitmapCache(std::uint64_t budgetBytes) : budget_(budgetBytes) {}

    // Returns the entry and marks it most recently used, or nullptr.
    std::shared_ptr<const Bitmap> Get(const Key& key);

    // Inserts or replaces, then evicts LRU entries until within budget. A
    // bitmap larger than the whole budget is not cached (returns false) so it
    // cannot flush everything else.
    bool Put(const Key& key, std::shared_ptr<const Bitmap> bitmap);

    // Drops every size of one page (after the page is edited or rotated).
    void InvalidatePage(int page);
    // Drops everything (document closed, pages inserted/deleted/reordered).
    void Clear();

    std::uint64_t Bytes() const { return bytes_; }
    std::size_t Count() const { return map_.size(); }

private:
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept;
    };
    using Entry = std::pair<Key, std::shared_ptr<const Bitmap>>;
    using List = std::list<Entry>;  // front = most recently used

    void Erase(List::iterator it);

    std::uint64_t budget_;
    std::uint64_t bytes_ = 0;
    List lru_;
    std::unordered_map<Key, List::iterator, KeyHash> map_;
};

}  // namespace pdf
