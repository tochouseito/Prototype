#pragma once

#include "GameCoreTypes.h"

#include <string>
#include <string_view>

namespace Cue::GameCore
{
    class GameWorld;

    // GameWorld が返す軽量ハンドル。EntityId と世代番号で有効性を判定する。
    class GameObject final
    {
    public:
        GameObject() = default;
        GameObject(GameWorld* a_world, EntityId a_entityId,
            Generation a_generation) noexcept;

        // 生成元の Entity がまだ同じ世代で生存しているかを返す。
        [[nodiscard]] bool is_valid() const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept { return is_valid(); }
        [[nodiscard]] EntityId entity_id() const noexcept { return m_entityId; }
        [[nodiscard]] Generation generation() const noexcept { return m_generation; }

        // GameWorld が管理する名前とタグへアクセスする。
        [[nodiscard]] std::string name() const;
        void set_name(std::string_view a_name);
        [[nodiscard]] std::string tag() const;
        void set_tag(std::string_view a_tag);

        // コンポーネント操作は内部的に GameWorld を経由する。
        template <typename T> T* get_component() noexcept;

        template <typename T, typename... Args>
        T& add_component(Args&&... a_args);

        template <typename T> bool has_component() const noexcept;
        template <typename T> void remove_component() noexcept;

        // 削除は即時ではなく、GameWorld の遅延削除キューへ登録される。
        void destroy() noexcept;

    private:
        GameWorld* m_world = nullptr;
        EntityId m_entityId = k_invalidEntityId;
        Generation m_generation = 0;
    };
}
