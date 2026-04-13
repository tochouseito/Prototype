#pragma once

// === C++ includes ===
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <algorithm>
#include <span>
#include <utility>

template <class T>
class BatchedRegistry final
{
public:
    using batch_id_type = uint64_t;
    using item_id_type = uint64_t;
    using item_type = T;

    static constexpr batch_id_type k_invalidBatchId = 0;
    static constexpr item_id_type k_invalidItemId = 0;

    struct InsertGroupResult final
    {
        batch_id_type batchId = k_invalidBatchId;
        std::vector<item_id_type> itemIds;
    };

private:
    struct StoredItem final
    {
        item_type value{};
        batch_id_type batchId = k_invalidBatchId;
        item_id_type itemId = k_invalidItemId;
        size_t batchPosition = 0;
    };

    struct BatchInfo final
    {
        std::vector<item_id_type> itemIds;
    };

public:
    [[nodiscard]] InsertGroupResult insert_group(std::span<const item_type> a_group)
    {
        // 1) batchId を発行
        const batch_id_type batchId = generate_batch_id();

        // 2) バッチ情報を作成
        BatchInfo batchInfo{};
        batchInfo.itemIds.reserve(a_group.size());

        // 3) 戻り値を準備
        InsertGroupResult result{};
        result.batchId = batchId;
        append_items_to_batch(batchId, batchInfo, a_group, result.itemIds);

        // 5) バッチ登録
        m_batches.emplace(batchId, std::move(batchInfo));

        return result;
    }

    [[nodiscard]] InsertGroupResult insert_group(std::vector<item_type>&& a_group)
    {
        // 1) batchId を発行
        const batch_id_type batchId = generate_batch_id();

        // 2) バッチ情報を作成
        BatchInfo batchInfo{};
        batchInfo.itemIds.reserve(a_group.size());

        // 3) 戻り値を準備
        InsertGroupResult result{};
        result.batchId = batchId;
        append_items_to_batch_move(batchId, batchInfo, std::move(a_group), result.itemIds);

        // 4) バッチ登録
        m_batches.emplace(batchId, std::move(batchInfo));

        return result;
    }

    [[nodiscard]] InsertGroupResult append_to_group(batch_id_type a_batchId, std::span<const item_type> a_group)
    {
        // 1) 既存 batch を探す
        auto batchIt = m_batches.find(a_batchId);
        if (batchIt == m_batches.end())
        {
            return {};
        }

        // 2) 戻り値を準備して追記
        InsertGroupResult result{};
        result.batchId = a_batchId;
        append_items_to_batch(a_batchId, batchIt->second, a_group, result.itemIds);
        return result;
    }

    [[nodiscard]] InsertGroupResult append_to_group(batch_id_type a_batchId, std::vector<item_type>&& a_group)
    {
        // 1) 既存 batch を探す
        auto batchIt = m_batches.find(a_batchId);
        if (batchIt == m_batches.end())
        {
            return {};
        }

        // 2) 戻り値を準備して追記
        InsertGroupResult result{};
        result.batchId = a_batchId;
        append_items_to_batch_move(a_batchId, batchIt->second, std::move(a_group), result.itemIds);
        return result;
    }

    [[nodiscard]] item_id_type append_item_to_group(batch_id_type a_batchId, const item_type& a_value)
    {
        // 1) 既存 batch を探す
        auto batchIt = m_batches.find(a_batchId);
        if (batchIt == m_batches.end())
        {
            return k_invalidItemId;
        }

        // 2) 1 要素だけ追記
        return append_item_to_batch(a_batchId, batchIt->second, a_value);
    }

    [[nodiscard]] item_id_type append_item_to_group(batch_id_type a_batchId, item_type&& a_value)
    {
        // 1) 既存 batch を探す
        auto batchIt = m_batches.find(a_batchId);
        if (batchIt == m_batches.end())
        {
            return k_invalidItemId;
        }

        // 2) 1 要素だけ追記
        return append_item_to_batch(a_batchId, batchIt->second, std::move(a_value));
    }

