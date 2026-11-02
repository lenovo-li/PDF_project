#include "BitmapCache.h"

namespace pdf {

std::size_t BitmapCache::KeyHash::operator()(const Key& k) const noexcept {
    std::uint64_t h = static_cast<std::uint32_t>(k.page);
    h = h * 0x9E3779B97F4A7C15ull ^ static_cast<std::uint32_t>(k.width);
    h = h * 0x9E3779B97F4A7C15ull ^ static_cast<std::uint32_t>(k.height);
    h = h * 0x9E3779B97F4A7C15ull ^ static_cast<std::uint32_t>(k.rotation);
    return static_cast<std::size_t>(h ^ (h >> 32));
}

std::shared_ptr<const Bitmap> BitmapCache::Get(const Key& key) {
    auto it = map_.find(key);
    if (it == map_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second);
    return it->second->second;
}

bool BitmapCache::Put(const Key& key, std::shared_ptr<const Bitmap> bitmap) {
    if (!bitmap) return false;
    if (auto it = map_.find(key); it != map_.end()) Erase(it->second);
    const std::uint64_t cost = bitmap->Bytes();
    if (cost > budget_) return false;
    while (bytes_ + cost > budget_ && !lru_.empty()) Erase(std::prev(lru_.end()));
    lru_.emplace_front(key, std::move(bitmap));
    try {
        map_.emplace(key, lru_.begin());
    } catch (...) {
        // Keep list, map and byte count consistent: an orphaned list entry
        // would later be subtracted from bytes_ without ever being added.
        lru_.pop_front();
        throw;
    }
    bytes_ += cost;
    return true;
}

void BitmapCache::InvalidatePage(int page) {
    for (auto it = lru_.begin(); it != lru_.end();) {
        auto next = std::next(it);
        if (it->first.page == page) Erase(it);
        it = next;
    }
}

void BitmapCache::Clear() {
    lru_.clear();
    map_.clear();
    bytes_ = 0;
}

void BitmapCache::Erase(List::iterator it) {
    bytes_ -= it->second->Bytes();
    map_.erase(it->first);
    lru_.erase(it);
}

}  // namespace pdf
