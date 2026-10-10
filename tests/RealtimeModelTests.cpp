#include "RealtimeModel.hpp"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

using namespace mcl;

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
public:
	int  getPlayhead() const { return playhead.load(std::memory_order_relaxed); }
	void setPlayhead(int v) const { playhead.store(v, std::memory_order_relaxed); }

private:
	mutable std::atomic<int> playhead{0};
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

/* waitForUpdate()
Polls take() until it reports a change, like the GUI timer would, and returns
that change. Returns NONE if nothing arrives in time.
Use this instead of watching getCurrent(): the snapshot is saved BEFORE the
change flag is set, so seeing the new snapshot doesn't guarantee take() will
report it yet. */
template <typename Notifier>
SwapType waitForUpdate(Notifier& notifier, std::chrono::milliseconds timeout = std::chrono::milliseconds(500))
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline)
	{
		auto update = notifier.take();
		if (update != SwapType::NONE)
			return update;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return SwapType::NONE;
}

/* flushWriter()
Blocks until every write issued so far has been completely applied, including
its notify() call. Writes run in order on a single writer thread, so once this
marker write is visible, every earlier write has fully finished. Needed when a
test must call take() only after several writes are done, and so can't poll it
(polling would consume the Update). */
bool flushWriter(Model& model)
{
	static std::atomic<int> nextKey{-1};
	const int               key = nextKey.fetch_sub(1);

	model.writeAssets(SwapType::NONE, [key](DummyAssets& a)
	{ a.files[key] = std::make_shared<std::string>("flush"); });

	return waitUntil([&]
	{ return model.read().getAssets().files.count(key) == 1; });
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

	model.writeDocument(SwapType::HARD, [](DummyDocument& d)
	{ d.trackCount = 3; });

	REQUIRE(waitUntil([&]
	{ return model.read().getDocument().trackCount == 3; }));
}

TEST_CASE("RealtimeModel - writeAssets() is applied and visible", "[RealtimeModel]")
{
	Model model;

	model.writeAssets(SwapType::NONE, [](DummyAssets& a)
	{ a.files[7] = std::make_shared<std::string>("hello"); });

	REQUIRE(waitUntil([&]
	{ return model.read().getAssets().files.count(7) == 1; }));

	const auto lock = model.read();
	REQUIRE(*lock.getAssets().files.at(7) == "hello");
}

TEST_CASE("RealtimeModel - writeDocumentAndAssets() applies both together", "[RealtimeModel]")
{
	Model model;

	model.writeDocumentAndAssets(SwapType::HARD, [](DummyDocument& d, DummyAssets& a)
	{
		a.files[42]         = std::make_shared<std::string>("sample.wav");
		d.referencedAssetId = 42;
	});

	REQUIRE(waitUntil([&]
	{ return model.read().getDocument().referencedAssetId == 42; }));

	// If the document has picked up the reference, the asset it points to
	// must already be there too.
	const auto lock = model.read();
	REQUIRE(lock.getAssets().files.count(42) == 1);
}

TEST_CASE("RealtimeModel - consecutive writes with no intervening read are not lost", "[RealtimeModel]")
{
	// Make sure multiple calls to writeDocument are all stored correctly
	// and nothing is lost in the way.
	Model model;

	for (int i = 0; i < 10; ++i)
		model.writeDocument(SwapType::HARD, [](DummyDocument& d)
		{ d.trackCount += 1; });

	REQUIRE(waitUntil([&]
	{ return model.read().getDocument().trackCount == 10; }));
}

TEST_CASE("RealtimeModel - load() replaces both Document and Assets", "[RealtimeModel]")
{
	// Reproduce a "load project from disk" process.
	Model model;

	model.writeDocumentAndAssets(SwapType::HARD, [](DummyDocument& d, DummyAssets& a)
	{
		a.files[1]          = std::make_shared<std::string>("old");
		d.referencedAssetId = 1;
		d.trackCount        = 5;
	});
	REQUIRE(waitUntil([&]
	{ return model.read().getDocument().trackCount == 5; }));

	DummyDocument newDoc;
	newDoc.trackCount        = 1;
	newDoc.referencedAssetId = 99;

	DummyAssets newAssets;
	newAssets.files[99] = std::make_shared<std::string>("new");

	model.load(std::move(newDoc), std::move(newAssets));

	REQUIRE(waitUntil([&]
	{ return model.read().getDocument().trackCount == 1; }));

	const auto lock = model.read();
	REQUIRE(lock.getDocument().referencedAssetId == 99);
	REQUIRE(lock.getAssets().files.count(99) == 1);
	REQUIRE(lock.getAssets().files.count(1) == 0); // old asset discarded, not merged in
}

