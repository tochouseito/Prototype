#pragma once

// === C++ includes ===
#include <algorithm>
#include <bitset>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <vector>

// === ECS includes ===
#include "ComponentIDHelper.h"

namespace Cue::ECS
{
    using Entity = uint32_t;
    using CompID = size_t;
    using Archetype = std::bitset<256>;

    // コンポーネントだと判別するためのタグ
    struct IComponentTag
    {
        virtual ~IComponentTag() = default; // 仮想デストラクタを定義

        // 初期化関数
        virtual void initialize() {}

        // アクティブ状態
        bool is_active() const noexcept { return active; }
        void set_active(bool a_active) noexcept { active = a_active; }

    private:
        bool active = true; // アクティブ状態
    };

    // コンポーネントが複数持てるか(デフォルトは持てない)
    template <typename T> struct IsMultiComponent : std::false_type {};

    // コンポーネント型のみ許可する
    template <typename T>
    concept ComponentType = std::derived_from<T, IComponentTag>;

    // ECS用ヘルパー
    template <typename C>
    concept HasInitialize = requires(C & a_c) { a_c.initialize(); };

    struct AwakeContext final {};
    struct InitializeContext final {};
    struct UpdateContext final
    {
        uint32_t bufferIndex = 0;
    };
    struct FinalizeContext final {};

    struct IIComponentEventListener
    {
        // ECS側から呼ばれる
        virtual void on_component_added(Entity a_e, CompID a_compType) = 0;
        virtual void on_component_copied(Entity a_src, Entity a_dst,
            CompID a_compType) = 0;
        virtual void on_component_removed(Entity a_e, CompID a_compType) = 0;
        virtual void on_component_removed_instance(Entity a_e, CompID a_compType,
            void* a_rawVec, size_t a_idx) = 0;
        // Prefabからの復元時に呼ばれる
        virtual void on_component_restored_from_prefab(Entity, CompID) {}
    };

    struct IEntityEventListener
    {
        virtual ~IEntityEventListener() = default;

        /// e が新しく生成された直後に呼ばれる
        virtual void on_entity_created(Entity a_e) = 0;

        /// e のすべてのコンポーネントがクリアされ、RecycleQueue
        /// に入った直後に呼ばれる
        virtual void on_entity_destroyed(Entity a_e) = 0;
    };

    class ECSManager
    {
        friend class Prototype;

        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

    public:
        /*---------------------------------------------------------------------
                エンティティ管理
            ---------------------------------------------------------------------*/
        ECSManager() = default;
        ~ECSManager() = default;

        // 公開API
        [[nodiscard]]
        bool is_entity_active(Entity a_e) const
        {
            return a_e < m_entityToActive.size() && m_entityToActive[a_e];
        }

        void set_entity_active(Entity a_e, bool a_f)
        {
            if (a_e < m_entityToActive.size())
            {
                m_entityToActive[a_e] = a_f;
            }
        }

        /*-------------------- Entity create / recycle ----------------------*/
        [[nodiscard]]
        inline const Entity generate_entity()
        {
            Entity entity =
                m_recycleEntities.empty() ? ++m_nextEntityId : m_recycleEntities.back();
            if (!m_recycleEntities.empty())
            {
                m_recycleEntities.pop_back();
            }

            // アクティブ化
            if (m_entityToActive.size() <= entity)
            {
                m_entityToActive.resize(entity + 1, false);
            }
            m_entityToActive[entity] = true;

            // Archetype 配列も拡張
            if (m_entityToArchetype.size() <= entity)
            {
                m_entityToArchetype.resize(entity + 1);
            }

            // ① 空のアーキタイプ（全ビットfalse）にも必ず登録しておく
            {
                Archetype emptyArch; // 全ビットfalse
                m_archToEntities[emptyArch].add(entity);
            }

            // ② エンティティ生成イベントを通知（更新中なら遅延）
            auto notify = [this, entity]() {
                for (auto& wp : m_entityListeners)
                {
                    if (auto sp = wp.lock())
                    {
                        sp->on_entity_created(entity);
                    }
                }
                };
            notify();

            if (m_isUpdating)
            {
                // Staging に積むだけ（本体に反映しない）
                m_stagingEntities.push_back(entity);
                m_stagingEntityActive.push_back(true);
                m_stagingEntityArchetypes.push_back(Archetype{});
            }
            else
            {
                // 通常通り即時反映
                if (m_entityToActive.size() <= entity)
                {
                    m_entityToActive.resize(entity + 1, false);
                }
                m_entityToActive[entity] = true;

                if (m_entityToArchetype.size() <= entity)
                {
                    m_entityToArchetype.resize(entity + 1);
                }
                m_entityToArchetype[entity] = Archetype{};

                // m_ArchToEntities[Archetype{}].Add(entity);
            }

            return entity;
        }

        /*-------------------- Clear all components ------------------------*/
        inline void clear_entity(const Entity& a_e)
        {
            if (a_e >= m_entityToArchetype.size())
            {
                return;
            }
            Archetype old = m_entityToArchetype[a_e];

            // ① 削除対象の CompID を collect
            std::vector<CompID> toRemove;
            for (CompID id = 0; id < old.size(); ++id)
            {
                if (old.test(id))
                {
                    toRemove.push_back(id);
                }
            }

            // ② 優先度→CompID でソート
            std::sort(toRemove.begin(), toRemove.end(), [&](CompID a_a, CompID a_b) {
                int pa = m_deletePriority.count(a_a) ? m_deletePriority[a_a] : 0;
                int pb = m_deletePriority.count(a_b) ? m_deletePriority[a_b] : 0;
                if (pa != pb)
                {
                    return pa < pb;
                }
                return a_a < a_b;
                });

            // ③ ソート後の順で通知＆削除
            for (CompID id : toRemove)
            {
                auto* pool = m_typeToComponents[id].get();
                size_t cnt = pool->get_component_count(a_e);

                const FinalizeContext finalizeContext{};
                for (auto& up : m_systems)
                {
                    up->finalize_entity(a_e, finalizeContext);
                }

                if (pool->is_multi_component_trait(id))
                {
                    void* raw = pool->get_raw_component(a_e);
                    for (size_t i = 0; i < cnt; ++i)
                    {
                        notify_component_removed_instance(a_e, id, raw, i);
                    }
                }
                else
                {
                    notify_component_removed(a_e, id);
                }
                pool->cleanup(a_e); // クリーンアップ呼び出し
                pool->remove_component(a_e);
            }

            // バケット・アーキタイプクリア
            m_archToEntities[old].remove(a_e);
            m_entityToArchetype[a_e].reset();
        }

        // ――――――――――――――――――
        //  他のシステムやメインループから呼んでよい、
        //  更新中も安全に追加できる汎用 Defer
        // ――――――――――――――――――
        void defer(std::function<void()> a_cmd)
        {
            m_deferredCommands.push_back(std::move(a_cmd));
        }

        /*-------------------- Disable entity ------------------------------*/
        inline void remove_entity(const Entity& a_e)
        {
            if (m_isUpdating)
            {
                // システム更新中なら遅延キューに積む
                defer([this, a_e] { remove_entity_impl(a_e); });
            }
            else
            {
                // 通常フレームなら即時実行
                remove_entity_impl(a_e);
            }
        }

        /*-------------------- Copy whole entity ---------------------------*/
        [[nodiscard]]
        Entity copy_entity(const Entity& a_src)
        {
            // ① 元のアーキタイプを取得
            const Archetype arch = get_archetype(a_src);

            // ② 生成（ステージングなら GenerateEntity
            // もステージングに入るよう修正済み）
            Entity dst = generate_entity();

            // ── コピーする CompID を集めて優先度順にソート ──
            std::vector<CompID> toCopy;
            for (CompID id = 0; id < arch.size(); ++id)
            {
                if (arch.test(id))
                {
                    toCopy.push_back(id);
                }
            }

            std::sort(toCopy.begin(), toCopy.end(), [&](CompID a_a, CompID a_b) {
                int pa = m_copyPriority.count(a_a) ? m_copyPriority[a_a] : 0;
                int pb = m_copyPriority.count(a_b) ? m_copyPriority[a_b] : 0;
                if (pa != pb)
                {
                    return pa < pb;
                }
                return a_a < a_b;
                });

            // ③ 優先度順にしてからコピー＆通知
            for (CompID id : toCopy)
            {
                auto* pool = m_typeToComponents[id].get();
                if (m_isUpdating)
                {
                    pool->copy_component_staging(a_src, dst);
                    size_t idx = staging_index_for_entity(dst);
                    m_stagingEntityArchetypes[idx].set(id);
                }
                else
                {
                    // 通常フレーム：即時コピー
                    pool->copy_component(a_src, dst);

                    // → ここで本番バッファにもビットを立てる
                    if (m_entityToArchetype.size() <= dst)
                    {
                        m_entityToArchetype.resize(dst + 1);
                    }
                    /* Archetype& archDst = m_EntityToArchetype[dst];
                     if (!archDst.test(id))
                     {
                         m_ArchToEntities[archDst].Remove(dst);
                         archDst.set(id);
                         m_ArchToEntities[archDst].Add(dst);
                     }*/
                    m_entityToArchetype[dst].set(id);
                }
                notify_component_copied(a_src, dst, id);
            }

            // ④ アーキタイプの反映
            if (m_isUpdating)
            {
                const AwakeContext awakeContext{};
                for (auto& sys : m_systems)
                {
                    sys->awake_entity(dst, awakeContext);
                }
            }
            else
            {
                // 即時反映
                /*if (m_EntityToArchetype.size() <= dst)
                    m_EntityToArchetype.resize(dst + 1);
                m_EntityToArchetype[dst] = arch;
                m_ArchToEntities[arch].Add(dst);*/
                Archetype oldArch = Archetype{};
                Archetype& newArch = m_entityToArchetype[dst];
                if (oldArch != newArch)
                {
                    m_archToEntities[oldArch].remove(dst);
                    m_archToEntities[newArch].add(dst);
                }
            }

            return dst;
        }

        //-------------------- Copy into existing entity --------------------
        void copy_entity(const Entity& a_src, const Entity& a_dst)
        {
            // ① ソースのアーキタイプを取得
            const Archetype arch = get_archetype(a_src);

            Archetype oldArch = m_isUpdating ? Archetype{} : get_archetype(a_dst);

            // ── コピーする CompID を集めて優先度順にソート ──
            std::vector<CompID> toCopy;
            for (CompID id = 0; id < arch.size(); ++id)
            {
                if (arch.test(id))
                {
                    toCopy.push_back(id);
                }
            }

            std::sort(toCopy.begin(), toCopy.end(), [&](CompID a_a, CompID a_b) {
                int pa = m_copyPriority.count(a_a) ? m_copyPriority[a_a] : 0;
                int pb = m_copyPriority.count(a_b) ? m_copyPriority[a_b] : 0;
                if (pa != pb)
                {
                    return pa < pb;
                }
                return a_a < a_b;
                });

            // ③ 優先度順にしてからコピー＆通知
            for (CompID id : toCopy)
            {
                auto* pool = m_typeToComponents[id].get();
                if (m_isUpdating)
                {
                    pool->copy_component_staging(a_src, a_dst);
                    size_t idx = staging_index_for_entity(a_dst);
                    m_stagingEntityArchetypes[idx].set(id);
                }
                else
                {
                    // 通常フレーム：即時コピー
                    pool->copy_component(a_src, a_dst);

                    // → ここで本番バッファにもビットを立てる
                    if (m_entityToArchetype.size() <= a_dst)
                    {
                        m_entityToArchetype.resize(a_dst + 1);
                    }
                    m_entityToArchetype[a_dst].set(id);
                }
                notify_component_copied(a_src, a_dst, id);
            }

            // ④ アーキタイプの反映
            if (m_isUpdating)
            {
                const AwakeContext awakeContext{};
                for (auto& sys : m_systems)
                {
                    sys->awake_entity(a_dst, awakeContext);
                }
            }
            else
            {
                // 即時反映
                /*if (m_EntityToArchetype.size() <= dst)
                    m_EntityToArchetype.resize(dst + 1);
                m_EntityToArchetype[dst] = arch;
                m_ArchToEntities[arch].Add(dst);*/
                Archetype& newArch = m_entityToArchetype[a_dst];
                if (oldArch != newArch)
                {
                    m_archToEntities[oldArch].remove(a_dst);
                    m_archToEntities[newArch].add(a_dst);
                }
            }
        }

