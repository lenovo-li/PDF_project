// DocumentStore: ids, lookup after close, and the engine-owned store that
// viewer jobs use. Runs on the engine thread like production code.

#include "../framework/Test.h"
#include "../framework/TestEngine.h"

#include <PDFCore/DocumentStore.h>
#include <PDFCore/Engine.h>

#include <cstdlib>
#include <new>
#include <stdexcept>

using namespace pdf;

namespace {
// Fails the current test (rather than crashing) if setup itself fails.
Document OnePageDoc() {
    auto d = Document::CreateEmpty();
    EXPECT_TRUE(d.ok());
    if (!d) std::abort();  // nothing below can run without a document
    EXPECT_TRUE(d.value().AddBlankPage(100, 100).ok());
    return std::move(d.value());
}
}  // namespace

TEST(DocumentStore, AddGetClose) {
    pdftest::OnEngine([] {
        DocumentStore store;
        const DocumentId a = store.Add(OnePageDoc());
        const DocumentId b = store.Add(OnePageDoc());
        EXPECT_NE(a, kNoDocument);
        EXPECT_NE(a, b);
        EXPECT_TRUE(store.Get(a) != nullptr);
        if (Document* d = store.Get(a)) EXPECT_EQ(d->PageCount(), 1);
        store.Close(a);
        EXPECT_TRUE(store.Get(a) == nullptr);  // a late job for a closed doc
        EXPECT_TRUE(store.Get(b) != nullptr);
        store.Close(a);                         // double close is harmless
        EXPECT_TRUE(store.Get(kNoDocument) == nullptr);
        EXPECT_EQ(store.Count(), static_cast<std::size_t>(1));
    });
}

TEST(DocumentStore, IdsAreNotReusedAfterClose) {
    pdftest::OnEngine([] {
        DocumentStore store;
        const DocumentId a = store.Add(OnePageDoc());
        store.Close(a);
        const DocumentId b = store.Add(OnePageDoc());
        EXPECT_NE(a, b);  // a stale job holding `a` must not find the new doc
        EXPECT_TRUE(store.Get(a) == nullptr);
    });
}

TEST(DocumentStore, EngineStoreSurvivesAcrossJobs) {
    // Uses the shared engine: a second Engine would re-init and then destroy
    // PDFium's global state under the shared one. The document is closed at
    // the end so it does not leak into other tests.
    Engine& engine = pdftest::SharedEngine();
    DocumentId id = kNoDocument;
    engine.RunSync([&] { id = engine.Documents().Add(OnePageDoc()); });
    int pages = -1;
    engine.RunSync([&] {
        if (Document* d = engine.Documents().Get(id)) pages = d->PageCount();
        engine.Documents().Close(id);
    });
    EXPECT_EQ(pages, 1);
    bool gone = false;
    engine.RunSync([&] { gone = engine.Documents().Get(id) == nullptr; });
    EXPECT_TRUE(gone);
}

// A throwing job must not take the process down (std::terminate) nor stop
// the engine: later jobs, posted or synchronous, still run.
TEST(DocumentStore, EngineSurvivesAThrowingJob) {
    Engine& engine = pdftest::SharedEngine();
    engine.Post([] { throw std::runtime_error("posted"); });
    EXPECT_FALSE(engine.RunSync([] { throw std::bad_alloc(); }));
    EXPECT_FALSE(engine.RunSync([] { throw 42; }));
    int ran = 0;
    EXPECT_TRUE(engine.RunSync([&] { ++ran; }));
    EXPECT_EQ(ran, 1);
}
