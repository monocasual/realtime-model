#include "RealtimeValue.hpp"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace mcl;

namespace
{
/* Non-POD on purpose (heap-allocated tag field): a plain int/int struct would
let copy-assignment degrade to a trivial memcpy and hide bugs that only show
up when Document has real copy/assignment semantics, closer to a real
project Document. */

struct DummyState
{
	int         val1{0};
	int         val2{0};
	std::string tag{"init"};
};

DummyState makeState(int v)
{
	return DummyState{v, v * 10, "v" + std::to_string(v)};
}

bool isConsistent(const DummyState& s)
{
	return s.val2 == s.val1 * 10 && s.tag == "v" + std::to_string(s.val1);
}
} // namespace

TEST_CASE("RealtimeValue - initial state is visible without any edit", "[RealtimeValue]")
{
	const DummyState          initial = makeState(10);
	RealtimeValue<DummyState> rdoc(initial);

	const auto& s = rdoc.read();
	REQUIRE(s.val1 == 10);
	REQUIRE(s.val2 == 100);
	REQUIRE(s.tag == "v10");
}

TEST_CASE("RealtimeValue - a single edit is fully visible on the next read", "[RealtimeValue]")
{
	RealtimeValue<DummyState> rdoc(makeState(1));

	rdoc.write([](DummyState& d)
	{ d = makeState(42); });

	const auto& s = rdoc.read();
	REQUIRE(s.val1 == 42);
	REQUIRE(s.val2 == 420);
	REQUIRE(s.tag == "v42");
}

TEST_CASE("RealtimeValue - reading again with no new edit returns the same buffer", "[RealtimeValue]")
{
	RealtimeValue<DummyState> rdoc(makeState(1));
	rdoc.write([](DummyState& d)
	{ d = makeState(5); });

	const auto& first  = rdoc.read();
	const auto& second = rdoc.read();

	// No new edit happened between these two calls, so read() must not
	// perform a phantom swap: same values, same underlying buffer.
	REQUIRE(&first == &second);
	REQUIRE(second.val1 == 5);
}

TEST_CASE("RealtimeValue - consecutive edits with no intervening read are not lost", "[RealtimeValue]")
{
	// Regression test: edit() must resync from the last state it published,
	// not from whatever stale content happens to sit in its scratch buffer.
	// Each edit here depends on the previous one, so any dropped/stale edit
	// would show up as a wrong final value.
	RealtimeValue<DummyState> rdoc(makeState(0));

	for (int i = 0; i < 5; ++i)
		rdoc.write([](DummyState& d)
		{ d = makeState(d.val1 + 1); });

	const auto& s = rdoc.read();
	REQUIRE(s.val1 == 5);
	REQUIRE(s.val2 == 50);
	REQUIRE(s.tag == "v5");
}

TEST_CASE("RealtimeValue - edits interleaved with reads stay cumulative", "[RealtimeValue]")
{
	RealtimeValue<DummyState> rdoc(makeState(0));

	for (int i = 0; i < 5; ++i)
	{
		rdoc.write([](DummyState& d)
		{ d = makeState(d.val1 + 2); });
		const auto& s = rdoc.read();
		REQUIRE(s.val1 == (i + 1) * 2);
	}
}

TEST_CASE("RealtimeValue - concurrent read/write: no torn reads, no lost updates", "[RealtimeValue]")
{
	RealtimeValue<DummyState> rdoc(makeState(0));

	std::atomic<bool> running{true};
	std::atomic<bool> tornRead{false};
	std::atomic<bool> wentBackwards{false};
	std::atomic<int>  lastPublished{0};

	std::thread writer([&]()
	{
		int counter = 0;
		while (running.load(std::memory_order_relaxed))
		{
			++counter;
			rdoc.write([counter](DummyState& d)
			{ d = makeState(counter); });
			lastPublished.store(counter, std::memory_order_relaxed);
		}
	});

	std::thread reader([&]()
	{
		int lastSeen = 0;
		while (running.load(std::memory_order_relaxed))
		{
			const auto& s = rdoc.read();

			if (!isConsistent(s))
				tornRead.store(true);

			if (s.val1 < lastSeen)
				wentBackwards.store(true);

			lastSeen = s.val1;
		}
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	running.store(false);

	writer.join();
	reader.join();

	REQUIRE_FALSE(tornRead.load());
	REQUIRE_FALSE(wentBackwards.load());

	// Nothing is reading concurrently anymore: a final read() must reflect
	// exactly the writer's last published edit, with nothing lost.
	const auto& finalState = rdoc.read();
	REQUIRE(finalState.val1 == lastPublished.load());
}
