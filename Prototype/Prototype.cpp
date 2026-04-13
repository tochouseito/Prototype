// Prototype.cpp : このファイルには 'main' 関数が含まれています。プログラム実行の開始と終了がそこで行われます。
//

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#pragma warning(disable : 4189)
#pragma warning(disable : 4201)

#include <iostream>
#include <utility>
#include <string>
#include <windows.h>
#include <assert.h>

#include "BatchedNumberRegistry.h"

int main()
{
    SetConsoleOutputCP(CP_UTF8);

    using Registry = BatchedRegistry<std::string>;

	Registry container;
    
    const std::vector<std::string> b = { "red", "green", "blue" };
    const std::vector<std::string> c = { "circle", "square", "triangle" };
    std::vector<std::string> cExtra = { "hexagon", "star" };
    std::string cSingleExtra = "octagon";

    const auto bResult = container.insert_group(b);
    const auto cResult1 = container.insert_group(c);
    const auto cResult2 = container.insert_group(c);
    const auto cAppendResult = container.append_to_group(cResult1.batchId, std::move(cExtra));
    const auto cSingleAppendItemId = container.append_item_to_group(cResult1.batchId, std::move(cSingleExtra));

    if (cAppendResult.batchId == Registry::k_invalidBatchId)
    {
        assert(false && "Failed to append to group.");
    }

    if (cSingleAppendItemId == Registry::k_invalidItemId)
    {
        assert(false && "Failed to append item to group.");
    }

    // 1回目の c の中の "square" を個別削除
    bool erased = container.erase_item(cResult1.itemIds[1]);

	if (!erased)
    {
		assert(false && "Failed to erase item.");
    }

    // 2回目の c をグループごと削除
    bool groupErased = container.erase_group(cResult2.batchId);

    if (!groupErased)
    {
		assert(false && "Failed to erase group.");
    }

    std::cout << "[Single Item Access]\n";
    const bool itemVisited = container.visit_item(
        cSingleAppendItemId,
        [](uint64_t a_itemId, uint64_t a_batchId, const std::string& a_value)
        {
            std::cout
                << "itemId=" << a_itemId
                << ", batchId=" << a_batchId
                << ", value=" << a_value
                << '\n';
        });

    if (!itemVisited)
    {
        assert(false && "Failed to visit item.");
    }

    std::cout << "\n[Group Access]\n";
    const bool groupVisited = container.for_each_item_in_group(
        cResult1.batchId,
        [](uint64_t a_itemId, uint64_t a_batchId, const std::string& a_value)
        {
            std::cout
                << "itemId=" << a_itemId
                << ", batchId=" << a_batchId
                << ", value=" << a_value
                << '\n';
        });

    if (!groupVisited)
    {
        assert(false && "Failed to visit group.");
    }

    std::cout << "\n[All Items Access]\n";
    container.for_each_item(
        [](uint64_t a_itemId, uint64_t a_batchId, const std::string& a_value)
        {
            std::cout
                << "itemId=" << a_itemId
                << ", batchId=" << a_batchId
                << ", value=" << a_value
                << '\n';
        });

    return 0;
}
