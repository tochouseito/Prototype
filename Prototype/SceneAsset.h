#pragma once

#include "GameCoreTypes.h"
#include "GameObjectProto.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace Cue::GameCore
{
    struct ObjectDefinition final
    {
        LocalObjectId localObjectId = k_invalidLocalObjectId;
        std::optional<LocalObjectId> parentLocalObjectId{};
        bool isActive = true;
        bool isPersistent = false;
        GameObjectProto prototype{};

        ObjectDefinition() = default;

        explicit ObjectDefinition(std::string a_name, std::string a_tag = "Default")
            : prototype(std::move(a_name), std::move(a_tag))
        {}

        ObjectDefinition(LocalObjectId a_localObjectId, std::string a_name,
            std::string a_tag = "Default")
            : localObjectId(a_localObjectId),
              prototype(std::move(a_name), std::move(a_tag))
        {}

        [[nodiscard]] const std::string& name() const noexcept { return prototype.name(); }
        [[nodiscard]] const std::string& tag() const noexcept { return prototype.tag(); }
    };

    class SceneAsset final
    {
    public:
        SceneAsset() = default;
        explicit SceneAsset(std::string a_name) : m_name(std::move(a_name)) {}

        [[nodiscard]] const std::string& name() const noexcept { return m_name; }

        ObjectDefinition& add_object(std::string a_name,
            std::string a_tag = "Default")
        {
            ObjectDefinition object(generate_local_object_id(), std::move(a_name),
                std::move(a_tag));
            return add_object(std::move(object));
        }

        ObjectDefinition& add_object(ObjectDefinition a_object)
        {
            if (a_object.localObjectId == k_invalidLocalObjectId)
            {
                a_object.localObjectId = generate_local_object_id();
            }
            else if (m_objectIndexByLocalId.contains(a_object.localObjectId))
            {
                throw std::runtime_error("SceneAsset localObjectId is duplicated.");
            }
            else
            {
                m_nextLocalObjectId = (std::max)(m_nextLocalObjectId,
                    a_object.localObjectId + 1);
            }

            const size_t index = m_objects.size();
            m_objectIndexByLocalId.emplace(a_object.localObjectId, index);
            m_objects.push_back(std::move(a_object));
            return m_objects.back();
        }

        void add_game_object_proto(const std::string& a_name)
        {
            (void)add_object(a_name);
        }

        [[nodiscard]] ObjectDefinition* find_object(
            LocalObjectId a_localObjectId) noexcept
        {
            const auto it = m_objectIndexByLocalId.find(a_localObjectId);
            if (it == m_objectIndexByLocalId.end())
            {
                return nullptr;
            }

            return &m_objects[it->second];
        }

        [[nodiscard]] const ObjectDefinition* find_object(
            LocalObjectId a_localObjectId) const noexcept
        {
            const auto it = m_objectIndexByLocalId.find(a_localObjectId);
            if (it == m_objectIndexByLocalId.end())
            {
                return nullptr;
            }

            return &m_objects.at(it->second);
        }

        [[nodiscard]] std::vector<ObjectDefinition>& objects() noexcept
        {
            return m_objects;
        }

        [[nodiscard]] const std::vector<ObjectDefinition>& objects() const noexcept
        {
            return m_objects;
        }

    private:
        [[nodiscard]] LocalObjectId generate_local_object_id() noexcept
        {
            return m_nextLocalObjectId++;
        }

        std::string m_name{};
        std::vector<ObjectDefinition> m_objects{};
        std::unordered_map<LocalObjectId, size_t> m_objectIndexByLocalId{};
        LocalObjectId m_nextLocalObjectId = 1;
    };
}
