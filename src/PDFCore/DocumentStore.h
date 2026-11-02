// DocumentStore.h — the open documents, owned by the engine thread.
//
// A Document must be created, used and destroyed on the engine thread (its
// destructor calls FPDF_CloseDocument). If the UI held a Document, or a
// shared_ptr to one, the last reference could drop on the UI thread. So the UI
// holds only a DocumentId; jobs posted to the engine look the document up here
// with Engine::Documents(). A job for an id that was closed meanwhile simply
// gets nullptr and reports "document closed" instead of touching freed memory.
//
// THREADING: engine thread only, including destruction (the Engine destroys
// its store on the engine thread before FPDF_DestroyLibrary).
#pragma once

#include "Document.h"

#include <cstdint>
#include <unordered_map>

namespace pdf {

using DocumentId = std::uint64_t;
constexpr DocumentId kNoDocument = 0;

class DocumentStore {
public:
    // Takes ownership; returns a new id, never reused within the process.
    DocumentId Add(Document doc);
    // nullptr if `id` was never added or has been closed.
    Document* Get(DocumentId id);
    // Closes the document. Unknown ids are ignored, so a double close is safe.
    void Close(DocumentId id);
    std::size_t Count() const { return docs_.size(); }

private:
    DocumentId next_ = 1;
    std::unordered_map<DocumentId, Document> docs_;
};

}  // namespace pdf
