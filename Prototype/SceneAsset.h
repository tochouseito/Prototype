#pragma once

#include "GameObjectProto.h"
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace Cue::GameCore
{
	class SceneAsset final
	{
	public:
		void add_game_object_proto(const std::string& name);

	private:
		std::vector<GameObjectProto> m_gameObjectPrefabs{};
		std::unordered_map<std::string, uint32_t> m_prefabsByTag{};
	};
}