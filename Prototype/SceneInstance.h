#pragma once

#include "GameCoreTypes.h"

#include <unordered_map>
#include <vector>

namespace Cue::GameCore
{
    class SceneAsset;

    struct SceneInstance final
    {
        SceneId sceneId = k_invalidSceneId;
        const SceneAsset* asset = nullptr;
        std::vector<EntityId> entities{};
        std::unordered_map<LocalObjectId, EntityId> localObjectToEntity{};
        bool isLoaded = false;
        bool isActive = true;
        bool isPendingUnload = false;
        LocalObjectId nextLocalObjectId = 1;
    };
}