        /// Entity e のステージングバッファ内インデックスを返す。
        /// なければ追加して新しいインデックスを返す。
        size_t staging_index_for_entity(Entity a_e)
        {
            // 1) 既存エントリがあればその位置を返す
            auto it =
                std::find(m_stagingEntities.begin(), m_stagingEntities.end(), a_e);
            if (it != m_stagingEntities.end())
            {
                return static_cast<size_t>(std::distance(m_stagingEntities.begin(), it));
            }

            // 2) なければバッファ末尾に追加
            size_t newIndex = m_stagingEntities.size();
            m_stagingEntities.push_back(a_e);

            // デフォルト active=true, archetype は空ビットセットで追加
            m_stagingEntityActive.push_back(true);
            m_stagingEntityArchetypes.push_back(Archetype{});

            return newIndex;
        }

        /*-------------------- Copy selected components --------------------*/
        void copy_components(Entity a_src, Entity a_dst, bool a_overwrite = true)
        {
            const Archetype& archSrc = get_archetype(a_src);

            // コピー先アーキタイプへの参照取得（ステージング中かどうかで使い分け）
            Archetype* pArchDst;
            Archetype oldArch;
            if (m_isUpdating)
            {
                // dst がステージングリストにあればそのインデックスを取得、
                // なければ追加してからインデックスを取得
                size_t idx = staging_index_for_entity(a_dst);
                pArchDst = &m_stagingEntityArchetypes[idx];
            }
            else
            {
                if (m_entityToArchetype.size() <= a_dst)
                {
                    m_entityToArchetype.resize(a_dst + 1);
                }
                pArchDst = &m_entityToArchetype[a_dst];
            }
            oldArch = *pArchDst;

            // ソースのビットセットを走査
            for (CompID id = 0; id < archSrc.size(); ++id)
            {
                if (!archSrc.test(id))
                {
                    continue;
                }
                if (!a_overwrite && pArchDst->test(id))
                {
                    continue;
                }

                auto* pool = m_typeToComponents[id].get();
                if (m_isUpdating)
                {
                    // フレーム中はステージングコピー
                    pool->copy_component_staging(a_src, a_dst);
                }
                else
                {
                    // フレーム外は即時コピー
                    pool->copy_component(a_src, a_dst);
                    notify_component_copied(a_src, a_dst, id);
                }

                // アーキタイプのビットをセット
                pArchDst->set(id);
            }

            // 即時フレーム外ならバケット（EntityContainer）も更新
            if (!m_isUpdating && *pArchDst != oldArch)
            {
                m_archToEntities[oldArch].remove(a_dst);
                m_archToEntities[*pArchDst].add(a_dst);
            }
        }

        /*-------------------- Add component -------------------------------*/
        template <ComponentType T> T* add_component(const Entity& a_entity)
        {
            CompID type = ComponentPool<T>::get_id();

            // プール取得 or 作成
            auto [it, _] = m_typeToComponents.try_emplace(
                type, std::make_shared<ComponentPool<T>>(4096));
            auto pool = std::static_pointer_cast<ComponentPool<T>>(it->second);

            // 更新中ならステージングに追加して即時参照可能にする
            if (m_isUpdating)
            {
                T* comp = pool->add_component_staging(
                    a_entity); // ← 後述の新関数で staging に追加

                // Archetype は staging にも保持しておく（後で FlushStagingEntities
                // で反映）
                if (std::find(m_stagingEntities.begin(), m_stagingEntities.end(),
                    a_entity) == m_stagingEntities.end())
                {
                    // Entity がまだ Staging に登録されていなければ追加
                    m_stagingEntities.push_back(a_entity);
                    m_stagingEntityActive.push_back(true);
                    m_stagingEntityArchetypes.push_back(Archetype{});
                }

                // Archetype を更新（set type ビット）
                for (size_t i = 0; i < m_stagingEntities.size(); ++i)
                {
                    if (m_stagingEntities[i] == a_entity)
                    {
                        m_stagingEntityArchetypes[i].set(type);
                        break;
                    }
                }

                if constexpr (HasInitialize<T>)
                {
                    comp->initialize();
                }
                // comp->SetOwner(entity);
                // comp->SetECSManager(this);
                notify_component_added(a_entity, type);

                return comp; // 即時取得可能
            }

            // 通常フロー（即時反映）
            T* comp = pool->add_component(a_entity);
            // comp->SetOwner(entity);
            // comp->SetECSManager(this);

            if (m_entityToArchetype.size() <= a_entity)
            {
                m_entityToArchetype.resize(a_entity + 1, Archetype{});
            }
            Archetype& arch = m_entityToArchetype[a_entity];
            if (!arch.test(type))
            {
                m_archToEntities[arch].remove(a_entity);
                arch.set(type);
                m_archToEntities[arch].add(a_entity);
            }

            if constexpr (HasInitialize<T>)
            {
                comp->initialize();
            }
            notify_component_added(a_entity, type);

            return comp;
        }

        /// Prefab復元時だけ使う、イベントを起こさずコンポーネントを追加
        template <ComponentType T>
        void prefab_add_component(Entity a_e, T const& a_comp)
        {
            CompID type = ComponentPool<T>::get_id();

            // プール取得または生成
            auto [it, _] = m_typeToComponents.try_emplace(
                type, std::make_shared<ComponentPool<T>>(4096));
            auto pool = std::static_pointer_cast<ComponentPool<T>>(it->second);

            if (m_isUpdating)
            {
                // ▼ ステージングに即時追加
                pool->add_prefab_component_staging(a_e, a_comp);
                // T* pComp = pool->GetComponent(e);
                // pComp->SetOwner(e);
                // pComp->SetECSManager(this);

                // ▼ Archetype も staging に反映
                auto itE =
                    std::find(m_stagingEntities.begin(), m_stagingEntities.end(), a_e);
                if (itE != m_stagingEntities.end())
                {
                    size_t i = std::distance(m_stagingEntities.begin(), itE);
                    m_stagingEntityArchetypes[i].set(type);
                }
                else
                {
                    m_stagingEntities.push_back(a_e);
                    m_stagingEntityActive.push_back(true);
                    Archetype arch{};
                    arch.set(type);
                    m_stagingEntityArchetypes.push_back(arch);
                }

                return; // 遅延不要
            }

            // ▼ 通常の即時追加（更新中でない場合）
            pool->add_prefab_component(a_e, a_comp);
            // T* pComp = pool->GetComponent(e);
            // pComp->SetOwner(e);
            // pComp->SetECSManager(this);

            if (m_entityToArchetype.size() <= a_e)
            {
                m_entityToArchetype.resize(a_e + 1, Archetype{});
            }
            Archetype& arch = m_entityToArchetype[a_e];

            if (!arch.test(type))
            {
                m_archToEntities[arch].remove(a_e);
                arch.set(type);
                m_archToEntities[arch].add(a_e);
            }

            notify_component_restored_from_prefab(a_e, ComponentPool<T>::get_id());
        }

        /*-------------------- Get component -------------------------------*/
        template <ComponentType T> T* get_component(const Entity& a_entity)
        {
            CompID type = ComponentPool<T>::get_id();
            bool has = false;

            // ステージング中はステージングとメイン両方をチェック
            if (m_isUpdating)
            {
                // メイン側チェック
                if (a_entity < m_entityToArchetype.size() &&
                    m_entityToArchetype[a_entity].test(type))
                {
                    has = true;
                }
                else
                {
                    // ステージングバッファ内のアーキタイプをチェック
                    auto it = std::find(m_stagingEntities.begin(), m_stagingEntities.end(),
                        a_entity);
                    if (it != m_stagingEntities.end())
                    {
                        size_t idx = it - m_stagingEntities.begin();
                        has = m_stagingEntityArchetypes[idx].test(type);
                    }
                }
            }
            else
            {
                // 通常フレームならメインのみチェック
                has = (a_entity < m_entityToArchetype.size() &&
                    m_entityToArchetype[a_entity].test(type));
            }

            if (!has)
            {
                return nullptr;
            }

            // プールから取得し、ステージング込みの get_component を呼ぶ
            auto pool = static_cast<ComponentPool<T> *>(get_raw_component_pool(type));
            return pool->get_component(a_entity);
        }

        /*-------------------- Get all (multi) -----------------------------*/
        template <ComponentType T>
        std::vector<T>* get_all_components(const Entity& a_entity)
            requires IsMultiComponent<T>::value
        {
            auto it = m_typeToComponents.find(ComponentPool<T>::get_id());
            if (it == m_typeToComponents.end())
            {
                return nullptr;
            }
            auto pool = std::static_pointer_cast<ComponentPool<T>>(it->second);
            return pool->get_all_components(a_entity);
        }

        /*-------------------- Remove component ----------------------------*/
        template <ComponentType T> void remove_component(const Entity& a_entity)
        {
            if (m_isUpdating)
            {
                defer([this, a_entity]() { remove_component<T>(a_entity); });
                return;
            }

            static_assert(!IsMultiComponent<T>::value,
                "Use RemoveAllComponents for multi-instance.");
            CompID type = ComponentPool<T>::get_id();
            if (a_entity >= m_entityToArchetype.size() ||
                !m_entityToArchetype[a_entity].test(type))
            {
                return;
            }
            auto pool = get_component_pool<T>();
            if (!pool)
            {
                return;
            }

            // ② 対応するシステムに「このコンポーネントを削除する前のFinalize」を呼ぶ
            const FinalizeContext finalizeContext{};
            for (auto& up : m_systems)
            {
                if (auto sys = dynamic_cast<System<T> *>(up.get()))
                {
                    sys->finalize_entity(a_entity, finalizeContext);
                }
            }

            notify_component_removed(a_entity, type);
            pool->remove_component(a_entity);

            Archetype& arch = m_entityToArchetype[a_entity];
            m_archToEntities[arch].remove(a_entity);
            arch.reset(type);
            m_archToEntities[arch].add(a_entity);
        }

        /*-------------------- Remove ALL multi ----------------------------*/
        template <ComponentType T>
        void remove_all_components(const Entity& a_entity)
            requires IsMultiComponent<T>::value
        {
            if (m_isUpdating)
            {
                defer([this, a_entity]() { remove_all_components<T>(a_entity); });
                return;
            }

            auto* pool = get_component_pool<T>();
            if (!pool)
            {
                return;
            }

            // マルチ用 finalize 呼び
            const FinalizeContext finalizeContext{};
            for (auto& up : m_systems)
            {
                if (auto ms = dynamic_cast<MultiComponentSystem<T> *>(up.get()))
                {
                    ms->finalize_entity(a_entity, finalizeContext);
                }
            }

            notify_component_removed(a_entity, ComponentPool<T>::get_id());
            pool->remove_all(a_entity);

            Archetype& arch = m_entityToArchetype[a_entity];
            arch.reset(ComponentPool<T>::get_id());
            m_archToEntities[arch].remove(a_entity);
            m_archToEntities[arch].add(a_entity);
        }

        // マルチコンポーネントの単一インスタンスを消す
        template <ComponentType T>
        void remove_component_instance(const Entity& a_e, size_t a_index)
            requires IsMultiComponent<T>::value
        {
            if (m_isUpdating)
            {
                defer([this, a_e, a_index]() {
                    remove_component_instance<T>(a_e, a_index);
                    });
                return;
            }

            CompID id = ComponentPool<T>::get_id();
            auto* pool = get_component_pool<T>();
            void* rawVec = pool ? pool->get_raw_component(a_e) : nullptr;
            if (pool)
            {
                auto* vec = static_cast<std::vector<T> *>(rawVec);
                if (vec && a_index < vec->size())
                {
                    notify_component_removed_instance(a_e, id, rawVec, a_index);
                }
            }

            if (pool)
            {
                pool->remove_instance(a_e, a_index);
            }

            auto& arch = m_entityToArchetype[a_e];
            auto* remaining = pool ? pool->get_all_components(a_e) : nullptr;
            if (!remaining || remaining->empty())
            {
                m_archToEntities[arch].remove(a_e);
                arch.reset(id);
                m_archToEntities[arch].add(a_e);
            }
        }

        // コンポーネント別に「削除時の優先度」を設定できるように
        template <ComponentType T> void set_deletion_priority(int a_priority)
        {
            m_deletePriority[ComponentPool<T>::get_id()] = a_priority;
        }

        // コピー時の優先度を設定するテンプレート関数
        template <ComponentType T> void set_copy_priority(int a_priority)
        {
            m_copyPriority[ComponentPool<T>::get_id()] = a_priority;
        }

