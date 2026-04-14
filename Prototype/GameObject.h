#pragma once

#include "GameCoreTypes.h"
#include "Result.h"

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
        [[nodiscard]] Result name(std::string& a_outName) const;
        [[nodiscard]] Result set_name(std::string_view a_name);
        [[nodiscard]] Result tag(std::string& a_outTag) const;
        [[nodiscard]] Result set_tag(std::string_view a_tag);
        [[nodiscard]] Result is_persistent(bool& a_outIsPersistent) const;
        [[nodiscard]] Result set_persistent(bool a_isPersistent);

        // コンポーネント操作は内部的に GameWorld を経由する。
        template <typename T> [[nodiscard]] Result get_component(T*& a_outComponent) noexcept;

        template <typename T, typename... Args>
        [[nodiscard]] Result add_component(T*& a_outComponent, Args&&... a_args);

        template <typename T> [[nodiscard]] Result has_component(bool& a_outHasComponent) const noexcept;
        template <typename T> [[nodiscard]] Result remove_component() noexcept;

        // 削除は即時ではなく、GameWorld の遅延削除キューへ登録される。
        [[nodiscard]] Result destroy() noexcept;

    private:
        GameWorld* m_world = nullptr;
        EntityId m_entityId = k_invalidEntityId;
        Generation m_generation = 0;
    };
}
