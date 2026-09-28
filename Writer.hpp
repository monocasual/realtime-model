#pragma once

#include <atomic>
#include <blockingconcurrentqueue.h>
#include <chrono>
#include <functional>
#include <thread>
#include <utility>

/* Writer
Runs queued commands, in order, on a single dedicated thread. Knows nothing
about what the commands do or what data they touch, that's entirely up to
whoever builds and pushes them. */

namespace mcl
{
class Writer final
{
public:
	using Command = std::function<void()>;

	Writer() = default;
	~Writer() { stop(); }

	Writer(const Writer&)            = delete;
	Writer& operator=(const Writer&) = delete;

	void start()
	{
		if (m_running.exchange(true))
			return;

		m_thread = std::thread(&Writer::runLoop, this);
	}

	void stop()
	{
		if (!m_running.exchange(false))
			return;

		if (m_thread.joinable())
			m_thread.join();

		// Process anything left over after the loop has exited.
		drainQueue();
	}

	/* push
	   Call from ANY thread (GUI, MIDI, Workers). */
	void push(Command cmd)
	{
		if (!cmd)
			return;

		m_queue.enqueue(std::move(cmd));
	}

private:
	void runLoop()
	{
		Command cmd;

		while (m_running.load(std::memory_order_relaxed))
		{
			/* Waits here until something is pushed. If nothing comes, it gives
			up after 50ms just to check whether it should stop, then goes back
			to waiting. A real push wakes it up right away, the 50ms never delays
			real work, it only limits how long shutdown might take if the queue
			happens to be empty. */
			if (m_queue.wait_dequeue_timed(cmd, std::chrono::milliseconds(50)))
				cmd();
		}
	}

	void drainQueue()
	{
		Command cmd;
		while (m_queue.try_dequeue(cmd))
		{
			cmd();
		}
	}

	moodycamel::BlockingConcurrentQueue<Command> m_queue;

	std::thread       m_thread;
	std::atomic<bool> m_running{false};
};
} // namespace mcl
