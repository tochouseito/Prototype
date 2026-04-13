#pragma once

#include "BaseComponent.h"
#include "GameObject.h"
#include "SceneAsset.h"
#include "SceneInstance.h"

#include <algorithm>
#include <array>
#include <cctype>
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
            bool isPendingDestroy = false;
            SceneId sourceSceneId = k_invalidSceneId;
            LocalObjectId sourceLocalObjectId = k_invalidLocalObjectId;
        };

        GameWorld() = default;

        [[nodiscard]] ECS::ECSManager& ecs() noexcept { return m_ecs; }

        [[nodiscard]] GameObject create_object(std::string_view a_name,
            std::string_view a_tag = "Default", bool a_isPersistent = false)
        {
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

        [[nodiscard]] LoadSceneResult append_to_scene(SceneId a_sceneId,
            const std::vector<ObjectDefinition>& a_objects)
        {
            return append_to_scene(a_sceneId, std::span<const ObjectDefinition>(a_objects));
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

        void destroy_object(EntityId a_entityId) noexcept
        {
            EntityRecord* record = try_get_entity_record(a_entityId);
            if (record == nullptr || !record->isAlive || record->isPendingDestroy)
            {
                return;
            }

            record->isPendingDestroy = true;
            m_pendingDestroyedEntities.push_back(a_entityId);
        }

        [[nodiscard]] bool unload_scene(SceneId a_sceneId) noexcept
        {
            auto sceneIt = m_scenes.find(a_sceneId);
            if (sceneIt == m_scenes.end() || sceneIt->second.isPendingUnload)
            {
                return false;
            }

            sceneIt->second.isPendingUnload = true;
            m_pendingUnloadedScenes.push_back(a_sceneId);
            return true;
        }

        void execute_deferred_deletions() noexcept
        {
            std::vector<SceneId> pendingScenes{};
            pendingScenes.swap(m_pendingUnloadedScenes);
            for (const SceneId sceneId : pendingScenes)
            {
                (void)unload_scene_immediately(sceneId);
            }

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

        void set_object_name(EntityId a_entityId, std::string_view a_name)
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
        }

        void set_object_tag(EntityId a_entityId, std::string_view a_tag)
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

        [[nodiscard]] size_t object_count() const noexcept { return m_liveObjectCount; }
        [[nodiscard]] size_t scene_count() const noexcept { return m_scenes.size(); }

        void clear() noexcept
        {
            std::vector<SceneId> sceneIds{};
            sceneIds.reserve(m_scenes.size());
            for (const auto& [sceneId, _] : m_scenes)
            {
                sceneIds.push_back(sceneId);
            }

            for (const SceneId sceneId : sceneIds)
            {
                (void)unload_scene(sceneId);
            }

            for (EntityId entity = 0;
                entity < static_cast<EntityId>(m_entityRecords.size()); ++entity)
            {
                if (contains_object(entity))
                {
                    destroy_object(entity);
                }
            }

            execute_deferred_deletions();
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

        template <typename T>
        void remove_component(EntityId a_entityId) noexcept
        {
            if (!contains_object(a_entityId))
            {
                return;
            }

            m_ecs.remove_component<T>(a_entityId);
        }

        template <class F>
        [[nodiscard]] bool visit_object(EntityId a_entityId, F&& a_func)
        {
            GameObject object = find_object(a_entityId);
            if (!object.is_valid())
            {
                return false;
            }

            a_func(object.entity_id(), source_scene_id(a_entityId), object);
            return true;
        }

        template <class F>
        [[nodiscard]] bool for_each_object_in_scene(SceneId a_sceneId, F&& a_func)
        {
            auto sceneIt = m_scenes.find(a_sceneId);
            if (sceneIt == m_scenes.end())
            {
                return false;
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

            return true;
        }

        template <class F>
        void for_each_object(F&& a_func)
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

        [[nodiscard]] bool destroy_object_by_name(std::string_view a_name) noexcept
        {
            const GameObject object = find_object_by_name(a_name);
            if (!object.is_valid())
            {
                return false;
            }

            destroy_object(object.entity_id());
            return true;
        }

        [[nodiscard]] size_t destroy_objects_by_name(
            std::string_view a_name) noexcept
        {
            const std::vector<GameObject> objects = find_objects_by_name(a_name);
            for (const GameObject& object : objects)
            {
                destroy_object(object.entity_id());
            }

            return objects.size();
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

        [[nodiscard]] size_t destroy_objects_by_name_series(
            std::string_view a_baseName) noexcept
        {
            const std::vector<GameObject> objects =
                find_objects_by_name_series(a_baseName);
            for (const GameObject& object : objects)
            {
                destroy_object(object.entity_id());
            }

            return objects.size();
        }

        [[nodiscard]] size_t destroy_objects_by_tag(
            std::string_view a_tag) noexcept
        {
            const std::vector<GameObject> objects = find_objects_by_tag(a_tag);
            for (const GameObject& object : objects)
            {
                destroy_object(object.entity_id());
            }

            return objects.size();
        }

    private:
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
        return m_world != nullptr && m_world->is_alive(m_entityId, m_generation);
    }

    inline std::string GameObject::name() const
    {
        if (!is_valid())
        {
            return {};
        }

        return m_world->get_object_name(m_entityId);
    }

    inline void GameObject::set_name(std::string_view a_name)
    {
        if (!is_valid())
        {
            throw std::runtime_error("GameObject is not valid.");
        }

        m_world->set_object_name(m_entityId, a_name);
    }

    inline std::string GameObject::tag() const
    {
        if (!is_valid())
        {
            return {};
        }

        return m_world->get_object_tag(m_entityId);
    }

    inline void GameObject::set_tag(std::string_view a_tag)
    {
        if (!is_valid())
        {
            throw std::runtime_error("GameObject is not valid.");
        }

        m_world->set_object_tag(m_entityId, a_tag);
    }

    template <typename T>
    inline T* GameObject::get_component() noexcept
    {
        if (!is_valid())
        {
            return nullptr;
        }

        return m_world->get_component<T>(m_entityId);
    }

    template <typename T, typename... Args>
    inline T& GameObject::add_component(Args&&... a_args)
    {
        if (!is_valid())
        {
            throw std::runtime_error("GameObject is not valid.");
        }

        return m_world->add_component<T>(m_entityId, std::forward<Args>(a_args)...);
    }

    template <typename T>
    inline bool GameObject::has_component() const noexcept
    {
        if (!is_valid())
        {
            return false;
        }

        return m_world->has_component<T>(m_entityId);
    }

    template <typename T>
    inline void GameObject::remove_component() noexcept
    {
        if (!is_valid())
        {
            return;
        }

        m_world->remove_component<T>(m_entityId);
    }

    inline void GameObject::destroy() noexcept
    {
        if (!is_valid())
        {
            return;
        }

        m_world->destroy_object(m_entityId);
    }
}
