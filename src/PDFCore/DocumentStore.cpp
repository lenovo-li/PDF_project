#include "DocumentStore.h"

namespace pdf {

DocumentId DocumentStore::Add(Document doc) {
    const DocumentId id = next_++;
    docs_.emplace(id, std::move(doc));
    return id;
}

Document* DocumentStore::Get(DocumentId id) {
    auto it = docs_.find(id);
    return it == docs_.end() ? nullptr : &it->second;
}

void DocumentStore::Close(DocumentId id) { docs_.erase(id); }

}  // namespace pdf
