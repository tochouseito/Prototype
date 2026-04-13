#pragma once

#include "GameCoreTypes.h"

namespace Cue::GameCore
{
    class GameWorld;

    class GameObject final
    {
    public:
        GameObject() = default;
        GameObject(GameWorld* a_world, EntityId a_entityId,
            Generation a_generation) noexcept;

        [[nodiscard]] bool is_valid() const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept { return is_valid(); }
        [[nodiscard]] EntityId entity_id() const noexcept { return m_entityId; }
        [[nodiscard]] Generation generation() const noexcept { return m_generation; }

        template <typename T> T* get_component() noexcept;

        template <typename T, typename... Args>
        T& add_component(Args&&... a_args);

        template <typename T> bool has_component() const noexcept;
        template <typename T> void remove_component() noexcept;

        void destroy() noexcept;

    private:
        GameWorld* m_world = nullptr;
        EntityId m_entityId = k_invalidEntityId;
        Generation m_generation = 0;
    };
}
