#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace mcl
{
template <typename Document, typename Assets, typename Parameters>
class RealtimeModel;

/* SwapType
Type of Document/Assets change, decided by whoever calls a write method:
    HARD: the structure changed (e.g. a new channel) - the GUI must rebuild;
    SOFT: a property changed (e.g. volume) - the GUI can just refresh;
    NONE: something changed but the GUI doesn't need to know. */
enum class SwapType
{
	HARD,
	SOFT,
	NONE
};

/* ChangeNotifier
Tells the GUI that something changed. The Writer thread calls notify() after
publishing a change; the GUI thread later calls take() (typically from an existing
polling timer) to pull the latest snapshot. The current() method is the separate
case of "give me the state right now", regardless of whether anything changed
since the last take(). */

template <typename T>
class ChangeNotifier
{
	template <typename Document, typename Assets, typename Parameters>
	friend class RealtimeModel;

public:
	struct Update
	{
		SwapType                 type{SwapType::NONE};
		std::shared_ptr<const T> value;
	};

	/* Call ONLY from the GUI thread. Returns what changed since the last
	   take(), or a default Update (type == NONE, value == nullptr) if
	   nothing did. */
	Update take()
	{
		const std::uint8_t bits = m_bits.exchange(0, std::memory_order_acquire);
		if (bits == 0)
			return {}; // nothing changed: no lock taken

		std::lock_guard lock(m_mutex);
		return {(bits & HARD) ? SwapType::HARD : SwapType::SOFT, m_snapshot};
	}

	/* getCurrent
	Call from any non-realtime thread. Returns the latest published snapshot
	regardless of whether it's already been seen via take(). Returns nullptr if
	notify() has never been called. */

	std::shared_ptr<const T> getCurrent() const
	{
		std::lock_guard lock(m_mutex);
		return m_snapshot;
	}

private:
	/* notify()
	Call ONLY from the Writer thread. */

	void notify(SwapType type, const T& latest)
	{
		if (type == SwapType::NONE)
			return;
		{
			// Snapshot first, bits second: a GUI thread that sees the bits
			// is guaranteed to see a snapshot at least as new.
			std::lock_guard lock(m_mutex);
			m_snapshot = std::make_shared<const T>(latest);
		}
		m_bits.fetch_or(type == SwapType::HARD ? HARD : SOFT, std::memory_order_release);
	}

	static constexpr std::uint8_t SOFT = 1;
	static constexpr std::uint8_t HARD = 2;

	std::atomic<std::uint8_t> m_bits{0};
	mutable std::mutex        m_mutex;
	std::shared_ptr<const T>  m_snapshot;
};
} // namespace mcl