        /*-------------------- Accessors ----------------------------------*/
        inline const Archetype& get_archetype(Entity a_e) const
        {
            static Archetype empty{};
            return (a_e < m_entityToArchetype.size()) ? m_entityToArchetype[a_e]
                : empty;
        }

        /// コンポーネントイベントリスナーを登録（shared_ptr で受け取る）
        void
            add_component_listener(std::shared_ptr<IIComponentEventListener> a_listener)
        {
            m_componentListeners.emplace_back(a_listener);
        }

        /// エンティティイベントリスナーを登録（shared_ptr で受け取る）
        void add_entity_listener(std::shared_ptr<IEntityEventListener> a_listener)
        {
            m_entityListeners.emplace_back(a_listener);
        }
        /// (オプション) 全リスナーをクリア
        void clear_component_listeners() { m_componentListeners.clear(); }
        void clear_entity_listeners() { m_entityListeners.clear(); }

        // ――――――――――――――――――
        //  システム登録
        // ――――――――――――――――――
        template <typename S, typename... Args> S& add_system(Args &&...a_args)
        {
            static_assert(std::is_base_of_v<ISystem, S>, "S must derive from ISystem");
            auto ptr = std::make_unique<S>(std::forward<Args>(a_args)...);
            S* raw = ptr.get();
            // 所属する ECSManager のポインタを渡す
            raw->on_register(this);
            m_systems.push_back(std::move(ptr));
            return *raw;
        }
        template <typename S> S* get_system()
        {
            for (auto& up : m_systems)
            {
                if (auto p = dynamic_cast<S*>(up.get()))
                {
                    return p;
                }
            }
            return nullptr;
        }

        // 更新ループを中止するAPI
        void cancel_update_loop() { m_cancelUpdate = true; }

        // ① ゲーム開始前に一度だけ
        void initialize_all_systems() { initialize_all_systems(InitializeContext{}); }

        void initialize_all_systems(const InitializeContext& a_context)
        {
            m_isUpdating = true;
            m_cancelUpdate = false; // ← 毎フレーム最初にリセット
            const TimePoint t0Total = capture_now();

            sort_systems_by_priority();
            for (auto& sys : m_systems)
            {
                if (sys->is_enabled())
                {
                    const TimePoint t0 = capture_now();
                    sys->initialize(a_context);
                    const TimePoint t1 = capture_now();
                    std::type_index ti(typeid(*sys));
                    m_lastSystemInitializeTimeMs[ti] = elapsed_ms(t0, t1);
                }
            }

            m_isUpdating = false;
            m_cancelUpdate = false;

            const TimePoint t1Total = capture_now();
            m_lastTotalInitializeTimeMs = elapsed_ms(t0Total, t1Total);
        }

        // ――――――――――――――――――
        //  フレーム毎に呼ぶ：全システムを優先度順に更新
        // ――――――――――――――――――
        void update_all_systems() { update_all_systems(UpdateContext{}); }

        void update_all_systems(const UpdateContext& a_context)
        {
            m_isUpdating = true;
            m_cancelUpdate = false; // ← 毎フレーム最初にリセット

            const TimePoint t0Total = capture_now();

            // 追加直後の Entity を初期化システムに通す
            const InitializeContext initializeContext{};
            for (Entity e : m_newEntitiesLastFrame)
            {
                for (auto& sys : m_systems)
                {
                    sys->initialize_entity(e, initializeContext);
                }
            }
            m_newEntitiesLastFrame.clear();

            // システム更新
            sort_systems_by_priority();
            for (auto& sys : m_systems)
            {
                if (sys->is_enabled())
                {
                    if (m_cancelUpdate)
                    {
                        break; // 中止判定
                    }
                    const TimePoint t0 = capture_now();
                    sys->update(a_context);
                    const TimePoint t1 = capture_now();
                    std::type_index ti(typeid(*sys));
                    m_lastSystemUpdateTimeMs[ti] = elapsed_ms(t0, t1);
                }
            }

            m_isUpdating = false;
            m_cancelUpdate = false;
            flush_staging_entities();
            flush_staging_components();
            flush_deferred();

            const TimePoint t1Total = capture_now();
            m_lastTotalUpdateTimeMs = elapsed_ms(t0Total, t1Total);
        }

        // ③ ゲーム終了後に一度だけ
        void finalize_all_systems() { finalize_all_systems(FinalizeContext{}); }

        void finalize_all_systems(const FinalizeContext& a_context)
        {
            m_isUpdating = true;
            m_cancelUpdate = false; // ← 毎フレーム最初にリセット
            const TimePoint t0Total = capture_now();

            sort_systems_by_priority();
            for (auto& sys : m_systems)
            {
                if (sys->is_enabled())
                {
                    const TimePoint t0 = capture_now();
                    sys->finalize(a_context);
                    const TimePoint t1 = capture_now();

                    std::type_index ti(typeid(*sys));
                    m_lastSystemFinalizeTimeMs[ti] = elapsed_ms(t0, t1);
                }
            }

            m_isUpdating = false;
            m_cancelUpdate = false;

            const TimePoint t1Total = capture_now();
            m_lastTotalFinalizeTimeMs = elapsed_ms(t0Total, t1Total);
        }

        // ① ゲーム開始前に一度だけ
        void awake_all_systems() { awake_all_systems(AwakeContext{}); }

        void awake_all_systems(const AwakeContext& a_context)
        {
            m_isUpdating = true;
            m_cancelUpdate = false; // ← 毎フレーム最初にリセット
            const TimePoint t0Total = capture_now();

            sort_systems_by_priority();
            for (auto& sys : m_systems)
            {
                if (sys->is_enabled())
                {
                    const TimePoint t0 = capture_now();
                    sys->awake(a_context);
                    const TimePoint t1 = capture_now();
                    std::type_index ti(typeid(*sys));
                    m_lastSystemAwakeTimeMs[ti] = elapsed_ms(t0, t1);
                }
            }

            m_isUpdating = false;
            m_cancelUpdate = false;

            const TimePoint t1Total = capture_now();
            m_lastTotalAwakeTimeMs = elapsed_ms(t0Total, t1Total);
        }

        /// 最後のフレームで更新に要した「全システム分」の時間を取得(ms)
        double get_last_total_update_time_ms() const
        {
            return m_lastTotalUpdateTimeMs;
        }

        /// 最後のフレームで更新に要した、システム型 S の時間を取得(ms)
        template <typename S> double get_last_system_update_time_ms() const
        {
            std::type_index ti(typeid(S));
            auto it = m_lastSystemUpdateTimeMs.find(ti);
            if (it == m_lastSystemUpdateTimeMs.end())
            {
                return 0.0;
            }
            return it->second;
        }

        // Initialize
        double get_last_total_initialize_time_ms() const
        {
            return m_lastTotalInitializeTimeMs;
        }

        template <typename S> double get_last_system_initialize_time_ms() const
        {
            auto it = m_lastSystemInitializeTimeMs.find(typeid(S));
            return it == m_lastSystemInitializeTimeMs.end() ? 0.0 : it->second;
        }

        // Finalize
        double get_last_total_finalize_time_ms() const
        {
            return m_lastTotalFinalizeTimeMs;
        }

        template <typename S> double get_last_system_finalize_time_ms() const
        {
            auto it = m_lastSystemFinalizeTimeMs.find(typeid(S));
            return it == m_lastSystemFinalizeTimeMs.end() ? 0.0 : it->second;
        }

        void flush_staging_entities()
        {
            for (size_t i = 0; i < m_stagingEntities.size(); ++i)
            {
                Entity e = m_stagingEntities[i];

                // m_EntityToActive を拡張して反映
                if (m_entityToActive.size() <= e)
                {
                    m_entityToActive.resize(e + 1, false);
                }
                m_entityToActive[e] = m_stagingEntityActive[i];

                // Archetype を反映
                if (m_entityToArchetype.size() <= e)
                {
                    m_entityToArchetype.resize(e + 1);
                }
                m_entityToArchetype[e] = m_stagingEntityArchetypes[i];

                // Archetype バケットに登録
                m_archToEntities[m_stagingEntityArchetypes[i]].add(e);

                // 新規Entityリストに追加
                m_newEntitiesLastFrame.push_back(e);
            }

            // 一時バッファをクリア
            m_stagingEntities.clear();
            m_stagingEntityActive.clear();
            m_stagingEntityArchetypes.clear();
        }
        void flush_staging_components()
        {
            for (auto& [id, pool] : m_typeToComponents)
            {
                pool->flush_staging();
            }
        }

        /*---------------------------------------------------------------------
            EntityContainer (archetype bucket)
        ---------------------------------------------------------------------*/
        class EntityContainer
        {
        public:
            void add(Entity a_e)
            {
                m_entities.emplace_back(a_e);
                if (m_entityToIndex.size() <= a_e)
                {
                    m_entityToIndex.resize(a_e + 1);
                }
                m_entityToIndex[a_e] = static_cast<uint32_t>(m_entities.size() - 1);
            }
            void remove(Entity a_e)
            {
                if (a_e >= m_entityToIndex.size())
                {
                    return;
                }
                uint32_t idx = m_entityToIndex[a_e];
                if (idx >= m_entities.size())
                {
                    return;
                }
                uint32_t last = static_cast<uint32_t>(m_entities.size() - 1);
                Entity back = m_entities[last];
                if (a_e != back)
                {
                    m_entities[idx] = back;
                    m_entityToIndex[back] = idx;
                }
                m_entities.pop_back();
            }
            const std::vector<Entity>& get_entities() const noexcept
            {
                return m_entities;
            }

        private:
            std::vector<Entity> m_entities;
            std::vector<uint32_t> m_entityToIndex;
        };

        /*---------------------------------------------------------------------
            IComponentPool (interface)
        ---------------------------------------------------------------------*/
        class IComponentPool
        {
        public:
            virtual ~IComponentPool() = default;
            virtual void copy_component(Entity a_src, Entity a_dst) = 0;
            virtual void remove_component(Entity a_e) = 0;
            virtual void* get_raw_component(Entity a_e) const = 0;
            virtual std::shared_ptr<void> clone_component(CompID a_id, void* a_ptr) = 0;
            virtual bool is_multi_component_trait(CompID a_id) const = 0;
            virtual size_t get_component_count(Entity a_e) const = 0;
            /// 生ポインタから直接エンティティ dst にクローンして追加
            virtual void clone_raw_component_to(Entity a_dst, void* a_raw) = 0;
            /// 削除前のクリーンアップ呼び出し用
            virtual void cleanup(Entity) {}
            virtual void flush_staging() {} // ステージングバッファをフラッシュ
            virtual void copy_component_staging(Entity a_src, Entity a_dst)
            {
                // デフォルトでは通常コピーにフォールバック
                copy_component(a_src, a_dst);
            }
        };

        IComponentPool* get_raw_component_pool(CompID a_id)
        {
            auto it = m_typeToComponents.find(a_id);
            return (it != m_typeToComponents.end()) ? it->second.get() : nullptr;
        }

