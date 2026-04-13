#pragma once

// === C++ includes ===
#include <cstdint>
#include <type_traits>

namespace Cue::ECS {
using ComponentTypeID = std::uint32_t;

extern "C" ComponentTypeID ECS_RegisterComponentID(const char *a_uniqueName);

template <class T> struct ComponentTypeName;

template <class T> inline const char *component_type_name() {
  static_assert(std::is_same_v<T, void>,
                "ComponentTypeName<T> must be specialized for each component "
                "type before requesting its ComponentID.");
  return "";
}

template <class T> inline const char *component_type_name()
requires requires { ComponentTypeName<T>::value(); }
{
  return ComponentTypeName<T>::value();
}

template <class T> inline ComponentTypeID ComponentID() {
  static ComponentTypeID id = ECS_RegisterComponentID(component_type_name<T>());
  return id;
}
} // namespace Cue::ECS
