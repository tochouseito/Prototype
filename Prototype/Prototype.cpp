// GameWorld の主要 API をまとめて確認するためのサンプル。

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#pragma warning(disable : 4189)
#pragma warning(disable : 4201)

#include <assert.h>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

#include "Components.h"
#include "GameWorld.h"

int main()
{
    SetConsoleOutputCP(CP_UTF8);

    using Cue::Result;
    using Cue::to_string;
    using Cue::ECS::TransformComponent;
    using Cue::GameCore::BaseComponent;
    using Cue::GameCore::GameObject;
    using Cue::GameCore::GameWorld;
    using Cue::GameCore::ObjectDefinition;
    using Cue::GameCore::SceneAsset;

    auto make_transform = [](float a_x, float a_y, float a_z)
    {
        TransformComponent transform{};
        transform.x = a_x;
        transform.y = a_y;
        transform.z = a_z;
        return transform;
    };

    auto expect_ok = [](const Result& a_result, const char* a_context)
    {
        if (a_result)
        {
            return;
        }

        std::cerr
            << a_context
            << " failed: code=" << to_string(a_result.code)
            << ", message=" << a_result.message
            << '\n';
        assert(false && "Unexpected Result failure.");
    };

    auto print_object = [](uint32_t a_entityId, uint64_t a_sceneId,
                            GameObject a_object)
    {
        BaseComponent* base = nullptr;
        (void)a_object.get_component<BaseComponent>(base);
        TransformComponent* transform = nullptr;
        (void)a_object.get_component<TransformComponent>(transform);

        std::cout
            << "entityId=" << a_entityId
            << ", sceneId=" << a_sceneId
            << ", name=" << (base ? base->name : "<missing>")
            << ", tag=" << (base ? base->tag : "<missing>");

        if (transform != nullptr)
        {
            std::cout
                << ", position=("
                << transform->x << ", "
                << transform->y << ", "
                << transform->z << ")";
        }

        std::cout << '\n';
    };

    SceneAsset colors("Colors");
    // SceneAsset 側に初期配置用の ObjectDefinition を定義しておく。
    auto& red = colors.add_object("red", "Color");
    red.prototype.add_component<TransformComponent>(make_transform(1.0f, 0.0f, 0.0f));
    auto& green = colors.add_object("green", "Color");
    green.prototype.add_component<TransformComponent>(make_transform(0.0f, 1.0f, 0.0f));
    auto& blue = colors.add_object("blue", "Color");
    blue.prototype.add_component<TransformComponent>(make_transform(0.0f, 0.0f, 1.0f));

    SceneAsset shapes("Shapes");
    auto& circle = shapes.add_object("circle", "Shape");
    circle.prototype.add_component<TransformComponent>(make_transform(10.0f, 0.0f, 0.0f));
    auto& square = shapes.add_object("square", "Shape");
    square.prototype.add_component<TransformComponent>(make_transform(20.0f, 0.0f, 0.0f));
    auto& triangle = shapes.add_object("triangle", "Shape");
    triangle.prototype.add_component<TransformComponent>(make_transform(30.0f, 0.0f, 0.0f));

    std::vector<ObjectDefinition> shapeExtras{};
    // 既存 Scene への追加投入用オブジェクト。
    shapeExtras.emplace_back("hexagon", "Shape");
    shapeExtras.back().prototype.add_component<TransformComponent>(
        make_transform(40.0f, 0.0f, 0.0f));
    shapeExtras.emplace_back("star", "Shape");
    shapeExtras.back().prototype.add_component<TransformComponent>(
        make_transform(50.0f, 0.0f, 0.0f));

    ObjectDefinition octagon("octagon", "Shape");
    octagon.prototype.add_component<TransformComponent>(
        make_transform(60.0f, 0.0f, 0.0f));

    GameWorld world;
    // 同名でも自動で連番が付与される。
    GameObject enemy0{};
    expect_ok(world.create_object("Enemy", "Actor", false, enemy0), "create enemy0");
    GameObject enemy1{};
    expect_ok(world.create_object("Enemy", "Actor", false, enemy1), "create enemy1");
    GameObject enemy2{};
    expect_ok(world.create_object("Enemy", "Actor", false, enemy2), "create enemy2");
    GameObject unnamedObject{};
    expect_ok(world.create_object("", "Actor", false, unnamedObject),
        "create unnamed object");

    auto object_name = [&expect_ok](const GameObject& a_object, const char* a_context)
    {
        std::string name{};
        expect_ok(a_object.name(name), a_context);
        return name;
    };

    auto object_persistent = [&expect_ok](const GameObject& a_object, const char* a_context)
    {
        bool isPersistent = false;
        expect_ok(a_object.is_persistent(isPersistent), a_context);
        return isPersistent;
    };

    auto object_active = [&expect_ok](const GameObject& a_object, const char* a_context)
    {
        bool isActive = false;
        expect_ok(a_object.is_active(isActive), a_context);
        return isActive;
    };

    auto contains_object = [&world, &expect_ok](uint32_t a_entityId)
    {
        bool contains = false;
        expect_ok(world.contains_object(a_entityId, contains), "contains_object");
        return contains;
    };

    auto contains_scene = [&world, &expect_ok](uint64_t a_sceneId)
    {
        bool contains = false;
        expect_ok(world.contains_scene(a_sceneId, contains), "contains_scene");
        return contains;
    };

    auto source_scene_id = [&world, &expect_ok](uint32_t a_entityId)
    {
        uint64_t sceneId = Cue::GameCore::k_invalidSceneId;
        expect_ok(world.source_scene_id(a_entityId, sceneId), "source_scene_id");
        return sceneId;
    };

    if (object_name(enemy0, "enemy0.name") != "Enemy" ||
        object_name(enemy1, "enemy1.name") != "Enemy(1)" ||
        object_name(enemy2, "enemy2.name") != "Enemy(2)")
    {
        assert(false && "Unexpected unique name assignment.");
    }

    if (object_name(unnamedObject, "unnamedObject.name") != "GameObject")
    {
        assert(false && "Unexpected default object name.");
    }

    expect_ok(enemy0.set_active(false), "enemy0.set_active(false)");
    if (object_active(enemy0, "enemy0.is_active false"))
    {
        assert(false && "enemy0 should be inactive.");
    }
    expect_ok(enemy0.set_active(true), "enemy0.set_active(true)");
    if (!object_active(enemy0, "enemy0.is_active true"))
    {
        assert(false && "enemy0 should be active.");
    }

    GameWorld::LoadSceneResult colorScene{};
    expect_ok(world.load_scene(colors, colorScene), "load colors");
    GameWorld::LoadSceneResult shapeScene1{};
    expect_ok(world.load_scene(shapes, shapeScene1), "load shapes 1");
    GameWorld::LoadSceneResult shapeScene2{};
    expect_ok(world.load_scene(shapes, shapeScene2), "load shapes 2");
    GameWorld::LoadSceneResult shapeAppendResult{};
    expect_ok(world.append_to_scene(shapeScene1.sceneId, shapeExtras, shapeAppendResult),
        "append shapes");
    GameObject octagonObject{};
    expect_ok(world.append_object_to_scene(shapeScene1.sceneId, octagon, octagonObject),
        "append octagon");

    if (!contains_scene(shapeAppendResult.sceneId))
    {
        assert(false && "Failed to append objects to scene.");
    }

    if (!octagonObject.is_valid())
    {
        assert(false && "Failed to append object to scene.");
    }

    std::cout << "\n[遅延削除キュー]\n";
    // Object 削除と Scene アンロードは、この時点では予約されるだけ。
    expect_ok(world.destroy_object(shapeScene1.objects[1].entity_id()), "destroy square");
    GameObject persistentShape = shapeScene2.objects[0];
    expect_ok(persistentShape.set_persistent(true), "persistentShape.set_persistent");
    expect_ok(world.unload_scene(shapeScene2.sceneId), "unload shapeScene2");

    const bool squareAliveBeforeFlush =
        contains_object(shapeScene1.objects[1].entity_id());
    const bool sceneAliveBeforeFlush = contains_scene(shapeScene2.sceneId);

    std::cout
        << "flush 前: squareAlive=" << squareAliveBeforeFlush
        << ", sceneLoaded=" << sceneAliveBeforeFlush
        << '\n';

    // ここで遅延削除キューを実行し、実際に破棄する。
    expect_ok(world.execute_deferred_deletions(), "execute_deferred_deletions");

    const bool squareAliveAfterFlush =
        contains_object(shapeScene1.objects[1].entity_id());
    const bool sceneAliveAfterFlush = contains_scene(shapeScene2.sceneId);
    const bool persistentAliveAfterFlush =
        contains_object(persistentShape.entity_id());
    const bool persistentFlagAfterFlush =
        object_persistent(persistentShape, "persistentShape.is_persistent");
    const bool persistentSourceCleared =
        source_scene_id(persistentShape.entity_id()) ==
        Cue::GameCore::k_invalidSceneId;

    std::cout
        << "flush 後: squareAlive=" << squareAliveAfterFlush
        << ", sceneLoaded=" << sceneAliveAfterFlush
        << ", persistentAlive=" << persistentAliveAfterFlush
        << ", persistentFlag=" << persistentFlagAfterFlush
        << '\n';

    if (!squareAliveBeforeFlush || !sceneAliveBeforeFlush ||
        squareAliveAfterFlush || sceneAliveAfterFlush ||
        !persistentAliveAfterFlush || !persistentFlagAfterFlush ||
        !persistentSourceCleared)
    {
        assert(false && "Deferred deletion queue behaved unexpectedly.");
    }

    expect_ok(persistentShape.destroy(), "persistentShape.destroy");
    expect_ok(world.execute_deferred_deletions(), "execute_deferred_deletions after destroy");

    std::cout << "[単体アクセス]\n";
    expect_ok(world.visit_object(octagonObject.entity_id(), print_object), "visit octagon");

    std::cout << "\n[Scene 単位アクセス]\n";
    expect_ok(world.for_each_object_in_scene(shapeScene1.sceneId, print_object),
        "for_each_object_in_scene");

    std::cout << "\n[タグ検索]\n";
    std::vector<GameObject> shapeObjects{};
    expect_ok(world.find_objects_by_tag("Shape", shapeObjects), "find_objects_by_tag");
    for (const GameObject& object : shapeObjects)
    {
        print_object(object.entity_id(), source_scene_id(object.entity_id()), object);
    }

    if (shapeObjects.size() != 5)
    {
        assert(false && "Unexpected tag search result.");
    }

    std::cout << "\n[名前検索]\n";
    GameObject triangleObject{};
    expect_ok(world.find_object_by_name("triangle", triangleObject), "find_object_by_name");
    if (!triangleObject.is_valid())
    {
        assert(false && "Failed to find object by name.");
    }
    print_object(
        triangleObject.entity_id(), source_scene_id(triangleObject.entity_id()), triangleObject);

    std::cout << "\n[連番名検索]\n";
    std::vector<GameObject> enemyObjects{};
    expect_ok(world.find_objects_by_name_series("Enemy", enemyObjects),
        "find_objects_by_name_series");
    for (const GameObject& object : enemyObjects)
    {
        print_object(object.entity_id(), source_scene_id(object.entity_id()), object);
    }

    if (enemyObjects.size() != 3)
    {
        assert(false && "Failed to find object name series.");
    }

    std::cout << "\n[全件アクセス]\n";
    expect_ok(world.for_each_object(print_object), "for_each_object");

    size_t objectCount = 0;
    expect_ok(world.object_count(objectCount), "object_count");
    size_t sceneCount = 0;
    expect_ok(world.scene_count(sceneCount), "scene_count");

    std::cout
        << "\nobjectCount=" << objectCount
        << ", sceneCount=" << sceneCount
        << ", colorSceneId=" << colorScene.sceneId
        << '\n';

    return 0;
}