        /*---------------------------------------------------------------------
            ComponentPool  (vector‑backed)
        ---------------------------------------------------------------------*/
        template <ComponentType T> class ComponentPool : public IComponentPool
        {
            using Storage = std::vector<T>; // ★ vector 版
            static constexpr uint32_t kInvalid = ~0u;

        public:
            explicit ComponentPool(size_t a_reserveEntities = 0)
            {
                m_storage.reserve(a_reserveEntities);
            }

            // 登録された型がマルチかどうかを返す
            bool is_multi_component_trait(CompID) const override
            {
                return IsMultiComponent<T>::value;
            }

            /*-------------------- add ---------------------*/
            T* add_component(Entity a_e)
            {
                if constexpr (IsMultiComponent<T>::value)
                {
                    return &m_multi[a_e].emplace_back(); // vector of components per entity
                }
                else
                {
                    if (m_entityToIndex.size() <= a_e)
                    {
                        m_entityToIndex.resize(a_e + 1, kInvalid);
                    }
                    uint32_t idx = m_entityToIndex[a_e];
                    if (idx == kInvalid)
                    {
                        idx = static_cast<uint32_t>(m_storage.size());
                        m_storage.emplace_back(); // default construct
                        if (m_indexToEntity.size() <= idx)
                        {
                            m_indexToEntity.resize(idx + 1, kInvalid);
                        }
                        m_entityToIndex[a_e] = idx;
                        m_indexToEntity[idx] = a_e;
                    }
                    return &m_storage[idx];
                }
            }

            T* add_component_staging(Entity a_e)
            {
                if constexpr (IsMultiComponent<T>::value)
                {
                    auto& vec = m_stagingMulti[a_e];
                    vec.emplace_back();
                    T& inst = vec.back();
                    // inst.SetOwner(e);
                    // inst.SetECSManager(m_pEcs);
                    return &inst;
                }
                else
                {
                    // 上書きも可能
                    T& inst = m_stagingSingle[a_e];
                    // inst.SetOwner(e);
                    // inst.SetECSManager(m_pEcs);
                    return &inst;
                }
            }

            /*-------------------- get ---------------------*/
            T* get_component(Entity a_e)
            {
                if constexpr (IsMultiComponent<T>::value)
                {
                    // ① 一時バッファにあるかチェック
                    auto itStaging = m_stagingMulti.find(a_e);
                    if (itStaging != m_stagingMulti.end() && !itStaging->second.empty())
                    {
                        return &itStaging->second.front();
                    }

                    // ② 本番マップから取得
                    auto it = m_multi.find(a_e);
                    return (it != m_multi.end() && !it->second.empty())
                        ? &it->second.front()
                        : nullptr;
                }
                else
                {
                    // ① 一時バッファにあるかチェック
                    auto itStaging = m_stagingSingle.find(a_e);
                    if (itStaging != m_stagingSingle.end())
                    {
                        return &itStaging->second;
                    }

                    // ② 本番ストレージから取得
                    if (a_e >= m_entityToIndex.size())
                    {
                        return nullptr;
                    }
                    uint32_t idx = m_entityToIndex[a_e];
                    return (idx != kInvalid) ? &m_storage[idx] : nullptr;
                }
            }

            /*-------------------- remove ------------------*/
            void remove_component(Entity a_e) override
            {
                if constexpr (IsMultiComponent<T>::value)
                {
                    m_multi.erase(a_e);
                }
                else
                {
                    if (a_e >= m_entityToIndex.size())
                    {
                        return;
                    }
                    uint32_t idx = m_entityToIndex[a_e];
                    if (idx == kInvalid)
                    {
                        return;
                    }
                    uint32_t last = static_cast<uint32_t>(m_storage.size() - 1);
                    if (idx != last)
                    {
                        m_storage[idx] = std::move(m_storage[last]);
                        Entity movedEnt = m_indexToEntity[last];
                        m_indexToEntity[idx] = movedEnt;
                        m_entityToIndex[movedEnt] = idx;
                    }
                    m_storage.pop_back();
                    m_entityToIndex[a_e] = kInvalid;
                    m_indexToEntity[last] = kInvalid;
                }
            }

            /*-------------------- multi helpers -----------*/
            std::vector<T>* get_all_components(Entity a_e)
                requires IsMultiComponent<T>::value
            {
                // ① ステージングバッファ中のマルチコンポーネントを優先
                auto itStaging = m_stagingMulti.find(a_e);
                if (itStaging != m_stagingMulti.end() && !itStaging->second.empty())
                {
                    return &itStaging->second;
                }

                // ② なければ本番マップを返す
                auto itMain = m_multi.find(a_e);
                return (itMain != m_multi.end() && !itMain->second.empty())
                    ? &itMain->second
                    : nullptr;
            }
            // マルチコンポーネント用：特定インデックスの要素を消す
            void remove_instance(Entity a_e, size_t a_index)
                requires IsMultiComponent<T>::value
            {
                auto it = m_multi.find(a_e);
                if (it == m_multi.end())
                {
                    return;
                }
                auto& vec = it->second;
                if (a_index >= vec.size())
                {
                    return;
                }
                vec.erase(vec.begin() + a_index);
                if (vec.empty())
                {
                    // すべて消えたらバケットからも削除
                    m_multi.erase(a_e);
                }
            }
            void remove_all(Entity a_e)
                requires IsMultiComponent<T>::value
            {
                m_multi.erase(a_e);
            }

            /*-------------------- copy --------------------*/
            void copy_component(Entity a_src, Entity a_dst) override
            {
                if constexpr (IsMultiComponent<T>::value)
                {
                    auto it = m_multi.find(a_src);
                    if (it == m_multi.end() || it->second.empty())
                    {
                        return;
                    }
                    // shallow copy
                    auto& vecSrc = it->second;
                    auto& vecDst = m_multi[a_dst] = vecSrc; // copy vector
                    vecDst;
                    //           for (auto& inst : vecDst)
                    //           {
                    ////inst.SetOwner(dst);
                    //           }
                }
                else
                {
                    uint32_t idxSrc = m_entityToIndex[a_src];
                    if (idxSrc == kInvalid)
                    {
                        return;
                    }
                    // すでに dst にコンポーネントがある場合
                    if (a_dst < m_entityToIndex.size() &&
                        m_entityToIndex[a_dst] != kInvalid)
                    {
                        auto& c = m_storage[m_entityToIndex[a_dst]];
                        c = m_storage[idxSrc];
                    }
                    else
                    {
                        // 新規にコピーする場合
                        m_storage.emplace_back(m_storage[idxSrc]);
                        auto& newComp = m_storage.back();
                        newComp;

                        uint32_t idxDst = static_cast<uint32_t>(m_storage.size() - 1);
                        if (m_entityToIndex.size() <= a_dst)
                        {
                            m_entityToIndex.resize(a_dst + 1, kInvalid);
                        }
                        if (m_indexToEntity.size() <= idxDst)
                        {
                            m_indexToEntity.resize(idxDst + 1, kInvalid);
                        }
                        m_entityToIndex[a_dst] = idxDst;
                        m_indexToEntity[idxDst] = a_dst;
                    }
                    /*T* comp = GetComponent(dst);
                    if (comp)
                    {
                        comp->SetOwner(dst);
                    }*/
                }
            }

            // 更新中のステージング用コピーをオーバーライド
            void copy_component_staging(Entity a_src, Entity a_dst) override
            {
                if constexpr (IsMultiComponent<T>::value)
                {
                    // マルチコンポーネントなら全インスタンスをステージングに追加
                    auto it = m_multi.find(a_src);
                    if (it == m_multi.end())
                    {
                        return;
                    }
                    for (auto const& inst : it->second)
                    {
                        m_stagingMulti[a_dst].push_back(inst);
                        // m_StagingMulti[dst].back().SetOwner(dst);
                        // m_StagingMulti[dst].back().SetECSManager(m_pEcs);
                    }
                }
                else
                {
                    // シングルコンポーネントなら最新の値をステージングバッファに上書き
                    T* srcComp = get_component(a_src);
                    if (!srcComp)
                    {
                        return;
                    }
                    m_stagingSingle[a_dst] = *srcComp;
                    // m_StagingSingle[dst].SetOwner(dst);
                    // m_StagingSingle[dst].SetECSManager(m_pEcs);
                }
            }

            void clone_raw_component_to(Entity a_dst, void* a_raw) override
            {
                if constexpr (IsMultiComponent<T>::value)
                {
                    // マルチコンポーネントなら vector<T> 全体をコピー
                    auto* srcVec = static_cast<std::vector<T> *>(a_raw);
                    m_multi[a_dst] = *srcVec;
                }
                else
                {
                    // 単一コンポーネントなら AddComponent＋コピー代入
                    T* dstComp = add_component(a_dst);
                    *dstComp = *static_cast<T*>(a_raw);
                }
            }

            /// Prefab復元時だけ使う、「生のコピー」を行う
            void prefab_clone_raw(Entity a_e, void* a_rawPtr)
            {
                if constexpr (IsMultiComponent<T>::value)
                {
                    // マルチコンポーネントなら vector<T> 全体を直接コピー
                    auto* srcVec = static_cast<std::vector<T> *>(a_rawPtr);
                    m_multi[a_e] = *srcVec; // コピーコンストラクタを使う
                }
                else
                {
                    // 単一コンポーネントなら storage に直接 emplace_back
                    // （operator= ではなく、T のコピーコンストラクタで構築される）
                    if (m_entityToIndex.size() <= a_e)
                    {
                        m_entityToIndex.resize(a_e + 1, kInvalid);
                    }
                    uint32_t idx = static_cast<uint32_t>(m_storage.size());
                    m_storage.emplace_back(*static_cast<T*>(a_rawPtr));
                    if (m_indexToEntity.size() <= idx)
                    {
                        m_indexToEntity.resize(idx + 1, kInvalid);
                    }
                    m_entityToIndex[a_e] = idx;
                    m_indexToEntity[idx] = a_e;
                }
            }

            void add_prefab_component_staging(Entity a_e, const T& a_comp)
            {
                if constexpr (IsMultiComponent<T>::value)
                {
                    m_stagingMulti[a_e].push_back(a_comp);
                    // m_StagingMulti[e].back().SetOwner(e);
                    // m_StagingMulti[e].back().SetECSManager(m_pEcs);
                }
                else
                {
                    m_stagingSingle[a_e] = a_comp;
                    // m_StagingSingle[e].SetOwner(e);
                    // m_StagingSingle[e].SetECSManager(m_pEcs);
                }
            }

            void add_prefab_component(Entity a_e, const T& a_comp)
            {
                if constexpr (IsMultiComponent<T>::value)
                {
                    m_multi[a_e].push_back(a_comp);
                }
                else
                {
                    if (m_entityToIndex.size() <= a_e)
                    {
                        m_entityToIndex.resize(a_e + 1, kInvalid);
                    }
                    uint32_t& idx = m_entityToIndex[a_e];
                    if (idx == kInvalid)
                    {
                        idx = static_cast<uint32_t>(m_storage.size());
                        m_storage.push_back(a_comp);
                        if (m_indexToEntity.size() <= idx)
                        {
                            m_indexToEntity.resize(idx + 1, kInvalid);
                        }
                        m_indexToEntity[idx] = a_e;
                    }
                    else
                    {
                        m_storage[idx] = a_comp;
                    }
                }
            }

            // ─── 削除前のクリーンアップ
            void cleanup(Entity a_e) override
            {
                if constexpr (IsMultiComponent<T>::value)
                {
                    auto it = m_multi.find(a_e);
                    if (it != m_multi.end())
                    {
                        for (auto& inst : it->second)
                        {
                            inst.initialize();
                        }
                    }
                }
                else
                {
                    T* comp = get_component(a_e);
                    if (comp)
                    {
                        comp->initialize();
                    }
                }
            }

            /*-------------------- static ID --------------*/
            static CompID get_id()
            {
                // static CompID id = ++ECSManager::m_NextCompTypeID; return id;
                return ComponentID<T>();
            }

            /*-------------------- expose map -------------*/
            auto& map() { return m_multi; }
            const auto& map() const { return m_multi; }

            // 単一コンポーネントを void* で取得
            void* get_raw_component(Entity a_e) const override
            {
                if constexpr (IsMultiComponent<T>::value)
                {
                    auto itS = m_stagingMulti.find(a_e);
                    if (itS != m_stagingMulti.end() && !itS->second.empty())
                    {
                        return (void*)&itS->second;
                    }
                    auto itM = m_multi.find(a_e);
                    return (itM != m_multi.end() && !itM->second.empty())
                        ? (void*)&itM->second
                        : nullptr;
                }
                else
                {
                    auto itS = m_stagingSingle.find(a_e);
                    if (itS != m_stagingSingle.end())
                    {
                        return (void*)&itS->second;
                    }
                    if (a_e >= m_entityToIndex.size())
                    {
                        return nullptr;
                    }
                    uint32_t idx = m_entityToIndex[a_e];
                    return idx != kInvalid ? (void*)&m_storage[idx] : nullptr;
                }
            }

            // 任意の型を shared_ptr<void> に包んでPrefabにコピー
            std::shared_ptr<void> clone_component(CompID, void* a_ptr) override
            {
                if constexpr (IsMultiComponent<T>::value)
                {
                    auto src = static_cast<std::vector<T> *>(a_ptr);
                    return std::make_shared<std::vector<T>>(*src); // Deep copy
                }
                else
                {
                    T* src = static_cast<T*>(a_ptr);
                    return std::make_shared<T>(*src); // Deep copy
                }
            }
            size_t get_component_count(Entity a_e) const override
            {
                if constexpr (IsMultiComponent<T>::value)
                {
                    auto it = m_multi.find(a_e);
                    return (it == m_multi.end()) ? 0u : it->second.size();
                }
                else
                {
                    if (a_e >= m_entityToIndex.size())
                    {
                        return 0u;
                    }
                    return (m_entityToIndex[a_e] != kInvalid) ? 1u : 0u;
                }
            }
            void flush_staging() override
            {
                if constexpr (IsMultiComponent<T>::value)
                {
                    for (auto& [e, stagingVec] : m_stagingMulti)
                    {
                        auto& vec = m_multi[e];
                        vec.insert(vec.end(), stagingVec.begin(), stagingVec.end());
                    }
                    m_stagingMulti.clear();
                }
                else
                {
                    for (auto& [e, comp] : m_stagingSingle)
                    {
                        if (m_entityToIndex.size() <= e)
                        {
                            m_entityToIndex.resize(e + 1, kInvalid);
                        }
                        uint32_t idx = m_entityToIndex[e];

                        if (idx == kInvalid)
                        {
                            idx = static_cast<uint32_t>(m_storage.size());
                            m_storage.push_back(comp);
                            if (m_indexToEntity.size() <= idx)
                            {
                                m_indexToEntity.resize(idx + 1, kInvalid);
                            }
                            m_entityToIndex[e] = idx;
                            m_indexToEntity[idx] = e;
                        }
                        else
                        {
                            m_storage[idx] = comp; // 上書き
                        }
                    }
                    m_stagingSingle.clear();
                }
            }
            const std::unordered_map<Entity, std::vector<T>>&
                get_staging_multi() const
            {
                return m_stagingMulti;
            }

        private:
            Storage m_storage;                                  // dense
            std::vector<uint32_t> m_entityToIndex;              // entity -> index
            std::vector<Entity> m_indexToEntity;                // index  -> entity
            std::unordered_map<Entity, std::vector<T>> m_multi; // multi‑instance
            std::unordered_map<Entity, T> m_stagingSingle; // フレーム中に追加された T
            std::unordered_map<Entity, std::vector<T>> m_stagingMulti;
        };

