#include "AYTest.h"

#include "AYEditor/EditorSceneDocument.h"
#include "AYEditor/EditorComponentPolicy.h"
#include "AYEditor/EditorSelection.h"
#include "AYEntity.h"
#include "AYEntity/components/TransformComponent.h"
#include "AYEntity/components/SpriteComponent.h"
#include "AYScene.h"

#include <chrono>
#include <filesystem>

using namespace ayt::editor;

TEST_SUITE(AYEditor_P0Core)

TEST_CASE(editor_document_keeps_scene_identity_across_new)
{
    EditorSceneDocument document;
    ayt::scene::Scene* scene = &document.scene();
    CHECK(!document.isDirty());

    document.newScene();

    CHECK(&document.scene() == scene);
    CHECK(document.path().empty());
    CHECK(document.title() == "Untitled");
    CHECK(document.isDirty());
}

TEST_CASE(editor_document_save_and_open_preserve_scene_identity)
{
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path = std::filesystem::temp_directory_path()
        / ("ayeditor_p0_" + std::to_string(nonce) + ".ayscene");

    EditorSceneDocument document;
    ayt::scene::Scene* scene = &document.scene();
    document.newScene();
    std::string error;
    CHECK(document.saveAs(path.string(), &error));
    CHECK(!document.isDirty());
    CHECK(document.path() == path.string());

    document.newScene();
    CHECK(document.open(path.string(), &error));
    CHECK(&document.scene() == scene);
    CHECK(!document.isDirty());

    std::error_code removeError;
    std::filesystem::remove(path, removeError);
}

TEST_CASE(editor_transform_command_executes_undoes_and_redoes)
{
    EditorSceneDocument document;
    ayt::entity::World& world = document.scene().world();
    ayt::entity::Entity* entity = world.createEntity();
    CHECK(entity != nullptr);
    auto* transform = entity->addComponent<ayt::entity::Transform>();
    CHECK(transform != nullptr);

    EditorSelection selection;
    CHECK(selection.select(entity->getId()));
    CHECK(selection.resolve(&world) == entity);

    int changeCount = 0;
    document.setHistoryChangedCallback(
        [&changeCount]() { ++changeCount; });

    EditorTransformState after;
    after.position = {4.0f, 5.0f, 6.0f};
    after.rotation = ayt::math::FQuaternion::identity();
    after.scale = {2.0f, 3.0f, 4.0f};
    CHECK(document.executeTransform(entity->getId(), after));
    CHECK(transform->position.x == 4.0f);
    CHECK(transform->scale.z == 4.0f);
    CHECK(document.canUndo());

    CHECK(document.undo());
    CHECK(transform->position.x == 0.0f);
    CHECK(transform->scale.z == 1.0f);
    CHECK(document.canRedo());

    CHECK(document.redo());
    CHECK(transform->position.x == 4.0f);
    CHECK(transform->scale.z == 4.0f);
    CHECK(changeCount == 3);
}

TEST_CASE(editor_scene_history_tracks_save_cursor_and_reload_boundary)
{
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path = std::filesystem::temp_directory_path()
        / ("ayeditor_history_" + std::to_string(nonce) + ".ayscene");

    EditorSceneDocument document;
    ayt::entity::World& world = document.scene().world();
    ayt::entity::Entity* entity = world.createEntity();
    CHECK(entity != nullptr);
    auto* transform = entity->addComponent<ayt::entity::Transform>();
    CHECK(transform != nullptr);
    document.markDirty();

    std::string error;
    CHECK(document.saveAs(path.string(), &error));
    CHECK(!document.isDirty());

    EditorTransformState after;
    after.position = {8.0f, 0.0f, 0.0f};
    CHECK(document.executeTransform(entity->getId(), after));
    CHECK(document.isDirty());
    CHECK(document.undo());
    CHECK(!document.isDirty());
    CHECK(document.redo());
    CHECK(document.isDirty());

    CHECK(document.open(path.string(), &error));
    CHECK(!document.isDirty());
    CHECK(!document.canUndo());
    CHECK(!document.canRedo());

    std::error_code removeError;
    std::filesystem::remove(path, removeError);
}