TEST_CASE("RealtimeModel - getParameters() and RealtimeReadLock refer to the same object", "[RealtimeModel]")
{
	Model model;

	model.getParameters().setPlayhead(123);

	const auto lock = model.read();
	REQUIRE(lock.getParameters().getPlayhead() == 123);

	// Mutate through the read lock, as the realtime thread would, and
	// confirm it's visible through getParameters() too - same object.
	lock.getParameters().setPlayhead(456);
	REQUIRE(model.getParameters().getPlayhead() == 456);
}

TEST_CASE("RealtimeModel - change notifiers start seeded, with nothing pending", "[RealtimeModel][ChangeNotifier]")
{
	Model model;

	// Nothing has changed yet, so take() reports nothing...
	const auto docUpdate = model.documentChanges.take();
	REQUIRE(docUpdate == SwapType::NONE);

	const auto assetsUpdate = model.assetsChanges.take();
	REQUIRE(assetsUpdate == SwapType::NONE);

	// ...but getCurrent() already has a snapshot, with default contents.
	const auto currentDoc = model.documentChanges.getCurrent();
	REQUIRE(currentDoc != nullptr);
	REQUIRE(currentDoc->trackCount == 0);
	REQUIRE(currentDoc->referencedAssetId == -1);

	const auto currentAssets = model.assetsChanges.getCurrent();
	REQUIRE(currentAssets != nullptr);
	REQUIRE(currentAssets->files.empty());
}

TEST_CASE("RealtimeModel - documentChanges reports HARD after a HARD write, then clears", "[RealtimeModel][ChangeNotifier]")
{
	Model model;

	model.writeDocument(SwapType::HARD, [](DummyDocument& d)
	{ d.trackCount = 1; });

	const auto update = waitForUpdate(model.documentChanges);
	REQUIRE(update == SwapType::HARD);
	REQUIRE(model.documentChanges.getCurrent()->trackCount == 1);

	// take() consumes what it reports: a second call with no new write
	// in between must come back empty.
	const auto second = model.documentChanges.take();
	REQUIRE(second == SwapType::NONE);
}

TEST_CASE("RealtimeModel - documentChanges reports SOFT after a SOFT write", "[RealtimeModel][ChangeNotifier]")
{
	Model model;

	model.writeDocument(SwapType::SOFT, [](DummyDocument& d)
	{ d.trackCount = 1; });

	const auto update = waitForUpdate(model.documentChanges);
	REQUIRE(update == SwapType::SOFT);
	REQUIRE(model.documentChanges.getCurrent()->trackCount == 1);
}

TEST_CASE("RealtimeModel - a SwapType::NONE write applies but never notifies", "[RealtimeModel][ChangeNotifier]")
{
	Model model;

	model.writeDocument(SwapType::NONE, [](DummyDocument& d)
	{ d.trackCount = 7; });

	REQUIRE(waitUntil([&]
	{ return model.read().getDocument().trackCount == 7; }));

	// The write landed, but since nobody needs to know, take() stays empty.
	const auto update = model.documentChanges.take();
	REQUIRE(update == SwapType::NONE);

	// With the current notify(), a NONE write doesn't refresh the snapshot
	// either: getCurrent() still shows the seeded default, not 7.
	REQUIRE(model.documentChanges.getCurrent()->trackCount == 0);
}

