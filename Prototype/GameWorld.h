#pragma once

#include "BaseComponent.h"
#include "GameObject.h"
#include "SceneAsset.h"
#include "SceneInstance.h"

#include <algorithm>
#include <array>
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
                        destroy_object(entity);
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
            if (record == nullptr || !record->isAlive)
            {
                return;
            }

            const bool unlinked = unlink_object_from_scene(a_entityId);
            (void)unlinked;
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

        [[nodiscard]] bool unload_scene(SceneId a_sceneId) noexcept
        {
            auto sceneIt = m_scenes.find(a_sceneId);
            if (sceneIt == m_scenes.end())
            {
                return false;
            }

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

                destroy_object(entity);
            }

            m_scenes.erase(sceneIt);
            return true;
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

            return objects;
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
            if (base == nullptr)
            {
                base = m_ecs.add_component<BaseComponent>(a_entityId);
            }

            if (base == nullptr)
            {
                throw std::runtime_error("GameWorld failed to initialize BaseComponent.");
            }

            base->name = std::string(a_name);
            base->tag = std::string(a_tag);
            base->owningSceneId = a_owningSceneId;
            base->parent = a_parent;
            base->isActiveSelf = a_isActive;
            base->isPersistent = a_isPersistent;

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
                    destroy_object(entity);
                }
                throw;
            }
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

        ECS::ECSManager m_ecs{};
        std::unordered_map<SceneId, SceneInstance> m_scenes{};
        std::unordered_map<std::string, std::unordered_set<EntityId>> m_tagIndex{};
        std::vector<EntityRecord> m_entityRecords{};
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
