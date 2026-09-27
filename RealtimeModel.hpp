#pragma once

#include "RealtimeValue.hpp"
#include "Writer.hpp"
#include <functional>
#include <utility>

/* RealtimeModel
The user-facing API. Owns the three pieces of shared state — Document,
Assets, and Parameters — and orchestrates RealtimeValue + Writer so callers
never touch either directly.

Document only ever holds IDs referencing entries in Assets, never the
assets themselves. Most edits only ever touch one of the two, so there are
three separate entry points rather than one that always pays for both:
  - write()            — Document + Parameters only (the common case)
  - writeAssets()       — Assets + Parameters only (load/unload an asset)
  - writeWithNewAsset() — both together, for the rare case of adding an
                          asset and referencing it from Document in one
                          logical transaction

writeWithNewAsset() nests Document's edit() around Assets' edit(), which
structurally guarantees Assets publishes before Document — so a Document
update that references a new asset can never become visible before that
asset does. read() mirrors this by reading Document before Assets. Don't
reorder either without re-deriving why — it's not cosmetic. */
template <typename Document, typename Assets, typename Parameters>
class RealtimeModel
{
public:
    /* A short-lived, per-callback snapshot bundling the current Document,
       Assets and Parameters together. Only valid until the next call to
       read() — don't hold on to it past that. */
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

    RealtimeModel(const Document& document, const Assets& assets, Parameters& parameters)
        : m_document(document)
        , m_assets(assets)
        , m_parameters(parameters)
        , m_writer()
    {
    }

    RealtimeModel(const RealtimeModel&) = delete;
    RealtimeModel& operator=(const RealtimeModel&) = delete;

    void start() { m_writer.start(); }
    void stop() { m_writer.stop(); }

    /* Call from ANY thread (GUI, MIDI, workers). For edits that only touch
       Document (volume, mute, reorder, ...). */
    void write(std::function<void(Document&, Parameters&)> f)
    {
        m_writer.push([this, f = std::move(f)]() {
            m_document.edit([&](Document& d) { f(d, m_parameters); });
        });
    }

    /* Call from ANY thread. For edits that only touch Assets (load/unload
       a file, swap a plugin instance, ...) without changing what
       Document currently references. */
    void writeAssets(std::function<void(Assets&, Parameters&)> f)
    {
        m_writer.push([this, f = std::move(f)]() {
            m_assets.edit([&](Assets& a) { f(a, m_parameters); });
        });
    }

    /* Call from ANY thread. For the rare case of adding an asset AND
       referencing it from Document in one logical transaction. See the
       class comment for why the nesting order matters. */
    void writeWithNewAsset(std::function<void(Document&, Assets&, Parameters&)> f)
    {
        m_writer.push([this, f = std::move(f)]() {
            m_document.edit([&](Document& d) {
                m_assets.edit([&](Assets& a) { f(d, a, m_parameters); });
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

private:
    RealtimeValue<Document> m_document;
    RealtimeValue<Assets>   m_assets;
    Parameters&             m_parameters;
    Writer                  m_writer;
};