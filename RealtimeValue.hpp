#pragma once

#include <array>
#include <atomic>

/* RealtimeValue
This class lets one thread read a value and another thread update it,
with neither one ever blocking on the other, and neither one ever touching
memory the other is using at the same instant.

How it works
------------
Instead of sharing one value and protecting it with a lock, this class
keeps 3 separate copies in a small pool. At any moment, each copy is
"owned" by one of three roles:
  - one copy is being read by the Audio thread
  - one copy is being edited by the Writer thread
  - one copy is just sitting in the middle, waiting to be picked up

A change of ownership is just an index swap (an atomic exchange), not a
copy of the actual value data. That's what makes this safe without
locks: the two threads are always looking at different array slots, so
they can never race on the same memory and handing off a new version is
as cheap as swapping two integers.

The "waiting in the middle" slot also carries one extra bit: whether it
holds content the Audio thread hasn't picked up yet. That bit lives
packed into the same atomic as the index, not as a separate flag: if it
were separate, the Writer could publish twice while the Audio thread was
partway through consuming the first signal, leaving a leftover "there's
something new" bit even though the Audio thread's next swap would just
get back its own old buffer. Packing them together makes "check if
there's something new" and "take it" a single indivisible step, so that
can't happen.

Usage notes
-----------
- read() must only ever be called from the single Real-Time Audio thread;
- edit() must only ever be called from the single Writer thread;
- This is a single-reader/single-writer design: it does not support
  multiple concurrent readers or multiple concurrent writers;
- The reference returned by read() is only valid until the next call to
  read(). Don't hold on to it across multiple calls. */

template <typename T>
class RealtimeValue final
{
	static_assert(
	    std::is_copy_constructible_v<T> &&
	        std::is_copy_assignable_v<T>,
	    "RealtimeValue<T> requires T to be copyable: it keeps 3 buffered "
	    "copies and republishes by full copy-assignment.");

public:
	explicit RealtimeValue(const T& v)
	: m_values{v, v, v}
	, m_realtimeIdx(0)
	, m_writerIdx(1)
	, m_sharedSlot(0b010)   // index 2, no pending update yet
	, m_lastPublishedIdx(1) // same as m_writerIdx: nothing published yet
	{
	}

	/* read()
	Call ONLY from the Real-Time Audio thread. Returns the current value.
	If the Writer thread has published a newer one since our last call, grab it
	first. */

	const T& read()
	{
		// Peek at the shared slot: is there something we haven't picked up yet?
		const std::uint32_t current = m_sharedSlot.load(std::memory_order_acquire);

		if (current & PENDING_BIT)
		{
			// Yes. Swap it out: we hand back our own buffer (marked as
			// "nothing pending", since we're not the Writer), and take
			// whatever's there now in one atomic step, so there's no
			// gap where a second publish could leave a leftover signal.
			const std::uint32_t old = m_sharedSlot.exchange(m_realtimeIdx, std::memory_order_acq_rel);
			m_realtimeIdx           = old & INDEX_MASK;
		}

		return m_values[m_realtimeIdx];
	}

	/* edit()
	Call ONLY from the single Writer thread. Applies f to the value and
	publishes the result. */

	template <typename Edit>
	void edit(Edit&& f)
	{
		// Our scratch buffer might be old (if the Audio thread hasn't
		// picked up our last update yet), so bring it up to date first.
		m_values[m_writerIdx] = m_values[m_lastPublishedIdx];

		// Now apply the change on top of the up-to-date copy.
		f(m_values[m_writerIdx]);

		// Remember this as the latest version we've produced.
		m_lastPublishedIdx = m_writerIdx;

		// Publish it: swap our buffer with the shared one, tagged as
		// "pending". Whatever comes back is guaranteed to be free (the
		// Audio thread can't be using it), regardless of its own tag.
		const std::uint32_t old = m_sharedSlot.exchange(m_writerIdx | PENDING_BIT, std::memory_order_acq_rel);
		m_writerIdx             = old & INDEX_MASK;
	}

private:
	static constexpr std::uint32_t PENDING_BIT = 0b100; // bit 2: "not yet picked up"
	static constexpr std::uint32_t INDEX_MASK  = 0b011; // bits 0-1: which buffer (0-2)

	std::array<T, 3> m_values;

	// Which buffer the Audio thread is currently reading (its own, private index)
	int m_realtimeIdx;

	// Which buffer the Writer thread is currently editing (its own, private index)
	int m_writerIdx;

	// The buffer currently "in transit" between the two threads, packed
	// together with whether it's still unread (see class comment above)
	std::atomic<std::uint32_t> m_sharedSlot;

	// Which buffer holds the latest version the Writer thread itself produced
	// (only the Writer thread touches this, so it needs no synchronization)
	int m_lastPublishedIdx;
};
