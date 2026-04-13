#pragma once

#include "GameCoreTypes.h"

#include <string>

namespace Cue::GameCore
{
    struct BaseComponent final : public ECS::IComponentTag
    {
        std::string name{};
        std::string tag{"Default"};
        SceneId owningSceneId = k_invalidSceneId;
        EntityId parent = k_invalidEntityId;
        bool isActiveSelf = true;
        bool isPersistent = false;
    };
}

namespace Cue::ECS
{
    template <>
    struct ComponentTypeName<Cue::GameCore::BaseComponent>
    {
        static constexpr const char* value() noexcept
        {
            return "Cue::GameCore::BaseComponent";
        }
    };
}
