#pragma once

#include "ECSManager.h"

namespace Cue::ECS
{
	struct TransformComponent final : public IComponentTag
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	template <>
	struct ComponentTypeName<TransformComponent>
	{
		static constexpr const char* value() noexcept
		{
			return "Cue::ECS::TransformComponent";
		}
	};
}
