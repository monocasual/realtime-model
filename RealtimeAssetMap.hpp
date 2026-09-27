#pragma once
#include <memory>
#include <unordered_map>

template <typename Key, typename T>
class RealtimeAssetMap
{
public:
	using Ptr = std::shared_ptr<T>;

	void set(Key key, Ptr asset)
	{
		m_map[std::move(key)] = std::move(asset);
	}

	void remove(const Key& key)
	{
		m_map.erase(key);
	}

	Ptr find(const Key& key) const
	{
		auto it = m_map.find(key);
		return it != m_map.end() ? it->second : nullptr;
	}

private:
	std::unordered_map<Key, Ptr> m_map;
};
