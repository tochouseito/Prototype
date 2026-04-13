#pragma once

#include <string>

namespace Cue::GameCore
{
	class GameObject
	{
	public:
		GameObject(const std::string& name) : m_name(name) {}
		GameObject(const GameObject&) = delete;
		GameObject& operator=(const GameObject&) = delete;
		GameObject(GameObject&&) = default;
		GameObject& operator=(GameObject&&) = default;
		~GameObject() = default;

		const std::string& name() const { return m_name; }
	private:
		std::string m_name;
	};
}