#include "AYTest.h"

#include "AYEditor/EditorSceneDocument.h"
#include "AYEditor/EditorComponentPolicy.h"
#include "AYEditor/EditorSelection.h"
#include "AYEntity.h"
#include "AYEntity/ComponentRegistry.h"
#include "AYSerializer/SerializerCore.h"
#include "AYEntity/components/TransformComponent.h"
#include "AYEntity/components/SpriteAnimationComponent.h"
#include "AYEntity/components/SpriteComponent.h"
#include "AYEntity/components/ScriptComponent.h"
#include "AYScene.h"

#include <chrono>
#include <filesystem>

using namespace ayt::editor;

namespace {
class EditorMultiProbe final : public ayt::entity::IComponent {
public:
    const char* getName() const override { return "EditorMultiProbe"; }
    Int32 value = 0;
};

bool registerEditorMultiProbe() {
    using namespace ayt::entity;
    ComponentDescriptor descriptor;
    descriptor.name = "test.EditorMultiProbe";
    descriptor.displayName = "Editor Multi Probe";
    descriptor.type = typeid(EditorMultiProbe);
    descriptor.size = sizeof(EditorMultiProbe);
    descriptor.alignment = alignof(EditorMultiProbe);
    descriptor.multiplicity = ComponentMultiplicity::Multiple;
    descriptor.editorAddable = true;
    descriptor.sceneSerializable = true;
    descriptor.add = [](Entity& entity) -> IComponent* {
        return entity.createComponent<EditorMultiProbe>();
    };
    descriptor.get = [](Entity& entity) -> IComponent* {
        return entity.getComponent<EditorMultiProbe>();
    };
    descriptor.has = [](const Entity& entity) {
        return entity.hasComponent<EditorMultiProbe>();
    };
    descriptor.remove = [](Entity& entity) { entity.removeComponent<EditorMultiProbe>(); };
    descriptor.serialize = [](ayt::serializer::ISerializer& s, const IComponent& c) {
        Int32 value = static_cast<const EditorMultiProbe&>(c).value;
        s.field("value", value);
    };
    descriptor.deserialize = [](ayt::serializer::ISerializer& s, IComponent& c) {
        s.field("value", static_cast<EditorMultiProbe&>(c).value);
    };
    return ComponentRegistry::instance().registerComponent(std::move(descriptor)).succeeded();
}
} // namespace

TEST_SUITE(AYEditor_P0Core)

TEST_CASE(editor_multi_components_have_independent_history_and_labels)
{
    CHECK(registerEditorMultiProbe());
    EditorSceneDocument document;
    auto* entity = document.scene().world().createEntity();
    entity->setName("Editor Multi");
    std::string error;
    CHECK(document.addComponent(entity->getId(), "test.EditorMultiProbe", nullptr, &error));
    CHECK(document.addComponent(entity->getId(), "test.EditorMultiProbe", nullptr, &error));
    auto values = entity->getComponents<EditorMultiProbe>();
    CHECK_INT_EQ(static_cast<int>(values.size()), 2);
    const std::string firstId = entity->componentInstance(values[0])->id;
    const std::string secondId = entity->componentInstance(values[1])->id;
    CHECK(firstId != secondId);
    CHECK(document.undo());
    CHECK(entity->findComponentById(secondId) == nullptr);
    CHECK(document.redo());
    CHECK(entity->findComponentById(secondId) != nullptr);
    CHECK(document.renameComponent(entity->getId(), secondId, "Right camera"));
    CHECK(entity->findComponentInstance(secondId)->displayName == "Right camera");
    CHECK(document.mutateComponentById(entity->getId(), secondId,
        "Set probe", {}, [](ayt::entity::IComponent& component) {
            static_cast<EditorMultiProbe&>(component).value = 42;
            return true;
        }));
    CHECK_INT_EQ(static_cast<EditorMultiProbe*>(entity->findComponentById(secondId))->value, 42);
    CHECK(document.removeComponentById(entity->getId(), firstId, &error));
    CHECK(entity->findComponentById(firstId) == nullptr);
    CHECK(document.undo());
    CHECK(entity->findComponentById(firstId) != nullptr);
    CHECK(document.undo());
    CHECK_INT_EQ(static_cast<EditorMultiProbe*>(entity->findComponentById(secondId))->value, 0);
    CHECK(document.redo());
    CHECK_INT_EQ(static_cast<EditorMultiProbe*>(entity->findComponentById(secondId))->value, 42);
}

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
        / ("ayeditor_p0_" + std::to_string(nonce) + ".scn");

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