        template <ComponentType T> ComponentPool<T>* get_component_pool()
        {
            auto it = m_typeToComponents.find(ComponentPool<T>::get_id());
            if (it == m_typeToComponents.end())
            {
                return nullptr;
            }
            return static_cast<ComponentPool<T> *>(it->second.get());
        }

        template <ComponentType T> ComponentPool<T>& ensure_pool()
        {
            CompID id = ComponentPool<T>::get_id();
            auto [it, inserted] = m_typeToComponents.try_emplace(
                id, std::make_shared<ComponentPool<T>>(4096));
            return *static_cast<ComponentPool<T> *>(it->second.get());
        }

        class ISystem
        {
        public:
            ISystem() : m_pEcs(nullptr) {}
            /// 開始時に一度だけ呼ばれる
            virtual void initialize() {}
            virtual void initialize(const InitializeContext&) { initialize(); }
            /// 毎フレーム呼ばれる
            virtual void update() = 0;
            virtual void update(const UpdateContext&) { update(); }
            /// 終了時に一度だけ呼ばれる
            virtual void finalize() {}
            virtual void finalize(const FinalizeContext&) { finalize(); }
            /// 一度だけ呼ばれる
            virtual void awake() {}
            virtual void awake(const AwakeContext&) { awake(); }
            virtual ~ISystem() = default;
            /// ECSManager に登録されたタイミングで呼び出される
            virtual void on_register(ECSManager* a_ecs) { m_pEcs = a_ecs; }
            /// フレーム中に遅延で追加されたエンティティ／コンポーネントを受け取って、
            /// そのエンティティだけフェーズの処理を走らせたいときに使う
            virtual void initialize_entity(Entity) {}
            virtual void initialize_entity(Entity a_e, const InitializeContext&)
            {
                initialize_entity(a_e);
            }
            virtual void finalize_entity(Entity) {}
            virtual void finalize_entity(Entity a_e, const FinalizeContext&)
            {
                finalize_entity(a_e);
            }
            virtual void awake_entity(Entity) {}
            virtual void awake_entity(Entity a_e, const AwakeContext&)
            {
                awake_entity(a_e);
            }
            virtual int get_priority() const { return m_priority; }
            virtual void set_priority(int a_p) { m_priority = a_p; }
            virtual bool is_enabled() const { return m_enabled; }
            virtual void set_enabled(bool a_e) { m_enabled = a_e; }

        protected:
            ECSManager* m_pEcs =
                nullptr;             // ECSManager へのポインタ（初期化時に設定される）
            uint32_t m_priority = 0; // 優先度
            bool m_enabled = true;   // 有効フラグ
        };

        /*---------------------------------------------------------------------
            System & MultiSystem  (unchanged logic)
        ---------------------------------------------------------------------*/
        template <ComponentType... T> class System : public ISystem
        {
            static constexpr bool kNoMulti = (!IsMultiComponent<T>::value && ...);
            static_assert(kNoMulti,
                "System<T...> cannot include multi-instance components");
            using UpdateFunc = std::function<void(Entity, T &...)>;
            using InitFunc = std::function<void(Entity, T &...)>;
            using FinFunc = std::function<void(Entity, T &...)>;
            using AwakeFunc = std::function<void(Entity, T &...)>;
            using UpdateWithContextFunc =
                std::function<void(Entity, T &..., const UpdateContext&)>;
            using InitWithContextFunc =
                std::function<void(Entity, T &..., const InitializeContext&)>;
            using FinWithContextFunc =
                std::function<void(Entity, T &..., const FinalizeContext&)>;
            using AwakeWithContextFunc =
                std::function<void(Entity, T &..., const AwakeContext&)>;

        public:
            explicit System(UpdateFunc a_u, InitFunc a_i = {}, FinFunc a_f = {},
                AwakeFunc a_w = {})
                : m_update(a_u), m_init(a_i), m_fin(a_f), m_awake(a_w)
            {
                (m_required.set(ComponentPool<T>::get_id()), ...);
            }
            explicit System(UpdateWithContextFunc a_u, InitWithContextFunc a_i = {},
                FinWithContextFunc a_f = {},
                AwakeWithContextFunc a_w = {})
                : m_updateWithContext(a_u), m_initWithContext(a_i),
                m_finWithContext(a_f), m_awakeWithContext(a_w)
            {
                (m_required.set(ComponentPool<T>::get_id()), ...);
            }
            // 初期化フェーズでエンティティごとの処理
            void initialize() override { initialize(InitializeContext{}); }

            void initialize(const InitializeContext& a_context) override
            {
                if (!m_init && !m_initWithContext)
                {
                    return;
                }
                for (auto& [arch, bucket] : m_pEcs->get_arch_to_entities())
                {
                    if ((arch & m_required) != m_required)
                    {
                        continue;
                    }
                    for (Entity e : bucket.get_entities())
                    {
                        if (!m_pEcs->is_entity_active(e))
                        {
                            continue;
                        }
                        if (!((m_pEcs->get_component<T>(e) != nullptr) && ...))
                        {
                            continue;
                        }
                        if (!((m_pEcs->get_component<T>(e)->is_active()) && ...))
                        {
                            continue;
                        }
                        if (m_initWithContext)
                        {
                            m_initWithContext(e, *m_pEcs->get_component<T>(e)..., a_context);
                        }
                        else
                        {
                            m_init(e, *m_pEcs->get_component<T>(e)...);
                        }
                    }
                }
            }
            void update() override { update(UpdateContext{}); }

            void update(const UpdateContext& a_context) override
            {
                for (auto& [arch, bucket] : m_pEcs->get_arch_to_entities())
                {
                    // アーキタイプフィルタ
                    if ((arch & m_required) != m_required)
                    {
                        continue;
                    }

                    for (Entity e : bucket.get_entities())
                    {
                        // 1) エンティティが非アクティブならスキップ
                        if (!m_pEcs->is_entity_active(e))
                        {
                            continue;
                        }

                        // 2) 全コンポーネントが存在するかチェック
                        if (!((m_pEcs->get_component<T>(e) != nullptr) && ...))
                        {
                            continue;
                        }

                        // 3) 全コンポーネントがアクティブかチェック
                        if (!((m_pEcs->get_component<T>(e)->is_active()) && ...))
                        {
                            continue;
                        }

                        // 4) 問題なければコールバック
                        if (m_updateWithContext)
                        {
                            m_updateWithContext(e, *m_pEcs->get_component<T>(e)..., a_context);
                        }
                        else
                        {
                            m_update(e, *m_pEcs->get_component<T>(e)...);
                        }
                    }
                }
            }
            // 終了フェーズでエンティティごとの処理
            void finalize() override { finalize(FinalizeContext{}); }

            void finalize(const FinalizeContext& a_context) override
            {
                if (!m_fin && !m_finWithContext)
                {
                    return;
                }
                for (auto& [arch, bucket] : m_pEcs->get_arch_to_entities())
                {
                    if ((arch & m_required) != m_required)
                    {
                        continue;
                    }
                    for (Entity e : bucket.get_entities())
                    {
                        if (!m_pEcs->is_entity_active(e))
                        {
                            continue;
                        }
                        if (!((m_pEcs->get_component<T>(e) != nullptr) && ...))
                        {
                            continue;
                        }
                        if (!((m_pEcs->get_component<T>(e)->is_active()) && ...))
                        {
                            continue;
                        }
                        if (m_finWithContext)
                        {
                            m_finWithContext(e, *m_pEcs->get_component<T>(e)..., a_context);
                        }
                        else
                        {
                            m_fin(e, *m_pEcs->get_component<T>(e)...);
                        }
                    }
                }
            }
            // Awake フェーズでエンティティごとの処理
            void awake() override { awake(AwakeContext{}); }

            void awake(const AwakeContext& a_context) override
            {
                if (!m_awake && !m_awakeWithContext)
                {
                    return;
                }
                for (auto& [arch, bucket] : m_pEcs->get_arch_to_entities())
                {
                    if ((arch & m_required) != m_required)
                    {
                        continue;
                    }
                    for (Entity e : bucket.get_entities())
                    {
                        if (!m_pEcs->is_entity_active(e))
                        {
                            continue;
                        }
                        if (!((m_pEcs->get_component<T>(e) != nullptr) && ...))
                        {
                            continue;
                        }
                        if (!((m_pEcs->get_component<T>(e)->is_active()) && ...))
                        {
                            continue;
                        }
                        if (m_awakeWithContext)
                        {
                            m_awakeWithContext(e, *m_pEcs->get_component<T>(e)..., a_context);
                        }
                        else
                        {
                            m_awake(e, *m_pEcs->get_component<T>(e)...);
                        }
                    }
                }
            }
            void initialize_entity(Entity a_e) override
            {
                initialize_entity(a_e, InitializeContext{});
            }

            void initialize_entity(Entity a_e,
                const InitializeContext& a_context) override
            {
                if (!m_init && !m_initWithContext)
                {
                    return;
                }
                // ステージングも含めて、必要なコンポーネントが存在＆アクティブかチェック
                if (!((m_pEcs->get_component<T>(a_e) &&
                    m_pEcs->get_component<T>(a_e)->is_active()) &&
                    ...))
                {
                    return;
                }
                // 初期化コールバックを実行
                if (m_initWithContext)
                {
                    m_initWithContext(a_e, *m_pEcs->get_component<T>(a_e)..., a_context);
                }
                else
                {
                    m_init(a_e, *m_pEcs->get_component<T>(a_e)...);
                }
            }
            void finalize_entity(Entity a_e) override
            {
                finalize_entity(a_e, FinalizeContext{});
            }

            void finalize_entity(Entity a_e,
                const FinalizeContext& a_context) override
            {
                if (!m_fin && !m_finWithContext)
                {
                    return;
                }
                // ステージングも含めて、必要なコンポーネントが存在＆アクティブかチェック
                if (!((m_pEcs->get_component<T>(a_e) &&
                    m_pEcs->get_component<T>(a_e)->is_active()) &&
                    ...))
                {
                    return;
                }
                // 終了コールバック
                if (m_finWithContext)
                {
                    m_finWithContext(a_e, *m_pEcs->get_component<T>(a_e)..., a_context);
                }
                else
                {
                    m_fin(a_e, *m_pEcs->get_component<T>(a_e)...);
                }
            }
            void awake_entity(Entity a_e) override { awake_entity(a_e, AwakeContext{}); }

            void awake_entity(Entity a_e, const AwakeContext& a_context) override
            {
                if (!m_awake && !m_awakeWithContext)
                {
                    return;
                }
                // ステージングも含めて、必要なコンポーネントが存在＆アクティブかチェック
                if (!((m_pEcs->get_component<T>(a_e) &&
                    m_pEcs->get_component<T>(a_e)->is_active()) &&
                    ...))
                {
                    return;
                }
                // Awakeコールバック
                if (m_awakeWithContext)
                {
                    m_awakeWithContext(a_e, *m_pEcs->get_component<T>(a_e)..., a_context);
                }
                else
                {
                    m_awake(a_e, *m_pEcs->get_component<T>(a_e)...);
                }
            }
            const Archetype& get_required() const { return m_required; }

        private:
            Archetype m_required;
            UpdateFunc m_update;
            InitFunc m_init;
            FinFunc m_fin;
            AwakeFunc m_awake; // 追加：Awake用のコールバック
            UpdateWithContextFunc m_updateWithContext;
            InitWithContextFunc m_initWithContext;
            FinWithContextFunc m_finWithContext;
            AwakeWithContextFunc m_awakeWithContext;
        };