TEST_CASE("RealtimeModel - multiple writes between polls collapse into one Update, HARD wins", "[RealtimeModel][ChangeNotifier]")
{
	Model model;

	model.writeDocument(SwapType::SOFT, [](DummyDocument& d)
	{ d.trackCount = 1; });
	model.writeDocument(SwapType::HARD, [](DummyDocument& d)
	{ d.trackCount = 2; });

	// Wait until both writes, and both of their notify() calls, are done.
	REQUIRE(flushWriter(model));

	const auto update = model.documentChanges.take();
	REQUIRE(update == SwapType::HARD); // more severe of SOFT + HARD
	REQUIRE(model.documentChanges.getCurrent()->trackCount == 2); // the newest snapshot, not the SOFT one
}

TEST_CASE("RealtimeModel - documentChanges.getCurrent() always reflects the latest state", "[RealtimeModel][ChangeNotifier]")
{
	Model model;

	model.writeDocument(SwapType::SOFT, [](DummyDocument& d)
	{ d.trackCount = 5; });
	REQUIRE(waitUntil([&]
	{ return model.documentChanges.getCurrent()->trackCount == 5; }));

	// getCurrent() doesn't consume anything, so polling it repeatedly
	// without calling take() must keep tracking new writes.
	model.writeDocument(SwapType::SOFT, [](DummyDocument& d)
	{ d.trackCount = 6; });
	REQUIRE(waitUntil([&]
	{ return model.documentChanges.getCurrent()->trackCount == 6; }));

	const auto update = model.documentChanges.take();
	REQUIRE(update == SwapType::SOFT);
	REQUIRE(model.documentChanges.getCurrent()->trackCount == 6);
}

TEST_CASE("RealtimeModel - load() is reported to documentChanges as HARD", "[RealtimeModel][ChangeNotifier]")
{
	Model model;

	DummyDocument newDoc;
	newDoc.trackCount = 9;

	model.load(std::move(newDoc), DummyAssets{});

	const auto update = waitForUpdate(model.documentChanges);
	REQUIRE(update == SwapType::HARD);
	REQUIRE(model.documentChanges.getCurrent()->trackCount == 9);
}

TEST_CASE("RealtimeModel - assetsChanges reports a writeAssets() change, documentChanges stays quiet", "[RealtimeModel][ChangeNotifier]")
{
	Model model;

	model.writeAssets(SwapType::SOFT, [](DummyAssets& a)
	{ a.files[5] = std::make_shared<std::string>("kick.wav"); });

	const auto update = waitForUpdate(model.assetsChanges);
	REQUIRE(update == SwapType::SOFT);
	REQUIRE(model.assetsChanges.getCurrent()->files.count(5) == 1);

	// writeAssets() never touches the Document, so nothing to report there.
	REQUIRE(model.documentChanges.take() == SwapType::NONE);
}

TEST_CASE("RealtimeModel - writeDocumentAndAssets() notifies both documentChanges and assetsChanges", "[RealtimeModel][ChangeNotifier]")
{
	Model model;

	model.writeDocumentAndAssets(SwapType::HARD, [](DummyDocument& d, DummyAssets& a)
	{
		a.files[42]         = std::make_shared<std::string>("sample.wav");
		d.referencedAssetId = 42;
	});

	const auto docUpdate = waitForUpdate(model.documentChanges);
	REQUIRE(docUpdate == SwapType::HARD);
	REQUIRE(model.documentChanges.getCurrent()->referencedAssetId == 42);

	const auto assetsUpdate = waitForUpdate(model.assetsChanges);
	REQUIRE(assetsUpdate == SwapType::HARD);
	REQUIRE(model.assetsChanges.getCurrent()->files.count(42) == 1);
}

TEST_CASE("RealtimeModel - getCurrentDocument() and getCurrentAssets() work from the start and follow writes", "[RealtimeModel]")
{
	Model model;

	REQUIRE(model.getCurrentDocument() != nullptr);
	REQUIRE(model.getCurrentAssets() != nullptr);
	REQUIRE(model.getCurrentDocument()->trackCount == 0);

	model.writeDocument(SwapType::SOFT, [](DummyDocument& d)
	{ d.trackCount = 4; });

	REQUIRE(waitUntil([&]
	{ return model.getCurrentDocument()->trackCount == 4; }));
}
