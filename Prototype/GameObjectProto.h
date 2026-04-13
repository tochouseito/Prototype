#pragma once

#include "ECSManager.h"

#include <string>
#include <utility>

namespace Cue::GameCore
{
    // GameObject 生成時に使う雛形。コンポーネント群と基本属性を保持する。
    class GameObjectProto final : public ECS::Prototype
    {
    public:
        GameObjectProto() : m_name(""), m_tag("Default") {}
        GameObjectProto(std::string name, std::string tag)
            : m_name(std::move(name)), m_tag(std::move(tag)) {}
        ~GameObjectProto() = default;

        const std::string& name() const { return m_name; }
        const std::string& tag() const { return m_tag; }

        template <ECS::ComponentType T>
        void add_component(const T& a_comp)
        {
            // 復元に必要なコピー関数を登録してから Prototype へ積む。
            register_component_type<T>();
            Prototype::add_component<T>(a_comp);
        }

        template <ECS::ComponentType T>
        void set_component(const T& a_comp)
        {
            // 既存コンポーネントを置き換える場合も同じ登録が必要。
            register_component_type<T>();
            Prototype::set_component<T>(a_comp);
        }

        void restore_components_into(ECS::Entity a_entity, ECS::ECSManager& a_ecs) const
        {
            // Prototype に保持したコンポーネント群を対象 Entity に復元する。
            instantiate_components(a_entity, a_ecs);
        }

        static GameObjectProto from_entity(ECS::ECSManager& a_ecs, ECS::Entity a_e,
            const std::string& a_name, const std::string& a_tag = "Default")
        {
            // 既存 Entity の状態から Prototype を逆生成する。
            GameObjectProto proto(a_name, a_tag);
            proto.populate_from_entity(a_ecs, a_e);
            return proto;
        }

    private:
        template <ECS::ComponentType T>
        static void register_component_type()
        {
            Prototype::register_copy_func<T>();
            Prototype::register_prefab_restore<T>();
        }

        ECS::Entity create_entity(ECS::ECSManager& a_ecs) const override
        {
            return Prototype::create_entity(a_ecs);
        }

        std::string m_name;
        std::string m_tag;
    };
}