    [[nodiscard]] bool erase_item(item_id_type a_itemId)
    {
        // 1) itemId からインデックス取得
        auto itemIt = m_itemToIndex.find(a_itemId);
        if (itemIt == m_itemToIndex.end())
        {
            return false;
        }

        const size_t removeIndex = itemIt->second;
        StoredItem& removeItem = m_items[removeIndex];

        const batch_id_type batchId = removeItem.batchId;
        const size_t batchPosition = removeItem.batchPosition;

        // 2) バッチ側からこの itemId を swap-erase で除去
        auto batchIt = m_batches.find(batchId);
        if (batchIt == m_batches.end())
        {
            return false;
        }

        BatchInfo& batchInfo = batchIt->second;
        const size_t lastBatchPos = batchInfo.itemIds.size() - 1;
        const item_id_type movedBatchItemId = batchInfo.itemIds[lastBatchPos];

        if (batchPosition != lastBatchPos)
        {
            batchInfo.itemIds[batchPosition] = movedBatchItemId;

            auto movedBatchItemIt = m_itemToIndex.find(movedBatchItemId);
            if (movedBatchItemIt == m_itemToIndex.end())
            {
                throw std::runtime_error("NumberContainer internal error: moved batch item not found.");
            }

            m_items[movedBatchItemIt->second].batchPosition = batchPosition;
        }

        batchInfo.itemIds.pop_back();

        // 3) バッチが空なら消す
        if (batchInfo.itemIds.empty())
        {
            m_batches.erase(batchIt);
        }

        // 4) 本体配列からこの要素を swap-erase で除去
        const size_t lastIndex = m_items.size() - 1;
        const item_id_type movedItemId = m_items[lastIndex].itemId;

        if (removeIndex != lastIndex)
        {
            m_items[removeIndex] = std::move(m_items[lastIndex]);
            m_itemToIndex[movedItemId] = removeIndex;
        }

        m_items.pop_back();
        m_itemToIndex.erase(itemIt);

        return true;
    }

    [[nodiscard]] bool erase_group(batch_id_type a_batchId)
    {
        // 1) バッチ存在確認
        auto batchIt = m_batches.find(a_batchId);
        if (batchIt == m_batches.end())
        {
            return false;
        }

        // 2) 対象 batch の全 item を後ろから個別削除
        //    erase_item() の中で batch 自体が消えるので、
        //    毎回 find し直して末尾を取る
        while (true)
        {
            auto currentBatchIt = m_batches.find(a_batchId);
            if (currentBatchIt == m_batches.end())
            {
                break;
            }

            BatchInfo& batchInfo = currentBatchIt->second;
            if (batchInfo.itemIds.empty())
            {
                m_batches.erase(currentBatchIt);
                break;
            }

            const item_id_type itemId = batchInfo.itemIds.back();
            const bool erased = erase_item(itemId);
            if (!erased)
            {
                throw std::runtime_error("NumberContainer internal error: erase_group failed.");
            }
        }

        return true;
    }

    template <class F>
    [[nodiscard]] bool visit_item(item_id_type a_itemId, F&& a_func) const
    {
        // 1) itemId からインデックス取得
        auto itemIt = m_itemToIndex.find(a_itemId);
        if (itemIt == m_itemToIndex.end())
        {
            return false;
        }

        // 2) 対象 item を 1 件だけ渡す
        const StoredItem& item = m_items[itemIt->second];
        a_func(item.itemId, item.batchId, item.value);
        return true;
    }

    template <class F>
    [[nodiscard]] bool for_each_item_in_group(batch_id_type a_batchId, F&& a_func) const
    {
        // 1) batch の存在確認
        auto batchIt = m_batches.find(a_batchId);
        if (batchIt == m_batches.end())
        {
            return false;
        }

        // 2) batch に属する item を列挙
        const BatchInfo& batchInfo = batchIt->second;
        for (const item_id_type itemId : batchInfo.itemIds)
        {
            auto itemIt = m_itemToIndex.find(itemId);
            if (itemIt == m_itemToIndex.end())
            {
                throw std::runtime_error("NumberContainer internal error: group item not found.");
            }

            const StoredItem& item = m_items[itemIt->second];
            a_func(item.itemId, item.batchId, item.value);
        }

        return true;
    }

