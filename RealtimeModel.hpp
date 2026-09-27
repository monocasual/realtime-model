#pragma once

#include "RealtimeValue.hpp"
#include "Writer.hpp"
#include <functional>
#include <utility>

/* RealtimeModel
The user-facing API, and the single source of truth: Document, Assets, and
Parameters all live inside RealtimeModel, owned by value. There's nothing to
pass in and nothing to keep alive externally.

Document only ever holds IDs referencing entries in Assets, never the
assets themselves. Most edits only ever touch one of the two, so there are
three separate entry points rather than one that always pays for both:
  - write()            — Document only (the common case)
  - writeAssets()       — Assets only (load/unload an asset)
  - writeWithNewAsset() — both together, for the rare case of adding an
                          asset and referencing it from Document in one
                          logical transaction

writeWithNewAsset() nests Document's edit() around Assets' edit(), which
structurally guarantees Assets publishes before Document — so a Document
update that references a new asset can never become visible before that
asset does. read() mirrors this by reading Document before Assets. Don't
reorder either without re-deriving why — it's not cosmetic.

Parameters is not part of that scheme at all: it's not triple-buffered,
there's only ever one instance, and its fields are individually atomic —
so it needs no snapshotting, just a live reference. The Real-Time Audio
thread reaches it through RealtimeReadLock, same call as everything else;
getParameters() below is the equivalent direct path for any other thread
(GUI, writer, ...). Both refer to the exact same object. */
template <typename Document, typename Assets, typename Parameters>
class RealtimeModel
{
public:
    /* A short-lived, per-callback bundle of the current Document, Assets,
       and Parameters. Document/Assets are snapshots valid until the next
       call to read() — don't hold on to them past that. Parameters is a
       live reference to the one instance, always current. */
    class RealtimeReadLock
    {
    public:
        RealtimeReadLock(const Document& d, const Assets& a, Parameters& p)
            : m_document(d)
            , m_assets(a)
            , m_parameters(p)
        {
        }

        const Document& getDocument() const { return m_document; }
        const Assets&   getAssets() const { return m_assets; }
        Parameters&     getParameters() const { return m_parameters; }

    private:
        const Document& m_document;
        const Assets&   m_assets;
        Parameters&     m_parameters;
    };

    RealtimeModel() = default;
    ~RealtimeModel() { stop(); }

    RealtimeModel(const RealtimeModel&) = delete;
    RealtimeModel& operator=(const RealtimeModel&) = delete;

    void start() { m_writer.start(); }
    void stop() { m_writer.stop(); }

    /* Call from ANY thread (GUI, MIDI, workers). For edits that only touch
       Document (volume, mute, reorder, ...). */
    void write(std::function<void(Document&)> f)
    {
        m_writer.push([this, f = std::move(f)]() {
            m_document.edit([&](Document& d) { f(d); });
        });
    }

    /* Call from ANY thread. For edits that only touch Assets (load/unload
       a file, swap a plugin instance, ...) without changing what
       Document currently references. */
    void writeAssets(std::function<void(Assets&)> f)
    {
        m_writer.push([this, f = std::move(f)]() {
            m_assets.edit([&](Assets& a) { f(a); });
        });
    }

    /* Call from ANY thread. For the rare case of adding an asset AND
       referencing it from Document in one logical transaction. See the
       class comment for why the nesting order matters. */
    void writeWithNewAsset(std::function<void(Document&, Assets&)> f)
    {
        m_writer.push([this, f = std::move(f)]() {
            m_document.edit([&](Document& d) {
                m_assets.edit([&](Assets& a) { f(d, a); });
            });
        });
    }

    /* Call ONLY from the Real-Time Audio thread. Document is read before
       Assets — see the class comment. */
    RealtimeReadLock read()
    {
        const Document& d = m_document.read();
        const Assets&   a = m_assets.read();
        return RealtimeReadLock(d, a, m_parameters);
    }

	/* Load
	Helper function for loading a new document + asset combo, used when
	you need to load new data read e.g. from disk. */
	
	void load(Document&& document, Assets&& assets)
    {
    	writeWithNewAsset([d = std::move(document), a = std::move(assets)]
						   (Document& doc, Assets& ass) mutable {
			ass = std::move(a);
			doc = std::move(d);
		});
    }

    /* Direct, unqueued access to Parameters for any non-realtime thread
       (GUI, writer, ...). Same object RealtimeReadLock::getParameters()
       refers to. */
    Parameters& getParameters() { return m_parameters; }

private:
    RealtimeValue<Document> m_document;
    RealtimeValue<Assets>   m_assets;
    Parameters              m_parameters;
    Writer                  m_writer;
};