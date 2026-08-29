#include "AYTest.h"

#include "AYEditor/EditorCommandStack.h"
#include "AYEditor/EditorSceneDocument.h"
#include "AYEditor/EditorSelection.h"
#include "AYEntity.h"
#include "AYEntity/components/TransformComponent.h"
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

    EditorCommandStack commands;
    int changeCount = 0;
    commands.setChangedCallback([&changeCount]() { ++changeCount; });

    EditorTransformState after;
    after.position = {4.0f, 5.0f, 6.0f};
    after.rotation = ayt::math::FQuaternion::identity();
    after.scale = {2.0f, 3.0f, 4.0f};
    CHECK(commands.executeTransform(world, entity->getId(), after));
    CHECK(transform->position.x == 4.0f);
    CHECK(transform->scale.z == 4.0f);
    CHECK(commands.canUndo());

    CHECK(commands.undo());
    CHECK(transform->position.x == 0.0f);
    CHECK(transform->scale.z == 1.0f);
    CHECK(commands.canRedo());

    CHECK(commands.redo());
    CHECK(transform->position.x == 4.0f);
    CHECK(transform->scale.z == 4.0f);
    CHECK(changeCount == 3);
}

TEST_SUITE_END
