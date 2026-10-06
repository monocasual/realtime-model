#pragma once

#include <memory>
#include <unordered_map>

/* RealtimeAssetMap
A key-value container designed for storing assets (plug-ins, audio files, ...).
Use this in your Asset class, e.g.:

    RealtimeAssetMap<ID, Plugin>    plugins;
    RealtimeAssetMap<ID, AudioFile> audioFiles;
    ...
*/

namespace mcl
{
template <typename Key, typename T>
class RealtimeAssetMap
{
public:
	using Ptr      = std::shared_ptr<T>;
	using PtrConst = std::shared_ptr<const T>;

	void set(Key key, Ptr asset)
	{
		m_map[std::move(key)] = std::move(asset);
	}

	void remove(const Key& key)
	{
		m_map.erase(key);
	}

	PtrConst find(const Key& key) const
	{
		auto it = m_map.find(key);
		return it != m_map.end() ? it->second : nullptr;
	}

private:
	std::unordered_map<Key, Ptr> m_map;
};
} // namespace mcl
