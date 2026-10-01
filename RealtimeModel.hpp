#pragma once

#include "ChangeNotifier.hpp"
#include "RealtimeValue.hpp"
#include "Writer.hpp"
#include <functional>
#include <utility>

/* RealtimeModel
Owns all shared state for the audio engine: Document (project structure),
Assets (audio files, plugins), and Parameters (things like the playhead).
It is the only way any thread should touch them. Nobody outside this class
holds a copy or a reference to that state directly.

Two threads use this differently:
  - The Real-Time Audio thread calls read() once per callback to get a
    snapshot, and getParameters() (via that snapshot) to update things
    like the playhead directly. It never blocks and never waits on anyone.
  - Every other thread (GUI, MIDI, workers) calls writeDocument(),
    writeAssets(), or writeDocumentAndAssets() to request a change. These
    changes are queued and applied, one at a time, on a single dedicated
    writer thread. Never directly and never on the calling thread.

Document and Assets are kept as two separate pieces because most edits
only touch one of them, and Document only ever refers to Assets by ID,
never the actual audio data. That split is why there are three write
methods instead of one: use whichever matches what you're actually
changing, so you're not paying to update Assets just to rename a track.

The GUI never gets something it could edit by accident: assets come back as
read-only pointers, so the only way to change one is through writeAssets(). To
find out something changed, the GUI checks documentChanges (usually from its
existing timer) by calling take(), which says what kind of change happened and
hands over the latest snapshot. current() is for when you just need today's
state right now, changed or not, like opening a Sample Editor window. See
ChangeNotifier.hpp for why it works this way (pull, not push).

Expectations:
  - read() is for the Real-Time Audio thread only, once per callback.
    Don't call it from anywhere else and don't hold on to what it
    returns past that one callback;
  - writeDocument()/writeAssets()/writeDocumentAndAssets() are for every
    other thread. Never call these from the Real-Time Audio thread.
    They queue work, they don't apply it immediately;
  - When adding a new asset that Document will reference, always use
    writeDocumentAndAssets() (or load(), for loading a whole project);
  - The Assets class must make use of the RealtimeAssetMap to hold its data,
    see the RealtimeAssetMap.hpp header for more information.  */

namespace mcl
{
template <typename Document, typename Assets, typename Parameters>
class RealtimeModel
{
public:
	/* RealtimeReadLock
	A short-lived, per-callback bundle of the current Document, Assets, and
	Parameters. Document/Assets are snapshots valid until the next call to
	read(): don't hold on to them past that. Parameters is a live reference to
	the one instance, always current. */

	class RealtimeReadLock
	{
	public:
		RealtimeReadLock(const Document& d, const Assets& a, const Parameters& p)
		: m_document(d)
		, m_assets(a)
		, m_parameters(p)
		{
		}

		const Document&   getDocument() const { return m_document; }
		const Assets&     getAssets() const { return m_assets; }
		const Parameters& getParameters() const { return m_parameters; }

	private:
		const Document&   m_document;
		const Assets&     m_assets;
		const Parameters& m_parameters;
	};

	/* RealtimeModel */

	RealtimeModel()
	: m_document(Document{})
	, m_assets(Assets{})
	{
		m_writer.start();
	}

	~RealtimeModel() { m_writer.stop(); }

	RealtimeModel(const RealtimeModel&)            = delete;
	RealtimeModel& operator=(const RealtimeModel&) = delete;

	/* writeDocument()
	Call from any non-realtime thread (GUI, MIDI, workers), for edits that
	only touch Document (new channels, mute, reorder, ...). The 'type' parameter
	tells the GUI, via documentChanges, whether it needs to rebuild (HARD) or just
	refresh (SOFT); use NONE if the GUI doesn't need to know at all. */

	void writeDocument(SwapType type, std::function<void(Document&)> f)
	{
		m_writer.push([this, type, f = std::move(f)]()
		{
			m_document.write([&](Document& d)
			{ f(d); });
			documentChanges.notify(type, m_document.getLastPublished());
		});
	}

	/* writeAssets()
	Call from any thread non-realtime thread (GUI, MIDI, workers), for edits
	that only touch Assets (load/unload a file, swap a plugin instance, ...)
	without changing the Document. */

	void writeAssets(std::function<void(Assets&)> f)
	{
		m_writer.push([this, f = std::move(f)]()
		{
			m_assets.write([&](Assets& a)
			{ f(a); });
		});
	}

	/* writeDocumentAndAssets()
	Call from any thread non-realtime thread (GUI, MIDI, workers), for the rare case
	of adding an asset AND changing the Document in one transaction. The method
	guarantees Assets publishes before Document, so a Document update that references
	a new asset can never become visible before that asset does. read() mirrors this
	by reading Document before Assets. */

	void writeDocumentAndAssets(SwapType type, std::function<void(Document&, Assets&)> f)
	{
		m_writer.push([this, type, f = std::move(f)]()
		{
			m_document.write([&](Document& d)
			{
				m_assets.write([&](Assets& a)
				{ f(d, a); });
			});
			documentChanges.notify(type, m_document.getLastPublished());
		});
	}

	/* read()
	Call ONLY from the Real-Time Audio thread. Document is read before Assets
	(see writeDocumentAndAssets() comment). */

	RealtimeReadLock read()
	{
		const Document& d = m_document.read();
		const Assets&   a = m_assets.read();
		return RealtimeReadLock(d, a, m_parameters);
	}

	/* load()
	Helper function for loading a new document + asset combo, used when
	you need to load new data e.g. read from disk. Always a HARD change. */

	void load(Document&& document, Assets&& assets)
	{
		writeDocumentAndAssets(SwapType::HARD,
		    [d = std::move(document), a = std::move(assets)](Document& doc, Assets& ass) mutable
		{
			ass = std::move(a);
			doc = std::move(d);
		});
	}

	/* getParameters()
	Direct and unqueued access to Parameters for any non-realtime thread
	(GUI, MIDI, ...). Same object RealtimeReadLock::getParameters() refers to. */

	const Parameters& getParameters() const { return m_parameters; }

	/* ChangeNotifier
	The GUI's notification channel. See ChangeNotifier.hpp comments. */

	ChangeNotifier<Document> documentChanges;

private:
	RealtimeValue<Document> m_document;
	RealtimeValue<Assets>   m_assets;
	Parameters              m_parameters;
	Writer                  m_writer;
};
} // namespace mcl