TEST_CASE(editor_scene_entity_commands_restore_serializable_components)
{
    EditorSceneDocument document;
    ayt::entity::World& world = document.scene().world();
    uint32_t entityId = 0;
    CHECK(document.createEntity(
        "Create Sprite",
        [](ayt::entity::Entity& entity) {
            entity.setName("Undo Sprite");
            auto* transform = entity.addComponent<ayt::entity::Transform>();
            auto* sprite = entity.addComponent<ayt::entity::SpriteComponent>();
            if (transform == nullptr || sprite == nullptr) return false;
            transform->setPosition(3.0f, 4.0f, 5.0f);
            sprite->texturePath = "textures/undo.aytex";
            return true;
        },
        &entityId));
    CHECK(world.findEntity(entityId) != nullptr);
    CHECK(document.undo());
    CHECK(world.findEntity(entityId) == nullptr);
    CHECK(document.redo());
    ayt::entity::Entity* restored = world.findEntity("Undo Sprite");
    CHECK(restored != nullptr);
    auto* restoredTransform = restored != nullptr
        ? restored->getComponent<ayt::entity::Transform>() : nullptr;
    auto* restoredSprite = restored != nullptr
        ? restored->getComponent<ayt::entity::SpriteComponent>() : nullptr;
    CHECK(restoredTransform != nullptr && restoredTransform->position.y == 4.0f);
    CHECK(restoredSprite != nullptr
        && restoredSprite->texturePath == "textures/undo.aytex");

    CHECK(restored != nullptr && document.mutateComponent(
        restored->getId(), "SpriteComponent", "Change Texture", {},
        [](ayt::entity::IComponent& component) {
            static_cast<ayt::entity::SpriteComponent&>(component).texturePath =
                "textures/changed.aytex";
            return true;
        }));

    CHECK(restored != nullptr && document.deleteEntity(restored->getId()));
    CHECK(world.findEntity("Undo Sprite") == nullptr);
    CHECK(document.undo());
    restored = world.findEntity("Undo Sprite");
    CHECK(restored != nullptr);
    CHECK(document.undo());
    restoredSprite = restored != nullptr
        ? restored->getComponent<ayt::entity::SpriteComponent>() : nullptr;
    CHECK(restoredSprite != nullptr
        && restoredSprite->texturePath == "textures/undo.aytex");
}

TEST_CASE(editor_scene_component_and_property_commands_share_history)
{
    EditorComponentPolicyRegistry::instance().installDefaults();
    EditorSceneDocument document;
    ayt::entity::World& world = document.scene().world();
    ayt::entity::Entity* entity = world.createEntity();
    CHECK(entity != nullptr);
    entity->setName("Component History");

    std::vector<std::string> added;
    std::string error;
    CHECK(document.addComponent(
        entity->getId(), "SpriteComponent", &added, &error));
    CHECK(entity->getComponent<ayt::entity::Transform>() != nullptr);
    auto* sprite = entity->getComponent<ayt::entity::SpriteComponent>();
    CHECK(sprite != nullptr);

    CHECK(document.mutateComponent(
        entity->getId(), "SpriteComponent", "Set Texture", {},
        [](ayt::entity::IComponent& component) {
            auto& sprite = static_cast<ayt::entity::SpriteComponent&>(component);
            sprite.texturePath = "textures/history.aytex";
            return true;
        }));
    CHECK(sprite->texturePath == "textures/history.aytex");
    CHECK(document.undo());
    CHECK(sprite->texturePath.empty());
    CHECK(document.redo());
    CHECK(sprite->texturePath == "textures/history.aytex");

    CHECK(document.removeComponent(
        entity->getId(), "SpriteComponent", &error));
    CHECK(entity->getComponent<ayt::entity::SpriteComponent>() == nullptr);
    CHECK(document.undo());
    sprite = entity->getComponent<ayt::entity::SpriteComponent>();
    CHECK(sprite != nullptr
        && sprite->texturePath == "textures/history.aytex");
}

TEST_SUITE_END