        template <ComponentType T>
            requires IsMultiComponent<T>::value
        class MultiComponentSystem : public ISystem
        {
            using InitFunc = std::function<void(Entity, std::vector<T>&)>;
            using UpdateFunc = std::function<void(Entity, std::vector<T>&)>;
            using FinFunc = std::function<void(Entity, std::vector<T>&)>;
            using AwakeFunc = std::function<void(Entity, std::vector<T>&)>;
            using InitWithContextFunc =
                std::function<void(Entity, std::vector<T>&, const InitializeContext&)>;
            using UpdateWithContextFunc =
                std::function<void(Entity, std::vector<T>&, const UpdateContext&)>;
            using FinWithContextFunc =
                std::function<void(Entity, std::vector<T>&, const FinalizeContext&)>;
            using AwakeWithContextFunc =
                std::function<void(Entity, std::vector<T>&, const AwakeContext&)>;

        public:
            explicit MultiComponentSystem(UpdateFunc a_u, InitFunc a_i = {},
                FinFunc a_f = {}, AwakeFunc a_w = {})
                : m_update(a_u), m_init(a_i), m_fin(a_f), m_awake(a_w)
            {}
            explicit MultiComponentSystem(UpdateWithContextFunc a_u,
                InitWithContextFunc a_i = {},
                FinWithContextFunc a_f = {},
                AwakeWithContextFunc a_w = {})
                : m_updateWithContext(a_u), m_initWithContext(a_i),
                m_finWithContext(a_f), m_awakeWithContext(a_w)
            {}
            // 起動時に一度だけ呼ばれる
            void initialize() override { initialize(InitializeContext{}); }

            void initialize(const InitializeContext& a_context) override
            {
                if (!m_init && !m_initWithContext)
                {
                    return;
                }
                if (m_initWithContext)
                {
                    process_all_with_context(m_initWithContext, a_context);
                }
                else
                {
                    process_all(m_init);
                }
            }

            // 毎フレーム呼ばれる
            void update() override { update(UpdateContext{}); }

            void update(const UpdateContext& a_context) override
            {
                if (m_updateWithContext)
                {
                    process_all_with_context(m_updateWithContext, a_context);
                }
                else
                {
                    process_all(m_update);
                }
            }

            // 終了時に一度だけ呼ばれる
            void finalize() override { finalize(FinalizeContext{}); }

            void finalize(const FinalizeContext& a_context) override
            {
                if (!m_fin && !m_finWithContext)
                {
                    return;
                }
                if (m_finWithContext)
                {
                    process_all_with_context(m_finWithContext, a_context);
                }
                else
                {
                    process_all(m_fin);
                }
            }

            // Awake フェーズでエンティティごとの処理
            void awake() override { awake(AwakeContext{}); }

            void awake(const AwakeContext& a_context) override
            {
                if (!m_awake && !m_awakeWithContext)
                {
                    return;
                }
                if (m_awakeWithContext)
                {
                    process_all_with_context(m_awakeWithContext, a_context);
                }
                else
                {
                    process_all(m_awake);
                }
            }
            void initialize_entity(Entity a_e) override
            {
                initialize_entity(a_e, InitializeContext{});
            }

            void initialize_entity(Entity a_e,
                const InitializeContext& a_context) override
            {
                if (!m_init && !m_initWithContext)
                {
                    return;
                }
                auto* pool = m_pEcs->get_component_pool<T>();
                if (!pool)
                {
                    return;
                }

                std::vector<T> filtered;

                // ① ステージングバッファのチェック
                const auto& stagingMap =
                    pool->get_staging_multi(); // m_stagingMulti への参照を返す
                if (auto itS = stagingMap.find(a_e); itS != stagingMap.end())
                {
                    for (auto& inst : itS->second)
                    {
                        if (inst.is_active())
                        {
                            filtered.push_back(inst);
                        }
                    }
                }

                // ② 本番データのチェック
                const auto& mainMap = pool->map(); // 既存の m_multi
                if (auto itM = mainMap.find(a_e); itM != mainMap.end())
                {
                    for (auto& inst : itM->second)
                    {
                        if (inst.is_active())
                        {
                            filtered.push_back(inst);
                        }
                    }
                }

                if (!filtered.empty())
                {
                    if (m_initWithContext)
                    {
                        m_initWithContext(a_e, filtered, a_context);
                    }
                    else
                    {
                        m_init(a_e, filtered);
                    }
                }
            }
            void finalize_entity(Entity a_e) override
            {
                finalize_entity(a_e, FinalizeContext{});
            }

            void finalize_entity(Entity a_e,
                const FinalizeContext& a_context) override
            {
                if (!m_fin && !m_finWithContext)
                {
                    return;
                }
                auto* pool = m_pEcs->get_component_pool<T>();
                if (!pool)
                {
                    return;
                }
                // 本番バッファのみ（ステージング反映後にここが呼ばれる想定）
                auto it = pool->map().find(a_e);
                if (it == pool->map().end() || it->second.empty())
                {
                    return;
                }
                // 有効なものだけを抜き出して呼ぶ
                std::vector<T> tmp;
                for (auto& c : it->second)
                {
                    if (c.is_active())
                    {
                        tmp.push_back(c);
                    }
                }
                if (!tmp.empty())
                {
                    if (m_finWithContext)
                    {
                        m_finWithContext(a_e, tmp, a_context);
                    }
                    else
                    {
                        m_fin(a_e, tmp);
                    }
                }
            }
            void awake_entity(Entity a_e) override { awake_entity(a_e, AwakeContext{}); }

            void awake_entity(Entity a_e, const AwakeContext& a_context) override
            {
                if (!m_awake && !m_awakeWithContext)
                {
                    return;
                }
                auto* pool = m_pEcs->get_component_pool<T>();
                if (!pool)
                {
                    return;
                }

                std::vector<T> filtered;

                // ① ステージングバッファのチェック
                const auto& stagingMap =
                    pool->get_staging_multi(); // m_stagingMulti への参照を返す
                if (auto itS = stagingMap.find(a_e); itS != stagingMap.end())
                {
                    for (auto& inst : itS->second)
                    {
                        if (inst.is_active())
                        {
                            filtered.push_back(inst);
                        }
                    }
                }

                // ② 本番データのチェック
                const auto& mainMap = pool->map(); // 既存の m_multi
                if (auto itM = mainMap.find(a_e); itM != mainMap.end())
                {
                    for (auto& inst : itM->second)
                    {
                        if (inst.is_active())
                        {
                            filtered.push_back(inst);
                        }
                    }
                }

                if (!filtered.empty())
                {
                    if (m_awakeWithContext)
                    {
                        m_awakeWithContext(a_e, filtered, a_context);
                    }
                    else
                    {
                        m_awake(a_e, filtered);
                    }
                }
            }

        private:
            // 実際にプールを走査してコールバックを呼び出す共通処理
            template <typename Func> void process_all(const Func& a_func)
            {
                auto* pool = m_pEcs->get_component_pool<T>();
                if (!pool)
                {
                    return;
                }

                for (auto& [e, vec] : pool->map())
                {
                    // 1) エンティティがアクティブでない、またはインスタンスが空ならスキップ
                    if (!m_pEcs->is_entity_active(e) || vec.empty())
                    {
                        continue;
                    }

                    // 2) インスタンスごとに IsActive フラグを確認して、filteredVec を作る
                    std::vector<T> filtered;
                    filtered.reserve(vec.size());
                    for (auto& inst : vec)
                    {
                        if (inst.is_active())
                        {
                            filtered.push_back(inst);
                        }
                    }

                    // 3) 有効インスタンスがひとつでもあればコール
                    if (!filtered.empty())
                    {
                        a_func(e, filtered);
                    }
                }
            }

            template <typename Func, typename Context>
            void process_all_with_context(const Func& a_func, const Context& a_context)
            {
                auto* pool = m_pEcs->get_component_pool<T>();
                if (!pool)
                {
                    return;
                }

                for (auto& [e, vec] : pool->map())
                {
                    if (!m_pEcs->is_entity_active(e) || vec.empty())
                    {
                        continue;
                    }

                    std::vector<T> filtered;
                    filtered.reserve(vec.size());
                    for (auto& inst : vec)
                    {
                        if (inst.is_active())
                        {
                            filtered.push_back(inst);
                        }
                    }

                    if (!filtered.empty())
                    {
                        a_func(e, filtered, a_context);
                    }
                }
            }

            UpdateFunc m_update;
            InitFunc m_init;
            FinFunc m_fin;
            AwakeFunc m_awake; // 追加：Awake用のコールバック
            UpdateWithContextFunc m_updateWithContext;
            InitWithContextFunc m_initWithContext;
            FinWithContextFunc m_finWithContext;
            AwakeWithContextFunc m_awakeWithContext;
        };

        std::unordered_map<Archetype, EntityContainer>& get_arch_to_entities()
        {
            return m_archToEntities;
        }

    private:
        [[nodiscard]] TimePoint capture_now() const noexcept
        {
            return Clock::now();
        }

        [[nodiscard]] double elapsed_ms(TimePoint a_begin,
            TimePoint a_end) const noexcept
        {
            return std::chrono::duration<double, std::milli>(a_end - a_begin)
                .count();
        }

        void sort_systems_by_priority()
        {
            std::sort(m_systems.begin(), m_systems.end(),
                [](const auto& a_left, const auto& a_right) {
                    return a_left->get_priority() < a_right->get_priority();
                });
        }

        void notify_component_added(Entity a_e, CompID a_c)
        {
            for (auto& wp : m_componentListeners)
            {
                if (auto sp = wp.lock())
                {
                    sp->on_component_added(a_e, a_c); // Notify listeners
                }
            }
        }
        void notify_component_copied(Entity a_src, Entity a_dst, CompID a_c)
        {
            for (auto& wp : m_componentListeners)
            {
                if (auto sp = wp.lock())
                {
                    sp->on_component_copied(a_src, a_dst, a_c); // Notify listeners
                }
            }
        }
        void notify_component_removed(Entity a_e, CompID a_c)
        {
            for (auto& wp : m_componentListeners)
            {
                if (auto sp = wp.lock())
                {
                    sp->on_component_removed(a_e, a_c);
                }
            }
        }
        void notify_component_removed_instance(Entity a_e, CompID a_c, void* a_v,
            size_t a_i)
        {
            for (auto& wp : m_componentListeners)
            {
                if (auto sp = wp.lock())
                {
                    sp->on_component_removed_instance(a_e, a_c, a_v, a_i);
                }
            }
        }
        void notify_component_restored_from_prefab(Entity a_e, CompID a_c)
        {
            for (auto& wp : m_componentListeners)
            {
                if (auto sp = wp.lock())
                {
                    sp->on_component_restored_from_prefab(a_e, a_c);
                }
            }
        }
        bool is_multi_component_by_id(CompID a_id) const
        {
            auto it = m_typeToComponents.find(a_id);
            if (it != m_typeToComponents.end())
            {
                return it->second->is_multi_component_trait(a_id);
            }
            return false;
        }
        // ――――――――――――――――――
        //  RemoveEntity の本体（ClearEntity → リスナ通知 → 再利用キュー）
        // ――――――――――――――――――
        void remove_entity_impl(Entity a_e)
        {
            // 1) すべてのコンポーネントをクリア（イベント込み）
            clear_entity(a_e);
            // 2) 非アクティブ化
            m_entityToActive[a_e] = false;
            // 3) リサイクル
            m_recycleEntities.push_back(a_e);
            // 4) エンティティ破棄イベント
            for (auto& wp : m_entityListeners)
            {
                if (auto sp = wp.lock())
                {
                    sp->on_entity_destroyed(a_e);
                }
            }
        }