TEST_CASE(editor_document_reads_legacy_scene_but_writes_current_suffix)
{
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto stem = std::filesystem::temp_directory_path()
        / ("ayeditor_legacy_" + std::to_string(nonce));
    const auto legacy = stem.string() + ".ayscene";
    const auto current = stem.string() + ".scn";
    EditorSceneDocument source;
    CHECK(source.scene().save(legacy));

    EditorSceneDocument document;
    std::string error;
    CHECK(document.open(legacy, &error));
    CHECK_FALSE(document.save(&error));
    CHECK_FALSE(document.saveAs(legacy, &error));
    CHECK(document.saveAs(current, &error));
    CHECK(std::filesystem::is_regular_file(current));

    std::error_code ignored;
    std::filesystem::remove(legacy, ignored);
    std::filesystem::remove(current, ignored);
}

TEST_CASE(editor_scene_rejects_transient_script_component_without_losing_file)
{
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path = std::filesystem::temp_directory_path()
        / ("ayeditor_transient_" + std::to_string(nonce) + ".scn");
    EditorSceneDocument document;
    auto* entity = document.scene().world().createEntity();
    CHECK(entity != nullptr);
    if (entity == nullptr) return;
    entity->setName("Transient Script");
    std::string error;
    CHECK_FALSE(document.addComponent(entity->getId(), "ScriptComponent",
                                      nullptr, &error));
    CHECK(error.find("cannot be saved") != std::string::npos);
    CHECK(entity->getComponent<ayt::entity::ScriptComponent>() == nullptr);

    CHECK(entity->addComponent<ayt::entity::ScriptComponent>() != nullptr);
    CHECK_FALSE(document.saveAs(path.string(), &error));
    CHECK(error.find("ScriptComponent") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(path));

    CHECK(document.removeComponent(entity->getId(), "ScriptComponent", &error));
    CHECK(document.saveAs(path.string(), &error));
    document.newScene();
    CHECK(document.open(path.string(), &error));
    CHECK(document.scene().world().findEntity("Transient Script") != nullptr);
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

TEST_CASE(editor_entity_rename_uses_scene_history)
{
    EditorSceneDocument document;
    ayt::entity::Entity* entity = document.scene().world().createEntity();
    CHECK(entity != nullptr);
    if (entity == nullptr) return;
    entity->setName("Cube");

    std::string error;
    CHECK(document.renameEntity(entity->getId(), "Player Cube", &error));
    CHECK(std::string(entity->getName()) == "Player Cube");
    CHECK(document.undo());
    CHECK(std::string(entity->getName()) == "Cube");
    CHECK(document.redo());
    CHECK(std::string(entity->getName()) == "Player Cube");
    CHECK_FALSE(document.renameEntity(entity->getId(), "   ", &error));
}

TEST_CASE(editor_scene_history_tracks_save_cursor_and_reload_boundary)
{
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path = std::filesystem::temp_directory_path()
        / ("ayeditor_history_" + std::to_string(nonce) + ".scn");

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

    CHECK_FALSE(document.removeComponent(
        entity->getId(), "Transform", &error));
    CHECK(entity->getComponent<ayt::entity::Transform>() != nullptr);
}

TEST_CASE(sprite_animation_authoring_adds_and_protects_sprite_requirement)
{
    EditorComponentPolicyRegistry::instance().installDefaults();
    EditorSceneDocument document;
    ayt::entity::World& world = document.scene().world();
    ayt::entity::Entity* entity = world.createEntity();
    CHECK(entity != nullptr);
    if (entity == nullptr) return;

    std::vector<std::string> added;
    std::string error;
    CHECK(document.addComponent(
        entity->getId(), "SpriteAnimationComponent", &added, &error));
    CHECK(error.empty());
    CHECK(entity->getComponent<ayt::entity::Transform>() != nullptr);
    CHECK(entity->getComponent<ayt::entity::SpriteComponent>() != nullptr);
    CHECK(entity->getComponent<
        ayt::entity::SpriteAnimationComponent>() != nullptr);
    CHECK_FALSE(document.removeComponent(
        entity->getId(), "SpriteComponent", &error));
    CHECK(document.removeComponent(
        entity->getId(), "SpriteAnimationComponent", &error));
    CHECK(document.removeComponent(
        entity->getId(), "SpriteComponent", &error));
}

TEST_SUITE_END
