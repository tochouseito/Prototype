#pragma once

#include "ECSManager.h"
#include "BatchedRegistry.h"
#include "GameObject.h"
#include "SceneAsset.h"
#include "SceneInstance.h"

namespace Cue::GameCore
{
    class GameWorld final
    {
        using EntityRegistry = Core::BatchedRegistry<ECS::Entity>;
    public:
        // Scene の追加
    private:
        std::unique_ptr<ECS::ECSManager> m_ecsManager = nullptr;
        EntityRegistry m_entityRegistry{};
        std::unordered_map<std::string, EntityRegistry::InsertGroupResult> m_namedEntityGroups{};
    };
}