        // フレーム末にまとめてコマンドを実行
        void flush_deferred()
        {
            for (auto& cmd : m_deferredCommands)
            {
                cmd();
            }
            m_deferredCommands.clear();
        }

        /*-------------------- data members --------------------------------*/

        bool m_isUpdating = false;
        bool m_cancelUpdate = false;
        Entity m_nextEntityId = static_cast<Entity>(-1);
        std::vector<bool> m_entityToActive;
        std::vector<Entity> m_recycleEntities;
        std::vector<Entity> m_stagingEntities; // フレーム中に生成された Entity の一覧
        std::vector<bool> m_stagingEntityActive; // 各 Entity の一時アクティブ状態
        std::vector<Archetype>
            m_stagingEntityArchetypes; // 各 Entity の一時 Archetype
        std::vector<Archetype> m_entityToArchetype;
        std::unordered_map<CompID, int>
            m_deletePriority; // デフォルトは 0 (CompID 昇順になる)
        std::unordered_map<CompID, int> m_copyPriority; // コピー時の優先度マップ
        std::vector<std::function<void()>> m_deferredCommands;
        std::vector<std::weak_ptr<IEntityEventListener>> m_entityListeners;
        std::vector<std::unique_ptr<ISystem>> m_systems;
        std::vector<std::weak_ptr<IIComponentEventListener>> m_componentListeners;
        std::unordered_map<Archetype, EntityContainer> m_archToEntities;
        std::unordered_map<CompID, std::shared_ptr<IComponentPool>>
            m_typeToComponents;
        double m_lastTotalUpdateTimeMs =
            0.0; // 最後に計測した全システム更新の所要時間（ms）
        std::unordered_map<std::type_index, double>
            m_lastSystemUpdateTimeMs; // 各システム型ごとに最後に計測した更新時間（ms）

        // Initialize／Finalize の計測用
        double m_lastTotalInitializeTimeMs = 0.0;
        double m_lastTotalFinalizeTimeMs = 0.0;
        double m_lastTotalAwakeTimeMs = 0.0;

        std::unordered_map<std::type_index, double> m_lastSystemInitializeTimeMs;
        std::unordered_map<std::type_index, double> m_lastSystemFinalizeTimeMs;
        std::unordered_map<std::type_index, double> m_lastSystemAwakeTimeMs;

        // SystemごとのUpdate直前に処理する初期化対象格納用
        std::unordered_map<std::type_index, std::vector<Entity>>
            m_pendingInitBeforeUpdate;
        // 新規に追加された Entity を記録するバッファ
        std::vector<Entity> m_newEntitiesLastFrame;
    };

    struct IComponentEventListener : public IIComponentEventListener
    {
        friend class ECSManager;

    public:
        IComponentEventListener() = default;
        virtual ~IComponentEventListener() { onAddSingle.clear(); }

        // 依存する ECSManager のポインタをセット
        void set_ecs_manager(ECSManager* a_ecs) { pEcs = a_ecs; }

        // 単一用
        template <ComponentType T>
        void register_on_add(std::function<void(Entity, T*)> a_f)
        {
            static_assert(!IsMultiComponent<T>::value, "Use singleComponent");
            CompID id = ECSManager::ComponentPool<T>::get_id();
            onAddSingle[id].push_back([a_f](Entity a_e, IComponentTag* a_raw) {
                a_f(a_e, static_cast<T*>(a_raw));
                });
        }
        // マルチ用
        template <ComponentType T>
        void register_on_add(std::function<void(Entity, T*, size_t)> a_f)
        {
            static_assert(IsMultiComponent<T>::value, "Use multiComponent");
            CompID id = ECSManager::ComponentPool<T>::get_id();
            onAddMulti[id].push_back([a_f](Entity a_e, void* a_rawVec, size_t a_idx) {
                auto* vec = static_cast<std::vector<T> *>(a_rawVec);
                a_f(a_e, &(*vec)[a_idx], a_idx);
                });
        }

        // 単一コンポーネント用
        template <ComponentType T>
        void register_on_copy(std::function<void(Entity, Entity, T*)> a_f)
        {
            static_assert(!IsMultiComponent<T>::value, "Use singleComponent");
            CompID id = ECSManager::ComponentPool<T>::get_id();
            onCopySingle[id].push_back(
                [a_f](Entity a_src, Entity a_dst, IComponentTag* a_raw) {
                    a_f(a_src, a_dst, static_cast<T*>(a_raw));
                });
        }

        // マルチコンポーネント用
        template <ComponentType T>
        void register_on_copy(std::function<void(Entity, Entity, T*, size_t)> a_f)
        {
            static_assert(IsMultiComponent<T>::value, "Use multiComponent");
            CompID id = ECSManager::ComponentPool<T>::get_id();
            onCopyMulti[id].push_back(
                [a_f](Entity a_src, Entity a_dst, void* a_rawVec, size_t a_idx) {
                    auto* vec = static_cast<std::vector<T> *>(a_rawVec);
                    a_f(a_src, a_dst, &(*vec)[a_idx], a_idx);
                });
        }

        // 単一コンポーネント用
        template <ComponentType T>
        void register_on_remove(std::function<void(Entity, T*)> a_f)
        {
            static_assert(!IsMultiComponent<T>::value, "Use singleComponent");
            CompID id = ECSManager::ComponentPool<T>::get_id();
            onRemoveSingle[id].push_back([a_f](Entity a_e, IComponentTag* a_raw) {
                a_f(a_e, static_cast<T*>(a_raw));
                });
        }

        // マルチコンポーネント用（インスタンスごとに index 付き）
        template <ComponentType T>
        void register_on_remove(std::function<void(Entity, T*, size_t)> a_f)
        {
            static_assert(IsMultiComponent<T>::value, "Use multiComponent");
            CompID id = ECSManager::ComponentPool<T>::get_id();
            onRemoveMulti[id].push_back(
                [a_f](Entity a_e, void* a_rawVec, size_t a_idx) {
                    auto* vec = static_cast<std::vector<T> *>(a_rawVec);
                    a_f(a_e, &(*vec)[a_idx], a_idx);
                });
        }
        // 単一コンポーネント向け
        template <ComponentType T>
        void register_on_restore(std::function<void(Entity, T*)> a_f)
        {
            static_assert(!IsMultiComponent<T>::value, "Use multi for multi-component");
            CompID id = ECSManager::ComponentPool<T>::get_id();
            onRestoreSingle[id].push_back([a_f](Entity a_e, IComponentTag* a_raw) {
                a_f(a_e, static_cast<T*>(a_raw));
                });
        }

        // マルチコンポーネント向け
        template <ComponentType T>
        void register_on_restore(std::function<void(Entity, T*, size_t)> a_f)
        {
            static_assert(IsMultiComponent<T>::value,
                "Use single for single-component");
            CompID id = ECSManager::ComponentPool<T>::get_id();
            onRestoreMulti[id].push_back(
                [a_f](Entity a_e, void* a_rawVec, size_t a_idx) {
                    auto* vec = static_cast<std::vector<T> *>(a_rawVec);
                    a_f(a_e, &(*vec)[a_idx], a_idx);
                });
        }

    private:
        // ECS側から呼ばれる
        void on_component_added(Entity a_e, CompID a_compType) override
        {
            // ① 単一コンポーネント向け
            if (auto it = onAddSingle.find(a_compType); it != onAddSingle.end())
            {
                void* raw =
                    pEcs->get_raw_component_pool(a_compType)->get_raw_component(a_e);
                for (auto& cb : it->second)
                {
                    cb(a_e, static_cast<IComponentTag*>(raw));
                }
            }

            // ② マルチコンポーネント向け
            if (auto it2 = onAddMulti.find(a_compType); it2 != onAddMulti.end())
            {
                ECSManager::IComponentPool* pool =
                    pEcs->get_raw_component_pool(a_compType);
                void* rawVec = pool->get_raw_component(a_e);
                size_t count = pool->get_component_count(a_e);

                // **新しく追加されたインスタンスの index = count-1**
                size_t idxNew = (count == 0 ? 0 : count - 1);

                for (auto& cb : it2->second)
                {
                    cb(a_e, rawVec, idxNew);
                }
            }
        }
        void on_component_copied(Entity a_src, Entity a_dst,
            CompID a_compType) override
        {
            // 単一コンポーネント向け
            if (auto it = onCopySingle.find(a_compType); it != onCopySingle.end())
            {
                void* raw =
                    pEcs->get_raw_component_pool(a_compType)->get_raw_component(a_dst);
                for (auto& cb : it->second)
                {
                    cb(a_src, a_dst, static_cast<IComponentTag*>(raw));
                }
            }

            // マルチコンポーネント向け
            if (auto it2 = onCopyMulti.find(a_compType); it2 != onCopyMulti.end())
            {
                ECSManager::IComponentPool* pool =
                    pEcs->get_raw_component_pool(a_compType);
                void* rawVec = pool->get_raw_component(a_dst);
                size_t count = pool->get_component_count(a_dst);

                for (size_t idx = 0; idx < count; ++idx)
                {
                    for (auto& cb : it2->second)
                    {
                        cb(a_src, a_dst, rawVec, idx);
                    }
                }
            }
        }
        void on_component_removed(Entity a_e, CompID a_compType) override
        {
            // ① 単一コンポーネント向け
            if (auto it = onRemoveSingle.find(a_compType);
                it != onRemoveSingle.end())
            {
                void* raw =
                    pEcs->get_raw_component_pool(a_compType)->get_raw_component(a_e);
                for (auto& cb : it->second)
                {
                    cb(a_e, static_cast<IComponentTag*>(raw));
                }
            }

            // ② マルチコンポーネント向け
            if (auto it2 = onRemoveMulti.find(a_compType);
                it2 != onRemoveMulti.end())
            {
                ECSManager::IComponentPool* pool =
                    pEcs->get_raw_component_pool(a_compType);
                void* rawVec = pool->get_raw_component(a_e);
                size_t count = pool->get_component_count(a_e);

                for (size_t idx = 0; idx < count; ++idx)
                {
                    for (auto& cb : it2->second)
                    {
                        cb(a_e, rawVec, idx);
                    }
                }
            }
        }
        void on_component_removed_instance(Entity a_e, CompID a_compType,
            void* a_rawVec, size_t a_idx) override
        {
            if (auto it = onRemoveMulti.find(a_compType);
                it != onRemoveMulti.end())
            {
                for (auto& cb : it->second)
                {
                    cb(a_e, a_rawVec, a_idx);
                }
            }
        }

        void on_component_restored_from_prefab(Entity a_e,
            CompID a_compType) override
        {
            // 単一
            if (auto it = onRestoreSingle.find(a_compType);
                it != onRestoreSingle.end())
            {
                void* raw =
                    pEcs->get_raw_component_pool(a_compType)->get_raw_component(a_e);
                for (auto& cb : it->second)
                {
                    cb(a_e, static_cast<IComponentTag*>(raw));
                }
            }
            // マルチ
            if (auto it2 = onRestoreMulti.find(a_compType);
                it2 != onRestoreMulti.end())
            {
                auto* pool = pEcs->get_raw_component_pool(a_compType);
                void* rawVec = pool->get_raw_component(a_e);
                size_t count = pool->get_component_count(a_e);
                for (size_t idx = 0; idx < count; ++idx)
                {
                    for (auto& cb : it2->second)
                    {
                        cb(a_e, rawVec, idx);
                    }
                }
            }
        }

    private:
        ECSManager* pEcs = nullptr;

    protected:
        // 単一用
        std::unordered_map<CompID,
            std::vector<std::function<void(Entity, IComponentTag*)>>>
            onAddSingle;
        // マルチ用
        std::unordered_map<CompID,
            std::vector<std::function<void(Entity, void*, size_t)>>>
            onAddMulti;
        // 単一用
        std::unordered_map<
            CompID, std::vector<std::function<void(Entity, Entity, IComponentTag*)>>>
            onCopySingle;
        // マルチ用
        std::unordered_map<
            CompID, std::vector<std::function<void(Entity, Entity, void*, size_t)>>>
            onCopyMulti;
        // 単一用
        std::unordered_map<CompID,
            std::vector<std::function<void(Entity, IComponentTag*)>>>
            onRemoveSingle;
        // マルチ用
        std::unordered_map<CompID,
            std::vector<std::function<void(Entity, void*, size_t)>>>
            onRemoveMulti;
        // Restore（Prefab 復元）用マップ
        std::unordered_map<CompID,
            std::vector<std::function<void(Entity, IComponentTag*)>>>
            onRestoreSingle;
        std::unordered_map<CompID,
            std::vector<std::function<void(Entity, void*, size_t)>>>
            onRestoreMulti;
    };

