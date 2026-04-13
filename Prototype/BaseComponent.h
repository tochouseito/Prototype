#pragma once

#include "GameCoreTypes.h"

#include <string>

namespace Cue::GameCore
{
    // GameWorld が全 GameObject に共通で持たせる基本情報。
    struct BaseComponent final : public ECS::IComponentTag
    {
        // GameWorld 内で一意になるよう管理される表示名。
        std::string name{};
        // 検索用の分類ラベル。
        std::string tag{"Default"};
        // 所属 Scene。永続 Object の場合は無効 SceneId を持つ。
        SceneId owningSceneId = k_invalidSceneId;
        // 親子関係を表す親 Entity。
        EntityId parent = k_invalidEntityId;
        // 自身のアクティブ状態。
        bool isActiveSelf = true;
        // Scene アンロード時に削除せず残すかどうか。
        bool isPersistent = false;
    };
}

namespace Cue::ECS
{
    template <>
    struct ComponentTypeName<Cue::GameCore::BaseComponent>
    {
        // 環境依存の型名ではなく固定文字列でコンポーネント種別名を与える。
        static constexpr const char* value() noexcept
        {
            return "Cue::GameCore::BaseComponent";
        }
    };
}
