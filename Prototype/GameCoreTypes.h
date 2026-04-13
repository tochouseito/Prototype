#pragma once

#include "ECSManager.h"

#include <cstdint>

namespace Cue::GameCore
{
    using EntityId = ECS::Entity;
    using Generation = std::uint32_t;
    using SceneId = std::uint64_t;
    using LocalObjectId = std::uint64_t;

    inline constexpr SceneId k_invalidSceneId = 0;
    inline constexpr EntityId k_invalidEntityId = static_cast<EntityId>(-1);
    inline constexpr LocalObjectId k_invalidLocalObjectId = 0;
}