    class Prototype
    {
    public:
        Prototype() = default;

        // ――――――――――――――――――
        //  ① 事前登録：扱う型すべてに対して呼ぶ
        //  Prefab::RegisterCopyFunc<YourComponent>();
        // ――――――――――――――――――
        template <ComponentType T> static void register_copy_func()
        {
            CompID id = ECSManager::ComponentPool<T>::get_id();

            if constexpr (IsMultiComponent<T>::value)
            {
                mMultiCopyFuncs[id] = [](Entity a_e, ECSManager& a_ecs, void* a_rawVec) {
                    auto* vec = static_cast<std::vector<T> *>(a_rawVec);
                    for (auto const& comp : *vec)
                    {
                        T* dst = a_ecs.add_component<T>(a_e);
                        *dst = comp;
                    }
                    };
            }
            else
            {
                mCopyFuncs[id] = [](Entity a_e, ECSManager& a_ecs, void* a_raw) {
                    T* dst = a_ecs.add_component<T>(a_e);
                    *dst = *static_cast<T*>(a_raw);
                    };
            }
        }

        template <ComponentType T> static void register_prefab_restore()
        {
            CompID id = ECSManager::ComponentPool<T>::get_id();

            // 単一コンポーネント用
            mPrefabRestoreFuncs[id] = [](Entity a_e, ECSManager& a_ecs, void* a_raw) {
                a_ecs.prefab_add_component<T>(a_e, *static_cast<T*>(a_raw));
                };

            // マルチコンポーネント用
            mPrefabRestoreMultiFuncs[id] = [](Entity a_e, ECSManager& a_ecs,
                void* a_rawVec) {
                    auto& vec = *static_cast<std::vector<T> *>(a_rawVec);
                    for (auto& inst : vec)
                    {
                        a_ecs.prefab_add_component<T>(a_e, inst);
                    }
                };
        }

        // ――――――――――――――――――
        //  ② 既存エンティティから Prefab を作る
        // ――――――――――――――――――
        static Prototype from_entity(ECSManager& a_ecs, Entity a_e)
        {
            Prototype prefab;
            const Archetype& arch = a_ecs.get_archetype(a_e);

            for (size_t id = 0; id < arch.size(); ++id)
            {
                if (!arch.test(id))
                {
                    continue;
                }
                auto* pool = a_ecs.get_raw_component_pool(id);
                void* raw = pool->get_raw_component(a_e);
                if (!raw)
                {
                    continue;
                }

                // 深いコピーを shared_ptr<void> で受け取る
                auto clonePtr = pool->clone_component(id, raw);
                if (pool->is_multi_component_trait(id))
                {
                    prefab.m_multiComponents[id] = std::move(clonePtr);
                }
                else
                {
                    prefab.m_components[id] = std::move(clonePtr);
                }

                prefab.m_archetype.set(id);
            }
            return prefab;
        }

        // ――――――――――――――――――
        //  ③ Instantiate するとき
        // ――――――――――――――――――
        Entity instantiate(ECSManager& a_ecs) const
        {
            // 1) エンティティ生成（派生で名前付けしたい場合はここをオーバーライド）
            Entity e = create_entity(a_ecs);

            // 2) コンポーネント復元（派生は基本的にそのまま使う）
            instantiate_components(e, a_ecs);

            // 3) 子 Prefab 再帰生成＆親子リンク
            instantiate_children(e, a_ecs);

            // 4) 完了フック（後始末的な処理があれば派生で）
            on_after_instantiate(e, a_ecs);

            return e;
        }

        // ――――――――――――――――――
        //  ④ Prefab にコンポーネントを追加するとき
        // ――――――――――――――――――
        template <ComponentType T> void add_component(const T& a_comp)
        {
            CompID id = ECSManager::ComponentPool<T>::get_id();
            if constexpr (IsMultiComponent<T>::value)
            {
                auto& ptr = m_multiComponents[id];
                if (!ptr)
                {
                    ptr = std::make_shared<std::vector<T>>();
                }
                auto& vec = *std::static_pointer_cast<std::vector<T>>(ptr);
                vec.push_back(a_comp);
            }
            else
            {
                m_components[id] = std::make_shared<T>(a_comp);
            }
            m_archetype.set(id);
        }

        // ――――――――――――――――――
        //  ⑤ 子 Prefab を追加
        // ――――――――――――――――――
        void add_sub_prefab(std::shared_ptr<Prototype> a_child)
        {
            m_subPrefabs.push_back(std::move(a_child));
            // 必要なら m_Archetype にもビットを立てる
        }

        // ――――――――――――――――――
        //  ⑥ 子 Prefab 一覧取得
        // ――――――――――――――――――
        std::vector<std::shared_ptr<Prototype>>& get_sub_prefabs() noexcept
        {
            return m_subPrefabs;
        }

        const std::vector<std::shared_ptr<Prototype>>&
            get_sub_prefabs() const noexcept
        {
            return m_subPrefabs;
        }

        // ――――――――――――――――――
        //  コンポーネントを 1 つだけ持つ場合の参照取得
        // ――――――――――――――――――
        template <ComponentType T> T& get_component()
        {
            CompID id = ECSManager::ComponentPool<T>::get_id();
            auto it = m_components.find(id);
            if (it == m_components.end())
            {
                throw std::runtime_error("Prefab にそのコンポーネントはありません");
            }
            return *std::static_pointer_cast<T>(it->second);
        }

        // ――――――――――――――――――
        //  マルチコンポーネントをすべて参照
        // ――――――――――――――――――
        template <ComponentType T>
        std::vector<T>& get_all_components()
            requires IsMultiComponent<T>::value
        {
            CompID id = ECSManager::ComponentPool<T>::get_id();
            auto it = m_multiComponents.find(id);
            if (it == m_multiComponents.end())
            {
                throw std::runtime_error("Prefab にそのマルチコンポーネントはありません");
            }
            return *std::static_pointer_cast<std::vector<T>>(it->second);
        }

        // 単一コンポーネント取得：存在しなければ nullptr
        template <ComponentType T> T* get_component_ptr() const noexcept
        {
            CompID id = ECSManager::ComponentPool<T>::get_id();
            auto it = m_components.find(id);
            if (it == m_components.end())
            {
                return nullptr;
            }
            // shared_ptr<void> を shared_ptr<T> にキャストして生ポインタを返す
            auto ptr = std::static_pointer_cast<T>(it->second);
            return ptr.get();
        }

        // マルチコンポーネント取得：存在しなければ nullptr
        template <ComponentType T>
        std::vector<T>* get_all_components_ptr() const noexcept
            requires IsMultiComponent<T>::value
        {
            CompID id = ECSManager::ComponentPool<T>::get_id();
            auto it = m_multiComponents.find(id);
            if (it == m_multiComponents.end())
            {
                return nullptr;
            }
            auto ptr = std::static_pointer_cast<std::vector<T>>(it->second);
            return ptr.get();
        }

        // ――――――――――――――――――
        //  単一コンポーネントを上書きする
        // ――――――――――――――――――
        template <ComponentType T> void set_component(const T& a_comp)
        {
            CompID id = ECSManager::ComponentPool<T>::get_id();
            m_components[id] = std::make_shared<T>(a_comp);
            m_archetype.set(id);
        }

        // ――――――――――――――――――
        //  単一コンポーネントを Prefab から削除
        // ――――――――――――――――――
        template <ComponentType T> void remove_component()
        {
            CompID id = ECSManager::ComponentPool<T>::get_id();
            // マップから単一コンポーネントのエントリを消す
            m_components.erase(id);
            // Archetype からビットをクリア
            m_archetype.reset(id);
        }

        // ――――――――――――――――――
        //  マルチコンポーネントの特定インスタンスを Prefab から削除
        // ――――――――――――――――――
        template <ComponentType T>
        void remove_component_instance(size_t a_idx)
            requires IsMultiComponent<T>::value
        {
            CompID id = ECSManager::ComponentPool<T>::get_id();
            auto it = m_multiComponents.find(id);
            if (it == m_multiComponents.end())
            {
                return;
            }

            auto& vec = *std::static_pointer_cast<std::vector<T>>(it->second);
            if (a_idx >= vec.size())
            {
                return;
            }
            vec.erase(vec.begin() + a_idx);

            // もし空になったら Prefab から完全に消す
            if (vec.empty())
            {
                m_multiComponents.erase(id);
                m_archetype.reset(id);
            }
        }

        // ――――――――――――――――――
        //  マルチコンポーネント全体を Prefab から削除
        // ――――――――――――――――――
        template <ComponentType T>
        void clear_all_components()
            requires IsMultiComponent<T>::value
        {
            CompID id = ECSManager::ComponentPool<T>::get_id();
            m_multiComponents.erase(id);
            m_archetype.reset(id);
        }

    protected:
        // —————— hooks ——————

        void populate_from_entity(ECSManager& a_ecs, Entity a_e)
        {
            const Archetype& arch = a_ecs.get_archetype(a_e);

            for (size_t id = 0; id < arch.size(); ++id)
            {
                if (!arch.test(id))
                {
                    continue;
                }
                auto* pool = a_ecs.get_raw_component_pool(id);
                void* raw = pool->get_raw_component(a_e);
                if (!raw)
                {
                    continue;
                }

                // 深いコピーを shared_ptr<void> で受け取る
                auto clonePtr = pool->clone_component(id, raw);
                if (pool->is_multi_component_trait(id))
                {
                    m_multiComponents[id] = std::move(clonePtr);
                }
                else
                {
                    m_components[id] = std::move(clonePtr);
                }

                m_archetype.set(id);
            }
        }

        // (1) まずエンティティを作る。追加の初期化／名前付けは here を override。
        virtual Entity create_entity(ECSManager& a_ecs) const
        {
            return a_ecs.generate_entity();
        }

        // (2) m_Components/m_MultiComponents を使って既存の復元ロジック
        virtual void instantiate_components(Entity a_e, ECSManager& a_ecs) const
        {
            // 単一コンポーネント
            for (auto const& [id, rawPtr] : m_components)
            {
                auto it = mPrefabRestoreFuncs.find(id);
                if (it != mPrefabRestoreFuncs.end())
                {
                    it->second(a_e, a_ecs, rawPtr.get());
                }
            }

            // マルチコンポーネント
            for (auto const& [id, rawVec] : m_multiComponents)
            {
                auto it = mPrefabRestoreMultiFuncs.find(id);
                if (it != mPrefabRestoreMultiFuncs.end())
                {
                    it->second(a_e, a_ecs, rawVec.get());
                }
            }
        }

        // (3) 子 Prefab を再帰的にインスタンス化し、リンク用フックを呼ぶ
        virtual void instantiate_children(Entity a_parent, ECSManager& a_ecs) const
        {
            for (auto const& child : m_subPrefabs)
            {
                Entity childEnt = child->instantiate(a_ecs);
                link_parent_child(a_parent, childEnt, a_ecs);
            }
        }

        // 親→子を結びつけたい場合はここを override
        virtual void link_parent_child(Entity /*parent*/, Entity /*child*/,
            ECSManager& /*ecs*/) const
        {
            // default: なにもしない
        }

        // (4) 全部終わったあとに追加処理したい場合はここ
        virtual void on_after_instantiate(Entity /*e*/, ECSManager& /*ecs*/) const
        {
            // default: なにもしない
        }

    private:
        Archetype m_archetype;
        // void ポインタで型消去したストレージ
        std::unordered_map<CompID, std::shared_ptr<void>> m_components;
        std::unordered_map<CompID, std::shared_ptr<void>> m_multiComponents;

        // 追加：ネストされた Prefab のリスト
        std::vector<std::shared_ptr<Prototype>> m_subPrefabs;

        // インスタンス化用マップ
        inline static std::unordered_map<
            CompID, std::function<void(Entity, ECSManager&, void*)>>
            mCopyFuncs;
        inline static std::unordered_map<
            CompID, std::function<void(Entity, ECSManager&, void*)>>
            mMultiCopyFuncs;
        // 復元処理マップ
        inline static std::unordered_map<
            CompID, std::function<void(Entity, ECSManager&, void*)>>
            mPrefabRestoreFuncs;
        inline static std::unordered_map<
            CompID, std::function<void(Entity, ECSManager&, void*)>>
            mPrefabRestoreMultiFuncs;
    };
} // namespace Cue::ECS