    template <class F>
    void for_each_item(F&& a_func) const
    {
        // 1) 全要素を列挙
        for (const StoredItem& item : m_items)
        {
            a_func(item.itemId, item.batchId, item.value);
        }
    }

    template <class F>
    void for_each_value(F&& a_func) const
    {
        // 1) 値だけ列挙
        for (const StoredItem& item : m_items)
        {
            a_func(item.value);
        }
    }

    [[nodiscard]] bool contains_item(item_id_type a_itemId) const
    {
        // 1) item の存在確認
        return m_itemToIndex.find(a_itemId) != m_itemToIndex.end();
    }

    [[nodiscard]] bool contains_batch(batch_id_type a_batchId) const
    {
        // 1) batch の存在確認
        return m_batches.find(a_batchId) != m_batches.end();
    }

    [[nodiscard]] size_t item_count() const noexcept
    {
        // 1) 生存要素数を返す
        return m_items.size();
    }

    [[nodiscard]] size_t batch_count() const noexcept
    {
        // 1) 生存 batch 数を返す
        return m_batches.size();
    }

    void clear() noexcept
    {
        // 1) 全データを破棄
        m_items.clear();
        m_itemToIndex.clear();
        m_batches.clear();

        // 2) ID カウンタはリセットしない
        //    過去に返した ID と衝突させないため
    }

private:
    template <class U>
    [[nodiscard]] item_id_type append_item_to_batch(
        batch_id_type a_batchId,
        BatchInfo& a_batchInfo,
        U&& a_value)
    {
        const item_id_type itemId = generate_item_id();
        const size_t batchPosition = a_batchInfo.itemIds.size();

        StoredItem item{};
        item.value = std::forward<U>(a_value);
        item.batchId = a_batchId;
        item.itemId = itemId;
        item.batchPosition = batchPosition;

        const size_t itemIndex = m_items.size();
        m_items.push_back(std::move(item));
        m_itemToIndex[itemId] = itemIndex;

        a_batchInfo.itemIds.push_back(itemId);
        return itemId;
    }

    void append_items_to_batch(
        batch_id_type a_batchId,
        BatchInfo& a_batchInfo,
        std::span<const item_type> a_group,
        std::vector<item_id_type>& a_outItemIds)
    {
        a_batchInfo.itemIds.reserve(a_batchInfo.itemIds.size() + a_group.size());
        a_outItemIds.reserve(a_group.size());

        for (const item_type& value : a_group)
        {
            const item_id_type itemId = append_item_to_batch(a_batchId, a_batchInfo, value);
            a_outItemIds.push_back(itemId);
        }
    }

    void append_items_to_batch_move(
        batch_id_type a_batchId,
        BatchInfo& a_batchInfo,
        std::vector<item_type>&& a_group,
        std::vector<item_id_type>& a_outItemIds)
    {
        a_batchInfo.itemIds.reserve(a_batchInfo.itemIds.size() + a_group.size());
        a_outItemIds.reserve(a_group.size());

        for (item_type& value : a_group)
        {
            const item_id_type itemId = append_item_to_batch(a_batchId, a_batchInfo, std::move(value));
            a_outItemIds.push_back(itemId);
        }
    }

    [[nodiscard]] batch_id_type generate_batch_id()
    {
        // 1) オーバーフロー防止
        if (m_nextBatchId == (std::numeric_limits<batch_id_type>::max)())
        {
            throw std::overflow_error("NumberContainer batch id overflow");
        }

        // 2) ID 発行
        const batch_id_type batchId = m_nextBatchId;
        ++m_nextBatchId;
        return batchId;
    }

    [[nodiscard]] item_id_type generate_item_id()
    {
        // 1) オーバーフロー防止
        if (m_nextItemId == (std::numeric_limits<item_id_type>::max)())
        {
            throw std::overflow_error("NumberContainer item id overflow");
        }

        // 2) ID 発行
        const item_id_type itemId = m_nextItemId;
        ++m_nextItemId;
        return itemId;
    }

private:
    std::vector<StoredItem> m_items;
    std::unordered_map<item_id_type, size_t> m_itemToIndex;
    std::unordered_map<batch_id_type, BatchInfo> m_batches;

    batch_id_type m_nextBatchId = 1;
    item_id_type m_nextItemId = 1;
};
