#pragma once

#include "ECSManager.h"

namespace Cue::GameCore
{
	class GameObjectProto final : public ECS::Prototype
	{
	public:
		GameObjectProto(const std::string& name, const std::string& tag) : m_name(name), m_tag(tag) {}
		~GameObjectProto() = default;

		const std::string& name() const { return m_name; }
		const std::string& tag() const { return m_tag; }

		static GameObjectProto from_entity(ECS::ECSManager& a_ecs, ECS::Entity a_e, const std::string& a_name, const std::string& a_tag = "Default")
		{
			GameObjectProto proto(a_name, a_tag);
			proto.populate_from_entity(a_ecs, a_e);
			return proto;
		}
	private:
		ECS::Entity create_entity(ECS::ECSManager& a_ecs) const override
		{
			return Prototype::create_entity(a_ecs);
		}

		std::string m_name;
		std::string m_tag;
	};
}