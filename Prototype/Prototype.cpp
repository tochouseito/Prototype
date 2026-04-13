// Prototype.cpp : このファイルには 'main' 関数が含まれています。プログラム実行の開始と終了がそこで行われます。
//

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

    auto print_object = [](uint32_t a_entityId, uint64_t a_sceneId,
                            GameObject a_object)
    {
        const BaseComponent* base = a_object.get_component<BaseComponent>();
        const TransformComponent* transform =
            a_object.get_component<TransformComponent>();

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
    const GameObject enemy0 = world.create_object("Enemy", "Actor");
    const GameObject enemy1 = world.create_object("Enemy", "Actor");
    const GameObject enemy2 = world.create_object("Enemy", "Actor");
    const GameObject unnamedObject = world.create_object("", "Actor");

    if (enemy0.name() != "Enemy" || enemy1.name() != "Enemy(1)" ||
        enemy2.name() != "Enemy(2)")
    {
        assert(false && "Unexpected unique name assignment.");
    }

    if (unnamedObject.name() != "GameObject")
    {
        assert(false && "Unexpected default object name.");
    }

    const auto colorScene = world.load_scene(colors);
    const auto shapeScene1 = world.load_scene(shapes);
    const auto shapeScene2 = world.load_scene(shapes);
    const auto shapeAppendResult =
        world.append_to_scene(shapeScene1.sceneId, shapeExtras);
    const auto octagonObject =
        world.append_object_to_scene(shapeScene1.sceneId, octagon);

    if (!world.contains_scene(shapeAppendResult.sceneId))
    {
        assert(false && "Failed to append objects to scene.");
    }

    if (!octagonObject.is_valid())
    {
        assert(false && "Failed to append object to scene.");
    }

    world.destroy_object(shapeScene1.objects[1].entity_id());

    const bool sceneUnloaded = world.unload_scene(shapeScene2.sceneId);
    if (!sceneUnloaded)
    {
        assert(false && "Failed to unload scene.");
    }

    world.execute_deferred_deletions();

    std::cout << "[Single Item Access]\n";
    const bool itemVisited =
        world.visit_object(octagonObject.entity_id(), print_object);
    if (!itemVisited)
    {
        assert(false && "Failed to visit item.");
    }

    std::cout << "\n[Group Access]\n";
    const bool groupVisited =
        world.for_each_object_in_scene(shapeScene1.sceneId, print_object);
    if (!groupVisited)
    {
        assert(false && "Failed to visit group.");
    }

    std::cout << "\n[Tag Access]\n";
    const std::vector<GameObject> shapeObjects = world.find_objects_by_tag("Shape");
    for (const GameObject& object : shapeObjects)
    {
        print_object(object.entity_id(), world.source_scene_id(object.entity_id()),
            object);
    }

    if (shapeObjects.size() != 5)
    {
        assert(false && "Unexpected tag search result.");
    }

    std::cout << "\n[Name Access]\n";
    const GameObject triangleObject = world.find_object_by_name("triangle");
    if (!triangleObject.is_valid())
    {
        assert(false && "Failed to find object by name.");
    }
    print_object(triangleObject.entity_id(),
        world.source_scene_id(triangleObject.entity_id()), triangleObject);

    std::cout << "\n[Name Series Access]\n";
    const std::vector<GameObject> enemyObjects =
        world.find_objects_by_name_series("Enemy");
    for (const GameObject& object : enemyObjects)
    {
        print_object(object.entity_id(), world.source_scene_id(object.entity_id()),
            object);
    }

    if (enemyObjects.size() != 3)
    {
        assert(false && "Failed to find object name series.");
    }

    std::cout << "\n[All Items Access]\n";
    world.for_each_object(print_object);

    std::cout
        << "\nobjectCount=" << world.object_count()
        << ", sceneCount=" << world.scene_count()
        << ", colorSceneId=" << colorScene.sceneId
        << '\n';

    return 0;
}
