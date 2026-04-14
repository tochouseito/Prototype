#pragma once

#include "BaseComponent.h"
#include "GameObject.h"
#include "Result.h"
#include "SceneAsset.h"
#include "SceneInstance.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Cue::GameCore
{
    class GameWorld final
    {
    public:
        struct LoadSceneResult final
        {
            SceneId sceneId = k_invalidSceneId;
            std::vector<GameObject> objects{};
        };

        struct EntityRecord final
        {
            Generation generation = 0;
            bool isAlive = false;
            // 遅延削除キューへ同じ Entity を二重登録しないためのフラグ。
            bool isPendingDestroy = false;
            SceneId sourceSceneId = k_invalidSceneId;
            LocalObjectId sourceLocalObjectId = k_invalidLocalObjectId;
        };

        GameWorld() = default;

        [[nodiscard]] Result ecs(ECS::ECSManager*& a_outEcs) noexcept
        {
            a_outEcs = &m_ecs;
            return Result::ok();
        }

        [[nodiscard]] Result create_object(std::string_view a_name,
            std::string_view a_tag, bool a_isPersistent, GameObject& a_outObject)
        {
            a_outObject = {};
            return capture_result([this, &a_outObject, a_name, a_tag, a_isPersistent]()
                {
                    a_outObject = create_object(a_name, a_tag, a_isPersistent);
                });
        }

        [[nodiscard]] Result create_object(std::string_view a_name,
            GameObject& a_outObject)
        {
            return create_object(a_name, "Default", false, a_outObject);
        }

        [[nodiscard]] Result load_scene(
            const SceneAsset& a_asset, LoadSceneResult& a_outResult)
        {
            a_outResult = {};
            return capture_result([this, &a_outResult, &a_asset]()
                {
                    a_outResult = load_scene(a_asset);
                });
        }

        [[nodiscard]] Result append_to_scene(SceneId a_sceneId,
            std::span<const ObjectDefinition> a_objects, LoadSceneResult& a_outResult)
        {
            a_outResult = {};
            return capture_result([this, &a_outResult, a_sceneId, a_objects]()
                {
                    a_outResult = append_to_scene(a_sceneId, a_objects);
                });
        }

        [[nodiscard]] Result append_to_scene(SceneId a_sceneId,
            const std::vector<ObjectDefinition>& a_objects, LoadSceneResult& a_outResult)
        {
            return append_to_scene(a_sceneId,
                std::span<const ObjectDefinition>(a_objects), a_outResult);
        }

        [[nodiscard]] Result append_object_to_scene(SceneId a_sceneId,
            const ObjectDefinition& a_object, GameObject& a_outObject)
        {
            a_outObject = {};
            return capture_result([this, &a_outObject, a_sceneId, &a_object]()
                {
                    a_outObject = append_object_to_scene(a_sceneId, a_object);
                });
        }

        [[nodiscard]] Result destroy_object(EntityId a_entityId) noexcept
        {
            if (!contains_object(a_entityId))
            {
                return Result::fail(
                    Code::NotFound, Severity::Warning, "GameWorld object was not found.");
            }

            destroy_object_internal(a_entityId);
            return Result::ok();
        }

        [[nodiscard]] Result unload_scene(SceneId a_sceneId) noexcept
        {
            return unload_scene_internal(a_sceneId)
                ? Result::ok()
                : Result::fail(
                    Code::NotFound, Severity::Warning, "GameWorld scene was not found.");
        }

        [[nodiscard]] Result execute_deferred_deletions() noexcept
        {
            execute_deferred_deletions_internal();
            return Result::ok();
        }

        [[nodiscard]] Result find_object(EntityId a_entityId, GameObject& a_outObject) noexcept
        {
            a_outObject = find_object(a_entityId);
            return a_outObject.is_valid()
                ? Result::ok()
                : Result::fail(
                    Code::NotFound, Severity::Warning, "GameWorld object was not found.");
        }

        [[nodiscard]] Result contains_object(
            EntityId a_entityId, bool& a_outContains) const noexcept
        {
            a_outContains = contains_object(a_entityId);
            return Result::ok();
        }

        [[nodiscard]] Result contains_scene(
            SceneId a_sceneId, bool& a_outContains) const noexcept
        {
            a_outContains = contains_scene(a_sceneId);
            return Result::ok();
        }

        [[nodiscard]] Result get_object_tag(
            EntityId a_entityId, std::string& a_outTag) const
        {
            a_outTag = get_object_tag(a_entityId);
            return a_outTag.empty() && !contains_object(a_entityId)
                ? Result::fail(
                    Code::NotFound, Severity::Warning, "GameWorld object was not found.")
                : Result::ok();
        }

        [[nodiscard]] Result get_object_name(
            EntityId a_entityId, std::string& a_outName) const
        {
            a_outName = get_object_name(a_entityId);
            return a_outName.empty() && !contains_object(a_entityId)
                ? Result::fail(
                    Code::NotFound, Severity::Warning, "GameWorld object was not found.")
                : Result::ok();
        }

        [[nodiscard]] Result set_object_name(
            EntityId a_entityId, std::string_view a_name)
        {
            return capture_result([this, a_entityId, a_name]()
                {
                    BaseComponent* base = get_component<BaseComponent>(a_entityId);
                    if (base == nullptr)
                    {
                        throw std::runtime_error("GameWorld BaseComponent is missing.");
                    }

                    const std::string resolvedName =
                        make_unique_object_name(a_name, a_entityId);
                    if (base->name == resolvedName)
                    {
                        return;
                    }

                    remove_object_from_name_index(a_entityId, base->name);
                    base->name = resolvedName;
                    add_object_to_name_index(a_entityId, base->name);
                });
        }

        [[nodiscard]] Result set_object_tag(
            EntityId a_entityId, std::string_view a_tag)
        {
            return capture_result([this, a_entityId, a_tag]()
                {
                    BaseComponent* base = get_component<BaseComponent>(a_entityId);
                    if (base == nullptr)
                    {
                        throw std::runtime_error("GameWorld BaseComponent is missing.");
                    }

                    if (base->tag == a_tag)
                    {
                        return;
                    }

                    remove_object_from_tag_index(a_entityId, base->tag);
                    base->tag = std::string(a_tag);
                    add_object_to_tag_index(a_entityId, base->tag);
                });
        }

        [[nodiscard]] Result is_object_persistent(
            EntityId a_entityId, bool& a_outIsPersistent) const noexcept
        {
            a_outIsPersistent = is_object_persistent(a_entityId);
            return contains_object(a_entityId)
                ? Result::ok()
                : Result::fail(
                    Code::NotFound, Severity::Warning, "GameWorld object was not found.");
        }

        [[nodiscard]] Result set_object_persistent(
            EntityId a_entityId, bool a_isPersistent)
        {
            return capture_result([this, a_entityId, a_isPersistent]()
                {
                    BaseComponent* base = get_component<BaseComponent>(a_entityId);
                    if (base == nullptr)
                    {
                        throw std::runtime_error("GameWorld BaseComponent is missing.");
                    }

                    if (base->isPersistent == a_isPersistent)
                    {
                        return;
                    }

                    base->isPersistent = a_isPersistent;
                    base->owningSceneId = a_isPersistent
                        ? k_invalidSceneId
                        : source_scene_id(a_entityId);
                });
        }

        [[nodiscard]] Result is_alive(EntityId a_entityId,
            Generation a_generation, bool& a_outIsAlive) const noexcept
        {
            a_outIsAlive = is_alive(a_entityId, a_generation);
            return Result::ok();
        }

        [[nodiscard]] Result source_scene_id(
            EntityId a_entityId, SceneId& a_outSceneId) const noexcept
        {
            a_outSceneId = source_scene_id(a_entityId);
            return contains_object(a_entityId)
                ? Result::ok()
                : Result::fail(
                    Code::NotFound, Severity::Warning, "GameWorld object was not found.");
        }

        [[nodiscard]] Result object_count(size_t& a_outCount) const noexcept
        {
            a_outCount = m_liveObjectCount;
            return Result::ok();
        }

        [[nodiscard]] Result scene_count(size_t& a_outCount) const noexcept
        {
            a_outCount = m_scenes.size();
            return Result::ok();
        }

        [[nodiscard]] Result clear() noexcept
        {
            // 公開 API を使って削除予約を積み、最後にまとめて flush する。
            std::vector<SceneId> sceneIds{};
            sceneIds.reserve(m_scenes.size());
            for (const auto& [sceneId, _] : m_scenes)
            {
                sceneIds.push_back(sceneId);
            }

            for (const SceneId sceneId : sceneIds)
            {
                const Result unloadResult = unload_scene(sceneId);
                if (!unloadResult)
                {
                    return unloadResult;
                }
            }

            for (EntityId entity = 0;
                entity < static_cast<EntityId>(m_entityRecords.size()); ++entity)
            {
                if (contains_object(entity))
                {
                    const Result destroyResult = destroy_object(entity);
                    if (!destroyResult)
                    {
                        return destroyResult;
                    }
                }
            }

            // clear() 完了時点ではワールドが空になっていることを保証する。
            return execute_deferred_deletions();
        }

        template <typename T>
        [[nodiscard]] Result get_component(EntityId a_entityId, T*& a_outComponent) noexcept
        {
            a_outComponent = get_component<T>(a_entityId);
            return a_outComponent != nullptr
                ? Result::ok()
                : Result::fail(
                    Code::NotFound, Severity::Warning, "GameWorld component was not found.");
        }

        template <typename T>
        [[nodiscard]] Result get_component(
            EntityId a_entityId, const T*& a_outComponent) const noexcept
        {
            a_outComponent = get_component<T>(a_entityId);
            return a_outComponent != nullptr
                ? Result::ok()
                : Result::fail(
                    Code::NotFound, Severity::Warning, "GameWorld component was not found.");
        }

        template <typename T, typename... Args>
        [[nodiscard]] Result add_component(
            EntityId a_entityId, T*& a_outComponent, Args&&... a_args)
        {
            a_outComponent = nullptr;
            return capture_result([this, &a_outComponent, a_entityId, &a_args...]()
                {
                    a_outComponent =
                        &add_component<T>(a_entityId, std::forward<Args>(a_args)...);
                });
        }

        template <typename T>
        [[nodiscard]] Result has_component(
            EntityId a_entityId, bool& a_outHasComponent) const noexcept
        {
            a_outHasComponent = has_component<T>(a_entityId);
            return contains_object(a_entityId)
                ? Result::ok()
                : Result::fail(
                    Code::NotFound, Severity::Warning, "GameWorld object was not found.");
        }

        template <typename T>
        [[nodiscard]] Result remove_component(EntityId a_entityId) noexcept
        {
            if (!contains_object(a_entityId))
            {
                return Result::fail(
                    Code::NotFound, Severity::Warning, "GameWorld object was not found.");
            }

            m_ecs.remove_component<T>(a_entityId);
            return Result::ok();
        }

        template <class F>
        [[nodiscard]] Result visit_object(EntityId a_entityId, F&& a_func)
        {
            GameObject object = find_object(a_entityId);
            if (!object.is_valid())
            {
                return Result::fail(
                    Code::NotFound, Severity::Warning, "GameWorld object was not found.");
            }

            a_func(object.entity_id(), source_scene_id(a_entityId), object);
            return Result::ok();
        }

        template <class F>
        [[nodiscard]] Result for_each_object_in_scene(SceneId a_sceneId, F&& a_func)
        {
            auto sceneIt = m_scenes.find(a_sceneId);
            if (sceneIt == m_scenes.end())
            {
                return Result::fail(
                    Code::NotFound, Severity::Warning, "GameWorld scene was not found.");
            }

            const std::vector<EntityId> entities = sceneIt->second.entities;
            for (const EntityId entity : entities)
            {
                GameObject object = find_object(entity);
                if (!object.is_valid())
                {
                    continue;
                }

                a_func(object.entity_id(), a_sceneId, object);
            }

            return Result::ok();
        }

        template <class F>
        [[nodiscard]] Result for_each_object(F&& a_func)
        {
            for (EntityId entity = 0;
                entity < static_cast<EntityId>(m_entityRecords.size()); ++entity)
            {
                GameObject object = find_object(entity);
                if (!object.is_valid())
                {
                    continue;
                }

                a_func(object.entity_id(), source_scene_id(entity), object);
            }

            return Result::ok();
        }

        [[nodiscard]] Result find_objects_by_tag(
            std::string_view a_tag, std::vector<GameObject>& a_outObjects)
        {
            a_outObjects = find_objects_by_tag(a_tag);
            return Result::ok();
        }

        [[nodiscard]] Result find_objects_by_name(
            std::string_view a_name, std::vector<GameObject>& a_outObjects)
        {
            a_outObjects = find_objects_by_name(a_name);
            return Result::ok();
        }

        [[nodiscard]] Result find_object_by_name(
            std::string_view a_name, GameObject& a_outObject)
        {
            a_outObject = find_object_by_name(a_name);
            return a_outObject.is_valid()
                ? Result::ok()
                : Result::fail(
                    Code::NotFound, Severity::Warning, "GameWorld object was not found.");
        }

        [[nodiscard]] Result destroy_object_by_name(std::string_view a_name) noexcept
        {
            const GameObject object = find_object_by_name(a_name);
            if (!object.is_valid())
            {
                return Result::fail(
                    Code::NotFound, Severity::Warning, "GameWorld object was not found.");
            }

            return destroy_object_internal(object.entity_id()), Result::ok();
        }

        [[nodiscard]] Result destroy_objects_by_name(
            std::string_view a_name, size_t& a_outCount) noexcept
        {
            const std::vector<GameObject> objects = find_objects_by_name(a_name);
            for (const GameObject& object : objects)
            {
                destroy_object_internal(object.entity_id());
            }

            a_outCount = objects.size();
            return Result::ok();
        }

        [[nodiscard]] Result find_objects_by_name_series(
            std::string_view a_baseName, std::vector<GameObject>& a_outObjects)
        {
            a_outObjects = find_objects_by_name_series(a_baseName);
            return Result::ok();
        }

        [[nodiscard]] Result destroy_objects_by_name_series(
            std::string_view a_baseName, size_t& a_outCount) noexcept
        {
            const std::vector<GameObject> objects =
                find_objects_by_name_series(a_baseName);
            for (const GameObject& object : objects)
            {
                destroy_object_internal(object.entity_id());
            }

            a_outCount = objects.size();
            return Result::ok();
        }

        [[nodiscard]] Result destroy_objects_by_tag(
            std::string_view a_tag, size_t& a_outCount) noexcept
        {
            const std::vector<GameObject> objects = find_objects_by_tag(a_tag);
            for (const GameObject& object : objects)
            {
                destroy_object_internal(object.entity_id());
            }

            a_outCount = objects.size();
            return Result::ok();
        }

    private:
        template <typename F>
        [[nodiscard]] static Result capture_result(F&& a_func)
        {
            try
            {
                a_func();
                return Result::ok();
            }
            catch (const std::bad_alloc&)
            {
                return Result::fail(
                    Code::OutOfMemory, Severity::Error, "GameWorld out of memory.");
            }
            catch (const std::overflow_error& a_error)
            {
                return map_exception_message(a_error.what());
            }
            catch (const std::runtime_error& a_error)
            {
                return map_exception_message(a_error.what());
            }
            catch (const std::exception&)
            {
                return Result::fail(
                    Code::UnknownError, Severity::Error, "GameWorld unknown exception.");
            }
        }

        [[nodiscard]] static Result map_exception_message(
            std::string_view a_message) noexcept
        {
            if (a_message == "GameWorld BaseComponent is missing." ||
                a_message == "GameWorld BaseComponent is missing while resolving parent.")
            {
                return Result::fail(Code::InternalError, Severity::Error, a_message);
            }
            if (a_message == "GameWorld object is not alive.")
            {
                return Result::fail(Code::InvalidState, Severity::Warning, a_message);
            }
            if (a_message == "GameWorld failed to add component." ||
                a_message == "GameWorld failed to initialize BaseComponent.")
            {
                return Result::fail(Code::CreateFailed, Severity::Error, a_message);
            }
            if (a_message == "GameWorld scene id overflow.")
            {
                return Result::fail(Code::InvalidState, Severity::Error, a_message);
            }
            if (a_message == "GameWorld internal error: entity slot is already alive." ||
                a_message == "GameWorld parentLocalObjectId could not be resolved.")
            {
                return Result::fail(Code::InternalError, Severity::Error, a_message);
            }
            if (a_message == "GameWorld scene was not found.")
            {
                return Result::fail(Code::NotFound, Severity::Warning, a_message);
            }
            if (a_message == "GameWorld scene is pending unload.")
            {
                return Result::fail(Code::InvalidState, Severity::Warning, a_message);
            }
            if (a_message == "GameWorld localObjectId is duplicated in scene.")
            {
                return Result::fail(Code::InvalidArgument, Severity::Warning, a_message);
            }

            return Result::fail(Code::UnknownError, Severity::Error, a_message);
        }

        [[nodiscard]] GameObject create_object(std::string_view a_name,
            std::string_view a_tag = "Default", bool a_isPersistent = false)
        {
            // Scene に属さない単体の GameObject を生成する。
            const EntityId entity =
                create_entity_record(k_invalidSceneId, k_invalidLocalObjectId);
            initialize_base_component(entity, a_name, a_tag, k_invalidSceneId,
                k_invalidEntityId, true, a_isPersistent);
            return make_handle(entity);
        }

        [[nodiscard]] LoadSceneResult load_scene(const SceneAsset& a_asset)
        {
            const SceneId sceneId = generate_scene_id();

            SceneInstance scene{};
            scene.sceneId = sceneId;
            scene.asset = &a_asset;
            scene.isLoaded = true;
            scene.isActive = true;

            m_scenes.emplace(sceneId, std::move(scene));

            try
            {
                return instantiate_into_scene(sceneId, a_asset.objects(), &a_asset);
            }
            catch (...)
            {
                auto it = m_scenes.find(sceneId);
                if (it != m_scenes.end())
                {
                    const std::vector<EntityId> created = it->second.entities;
                    for (const EntityId entity : created)
                    {
                        destroy_object_immediately(entity);
                    }
                    m_scenes.erase(it);
                }
                throw;
            }
        }

        [[nodiscard]] LoadSceneResult append_to_scene(SceneId a_sceneId,
            std::span<const ObjectDefinition> a_objects)
        {
            return instantiate_into_scene(a_sceneId, a_objects, nullptr);
        }

        [[nodiscard]] GameObject append_object_to_scene(SceneId a_sceneId,
            const ObjectDefinition& a_object)
        {
            const std::array<ObjectDefinition, 1> objects = {a_object};
            LoadSceneResult result = append_to_scene(a_sceneId, objects);
            if (result.objects.empty())
            {
                return {};
            }

            return result.objects.front();
        }

        void destroy_object_internal(EntityId a_entityId) noexcept
        {
            EntityRecord* record = try_get_entity_record(a_entityId);
            if (record == nullptr || !record->isAlive || record->isPendingDestroy)
            {
                return;
            }

            // 実際の削除は execute_deferred_deletions() が呼ばれるまで遅延させる。
            record->isPendingDestroy = true;
            m_pendingDestroyedEntities.push_back(a_entityId);
        }

        [[nodiscard]] bool unload_scene_internal(SceneId a_sceneId) noexcept
        {
            auto sceneIt = m_scenes.find(a_sceneId);
            if (sceneIt == m_scenes.end() || sceneIt->second.isPendingUnload)
            {
                return false;
            }

            // Scene の破棄も遅延させ、呼び出し側が flush のタイミングを制御できるようにする。
            sceneIt->second.isPendingUnload = true;
            m_pendingUnloadedScenes.push_back(a_sceneId);
            return true;
        }

        void execute_deferred_deletions_internal() noexcept
        {
            // Scene のアンロードでは非永続 Object がまとめて消えるため、先に Scene 側を処理する。
            std::vector<SceneId> pendingScenes{};
            pendingScenes.swap(m_pendingUnloadedScenes);
            for (const SceneId sceneId : pendingScenes)
            {
                (void)unload_scene_immediately(sceneId);
            }

            // 続いて、単体で予約されていた Object の削除を処理する。
            std::vector<EntityId> pendingEntities{};
            pendingEntities.swap(m_pendingDestroyedEntities);
            for (const EntityId entity : pendingEntities)
            {
                destroy_object_immediately(entity);
            }
        }

        [[nodiscard]] GameObject find_object(EntityId a_entityId) noexcept
        {
            if (!contains_object(a_entityId))
            {
                return {};
            }

            return make_handle(a_entityId);
        }

        [[nodiscard]] bool contains_object(EntityId a_entityId) const noexcept
        {
            const EntityRecord* record = try_get_entity_record(a_entityId);
            return record != nullptr && record->isAlive;
        }

        [[nodiscard]] bool contains_scene(SceneId a_sceneId) const noexcept
        {
            return m_scenes.find(a_sceneId) != m_scenes.end();
        }

        [[nodiscard]] std::string get_object_tag(EntityId a_entityId) const
        {
            const BaseComponent* base = get_component<BaseComponent>(a_entityId);
            if (base == nullptr)
            {
                return {};
            }

            return base->tag;
        }

        [[nodiscard]] std::string get_object_name(EntityId a_entityId) const
        {
            const BaseComponent* base = get_component<BaseComponent>(a_entityId);
            if (base == nullptr)
            {
                return {};
            }

            return base->name;
        }

        [[nodiscard]] bool is_object_persistent(EntityId a_entityId) const noexcept
        {
            const BaseComponent* base = get_component<BaseComponent>(a_entityId);
            if (base == nullptr)
            {
                return false;
            }

            return base->isPersistent;
        }

        [[nodiscard]] bool is_alive(EntityId a_entityId,
            Generation a_generation) const noexcept
        {
            const EntityRecord* record = try_get_entity_record(a_entityId);
            return record != nullptr && record->isAlive &&
                record->generation == a_generation;
        }

        [[nodiscard]] SceneId source_scene_id(EntityId a_entityId) const noexcept
        {
            const EntityRecord* record = try_get_entity_record(a_entityId);
            if (record == nullptr || !record->isAlive)
            {
                return k_invalidSceneId;
            }

            return record->sourceSceneId;
        }

        template <typename T>
        [[nodiscard]] T* get_component(EntityId a_entityId) noexcept
        {
            if (!contains_object(a_entityId))
            {
                return nullptr;
            }

            return m_ecs.get_component<T>(a_entityId);
        }

        template <typename T>
        [[nodiscard]] const T* get_component(EntityId a_entityId) const noexcept
        {
            return const_cast<GameWorld*>(this)->get_component<T>(a_entityId);
        }

        template <typename T, typename... Args>
        T& add_component(EntityId a_entityId, Args&&... a_args)
        {
            if (!contains_object(a_entityId))
            {
                throw std::runtime_error("GameWorld object is not alive.");
            }

            T* component = m_ecs.add_component<T>(a_entityId);
            if (component == nullptr)
            {
                throw std::runtime_error("GameWorld failed to add component.");
            }

            *component = T{std::forward<Args>(a_args)...};
            return *component;
        }

        template <typename T>
        [[nodiscard]] bool has_component(EntityId a_entityId) const noexcept
        {
            return get_component<T>(a_entityId) != nullptr;
        }

        [[nodiscard]] std::vector<GameObject> find_objects_by_tag(
            std::string_view a_tag)
        {
            const auto it = m_tagIndex.find(std::string(a_tag));
            if (it == m_tagIndex.end())
            {
                return {};
            }

            std::vector<GameObject> objects{};
            objects.reserve(it->second.size());
            for (const EntityId entity : it->second)
            {
                if (!contains_object(entity))
                {
                    continue;
                }

                objects.push_back(make_handle(entity));
            }

            std::sort(objects.begin(), objects.end(),
                [](const GameObject& a_left, const GameObject& a_right) {
                    return a_left.entity_id() < a_right.entity_id();
                });

            return objects;
        }

        [[nodiscard]] std::vector<GameObject> find_objects_by_name(
            std::string_view a_name)
        {
            const auto it = m_nameIndex.find(std::string(a_name));
            if (it == m_nameIndex.end())
            {
                return {};
            }

            std::vector<GameObject> objects{};
            objects.reserve(it->second.size());
            for (const EntityId entity : it->second)
            {
                if (!contains_object(entity))
                {
                    continue;
                }

                objects.push_back(make_handle(entity));
            }

            std::sort(objects.begin(), objects.end(),
                [](const GameObject& a_left, const GameObject& a_right) {
                    return a_left.entity_id() < a_right.entity_id();
                });

            return objects;
        }

        [[nodiscard]] GameObject find_object_by_name(std::string_view a_name)
        {
            std::vector<GameObject> objects = find_objects_by_name(a_name);
            if (objects.empty())
            {
                return {};
            }

            return objects.front();
        }

        [[nodiscard]] std::vector<GameObject> find_objects_by_name_series(
            std::string_view a_baseName)
        {
            const std::string normalizedBaseName =
                normalize_object_name(a_baseName);

            std::vector<GameObject> objects{};
            for (const auto& [name, entityIds] : m_nameIndex)
            {
                std::uint32_t seriesIndex = 0;
                if (!try_get_name_series_index(name, normalizedBaseName, seriesIndex))
                {
                    continue;
                }

                for (const EntityId entity : entityIds)
                {
                    if (!contains_object(entity))
                    {
                        continue;
                    }

                    objects.push_back(make_handle(entity));
                }
            }

            std::sort(objects.begin(), objects.end(),
                [this, &normalizedBaseName](
                    const GameObject& a_left, const GameObject& a_right) {
                    std::uint32_t leftSeriesIndex = 0;
                    std::uint32_t rightSeriesIndex = 0;
                    const bool leftMatched = try_get_name_series_index(
                        get_object_name(a_left.entity_id()), normalizedBaseName,
                        leftSeriesIndex);
                    const bool rightMatched = try_get_name_series_index(
                        get_object_name(a_right.entity_id()), normalizedBaseName,
                        rightSeriesIndex);

                    if (leftMatched != rightMatched)
                    {
                        return leftMatched;
                    }
                    if (leftSeriesIndex != rightSeriesIndex)
                    {
                        return leftSeriesIndex < rightSeriesIndex;
                    }

                    return a_left.entity_id() < a_right.entity_id();
                });

            return objects;
        }

        [[nodiscard]] SceneId generate_scene_id()
        {
            if (m_nextSceneId == 0)
            {
                throw std::overflow_error("GameWorld scene id overflow.");
            }

            const SceneId sceneId = m_nextSceneId;
            ++m_nextSceneId;
            return sceneId;
        }

        [[nodiscard]] EntityId create_entity_record(SceneId a_sourceSceneId,
            LocalObjectId a_localObjectId)
        {
            // ECS の Entity と GameWorld の管理情報を対応付ける。
            const EntityId entity = m_ecs.generate_entity();

            if (m_entityRecords.size() <= entity)
            {
                m_entityRecords.resize(entity + 1);
            }

            EntityRecord& record = m_entityRecords[entity];
            if (record.isAlive)
            {
                throw std::runtime_error("GameWorld internal error: entity slot is already alive.");
            }

            if (record.generation == 0)
            {
                record.generation = 1;
            }

            record.isAlive = true;
            record.isPendingDestroy = false;
            record.sourceSceneId = a_sourceSceneId;
            record.sourceLocalObjectId = a_localObjectId;
            ++m_liveObjectCount;

            return entity;
        }

        void initialize_base_component(EntityId a_entityId,
            std::string_view a_name, std::string_view a_tag,
            SceneId a_owningSceneId, EntityId a_parent, bool a_isActive,
            bool a_isPersistent)
        {
            BaseComponent* base = m_ecs.get_component<BaseComponent>(a_entityId);
            std::string previousName{};
            std::string previousTag{};
            const bool hadBaseComponent = base != nullptr;
            if (hadBaseComponent)
            {
                previousName = base->name;
                previousTag = base->tag;
            }

            if (base == nullptr)
            {
                base = m_ecs.add_component<BaseComponent>(a_entityId);
            }

            if (base == nullptr)
            {
                throw std::runtime_error("GameWorld failed to initialize BaseComponent.");
            }

            base->name = make_unique_object_name(a_name, a_entityId);
            base->tag = std::string(a_tag);
            base->owningSceneId = a_owningSceneId;
            base->parent = a_parent;
            base->isActiveSelf = a_isActive;
            base->isPersistent = a_isPersistent;

            if (hadBaseComponent && previousName != base->name)
            {
                remove_object_from_name_index(a_entityId, previousName);
            }

            if (hadBaseComponent && previousTag != base->tag)
            {
                remove_object_from_tag_index(a_entityId, previousTag);
            }

            add_object_to_name_index(a_entityId, base->name);
            add_object_to_tag_index(a_entityId, base->tag);
            m_ecs.set_entity_active(a_entityId, a_isActive);
        }

        [[nodiscard]] LoadSceneResult instantiate_into_scene(SceneId a_sceneId,
            std::span<const ObjectDefinition> a_objects,
            const SceneAsset* a_asset)
        {
            // ObjectDefinition 群を実 Entity として生成し、Scene に紐付ける。
            auto sceneIt = m_scenes.find(a_sceneId);
            if (sceneIt == m_scenes.end())
            {
                throw std::runtime_error("GameWorld scene was not found.");
            }

            SceneInstance& scene = sceneIt->second;
            if (scene.isPendingUnload)
            {
                throw std::runtime_error("GameWorld scene is pending unload.");
            }

            if (a_asset != nullptr)
            {
                scene.asset = a_asset;
            }

            LoadSceneResult result{};
            result.sceneId = a_sceneId;
            result.objects.reserve(a_objects.size());

            struct PendingObjectInstantiation final
            {
                const ObjectDefinition* definition = nullptr;
                LocalObjectId localObjectId = k_invalidLocalObjectId;
                EntityId entityId = k_invalidEntityId;
            };

            std::vector<PendingObjectInstantiation> pending{};
            pending.reserve(a_objects.size());
            std::unordered_map<LocalObjectId, EntityId> newLocalObjectToEntity{};
            newLocalObjectToEntity.reserve(a_objects.size());
            std::vector<EntityId> createdEntities{};
            createdEntities.reserve(a_objects.size());

            try
            {
                for (const ObjectDefinition& object : a_objects)
                {
                    LocalObjectId localObjectId = object.localObjectId;
                    if (localObjectId == k_invalidLocalObjectId)
                    {
                        localObjectId = scene.nextLocalObjectId++;
                    }
                    else
                    {
                        if (scene.localObjectToEntity.contains(localObjectId) ||
                            newLocalObjectToEntity.contains(localObjectId))
                        {
                            throw std::runtime_error("GameWorld localObjectId is duplicated in scene.");
                        }

                        scene.nextLocalObjectId = (std::max)(scene.nextLocalObjectId,
                            localObjectId + 1);
                    }

                    const EntityId entity =
                        create_entity_record(a_sceneId, localObjectId);
                    createdEntities.push_back(entity);

                    object.prototype.restore_components_into(entity, m_ecs);

                    const SceneId owningSceneId = object.isPersistent
                        ? k_invalidSceneId
                        : a_sceneId;
                    initialize_base_component(entity, object.name(), object.tag(),
                        owningSceneId, k_invalidEntityId, object.isActive,
                        object.isPersistent);

                    scene.entities.push_back(entity);
                    scene.localObjectToEntity.emplace(localObjectId, entity);
                    newLocalObjectToEntity.emplace(localObjectId, entity);

                    pending.push_back({&object, localObjectId, entity});
                    result.objects.push_back(make_handle(entity));
                }

                for (const PendingObjectInstantiation& entry : pending)
                {
                    if (!entry.definition->parentLocalObjectId.has_value())
                    {
                        continue;
                    }

                    const LocalObjectId parentLocalObjectId =
                        *entry.definition->parentLocalObjectId;
                    EntityId parentEntity = k_invalidEntityId;

                    if (const auto newIt =
                        newLocalObjectToEntity.find(parentLocalObjectId);
                        newIt != newLocalObjectToEntity.end())
                    {
                        parentEntity = newIt->second;
                    }
                    else if (const auto sceneLocalIt =
                        scene.localObjectToEntity.find(parentLocalObjectId);
                        sceneLocalIt != scene.localObjectToEntity.end())
                    {
                        parentEntity = sceneLocalIt->second;
                    }
                    else
                    {
                        throw std::runtime_error(
                            "GameWorld parentLocalObjectId could not be resolved.");
                    }

                    BaseComponent* base = get_component<BaseComponent>(entry.entityId);
                    if (base == nullptr)
                    {
                        throw std::runtime_error(
                            "GameWorld BaseComponent is missing while resolving parent.");
                    }

                    base->parent = parentEntity;
                }

                return result;
            }
            catch (...)
            {
                for (const EntityId entity : createdEntities)
                {
                    destroy_object_immediately(entity);
                }
                throw;
            }
        }

        void destroy_object_immediately(EntityId a_entityId) noexcept
        {
            EntityRecord* record = try_get_entity_record(a_entityId);
            if (record == nullptr || !record->isAlive)
            {
                return;
            }

            // flush 実行時と、例外時に即座に巻き戻す必要がある経路で使う。
            record->isPendingDestroy = false;

            const bool unlinked = unlink_object_from_scene(a_entityId);
            (void)unlinked;
            remove_object_from_name_index(a_entityId, get_object_name(a_entityId));
            remove_object_from_tag_index(a_entityId, get_object_tag(a_entityId));

            record->isAlive = false;
            record->sourceSceneId = k_invalidSceneId;
            record->sourceLocalObjectId = k_invalidLocalObjectId;
            ++record->generation;
            if (record->generation == 0)
            {
                record->generation = 1;
            }

            if (m_liveObjectCount > 0)
            {
                --m_liveObjectCount;
            }

            m_ecs.remove_entity(a_entityId);
        }

        [[nodiscard]] bool unload_scene_immediately(SceneId a_sceneId) noexcept
        {
            auto sceneIt = m_scenes.find(a_sceneId);
            if (sceneIt == m_scenes.end())
            {
                return false;
            }

            // 遅延状態を解除し、ここで実際の Scene アンロードを行う。
            sceneIt->second.isPendingUnload = false;

            const std::vector<EntityId> entities = sceneIt->second.entities;
            for (const EntityId entity : entities)
            {
                if (!contains_object(entity))
                {
                    continue;
                }

                BaseComponent* base = get_component<BaseComponent>(entity);
                if (base != nullptr && base->isPersistent)
                {
                    if (base->parent != k_invalidEntityId &&
                        source_scene_id(base->parent) == a_sceneId)
                    {
                        base->parent = k_invalidEntityId;
                    }
                    base->owningSceneId = k_invalidSceneId;

                    if (EntityRecord* record = try_get_entity_record(entity))
                    {
                        record->sourceSceneId = k_invalidSceneId;
                        record->sourceLocalObjectId = k_invalidLocalObjectId;
                    }
                    continue;
                }

                destroy_object_immediately(entity);
            }

            m_scenes.erase(sceneIt);
            return true;
        }

        [[nodiscard]] bool unlink_object_from_scene(EntityId a_entityId) noexcept
        {
            EntityRecord* record = try_get_entity_record(a_entityId);
            if (record == nullptr || record->sourceSceneId == k_invalidSceneId)
            {
                return false;
            }

            auto sceneIt = m_scenes.find(record->sourceSceneId);
            if (sceneIt == m_scenes.end())
            {
                return false;
            }

            std::vector<EntityId>& entities = sceneIt->second.entities;
            const auto entityIt =
                std::find(entities.begin(), entities.end(), a_entityId);
            if (entityIt != entities.end())
            {
                entities.erase(entityIt);
            }

            if (record->sourceLocalObjectId != k_invalidLocalObjectId)
            {
                sceneIt->second.localObjectToEntity.erase(record->sourceLocalObjectId);
            }

            return true;
        }

        [[nodiscard]] GameObject make_handle(EntityId a_entityId) noexcept
        {
            EntityRecord* record = try_get_entity_record(a_entityId);
            if (record == nullptr || !record->isAlive)
            {
                return {};
            }

            return GameObject(this, a_entityId, record->generation);
        }

        [[nodiscard]] EntityRecord* try_get_entity_record(
            EntityId a_entityId) noexcept
        {
            if (a_entityId >= m_entityRecords.size())
            {
                return nullptr;
            }

            return &m_entityRecords[a_entityId];
        }

        [[nodiscard]] const EntityRecord* try_get_entity_record(
            EntityId a_entityId) const noexcept
        {
            if (a_entityId >= m_entityRecords.size())
            {
                return nullptr;
            }

            return &m_entityRecords[a_entityId];
        }

        void add_object_to_tag_index(EntityId a_entityId, const std::string& a_tag)
        {
            m_tagIndex[a_tag].insert(a_entityId);
        }

        void add_object_to_name_index(EntityId a_entityId, const std::string& a_name)
        {
            m_nameIndex[a_name].insert(a_entityId);
        }

        [[nodiscard]] std::string normalize_object_name(
            std::string_view a_name) const
        {
            if (a_name.empty())
            {
                return "GameObject";
            }

            return std::string(a_name);
        }

        [[nodiscard]] bool is_name_taken(std::string_view a_name,
            EntityId a_ignoredEntityId = k_invalidEntityId) const
        {
            const auto it = m_nameIndex.find(std::string(a_name));
            if (it == m_nameIndex.end())
            {
                return false;
            }

            for (const EntityId entity : it->second)
            {
                if (entity == a_ignoredEntityId)
                {
                    continue;
                }
                if (contains_object(entity))
                {
                    return true;
                }
            }

            return false;
        }

        [[nodiscard]] std::string make_unique_object_name(
            std::string_view a_requestedName,
            EntityId a_ignoredEntityId = k_invalidEntityId) const
        {
            const std::string baseName = normalize_object_name(a_requestedName);
            if (!is_name_taken(baseName, a_ignoredEntityId))
            {
                return baseName;
            }

            std::uint32_t suffix = 1;
            while (true)
            {
                const std::string candidate =
                    baseName + "(" + std::to_string(suffix) + ")";
                if (!is_name_taken(candidate, a_ignoredEntityId))
                {
                    return candidate;
                }
                ++suffix;
            }
        }

        [[nodiscard]] bool try_get_name_series_index(const std::string& a_name,
            std::string_view a_baseName, std::uint32_t& a_outSeriesIndex) const
        {
            if (a_name == a_baseName)
            {
                a_outSeriesIndex = 0;
                return true;
            }

            if (!a_name.starts_with(a_baseName) || a_name.size() <= a_baseName.size() + 2)
            {
                return false;
            }

            if (a_name[a_baseName.size()] != '(' || a_name.back() != ')')
            {
                return false;
            }

            const size_t digitsBegin = a_baseName.size() + 1;
            const size_t digitsCount = a_name.size() - digitsBegin - 1;
            if (digitsCount == 0)
            {
                return false;
            }

            std::uint32_t seriesIndex = 0;
            for (size_t i = digitsBegin; i < a_name.size() - 1; ++i)
            {
                const unsigned char ch = static_cast<unsigned char>(a_name[i]);
                if (!std::isdigit(ch))
                {
                    return false;
                }

                seriesIndex =
                    (seriesIndex * 10u) + static_cast<std::uint32_t>(ch - '0');
            }

            a_outSeriesIndex = seriesIndex;
            return true;
        }

        void remove_object_from_tag_index(EntityId a_entityId,
            const std::string& a_tag)
        {
            const auto it = m_tagIndex.find(a_tag);
            if (it == m_tagIndex.end())
            {
                return;
            }

            it->second.erase(a_entityId);
            if (it->second.empty())
            {
                m_tagIndex.erase(it);
            }
        }

        void remove_object_from_name_index(EntityId a_entityId,
            const std::string& a_name)
        {
            const auto it = m_nameIndex.find(a_name);
            if (it == m_nameIndex.end())
            {
                return;
            }

            it->second.erase(a_entityId);
            if (it->second.empty())
            {
                m_nameIndex.erase(it);
            }
        }

        ECS::ECSManager m_ecs{};
        std::unordered_map<SceneId, SceneInstance> m_scenes{};
        std::unordered_map<std::string, std::unordered_set<EntityId>> m_nameIndex{};
        std::unordered_map<std::string, std::unordered_set<EntityId>> m_tagIndex{};
        std::vector<EntityRecord> m_entityRecords{};
        // 公開 API の遅延削除要求を一時的に保持するキュー。
        std::vector<EntityId> m_pendingDestroyedEntities{};
        std::vector<SceneId> m_pendingUnloadedScenes{};
        SceneId m_nextSceneId = 1;
        size_t m_liveObjectCount = 0;
    };

    inline GameObject::GameObject(GameWorld* a_world, EntityId a_entityId,
        Generation a_generation) noexcept
        : m_world(a_world),
          m_entityId(a_entityId),
          m_generation(a_generation)
    {}

    inline bool GameObject::is_valid() const noexcept
    {
        if (m_world == nullptr)
        {
            return false;
        }

        bool isAlive = false;
        const Result result = m_world->is_alive(m_entityId, m_generation, isAlive);
        return result && isAlive;
    }

    inline Result GameObject::name(std::string& a_outName) const
    {
        if (!is_valid())
        {
            a_outName.clear();
            return Result::fail(
                Code::InvalidState, Severity::Warning, "GameObject is not valid.");
        }

        return m_world->get_object_name(m_entityId, a_outName);
    }

    inline Result GameObject::set_name(std::string_view a_name)
    {
        if (!is_valid())
        {
            return Result::fail(
                Code::InvalidState, Severity::Warning, "GameObject is not valid.");
        }

        return m_world->set_object_name(m_entityId, a_name);
    }

    inline Result GameObject::tag(std::string& a_outTag) const
    {
        if (!is_valid())
        {
            a_outTag.clear();
            return Result::fail(
                Code::InvalidState, Severity::Warning, "GameObject is not valid.");
        }

        return m_world->get_object_tag(m_entityId, a_outTag);
    }

    inline Result GameObject::set_tag(std::string_view a_tag)
    {
        if (!is_valid())
        {
            return Result::fail(
                Code::InvalidState, Severity::Warning, "GameObject is not valid.");
        }

        return m_world->set_object_tag(m_entityId, a_tag);
    }

    inline Result GameObject::is_persistent(bool& a_outIsPersistent) const
    {
        if (!is_valid())
        {
            a_outIsPersistent = false;
            return Result::fail(
                Code::InvalidState, Severity::Warning, "GameObject is not valid.");
        }

        return m_world->is_object_persistent(m_entityId, a_outIsPersistent);
    }

    inline Result GameObject::set_persistent(bool a_isPersistent)
    {
        if (!is_valid())
        {
            return Result::fail(
                Code::InvalidState, Severity::Warning, "GameObject is not valid.");
        }

        return m_world->set_object_persistent(m_entityId, a_isPersistent);
    }

    template <typename T>
    inline Result GameObject::get_component(T*& a_outComponent) noexcept
    {
        if (!is_valid())
        {
            a_outComponent = nullptr;
            return Result::fail(
                Code::InvalidState, Severity::Warning, "GameObject is not valid.");
        }

        return m_world->get_component<T>(m_entityId, a_outComponent);
    }

    template <typename T, typename... Args>
    inline Result GameObject::add_component(T*& a_outComponent, Args&&... a_args)
    {
        if (!is_valid())
        {
            a_outComponent = nullptr;
            return Result::fail(
                Code::InvalidState, Severity::Warning, "GameObject is not valid.");
        }

        return m_world->add_component<T>(
            m_entityId, a_outComponent, std::forward<Args>(a_args)...);
    }

    template <typename T>
    inline Result GameObject::has_component(bool& a_outHasComponent) const noexcept
    {
        if (!is_valid())
        {
            a_outHasComponent = false;
            return Result::fail(
                Code::InvalidState, Severity::Warning, "GameObject is not valid.");
        }

        return m_world->has_component<T>(m_entityId, a_outHasComponent);
    }

    template <typename T>
    inline Result GameObject::remove_component() noexcept
    {
        if (!is_valid())
        {
            return Result::fail(
                Code::InvalidState, Severity::Warning, "GameObject is not valid.");
        }

        return m_world->remove_component<T>(m_entityId);
    }

    inline Result GameObject::destroy() noexcept
    {
        if (!is_valid())
        {
            return Result::fail(
                Code::InvalidState, Severity::Warning, "GameObject is not valid.");
        }

        return m_world->destroy_object(m_entityId);
    }
}
