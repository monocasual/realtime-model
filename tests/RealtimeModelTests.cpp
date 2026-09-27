#include "RealtimeModel.hpp"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

namespace
{
/* Document only ever holds an asset ID, never the asset itself - mirrors
the real design so the ordering test below actually means something. */
struct DummyDocument
{
    int trackCount{0};
    int referencedAssetId{-1}; // -1 = none
};

/* Assets maps an ID to a heap-allocated resource via shared_ptr, standing
in for a real audio file/plugin. */
struct DummyAssets
{
    std::unordered_map<int, std::shared_ptr<std::string>> files;
};

struct DummyParameters
{
    std::atomic<int> playhead{0};
};

using Model = RealtimeModel<DummyDocument, DummyAssets, DummyParameters>;

/* waitUntil()
Helper function to wait until something happens before passing it to
the REQUIRE macro.
Writes land asynchronously on the writer thread, so tests can't assert
immediately after calling write[...](), they need to wait for the condition
to become true, same as the realtime thread would poll via read(). */
template <typename Predicate>
bool waitUntil(Predicate&& pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(500))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
} // namespace

TEST_CASE("RealtimeModel - initial state is visible without any write", "[RealtimeModel]")
{
    Model model;

    const auto lock = model.read();
    REQUIRE(lock.getDocument().trackCount == 0);
    REQUIRE(lock.getAssets().files.empty());
}

TEST_CASE("RealtimeModel - writeDocument() is applied and visible", "[RealtimeModel]")
{
    Model model;
    model.start();

    model.writeDocument([](DummyDocument& d) { d.trackCount = 3; });

    REQUIRE(waitUntil([&] { return model.read().getDocument().trackCount == 3; }));

    model.stop();
}

TEST_CASE("RealtimeModel - writeAssets() is applied and visible", "[RealtimeModel]")
{
    Model model;
    model.start();

    model.writeAssets([](DummyAssets& a) { a.files[7] = std::make_shared<std::string>("hello"); });

    REQUIRE(waitUntil([&] { return model.read().getAssets().files.count(7) == 1; }));

    const auto lock = model.read();
    REQUIRE(*lock.getAssets().files.at(7) == "hello");

    model.stop();
}

TEST_CASE("RealtimeModel - writeDocumentAndAssets() applies both together", "[RealtimeModel]")
{
    Model model;
    model.start();

    model.writeDocumentAndAssets([](DummyDocument& d, DummyAssets& a) {
        a.files[42] = std::make_shared<std::string>("sample.wav");
        d.referencedAssetId = 42;
    });

    REQUIRE(waitUntil([&] { return model.read().getDocument().referencedAssetId == 42; }));

    // If the document has picked up the reference, the asset it points to
    // must already be there too.
    const auto lock = model.read();
    REQUIRE(lock.getAssets().files.count(42) == 1);

    model.stop();
}

TEST_CASE("RealtimeModel - consecutive writes with no intervening read are not lost", "[RealtimeModel]")
{
    // Make sure multiple calls to writeDocument are all stored correctly
	// and nothing is lost in the way.
    Model model;
    model.start();

    for (int i = 0; i < 10; ++i)
        model.writeDocument([](DummyDocument& d) { d.trackCount += 1; });

    REQUIRE(waitUntil([&] { return model.read().getDocument().trackCount == 10; }));

    model.stop();
}


TEST_CASE("RealtimeModel - load() replaces both Document and Assets", "[RealtimeModel]")
{
	// Reproduce a "load project from disk" process.
    Model model;
    model.start();

    model.writeDocumentAndAssets([](DummyDocument& d, DummyAssets& a) {
        a.files[1] = std::make_shared<std::string>("old");
        d.referencedAssetId = 1;
        d.trackCount        = 5;
    });
    REQUIRE(waitUntil([&] { return model.read().getDocument().trackCount == 5; }));

    DummyDocument newDoc;
    newDoc.trackCount        = 1;
    newDoc.referencedAssetId = 99;

    DummyAssets newAssets;
    newAssets.files[99] = std::make_shared<std::string>("new");

    model.load(std::move(newDoc), std::move(newAssets));

    REQUIRE(waitUntil([&] { return model.read().getDocument().trackCount == 1; }));

    const auto lock = model.read();
    REQUIRE(lock.getDocument().referencedAssetId == 99);
    REQUIRE(lock.getAssets().files.count(99) == 1);
    REQUIRE(lock.getAssets().files.count(1) == 0); // old asset discarded, not merged in

    model.stop();
}


TEST_CASE("RealtimeModel - getParameters() and RealtimeReadLock refer to the same object", "[RealtimeModel]")
{
    Model model;

    model.getParameters().playhead.store(123, std::memory_order_relaxed);

    const auto lock = model.read();
    REQUIRE(lock.getParameters().playhead.load(std::memory_order_relaxed) == 123);

    // Mutate through the read lock, as the realtime thread would, and
    // confirm it's visible through getParameters() too - same object.
    lock.getParameters().playhead.store(456, std::memory_order_relaxed);
    REQUIRE(model.getParameters().playhead.load(std::memory_order_relaxed) == 456);
}
