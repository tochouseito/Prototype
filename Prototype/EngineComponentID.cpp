
// === C++ includes ===
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

#include "ComponentIDHelper.h"

namespace Cue::ECS {
extern "C" ComponentTypeID
ECS_RegisterComponentID(const char *a_uniqueName) // C で export すると楽
{
  static std::unordered_map<std::string, ComponentTypeID> map;
  static ComponentTypeID nextID = 0;
  static std::mutex m;
  std::lock_guard lk(m);
  if (nextID == (std::numeric_limits<ComponentTypeID>::max)()) {
    std::abort();
  }
  auto [it, inserted] = map.try_emplace(a_uniqueName, nextID);
  if (inserted) {
    ++nextID; // 新規登録なら採番
  }
  return it->second; // 既存なら同じ ID を返す
}
} // namespace Cue::ECS
